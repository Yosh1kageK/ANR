// The planes of a decoded frame and the constants of the YUV to RGB conversion the shader applies. Pure (tested by
// yuv_test.cpp).
//
// A frame is two planes as the video sources hand them over (video_source.h): luma (R8 or R16 UNORM, full size) and
// chroma (RG8 or RG16 UNORM, half size, 4:2:0: U in red, V in green), NV12 or P010-like. Samples above 8 bits sit in
// the high bits of 16 (P010: 10 bits shifted left by 6), so the shader scales them back with sample_scale.

#pragma once

#include <cstdint>

#include "renderer/clips/video_source.h"

namespace renderer::clips {

struct PlaneLayout {
    uint32_t luma_width = 0, luma_height = 0, chroma_width = 0, chroma_height = 0;  // in samples
    uint32_t bytes_per_sample = 1;  // 1 (8 bits) or 2
    uint64_t luma_row_bytes = 0, chroma_row_bytes = 0;
    uint64_t luma_bytes = 0, chroma_bytes = 0;
    uint64_t frame_bytes() const { return luma_bytes + chroma_bytes; }
};

inline PlaneLayout MakePlaneLayout(uint32_t width, uint32_t height, uint32_t bit_depth) {
    PlaneLayout p;
    p.bytes_per_sample = bit_depth > 8 ? 2 : 1;
    p.luma_width = width;
    p.luma_height = height;
    p.chroma_width = (width + 1) / 2;
    p.chroma_height = (height + 1) / 2;
    p.luma_row_bytes = (uint64_t)width * p.bytes_per_sample;
    p.chroma_row_bytes = (uint64_t)p.chroma_width * 2 * p.bytes_per_sample;  // U and V interleaved
    p.luma_bytes = p.luma_row_bytes * height;
    p.chroma_bytes = p.chroma_row_bytes * p.chroma_height;
    return p;
}

// y' = (sample * sample_scale - y_black) * y_scale and c' = (sample * sample_scale - c_mid) * c_scale, with y' in
// [0, 1] and c' in [-0.5, 0.5]; then R = y' + 2 (1 - kr) cr, B = y' + 2 (1 - kb) cb, G = (y' - kr R - kb B) / (1 - kr - kb).
struct YuvParams {
    float kr = 0.2126f, kb = 0.0722f;
    float y_black = 16.0f / 255.0f, y_scale = 255.0f / 219.0f;
    float c_mid = 128.0f / 255.0f, c_scale = 255.0f / 224.0f;
    float sample_scale = 1.0f;
};

inline YuvParams MakeYuvParams(YuvMatrix matrix, YuvRange range, uint32_t bit_depth) {
    YuvParams p;
    switch (matrix) {
        case YuvMatrix::BT709: p.kr = 0.2126f; p.kb = 0.0722f; break;
        case YuvMatrix::BT601: p.kr = 0.299f; p.kb = 0.114f; break;
        case YuvMatrix::BT2020: p.kr = 0.2627f; p.kb = 0.0593f; break;
    }
    const uint32_t bits = bit_depth < 8 ? 8 : bit_depth > 16 ? 16 : bit_depth;
    const double top = (double)((1u << bits) - 1u);
    const double shift = (double)(1u << (bits - 8));
    p.sample_scale = bits > 8 ? (float)(65535.0 / (top * (double)(1u << (16 - bits)))) : 1.0f;
    if (range == YuvRange::LIMITED) {
        p.y_black = (float)(16.0 * shift / top);
        p.y_scale = (float)(top / (219.0 * shift));
        p.c_mid = (float)(128.0 * shift / top);
        p.c_scale = (float)(top / (224.0 * shift));
    } else {
        p.y_black = 0.0f;
        p.y_scale = 1.0f;
        p.c_mid = (float)(128.0 * shift / top);
        p.c_scale = 1.0f;
    }
    return p;
}

inline YuvParams MakeYuvParams(const VideoInfo& info) {
    return MakeYuvParams(info.matrix, info.range, info.bit_depth);
}

// The same conversion on the CPU, for tests and for readbacks: y, u, v as the planes hold them (0 to 1) to R, G, B.
inline void YuvToRgb(const YuvParams& p, float y, float u, float v, float rgb[3]) {
    const float yy = (y * p.sample_scale - p.y_black) * p.y_scale;
    const float cb = (u * p.sample_scale - p.c_mid) * p.c_scale, cr = (v * p.sample_scale - p.c_mid) * p.c_scale;
    const float r = yy + 2.0f * (1.0f - p.kr) * cr, b = yy + 2.0f * (1.0f - p.kb) * cb;
    const float g = (yy - p.kr * r - p.kb * b) / (1.0f - p.kr - p.kb);
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
}

}  // namespace renderer::clips
