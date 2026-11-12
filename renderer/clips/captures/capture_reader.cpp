#include "renderer/clips/captures/capture_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "renderer/clips/captures/curves.h"

namespace renderer::clips::captures {
namespace fs = std::filesystem;

namespace {

constexpr char RAW_MAGIC[8] = {'N', 'R', 'R', 'A', 'W', '1', 0, 0};

struct RawHeader {
    char magic[8];
    uint32_t version, width, height, dxgi, channels, reserved;
};
static_assert(sizeof(RawHeader) == 32);

struct File {
    FILE* f = nullptr;
    explicit File(const std::string& path, const char* mode) { f = std::fopen(path.c_str(), mode); }
    ~File() {
        if (f)
            std::fclose(f);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

uint64_t FileSize(const std::string& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    return ec ? 0 : (uint64_t)size;
}

bool ReadExactly(FILE* f, void* data, size_t bytes) {
    return std::fread(data, 1, bytes, f) == bytes;
}

}  // namespace

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu, bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {  // subnormal
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400u)) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x3ffu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint16_t FloatToHalf(float value) {
    uint32_t x;
    std::memcpy(&x, &value, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int exponent = (int)((x >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = x & 0x7fffffu;
    if (((x >> 23) & 0xffu) == 0xffu)  // inf, nan
        return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    if (exponent >= 31)
        return (uint16_t)(sign | 0x7c00u);
    if (exponent <= 0) {
        if (exponent < -10)
            return (uint16_t)sign;
        mantissa |= 0x800000u;
        const int shift = 14 - exponent;
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u), middle = 1u << (shift - 1);
        if (rest > middle || (rest == middle && (half & 1u)))
            half++;
        return (uint16_t)(sign | half);
    }
    uint32_t half = ((uint32_t)exponent << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
        half++;  // may carry into the exponent, which is the right rounding
    return (uint16_t)(sign | half);
}

int CodeSteps(int dxgi) {
    switch (dxgi) {
        case 24: return 1023;
        case 28: case 29: case 87: case 91: return 255;
        default: return 0;
    }
}

bool ReadColor(const std::string& path, const TextureInfo& info, Image& image, std::string& error) {
    const uint64_t bytes = (uint64_t)info.width * info.height * 4 * 2;
    if (info.texel != "float16x4") {
        error = path + ": not a float16x4 texture";
        return false;
    }
    if (FileSize(path) != bytes) {
        error = path + ": the file is not " + std::to_string(info.width) + "x" + std::to_string(info.height) + " float16x4";
        return false;
    }
    File file(path, "rb");
    image.width = info.width;
    image.height = info.height;
    image.dxgi = info.dxgi;
    image.rgba.resize((size_t)info.width * info.height * 4);
    if (!file.f || !ReadExactly(file.f, image.rgba.data(), (size_t)bytes)) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

bool ReadRawHeader(const std::string& path, uint32_t& width, uint32_t& height, int& dxgi) {
    File file(path, "rb");
    RawHeader h;
    if (!file.f || !ReadExactly(file.f, &h, sizeof(h)) || std::memcmp(h.magic, RAW_MAGIC, 8) != 0 || h.version != 1 || h.channels != 4)
        return false;
    width = h.width;
    height = h.height;
    dxgi = (int)h.dxgi;
    return true;
}

bool ReadRawFrame(const std::string& path, Image& image, std::string& error) {
    File file(path, "rb");
    RawHeader h;
    if (!file.f || !ReadExactly(file.f, &h, sizeof(h)) || std::memcmp(h.magic, RAW_MAGIC, 8) != 0 || h.version != 1 || h.channels != 4) {
        error = path + ": not a raw frame";
        return false;
    }
    const uint64_t bytes = (uint64_t)h.width * h.height * 8;
    if (FileSize(path) != bytes + sizeof(h)) {
        error = path + ": the size does not match its header";
        return false;
    }
    image.width = h.width;
    image.height = h.height;
    image.dxgi = (int)h.dxgi;
    image.rgba.resize((size_t)h.width * h.height * 4);
    if (!ReadExactly(file.f, image.rgba.data(), (size_t)bytes)) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

bool WriteRawFrame(const std::string& path, const Image& image, std::string& error) {
    File file(path, "wb");
    RawHeader h = {};
    std::memcpy(h.magic, RAW_MAGIC, 8);
    h.version = 1;
    h.width = image.width;
    h.height = image.height;
    h.dxgi = (uint32_t)image.dxgi;
    h.channels = 4;
    if (!file.f || std::fwrite(&h, sizeof(h), 1, file.f) != 1 ||
        std::fwrite(image.rgba.data(), 2, image.rgba.size(), file.f) != image.rgba.size()) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

bool ReadDepth(const std::string& path, const TextureInfo& info, std::vector<float>& depth, std::string& error) {
    const uint64_t bytes = (uint64_t)info.width * info.height * 4;
    if (info.texel != "float32" || FileSize(path) != bytes) {
        error = path + ": not a float32 depth texture of the size capture.json says";
        return false;
    }
    File file(path, "rb");
    depth.resize((size_t)info.width * info.height);
    if (!file.f || !ReadExactly(file.f, depth.data(), (size_t)bytes)) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

bool ReadMotion(const std::string& path, const TextureInfo& info, std::vector<float>& motion, std::string& error) {
    const uint64_t bytes = (uint64_t)info.width * info.height * 4;
    if (info.texel != "float16x2" || FileSize(path) != bytes) {
        error = path + ": not a float16x2 motion texture of the size capture.json says";
        return false;
    }
    File file(path, "rb");
    std::vector<uint16_t> raw((size_t)info.width * info.height * 2);
    if (!file.f || !ReadExactly(file.f, raw.data(), (size_t)bytes)) {
        error = "cannot read " + path;
        return false;
    }
    motion.resize(raw.size());
    for (size_t i = 0; i < raw.size(); i++)
        motion[i] = HalfToFloat(raw[i]);
    return true;
}

namespace {

// The value below which `fraction` of the finite values lie (sampled: at most ~64k values).
float Percentile(const std::vector<float>& values, size_t stride, size_t offset, float fraction) {
    std::vector<float> sample;
    const size_t step = std::max<size_t>(values.size() / stride / 65536, 1);
    for (size_t i = offset; i < values.size(); i += stride * step)
        if (std::isfinite(values[i]))
            sample.push_back(values[i]);
    if (sample.empty())
        return 0.0f;
    const size_t at = std::min((size_t)(fraction * (float)(sample.size() - 1)), sample.size() - 1);
    std::nth_element(sample.begin(), sample.begin() + (long)at, sample.end());
    return sample[at];
}

uint16_t Code(float v) {
    return FloatToHalf(std::clamp(v, 0.0f, 1.0f));
}

}  // namespace

Image DepthImage(const std::vector<float>& depth, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h) {
    Image image;
    image.width = out_w;
    image.height = out_h;
    image.rgba.resize((size_t)out_w * out_h * 4);
    const float lo = Percentile(depth, 1, 0, 0.01f), hi = Percentile(depth, 1, 0, 0.99f);
    const float span = std::max(hi - lo, 1e-9f);
    for (uint32_t y = 0; y < out_h; y++)
        for (uint32_t x = 0; x < out_w; x++) {
            const float d = depth[(size_t)((uint64_t)y * h / out_h) * w + (size_t)((uint64_t)x * w / out_w)];
            // magma scaled so that its 8x factor spans 0..1 (Magma takes codes: 1023 / 8 at the top)
            const Rgb c = Magma(std::clamp((d - lo) / span, 0.0f, 1.0f) * (1023.0f / 8.0f));
            uint16_t* p = &image.rgba[((size_t)y * out_w + x) * 4];
            p[0] = Code(c.r);
            p[1] = Code(c.g);
            p[2] = Code(c.b);
            p[3] = FloatToHalf(1.0f);
        }
    return image;
}

Image MotionImage(const std::vector<float>& motion, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h) {
    Image image;
    image.width = out_w;
    image.height = out_h;
    image.rgba.resize((size_t)out_w * out_h * 4);
    std::vector<float> length(motion.size() / 2);
    for (size_t i = 0; i < length.size(); i++)
        length[i] = std::hypot(motion[2 * i], motion[2 * i + 1]);
    const float top = std::max(Percentile(length, 1, 0, 0.99f), 1e-9f);
    for (uint32_t y = 0; y < out_h; y++)
        for (uint32_t x = 0; x < out_w; x++) {
            const size_t at = ((size_t)((uint64_t)y * h / out_h) * w + (size_t)((uint64_t)x * w / out_w));
            const float mx = motion[2 * at], my = motion[2 * at + 1], len = length[at];
            const float value = std::clamp(len / top, 0.0f, 1.0f);
            const float hue = (std::atan2(my, mx) / 3.14159265f * 0.5f + 0.5f) * 6.0f;  // 0..6
            const float k = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
            const float rgb[6][3] = {{1, k, 0}, {k, 1, 0}, {0, 1, k}, {0, k, 1}, {k, 0, 1}, {1, 0, k}};
            const float* c = rgb[std::min((int)hue, 5)];
            uint16_t* p = &image.rgba[((size_t)y * out_w + x) * 4];
            p[0] = Code(c[0] * value);
            p[1] = Code(c[1] * value);
            p[2] = Code(c[2] * value);
            p[3] = FloatToHalf(1.0f);
        }
    return image;
}

}  // namespace renderer::clips::captures
