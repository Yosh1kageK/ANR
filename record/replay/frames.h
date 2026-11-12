// Frames as nr-replay writes them and nr-video reads them (README.md "Output"): the raw frame files (.nrraw), the frame
// stream, and the SDR curve and 4:2:0 conversion of the videos. Portable: no graphics API.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace replay {

// The color formats a replay's frames can have (DXGI values), and how their texels are stored: RGB10A2 words (red in
// the low bits), 8-bit RGBA bytes (BGRA formats swapped to RGBA), or FP16 RGBA.
enum class Layout { RGB10A2, RGBA8, RGBA16F };
struct ColorFormat {
    uint32_t dxgi = 0;
    Layout layout = Layout::RGBA8;
    uint32_t texel = 4;  // bytes
    bool bgra = false;   // stored as BGRA in the graphics API (swapped to RGBA in files and streams)
    bool srgb = false;   // an sRGB format (its codes are sRGB codes)
};
// False for a format this tool does not write.
bool ColorFormatOf(uint32_t dxgi, ColorFormat& out);

uint16_t ToHalf(float value);
float FromHalf(uint16_t half);

// "<kind>_f<a>-f<b>_t<t>" with t to 3 decimals: a generated frame between real frames a and b, or a real frame (a = b,
// t = 0).
std::string FrameStem(const std::string& kind, uint32_t a, uint32_t b, double t);

// A frame's texels: width x height texels of `format`, rows packed, RGBA order.
struct Image {
    uint32_t width = 0, height = 0;
    ColorFormat format;
    std::vector<uint8_t> texels;
};

// Raw frame files (.nrraw): a 32-byte header ("NRRAW1\0\0", u32 version 1, width, height, DXGI format, 4 channels, 0;
// little-endian), then float16 RGBA: UNORM codes divided by their steps (1023 or 3 for RGB10A2, 255 for 8 bits),
// FP16 values as they are. Writing is atomic (a temporary file, then a rename).
bool WriteRaw(const std::string& path, const Image& image, std::string& error);
bool ReadRaw(const std::string& path, Image& image, std::string& error);

// The frame stream: "NRSTRM1\0", u32 version 1, u32 header bytes, the header (replay.json without "frames"), then per
// frame a StreamFrame and its texels (width * height * texel bytes, RGBA order), and a StreamFrame with magic "NRSE"
// at the end.
struct StreamFrame {
    char magic[4] = {'N', 'R', 'S', 'F'};
    uint32_t kind = 0;          // 0 real, 1 generated, 2 an SR or RR output (of recorded frame a = b)
    uint32_t a = 0, b = 0;      // the real frames it lies between (a real frame: a = b)
    uint32_t index = 0;         // the generated frame's index (1 to multiplier - 1); 0 for a real frame
    float t = 0;                // index / multiplier; 0 for a real frame
    uint32_t width = 0, height = 0, dxgi = 0, texel = 0;
    uint64_t bytes = 0;
};
static_assert(sizeof(StreamFrame) == 48);

// The stream's reading side (a file, FIFO or "-" for standard input).
class StreamReader {
public:
    ~StreamReader();
    bool Open(const std::string& path, std::string& error);
    const nlohmann::json& header() const { return header_; }
    // The next frame; false at the end record (ended() true) or on an error (error set).
    bool Next(StreamFrame& frame, Image& image, std::string& error);
    bool ended() const { return ended_; }

private:
    std::FILE* file_ = nullptr;
    bool own_ = false, ended_ = false;
    nlohmann::json header_;
};

// Frame stream bytes, for writers that have their own file handles.
std::vector<uint8_t> StreamHeaderBytes(const nlohmann::json& header);

// The SDR curve of the videos and images: 10-bit PQ (HDR) codes to nits, BT.2020 to BT.709, 203 nits as 1.0, Reinhard
// x / (1 + x) per channel, sRGB; FP16 scRGB values at 80 nits per 1.0 through the same curve; 8-bit and other 10-bit
// codes (sRGB) as they are.
class SdrCurve {
public:
    SdrCurve(const ColorFormat& format, bool hdr);
    // One row of texels to sRGB values in [0, 1], 3 per texel.
    void Row(const uint8_t* texels, uint32_t width, float* rgb) const;
    std::string Describe() const;

private:
    ColorFormat format_;
    bool pq_ = false;
    std::vector<float> pq_nits_, srgb_;
};

// One row pair of sRGB values to BT.709 limited-range 10-bit 4:2:0 as P010 (each code in a 16-bit word's top bits):
// two rows of Y words, and one row of interleaved U and V words. width is even.
void ToP010(const float* rgb0, const float* rgb1, uint32_t width, uint16_t* y0, uint16_t* y1, uint16_t* uv);

}  // namespace replay
