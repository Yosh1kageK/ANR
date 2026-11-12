#include "renderer/clips/captures/png_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// stb_image_write.h's deflate (compiled into NRIFramework's detex library, which declares it only inside its implementation).
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace renderer::clips::captures {
namespace {

uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        out.push_back((uint8_t)(v >> s));
}

void Chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t n) {
    Put32(out, (uint32_t)n);
    const size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data, data + n);
    Put32(out, Crc32(out.data() + at, n + 4));
}

}  // namespace

std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height, int channels, int bits, const uint8_t* rows, int row_stride_bytes,
                               bool cicp_pq, bool big_endian_16) {
    const int bytes_per_sample = bits / 8;
    const size_t bpp = (size_t)channels * bytes_per_sample, row = (size_t)width * bpp;
    // Sub filter on every row, 16-bit samples converted to the file's big-endian order first.
    std::vector<uint8_t> filtered((row + 1) * height), line(row);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* source = rows + (size_t)y * (size_t)row_stride_bytes;
        if (bits == 16 && !big_endian_16)
            for (size_t i = 0; i < row; i += 2) {
                line[i] = source[i + 1];
                line[i + 1] = source[i];
            }
        else
            std::memcpy(line.data(), source, row);
        uint8_t* out = &filtered[(size_t)y * (row + 1)];
        out[0] = 1;
        for (size_t i = 0; i < row; i++)
            out[1 + i] = (uint8_t)(line[i] - (i >= bpp ? line[i - bpp] : 0));
    }
    int compressed_size = 0;
    unsigned char* compressed = stbi_zlib_compress(filtered.data(), (int)filtered.size(), &compressed_size, 5);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (!compressed)
        return {};
    uint8_t header[13];
    for (int i = 0; i < 4; i++) {
        header[i] = (uint8_t)(width >> (24 - 8 * i));
        header[4 + i] = (uint8_t)(height >> (24 - 8 * i));
    }
    header[8] = (uint8_t)bits;
    header[9] = channels == 4 ? 6 : 2;
    header[10] = header[11] = header[12] = 0;
    Chunk(out, "IHDR", header, sizeof(header));
    if (cicp_pq) {
        const uint8_t cicp[4] = {9, 16, 0, 1};  // BT.2020 primaries, PQ, RGB (matrix 0), full range
        Chunk(out, "cICP", cicp, sizeof(cicp));
    }
    Chunk(out, "IDAT", compressed, (size_t)compressed_size);
    Chunk(out, "IEND", nullptr, 0);
    std::free(compressed);
    return out;
}

static bool WriteAtomic(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error) {
    if (bytes.empty()) {
        error = "cannot compress the image";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path target(path);
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), ec);
    const std::string temporary = path + ".part";
    FILE* f = std::fopen(temporary.c_str(), "wb");
    if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        if (f)
            std::fclose(f);
        error = "cannot write " + temporary;
        return false;
    }
    std::fclose(f);
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        error = "cannot rename " + temporary + ": " + ec.message();
        return false;
    }
    return true;
}

bool WritePng8(const std::string& path, uint32_t width, uint32_t height, int channels, const uint8_t* pixels, int row_stride_bytes,
               std::string& error) {
    return WriteAtomic(path, EncodePng(width, height, channels, 8, pixels, row_stride_bytes, false), error);
}

bool WritePng16(const std::string& path, uint32_t width, uint32_t height, const uint16_t* rgb, bool cicp_pq, std::string& error) {
    return WriteAtomic(path, EncodePng(width, height, 3, 16, (const uint8_t*)rgb, (int)(width * 6), cicp_pq), error);
}

}  // namespace renderer::clips::captures
