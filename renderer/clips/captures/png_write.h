// PNG files: 8-bit RGB or RGBA (SDR images), and 16-bit RGB with a cICP chunk that tags BT.2020 primaries, the PQ transfer
// function and full range (HDR images: the lossless 16-bit PQ codes). Deflate is
// stb_image_write's (the copy NRIFramework carries), the chunks and the Sub filter are written here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace renderer::clips::captures {

// pixels: rows of width * channels bytes (channels 3 or 4). Written atomically (a temporary file, then a rename).
bool WritePng8(const std::string& path, uint32_t width, uint32_t height, int channels, const uint8_t* pixels,
               int row_stride_bytes, std::string& error);
// pixels: width * height * 3 little-endian 16-bit values (the file holds them big-endian), `cicp_pq` adds the cICP chunk.
bool WritePng16(const std::string& path, uint32_t width, uint32_t height, const uint16_t* rgb, bool cicp_pq,
                std::string& error);

// The file's bytes without writing them (tests).
std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height, int channels, int bits, const uint8_t* rows,
                               int row_stride_bytes, bool cicp_pq, bool big_endian_16 = false);

}  // namespace renderer::clips::captures
