// The constants of the video pass (clips.slang VideoConstants), packed from the view regions and the sources' YUV
// parameters. Pure (tested by video_constants_test.cpp).

#pragma once

#include <cstdint>

#include "renderer/clips/view_math.h"
#include "renderer/clips/yuv.h"

namespace renderer::clips {

// What a region shows with: nearest pixel from a scale of 1, the average of the covered source pixels below it; the
// others are for exports that ask for them.
enum class Filter : int { NEAREST = 0, BOX = 1, BILINEAR = 2, BICUBIC = 3, AUTO = -1 };

inline Filter AutoFilter(double scale) { return scale >= 1.0 - 1e-9 ? Filter::NEAREST : Filter::BOX; }

struct alignas(16) RegionData {
    int32_t rect[4];
    float map[4];
    int32_t mode[4];
};

struct alignas(16) VideoData {
    float a[4];
    float b[4];
    uint32_t size[4];
};

struct alignas(16) VideoConstants {
    RegionData region[2];
    VideoData video[2];
    int32_t header[4];
    float target[4];
};

static_assert(sizeof(RegionData) == 48 && sizeof(VideoData) == 48 && sizeof(VideoConstants) == 224);

// One recording as the shader needs it.
struct VideoSourceParams {
    uint32_t width = 0, height = 0;
    YuvParams yuv;
    bool exists = false;  // this frame has a picture of it
};

struct PassTarget {
    uint32_t encoding = 0;       // the swap chain's or target's: Encoding (SRGB8, PQ10, SCRGB16)
    uint32_t code_encoding = 0;  // the codes of the pictures
    float paper_white = 203.0f;
    bool hw_srgb = false;        // the target is an sRGB format that encodes itself
    float gap_gray = 0.0f;       // code value where a recording has no picture at this time
};

inline VideoConstants MakeVideoConstants(const Regions& regions, const VideoSourceParams (&source)[2], Filter filter,
                                         const PassTarget& target) {
    VideoConstants c = {};
    c.header[0] = regions.count;
    c.header[1] = regions.split;
    c.header[2] = (int32_t)target.encoding;
    c.header[3] = (int32_t)target.code_encoding;
    c.target[0] = target.paper_white;
    c.target[1] = target.hw_srgb ? 1.0f : 0.0f;
    c.target[2] = target.gap_gray;
    for (int i = 0; i < regions.count && i < 2; i++) {
        const Region& r = regions.region[i];
        RegionData& d = c.region[i];
        for (int k = 0; k < 4; k++)
            d.rect[k] = r.rect[k];
        d.map[0] = r.origin[0];
        d.map[1] = r.origin[1];
        d.map[2] = r.scale;
        d.mode[0] = r.version;
        d.mode[1] = (int)(filter == Filter::AUTO ? AutoFilter(r.scale) : filter);
        d.mode[2] = (source[0].exists ? 1 : 0) | (source[1].exists ? 2 : 0);
    }
    for (int v = 0; v < 2; v++) {
        const YuvParams& y = source[v].yuv;
        c.video[v].a[0] = y.kr;
        c.video[v].a[1] = y.kb;
        c.video[v].a[2] = y.y_black;
        c.video[v].a[3] = y.y_scale;
        c.video[v].b[0] = y.c_mid;
        c.video[v].b[1] = y.c_scale;
        c.video[v].b[2] = y.sample_scale;
        c.video[v].size[0] = source[v].width;
        c.video[v].size[1] = source[v].height;
    }
    return c;
}

}  // namespace renderer::clips
