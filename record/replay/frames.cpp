#include "record/replay/frames.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace replay {

namespace {

constexpr char RAW_MAGIC[8] = {'N', 'R', 'R', 'A', 'W', '1', '\0', '\0'};
constexpr char STREAM_MAGIC[8] = {'N', 'R', 'S', 'T', 'R', 'M', '1', '\0'};

struct RawHeader {
    char magic[8];
    uint32_t version, width, height, dxgi, channels, reserved;
};
static_assert(sizeof(RawHeader) == 32);

// The UNORM steps of a layout's channels.
void Steps(Layout layout, uint32_t steps[4]) {
    const bool ten = layout == Layout::RGB10A2;
    steps[0] = steps[1] = steps[2] = ten ? 1023 : 255;
    steps[3] = ten ? 3 : 255;
}

// A texel's 4 channels as UNORM codes (not for FP16).
inline void Codes(const uint8_t* texel, Layout layout, uint32_t c[4]) {
    if (layout == Layout::RGB10A2) {
        uint32_t w;
        std::memcpy(&w, texel, 4);
        c[0] = w & 1023;
        c[1] = (w >> 10) & 1023;
        c[2] = (w >> 20) & 1023;
        c[3] = w >> 30;
    } else {
        for (int i = 0; i < 4; i++)
            c[i] = texel[i];
    }
}

// From the bits: fast-math builds may drop std::isnan.
inline bool IsNan(uint16_t h) { return (h & 0x7C00) == 0x7C00 && (h & 0x3FF); }

double PqToNits(double code) {
    constexpr double M1 = 0.1593017578125, M2 = 78.84375, C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;
    const double p = std::pow(std::clamp(code, 0.0, 1.0), 1 / M2);
    return 10000 * std::pow(std::max(p - C1, 0.0) / (C2 - C3 * p), 1 / M1);
}

}  // namespace

bool ColorFormatOf(uint32_t dxgi, ColorFormat& out) {
    out = ColorFormat();
    out.dxgi = dxgi;
    switch (dxgi) {
    case 23: case 24:  // R10G10B10A2 TYPELESS, UNORM
        out.layout = Layout::RGB10A2;
        return true;
    case 27: case 28: case 29:  // R8G8B8A8 TYPELESS, UNORM, UNORM_SRGB
        out.srgb = dxgi == 29;
        return true;
    case 87: case 88: case 90: case 91:  // B8G8R8A8_UNORM, B8G8R8X8_UNORM, B8G8R8A8 TYPELESS, UNORM_SRGB
        out.bgra = true;
        out.srgb = dxgi == 91;
        return true;
    case 9: case 10:  // R16G16B16A16 TYPELESS, FLOAT
        out.layout = Layout::RGBA16F;
        out.texel = 8;
        return true;
    default:
        return false;
    }
}

uint16_t ToHalf(float value) {
    uint32_t x;
    std::memcpy(&x, &value, 4);
    const uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF;
    uint32_t mantissa = x & 0x7FFFFF;
    if (e == 0xFF)
        return (uint16_t)(sign | 0x7C00 | (mantissa ? 0x200 : 0));
    const int exponent = (int)e - 127 + 15;
    if (exponent >= 31)
        return (uint16_t)(sign | 0x7C00);
    if (exponent <= 0) {  // a half subnormal, or zero
        if (exponent < -10)
            return (uint16_t)sign;
        mantissa |= 0x800000;
        const int shift = 14 - exponent;
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half & 1)))
            half++;
        return (uint16_t)(sign | half);
    }
    uint32_t half = sign | (uint32_t)exponent << 10 | mantissa >> 13;
    const uint32_t rest = mantissa & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1)))
        half++;  // a carry into the exponent is the right rounding (up to infinity)
    return (uint16_t)half;
}

float FromHalf(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (e == 0) {
        const float v = std::ldexp((float)mantissa, -24);
        return sign ? -v : v;
    }
    const uint32_t bits = e == 31 ? sign | 0x7F800000 | mantissa << 13 : sign | (e + 112) << 23 | mantissa << 13;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::string FrameStem(const std::string& kind, uint32_t a, uint32_t b, double t) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s_f%u-f%u_t%.3f", kind.c_str(), a, b, t);
    return text;
}

bool WriteRaw(const std::string& path, const Image& image, std::string& error) {
    const size_t n = (size_t)image.width * image.height;
    if (image.texels.size() != n * image.format.texel) {
        error = path + ": the image's size does not match its texels";
        return false;
    }
    std::vector<uint16_t> values(4 * n);
    if (image.format.layout == Layout::RGBA16F) {
        std::memcpy(values.data(), image.texels.data(), 8 * n);
    } else {
        uint32_t steps[4];
        Steps(image.format.layout, steps);
        std::vector<uint16_t> table[4];  // code / steps as float16, per channel
        for (int c = 0; c < 4; c++)
            for (uint32_t code = 0; code <= steps[c]; code++)
                table[c].push_back(ToHalf((float)code / (float)steps[c]));
        uint32_t codes[4];
        for (size_t p = 0; p < n; p++) {
            Codes(image.texels.data() + p * image.format.texel, image.format.layout, codes);
            for (int c = 0; c < 4; c++)
                values[4 * p + c] = table[c][codes[c]];
        }
    }
    RawHeader h;
    std::memcpy(h.magic, RAW_MAGIC, 8);
    h.version = 1;
    h.width = image.width;
    h.height = image.height;
    h.dxgi = image.format.dxgi;
    h.channels = 4;
    h.reserved = 0;
    const std::string part = path + ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write((const char*)&h, sizeof(h));
        out.write((const char*)values.data(), (std::streamsize)(values.size() * 2));
        if (!out) {
            error = "cannot write " + part;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(part, path, ec);
    if (ec) {
        error = "cannot rename " + part + ": " + ec.message();
        return false;
    }
    return true;
}

bool ReadRaw(const std::string& path, Image& image, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    RawHeader h;
    if (!in.read((char*)&h, sizeof(h)) || std::memcmp(h.magic, RAW_MAGIC, 8) != 0 || h.version != 1 ||
        h.channels != 4) {
        error = path + ": not a raw frame";
        return false;
    }
    if (!ColorFormatOf(h.dxgi, image.format)) {
        error = path + ": DXGI format " + std::to_string(h.dxgi) + " is not one this tool reads";
        return false;
    }
    image.width = h.width;
    image.height = h.height;
    const size_t n = (size_t)h.width * h.height;
    std::vector<uint16_t> values(4 * n);
    if (!in.read((char*)values.data(), (std::streamsize)(values.size() * 2))) {
        error = path + ": shorter than its size";
        return false;
    }
    image.texels.resize(n * image.format.texel);
    if (image.format.layout == Layout::RGBA16F) {
        std::memcpy(image.texels.data(), values.data(), 8 * n);
        return true;
    }
    uint32_t steps[4];
    Steps(image.format.layout, steps);
    std::vector<uint16_t> table[4];  // float16 bits to the nearest code, per channel
    for (int c = 0; c < 4; c++) {
        table[c].resize(65536);
        for (uint32_t bits = 0; bits < 65536; bits++) {
            const float v = IsNan((uint16_t)bits) ? 0.0f : FromHalf((uint16_t)bits);
            table[c][bits] = (uint16_t)std::lround(std::clamp(v, 0.0f, 1.0f) * steps[c]);
        }
    }
    for (size_t p = 0; p < n; p++) {
        const uint16_t* v = values.data() + 4 * p;
        uint8_t* texel = image.texels.data() + p * image.format.texel;
        if (image.format.layout == Layout::RGB10A2) {
            const uint32_t w = table[0][v[0]] | (uint32_t)table[1][v[1]] << 10 | (uint32_t)table[2][v[2]] << 20 |
                               (uint32_t)table[3][v[3]] << 30;
            std::memcpy(texel, &w, 4);
        } else {
            for (int c = 0; c < 4; c++)
                texel[c] = (uint8_t)table[c][v[c]];
        }
    }
    return true;
}

std::vector<uint8_t> StreamHeaderBytes(const nlohmann::json& header) {
    const std::string text = header.dump();
    std::vector<uint8_t> out(STREAM_MAGIC, STREAM_MAGIC + 8);
    const uint32_t fields[2] = {1, (uint32_t)text.size()};
    out.insert(out.end(), (const uint8_t*)fields, (const uint8_t*)fields + 8);
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

StreamReader::~StreamReader() {
    if (file_ && own_)
        std::fclose(file_);
}

bool StreamReader::Open(const std::string& path, std::string& error) {
    if (path == "-") {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        file_ = stdin;
    } else {
        file_ = std::fopen(path.c_str(), "rb");
        own_ = true;
    }
    if (!file_) {
        error = "cannot open the stream " + path;
        return false;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 4 << 20);
    char magic[8];
    uint32_t fields[2];
    if (std::fread(magic, 1, 8, file_) != 8 || std::memcmp(magic, STREAM_MAGIC, 8) != 0 ||
        std::fread(fields, 4, 2, file_) != 2 || fields[0] != 1) {
        error = path + ": not a frame stream (version 1)";
        return false;
    }
    std::string text(fields[1], '\0');
    if (std::fread(text.data(), 1, text.size(), file_) != text.size()) {
        error = path + ": the stream ends in its header";
        return false;
    }
    header_ = nlohmann::json::parse(text, nullptr, false);
    if (!header_.is_object()) {
        error = path + ": the stream's header is not a JSON object";
        return false;
    }
    return true;
}

bool StreamReader::Next(StreamFrame& frame, Image& image, std::string& error) {
    if (ended_)
        return false;
    if (std::fread(&frame, sizeof(frame), 1, file_) != 1) {
        error = "the stream ended without its end record (the replay stopped)";
        return false;
    }
    if (std::memcmp(frame.magic, "NRSE", 4) == 0) {
        ended_ = true;
        return false;
    }
    if (std::memcmp(frame.magic, "NRSF", 4) != 0 || !ColorFormatOf(frame.dxgi, image.format) ||
        frame.texel != image.format.texel || frame.bytes != (uint64_t)frame.width * frame.height * frame.texel) {
        error = "a stream frame's record is not valid";
        return false;
    }
    image.width = frame.width;
    image.height = frame.height;
    image.texels.resize(frame.bytes);
    if (std::fread(image.texels.data(), 1, frame.bytes, file_) != frame.bytes) {
        error = "the stream ended inside a frame";
        return false;
    }
    return true;
}

SdrCurve::SdrCurve(const ColorFormat& format, bool hdr) : format_(format) {
    pq_ = hdr && format.layout == Layout::RGB10A2;
    if (pq_)
        for (int i = 0; i < 1024; i++)
            pq_nits_.push_back((float)PqToNits(i / 1023.0));
    for (int i = 0; i <= 16384; i++) {
        const double x = i / 16384.0;
        srgb_.push_back((float)(x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055));
    }
    // FP16: HDR scRGB (80 nits per 1.0) through the tone curve, or SDR linear values encoded to sRGB.
    if (format.layout == Layout::RGBA16F)
        pq_nits_.assign(1, hdr ? 80.0f / 203.0f : -1.0f);
}

std::string SdrCurve::Describe() const {
    if (pq_)
        return "PQ to nits, BT.2020 to BT.709, 203 nits as 1.0, Reinhard x / (1 + x) per channel, sRGB";
    if (format_.layout == Layout::RGBA16F)
        return pq_nits_[0] > 0 ? "scRGB at 80 nits per 1.0, 203 nits as 1.0, Reinhard x / (1 + x) per channel, sRGB"
                               : "linear values to sRGB";
    return "sRGB codes as they are";
}

void SdrCurve::Row(const uint8_t* texels, uint32_t width, float* rgb) const {
    // BT.2020 to BT.709 (linear light).
    static constexpr float M[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f,
                                   1.1187f};
    auto tone = [&](float x) {  // x: relative to 203 nits
        x = std::max(x, 0.0f);
        x = std::min(x / (1.0f + x), 1.0f);
        return srgb_[(size_t)std::lround(x * 16384.0f)];
    };
    if (format_.layout == Layout::RGBA16F) {
        const uint16_t* v = (const uint16_t*)texels;
        const float scale = pq_nits_[0];
        for (uint32_t p = 0; p < width; p++)
            for (int c = 0; c < 3; c++) {
                const uint16_t h = v[4 * p + c];
                const float x = IsNan(h) ? 65504.0f : std::clamp(FromHalf(h), 0.0f, 65504.0f);
                rgb[3 * p + c] = scale > 0 ? tone(x * scale)
                                           : srgb_[(size_t)std::lround(std::min(x, 1.0f) * 16384.0f)];
            }
        return;
    }
    uint32_t codes[4];
    const float step = format_.layout == Layout::RGB10A2 ? 1.0f / 1023.0f : 1.0f / 255.0f;
    for (uint32_t p = 0; p < width; p++) {
        Codes(texels + (size_t)p * format_.texel, format_.layout, codes);
        if (!pq_) {
            for (int c = 0; c < 3; c++)
                rgb[3 * p + c] = codes[c] * step;
            continue;
        }
        const float n[3] = {pq_nits_[codes[0]], pq_nits_[codes[1]], pq_nits_[codes[2]]};
        for (int c = 0; c < 3; c++)
            rgb[3 * p + c] = tone((M[3 * c] * n[0] + M[3 * c + 1] * n[1] + M[3 * c + 2] * n[2]) / 203.0f);
    }
}

void ToP010(const float* rgb0, const float* rgb1, uint32_t width, uint16_t* y0, uint16_t* y1, uint16_t* uv) {
    auto code = [](float v) { return (uint16_t)(std::lround(std::clamp(v, 0.0f, 1023.0f)) << 6); };
    for (uint32_t x = 0; x + 1 < width; x += 2) {
        float cb = 0, cr = 0;
        const float* rows[2] = {rgb0, rgb1};
        uint16_t* ys[2] = {y0, y1};
        for (int r = 0; r < 2; r++)
            for (uint32_t dx = 0; dx < 2; dx++) {
                const float* p = rows[r] + 3 * (x + dx);
                const float luma = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                ys[r][x + dx] = code(64.0f + 876.0f * luma);
                cb += (p[2] - luma) / 1.8556f;
                cr += (p[0] - luma) / 1.5748f;
            }
        uv[x] = code(512.0f + 896.0f * cb / 4.0f);
        uv[x + 1] = code(512.0f + 896.0f * cr / 4.0f);
    }
}

}  // namespace replay
