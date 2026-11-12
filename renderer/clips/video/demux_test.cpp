// The demuxer and the codec parsers on the clips in renderer/clips/testdata: the sample table (counts, sizes, sync
// samples, display order), the AV1 frame headers against ffmpeg's trace_headers (testdata/av1_expected.py), and the
// H.264 and H.265 parameter sets and picture order counts against ffprobe's presentation times. No GPU.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>

#include "renderer/clips/video/av1.h"
#include "renderer/clips/video/codec_backend.h"
#include "renderer/clips/video/mp4.h"

using namespace renderer::clips::video;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                                 \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                       \
    do {                                                                                                     \
        const long long va_ = (long long)(a), vb_ = (long long)(b);                                          \
        if (va_ != vb_) {                                                                                    \
            std::fprintf(stderr, "%s:%d: %s = %lld, expected %s = %lld\n", __FILE__, __LINE__, #a, va_, #b, vb_); \
            g_failures++;                                                                                    \
        }                                                                                                    \
    } while (0)

// ffprobe -show_entries packet=pts,size,flags, in decode order.
struct SampleExpect {
    int64_t pts;
    uint32_t size;
};

struct ClipExpect {
    const char* name;
    Codec codec;
    uint32_t width, height;
    std::vector<SampleExpect> samples;  // decode order
    std::vector<uint32_t> sync;         // decode order indices
};

const ClipExpect kClips[] = {
    {"av1_svt_128.mp4", Codec::AV1, 128, 128,
     {{0, 1109}, {512, 1081}, {1024, 3}, {1536, 108}, {2048, 3}, {2560, 304}, {3072, 3}, {3584, 106}, {4096, 3}, {4608, 103},
      {5120, 1163}, {5632, 1101}, {6144, 3}, {6656, 145}, {7168, 3}, {7680, 291}, {8192, 3}, {8704, 120}, {9216, 3}, {9728, 143}},
     {0, 10}},
    {"av1_aom_128.mp4", Codec::AV1, 128, 128,
     {{0, 790}, {512, 203}, {1024, 231}, {1536, 153}, {2048, 176}, {2560, 215}, {3072, 116}, {3584, 114}, {4096, 192}, {4608, 108},
      {5120, 883}, {5632, 247}, {6144, 184}, {6656, 173}, {7168, 210}, {7680, 158}, {8192, 103}, {8704, 201}, {9216, 150}, {9728, 157}},
     {0, 10}},
    {"av1_svt_128_10bit.mp4", Codec::AV1, 128, 128,
     {{0, 1068}, {512, 1051}, {1024, 3}, {1536, 100}, {2048, 3}, {2560, 308}, {3072, 3}, {3584, 109}, {4096, 3}, {4608, 107},
      {5120, 1220}, {5632, 1071}, {6144, 3}, {6656, 158}, {7168, 3}, {7680, 291}, {8192, 3}, {8704, 127}, {9216, 3}, {9728, 161}},
     {0, 10}},
    {"h264_bf_160.mp4", Codec::H264, 160, 160,
     {{0, 1793}, {1536, 400}, {512, 203}, {1024, 136}, {3584, 437}, {2560, 142}, {2048, 176}, {3072, 201}, {4608, 288}, {4096, 152},
      {5120, 1522}, {7168, 413}, {6144, 142}, {5632, 181}, {6656, 247}, {9216, 358}, {8192, 208}, {7680, 255}, {8704, 268}, {9728, 233}},
     {0, 10}},
    {"h265_144.mp4", Codec::H265, 144, 144,
     {{0, 978}, {1536, 516}, {1024, 43}, {512, 46}, {3072, 327}, {2560, 42}, {2048, 76}, {4608, 122}, {4096, 42}, {3584, 35},
      {5120, 1305}, {7168, 206}, {6144, 109}, {5632, 31}, {6656, 27}, {8192, 218}, {7680, 37}, {9728, 106}, {9216, 60}, {8704, 31}},
     {0, 10}},
};

// ffmpeg -bsf:v trace_headers (testdata/av1_expected.py): frame_type, show_frame, order_hint, refresh_frame_flags,
// primary_ref_frame, base_q_idx, ref_frame_idx; frame_type 255 is a show_existing_frame of slot "show_frame".
struct Av1Row {
    int type, show, order_hint, refresh, primary, base_q;
    int refs[7];
};

const std::vector<Av1Row> kAv1Svt = {
    {0, 1, 0, 255, 7, 128, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 8, 1, 0, 166, {2, 2, 2, 2, 2, 2, 2}},
    {1, 0, 4, 8, 0, 185, {2, 2, 2, 2, 0, 0, 0}},
    {1, 0, 2, 32, 4, 193, {2, 2, 2, 2, 3, 3, 3}},
    {1, 1, 1, 0, 4, 200, {2, 2, 2, 2, 5, 5, 5}},
    {255, 5, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 3, 0, 0, 200, {5, 5, 5, 5, 3, 3, 3}},
    {255, 3, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 6, 32, 0, 193, {3, 3, 3, 3, 0, 0, 0}},
    {1, 1, 5, 0, 4, 200, {3, 3, 3, 3, 5, 5, 5}},
    {255, 5, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 7, 0, 0, 200, {5, 5, 5, 5, 0, 0, 0}},
    {255, 0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 9, 8, 0, 200, {0, 0, 0, 0, 0, 0, 0}},
    {0, 1, 10, 255, 7, 125, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 18, 1, 0, 166, {2, 2, 2, 2, 2, 2, 2}},
    {1, 0, 14, 8, 0, 185, {2, 2, 2, 2, 0, 0, 0}},
    {1, 0, 12, 32, 4, 193, {2, 2, 2, 2, 3, 3, 3}},
    {1, 1, 11, 0, 4, 200, {2, 2, 2, 2, 5, 5, 5}},
    {255, 5, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 13, 0, 0, 200, {5, 5, 5, 5, 3, 3, 3}},
    {255, 3, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 16, 32, 0, 193, {3, 3, 3, 3, 0, 0, 0}},
    {1, 1, 15, 0, 4, 200, {3, 3, 3, 3, 5, 5, 5}},
    {255, 5, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 17, 0, 0, 200, {5, 5, 5, 5, 0, 0, 0}},
    {255, 0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 19, 8, 0, 200, {0, 0, 0, 0, 0, 0, 0}},
};
const std::vector<Av1Row> kAv1Aom = {
    {0, 1, 0, 255, 7, 156, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 1, 2, 7, 188, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 2, 4, 0, 185, {1, 0, 0, 0, 0, 0, 0}},
    {1, 1, 3, 8, 0, 190, {2, 1, 0, 0, 0, 0, 0}},
    {1, 1, 4, 16, 7, 193, {3, 2, 1, 0, 0, 0, 0}},
    {1, 1, 5, 32, 0, 185, {3, 2, 1, 4, 0, 0, 0}},
    {1, 1, 6, 64, 0, 198, {5, 3, 2, 4, 0, 1, 0}},
    {1, 1, 7, 128, 0, 200, {6, 5, 3, 4, 0, 2, 1}},
    {1, 1, 8, 2, 4, 187, {7, 6, 5, 4, 0, 3, 2}},
    {1, 1, 9, 4, 5, 200, {7, 6, 5, 1, 0, 4, 3}},
    {0, 1, 0, 255, 7, 156, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 1, 2, 7, 180, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 2, 4, 0, 190, {1, 0, 0, 0, 0, 0, 0}},
    {1, 1, 3, 8, 0, 196, {2, 1, 0, 0, 0, 0, 0}},
    {1, 1, 4, 16, 7, 194, {3, 2, 1, 0, 0, 0, 0}},
    {1, 1, 5, 32, 0, 193, {3, 2, 1, 4, 0, 0, 0}},
    {1, 1, 6, 64, 0, 199, {5, 3, 2, 4, 0, 1, 0}},
    {1, 1, 7, 128, 0, 200, {6, 5, 3, 4, 0, 2, 1}},
    {1, 1, 8, 2, 4, 193, {7, 6, 5, 4, 0, 3, 2}},
    {1, 1, 9, 4, 5, 200, {7, 6, 5, 1, 0, 4, 3}},
};
const std::vector<Av1Row> kAv1Svt10 = {
    {0, 1, 0, 255, 7, 128, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 8, 1, 0, 166, {2, 2, 2, 2, 2, 2, 2}},
    {1, 0, 4, 8, 0, 185, {2, 2, 2, 2, 0, 0, 0}},
    {1, 0, 2, 32, 4, 193, {2, 2, 2, 2, 3, 3, 3}},
    {1, 1, 1, 0, 4, 200, {2, 2, 2, 2, 5, 5, 5}},
    {255, 5, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 3, 0, 0, 200, {5, 5, 5, 5, 3, 3, 3}},
    {255, 3, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 6, 32, 0, 193, {3, 3, 3, 3, 0, 0, 0}},
    {1, 1, 5, 0, 4, 200, {3, 3, 3, 3, 5, 5, 5}},
    {255, 5, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 7, 0, 0, 200, {5, 5, 5, 5, 0, 0, 0}},
    {255, 0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 9, 8, 0, 200, {0, 0, 0, 0, 0, 0, 0}},
    {0, 1, 10, 255, 7, 126, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 18, 1, 0, 167, {2, 2, 2, 2, 2, 2, 2}},
    {1, 0, 14, 8, 0, 185, {2, 2, 2, 2, 0, 0, 0}},
    {1, 0, 12, 32, 4, 193, {2, 2, 2, 2, 3, 3, 3}},
    {1, 1, 11, 0, 4, 200, {2, 2, 2, 2, 5, 5, 5}},
    {255, 5, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 13, 0, 0, 200, {5, 5, 5, 5, 3, 3, 3}},
    {255, 3, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 16, 32, 0, 193, {3, 3, 3, 3, 0, 0, 0}},
    {1, 1, 15, 0, 4, 200, {3, 3, 3, 3, 5, 5, 5}},
    {255, 5, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 17, 0, 0, 200, {5, 5, 5, 5, 0, 0, 0}},
    {255, 0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 19, 8, 0, 200, {0, 0, 0, 0, 0, 0, 0}},
};

const std::vector<Av1Row> kAv1AomSeg = {
    {0, 1, 0, 255, 7, 148, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 11, 2, 7, 173, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 5, 4, 7, 181, {0, 0, 0, 0, 0, 0, 1}},
    {1, 0, 2, 8, 4, 196, {0, 0, 0, 0, 2, 0, 1}},
    {1, 1, 1, 16, 7, 200, {0, 0, 0, 0, 3, 2, 1}},
    {255, 3, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 3, 32, 1, 196, {3, 4, 0, 0, 2, 0, 1}},
    {1, 1, 4, 64, 0, 200, {5, 3, 4, 0, 2, 0, 1}},
    {255, 2, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 8, 128, 4, 189, {2, 6, 5, 0, 3, 4, 1}},
    {1, 1, 6, 16, 1, 198, {2, 6, 5, 0, 7, 3, 1}},
    {1, 1, 7, 8, 0, 200, {4, 2, 6, 0, 7, 5, 1}},
    {255, 7, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 9, 32, 1, 198, {7, 3, 4, 0, 2, 6, 1}},
    {1, 1, 10, 64, 0, 200, {5, 7, 3, 0, 4, 2, 1}},
    {1, 1, 11, 0, 0, 200, {6, 5, 7, 0, 3, 4, 1}},
    {0, 1, 0, 255, 7, 152, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 11, 2, 7, 173, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 5, 4, 7, 182, {0, 0, 0, 0, 0, 0, 1}},
    {1, 0, 2, 8, 4, 195, {0, 0, 0, 0, 2, 0, 1}},
    {1, 1, 1, 16, 7, 200, {0, 0, 0, 0, 3, 2, 1}},
    {255, 3, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 3, 32, 1, 194, {3, 4, 0, 0, 2, 0, 1}},
    {1, 1, 4, 64, 0, 200, {5, 3, 4, 0, 2, 0, 1}},
    {255, 2, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 8, 128, 4, 191, {2, 6, 5, 0, 3, 4, 1}},
    {1, 1, 6, 16, 1, 195, {2, 6, 5, 0, 7, 3, 1}},
    {1, 1, 7, 8, 0, 200, {4, 2, 6, 0, 7, 5, 1}},
    {255, 7, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 9, 32, 1, 198, {7, 3, 4, 0, 2, 6, 1}},
    {1, 1, 10, 64, 0, 200, {5, 7, 3, 0, 4, 2, 1}},
    {1, 1, 11, 0, 0, 200, {6, 5, 7, 0, 3, 4, 1}},
};
const std::vector<Av1Row> kAv1AomTools = {
    {0, 1, 0, 255, 7, 132, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 11, 2, 7, 156, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 5, 4, 7, 158, {0, 0, 0, 0, 0, 0, 1}},
    {1, 0, 2, 8, 4, 173, {0, 0, 0, 0, 2, 0, 1}},
    {1, 1, 1, 16, 7, 177, {0, 0, 0, 0, 3, 2, 1}},
    {255, 3, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 3, 32, 1, 176, {3, 4, 0, 0, 2, 0, 1}},
    {1, 1, 4, 64, 0, 179, {5, 3, 4, 0, 2, 0, 1}},
    {255, 2, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 8, 128, 4, 165, {2, 6, 5, 0, 3, 4, 1}},
    {1, 1, 6, 16, 1, 177, {2, 6, 5, 0, 7, 3, 1}},
    {1, 1, 7, 8, 0, 179, {4, 2, 6, 0, 7, 5, 1}},
    {255, 7, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 9, 32, 1, 178, {7, 3, 4, 0, 2, 6, 1}},
    {1, 1, 10, 64, 0, 180, {5, 7, 3, 0, 4, 2, 1}},
    {1, 1, 11, 0, 0, 180, {6, 5, 7, 0, 3, 4, 1}},
    {0, 1, 0, 255, 7, 138, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 11, 2, 7, 156, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 5, 4, 7, 161, {0, 0, 0, 0, 0, 0, 1}},
    {1, 0, 2, 8, 4, 171, {0, 0, 0, 0, 2, 0, 1}},
    {1, 1, 1, 16, 7, 175, {0, 0, 0, 0, 3, 2, 1}},
    {255, 3, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 3, 32, 1, 175, {3, 4, 0, 0, 2, 0, 1}},
    {1, 1, 4, 64, 0, 176, {5, 3, 4, 0, 2, 0, 1}},
    {255, 2, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 0, 8, 128, 4, 166, {2, 6, 5, 0, 3, 4, 1}},
    {1, 1, 6, 16, 1, 177, {2, 6, 5, 0, 7, 3, 1}},
    {1, 1, 7, 8, 0, 178, {4, 2, 6, 0, 7, 5, 1}},
    {255, 7, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}},
    {1, 1, 9, 32, 1, 178, {7, 3, 4, 0, 2, 6, 1}},
    {1, 1, 10, 64, 0, 180, {5, 7, 3, 0, 4, 2, 1}},
    {1, 1, 11, 0, 0, 180, {6, 5, 7, 0, 3, 4, 1}},
};

std::string g_dir;

std::string Path(const char* name) { return g_dir + "/" + name; }

void TestMp4(const ClipExpect& clip) {
    std::string why;
    auto mp4 = Mp4::Open(Path(clip.name), &why);
    if (!mp4) {
        std::fprintf(stderr, "%s: %s\n", clip.name, why.c_str());
        g_failures++;
        return;
    }
    const Mp4Track& t = mp4->track();
    CHECK(t.codec == clip.codec);
    CHECK_EQ(t.width, clip.width);
    CHECK_EQ(t.height, clip.height);
    CHECK_EQ(t.samples.size(), clip.samples.size());
    CHECK_EQ(t.display_to_sample.size(), 20);
    CHECK(std::abs(t.fps - 24.0) < 1e-9);
    CHECK_EQ(t.timescale, 12288);
    CHECK_EQ(t.pts_step, 512);
    for (size_t i = 0; i < clip.samples.size() && i < t.samples.size(); i++) {
        CHECK_EQ(t.samples[i].size, clip.samples[i].size);
        // The edit list drops the B-frame delay of H.264 and H.265: the container's pts start at 1024 there.
        const int64_t displayed_pts = t.samples[i].pts - t.first_pts;
        CHECK_EQ(displayed_pts, clip.samples[i].pts);
        CHECK_EQ(t.sample_to_display[i], clip.samples[i].pts / 512);
        CHECK_EQ(t.display_to_sample[size_t(clip.samples[i].pts / 512)], i);
    }
    std::vector<uint32_t> sync;
    for (size_t i = 0; i < t.samples.size(); i++)
        if (t.samples[i].sync)
            sync.push_back(uint32_t(i));
    CHECK(sync == clip.sync);
    // Samples read back as the file holds them.
    std::vector<uint8_t> bytes;
    CHECK(mp4->ReadSample(0, &bytes));
    CHECK_EQ(bytes.size(), clip.samples[0].size);
    CHECK(!mp4->ReadSample(uint32_t(t.samples.size()), &bytes));
    // Colour description: BT.709 is not tagged by these clips (ffprobe: unknown), AV1 carries it in the sequence header.
    CHECK(t.config.size() > 4);
}

void TestAv1(const char* name, const std::vector<Av1Row>& rows, uint32_t bit_depth, uint32_t size = 128) {
    std::string why;
    auto mp4 = Mp4::Open(Path(name), &why);
    CHECK(mp4 != nullptr);
    if (!mp4)
        return;
    const Mp4Track& t = mp4->track();
    av1::Parser parser;
    CHECK(parser.Configure(t.config, &why));
    size_t row = 0;
    std::vector<uint8_t> bytes;
    for (uint32_t s = 0; s < t.samples.size(); s++) {
        mp4->ReadSample(s, &bytes);
        CHECK_EQ(av1::StartsWithKeyFrame(bytes.data(), bytes.size()), t.samples[s].sync);
        if (t.samples[s].sync)
            parser.Reset();
        std::vector<av1::Picture> pictures;
        if (!parser.ParseTemporalUnit(bytes.data(), bytes.size(), &pictures, &why)) {
            std::fprintf(stderr, "%s sample %u: %s\n", name, s, why.c_str());
            g_failures++;
            return;
        }
        int shown = 0;
        for (const av1::Picture& p : pictures) {
            if (p.shown)
                shown++;
            if (row >= rows.size()) {
                CHECK(false);
                continue;
            }
            const Av1Row& e = rows[row++];
            const av1::FrameHeader& h = p.h;
            if (e.type == 255) {
                CHECK(h.show_existing_frame);
                CHECK_EQ(h.frame_to_show_map_idx, e.show);
                continue;
            }
            CHECK(!h.show_existing_frame);
            CHECK_EQ(h.frame_type, e.type);
            CHECK_EQ(h.show_frame, e.show);
            CHECK_EQ(h.order_hint, e.order_hint);
            CHECK_EQ(h.refresh_frame_flags, e.refresh);
            CHECK_EQ(h.primary_ref_frame, e.primary);
            CHECK_EQ(h.base_q_idx, e.base_q);
            if (h.frame_type != av1::kKeyFrame)
                for (int i = 0; i < 7; i++)
                    CHECK_EQ(h.ref_frame_idx[i], e.refs[i]);
            CHECK_EQ(p.tiles.size(), 1);
            for (const av1::TileRange& tile : p.tiles)
                CHECK(size_t(tile.offset) + tile.size <= bytes.size());
            CHECK_EQ(h.frame_width, size);
            CHECK_EQ(h.frame_height, size);
        }
        CHECK_EQ(shown, 1);  // one displayed frame per temporal unit
    }
    CHECK_EQ(row, rows.size());
    CHECK_EQ(parser.sequence().bit_depth, bit_depth);
    CHECK_EQ(parser.sequence().width(), size);
    CHECK(parser.sequence().enable_order_hint);
}

// The AV1 backend: one job per picture, a displayed picture per sample, slots within the DPB, references that exist.
void TestAv1Backend(const char* name) {
    std::string why;
    auto mp4 = Mp4::Open(Path(name), &why);
    auto backend = MakeAv1Backend();
    CHECK(mp4 && backend->Init(*mp4, &why));
    if (!mp4)
        return;
    CHECK_EQ(backend->start_samples().size(), 2);
    CHECK_EQ(backend->format().dpb_slots, 9);
    std::vector<uint8_t> bytes;
    std::vector<PictureJob> jobs;
    for (uint32_t s = 0; s < mp4->track().samples.size(); s++) {
        if (mp4->track().samples[s].sync)
            backend->Reset();
        mp4->ReadSample(s, &bytes);
        CHECK(backend->ParseSample(s, bytes, &jobs, &why));
        int shown = 0;
        for (const PictureJob& j : jobs) {
            shown += j.shown ? 1 : 0;
            CHECK(j.show_slot >= 0 && j.show_slot < 9);
            if (j.decode) {
                CHECK(j.setup_slot >= 0 && j.setup_slot < 9);
                CHECK(!j.bitstream.empty());
                for (int slot : j.bound_slots)
                    CHECK(slot >= 0 && slot < 9 && slot != j.setup_slot);
                const Av1Job* a = std::get_if<Av1Job>(&j.codec);
                CHECK(a != nullptr);
                if (a)
                    CHECK_EQ(a->tile_offsets.size(), a->tile_sizes.size());
            }
        }
        CHECK_EQ(shown, 1);
    }
}

// The H.264 and H.265 backends: POC orders agree with the container's presentation order in each GOP, which checks
// frame_num, POC type 0 and the slice header parse; the parameter sets carry what the stream's headers say.
void TestPocOrder(const Mp4Track& t, const std::vector<int32_t>& poc, const std::vector<uint32_t>& starts) {
    for (size_t g = 0; g < starts.size(); g++) {
        const uint32_t begin = starts[g];
        const uint32_t end = g + 1 < starts.size() ? starts[g + 1] : uint32_t(t.samples.size());
        std::vector<uint32_t> by_poc, by_display;
        for (uint32_t s = begin; s < end; s++)
            by_poc.push_back(s), by_display.push_back(s);
        std::sort(by_poc.begin(), by_poc.end(), [&](uint32_t a, uint32_t b) { return poc[a] < poc[b]; });
        std::sort(by_display.begin(), by_display.end(),
                  [&](uint32_t a, uint32_t b) { return t.sample_to_display[a] < t.sample_to_display[b]; });
        CHECK(by_poc == by_display);
    }
}

void TestH264(const char* name) {
    std::string why;
    auto mp4 = Mp4::Open(Path(name), &why);
    auto backend = MakeH264Backend();
    CHECK(mp4 && backend->Init(*mp4, &why));
    if (!mp4)
        return;
    const Mp4Track& t = mp4->track();
    const StreamFormat& f = backend->format();
    CHECK_EQ(f.coded_width, 160);
    CHECK_EQ(f.coded_height, 160);
    CHECK_EQ(f.width, 160);
    CHECK_EQ(f.dpb_slots, 5);  // max_num_ref_frames 4, plus the picture being decoded
    CHECK(backend->start_samples() == (std::vector<uint32_t>{0, 10}));
    nri::VideoSessionParametersDesc desc;
    backend->FillSessionParameters(&desc);
    CHECK(desc.h264Parameters != nullptr);
    if (desc.h264Parameters) {
        CHECK_EQ(desc.h264Parameters->sequenceParameterSetNum, 1);
        CHECK_EQ(desc.h264Parameters->pictureParameterSetNum, 1);
        const nri::VideoH264SequenceParameterSetDesc& sps = desc.h264Parameters->sequenceParameterSets[0];
        CHECK_EQ(sps.profileIdc, 100);
        CHECK_EQ(sps.levelIdc, 11);
        CHECK_EQ(sps.pictureOrderCountType, 0);
        CHECK_EQ(sps.log2MaxPictureOrderCountLsbMinus4, 2);
        CHECK_EQ(sps.referenceFrameNum, 4);
        CHECK_EQ(sps.pictureWidthInMbsMinus1, 9);
        CHECK_EQ(sps.pictureHeightInMapUnitsMinus1, 9);
        CHECK(!!(sps.flags & nri::VideoH264SequenceParameterSetBits::FRAME_MBS_ONLY));
        CHECK(!!(sps.flags & nri::VideoH264SequenceParameterSetBits::DIRECT_8X8_INFERENCE));
        CHECK(!(sps.flags & nri::VideoH264SequenceParameterSetBits::FRAME_CROPPING));
        const nri::VideoH264PictureParameterSetDesc& pps = desc.h264Parameters->pictureParameterSets[0];
        CHECK_EQ(pps.refIndexL0DefaultActiveMinus1, 2);
        CHECK_EQ(pps.weightedBipredIdc, 2);
        CHECK_EQ(pps.pictureInitQpMinus26, 12);
        CHECK_EQ(pps.chromaQpIndexOffset, -2);
        CHECK_EQ(pps.secondChromaQpIndexOffset, -2);
        using B = nri::VideoH264PictureParameterSetBits;
        CHECK(!!(pps.flags & B::TRANSFORM_8X8_MODE));
        CHECK(!!(pps.flags & B::ENTROPY_CODING_MODE));
        CHECK(!!(pps.flags & B::WEIGHTED_PRED));
        CHECK(!!(pps.flags & B::DEBLOCKING_FILTER_CONTROL_PRESENT));
        CHECK(!(pps.flags & B::CONSTRAINED_INTRA_PRED));
    }
    std::vector<int32_t> poc(t.samples.size());
    std::vector<uint8_t> bytes;
    std::vector<PictureJob> jobs;
    int idr_count = 0;
    for (uint32_t s = 0; s < t.samples.size(); s++) {
        if (s == 0 || s == 10)
            backend->Reset();
        mp4->ReadSample(s, &bytes);
        CHECK(backend->ParseSample(s, bytes, &jobs, &why));
        CHECK_EQ(jobs.size(), 1);
        const H264Job* h = jobs.empty() ? nullptr : std::get_if<H264Job>(&jobs[0].codec);
        CHECK(h != nullptr);
        if (!h)
            continue;
        poc[s] = h->poc[0];
        idr_count += h->idr ? 1 : 0;
        CHECK_EQ(h->idr, t.samples[s].sync);
        CHECK(jobs[0].setup_slot >= 0 && jobs[0].setup_slot < 5);
        CHECK_EQ(h->slice_offsets.size(), 1);
        CHECK_EQ(h->slice_offsets[0], 0);
        CHECK_EQ(jobs[0].bitstream[3], 1);  // a 4-byte start code
        for (const H264Ref& r : h->refs) {
            CHECK(r.slot >= 0 && r.slot < 5 && r.slot != jobs[0].setup_slot);
            CHECK(!r.long_term);
        }
        CHECK(h->refs.size() <= 4);
    }
    CHECK_EQ(idr_count, 2);
    TestPocOrder(t, poc, {0, 10});
}

void TestH265(const char* name) {
    std::string why;
    auto mp4 = Mp4::Open(Path(name), &why);
    auto backend = MakeH265Backend();
    CHECK(mp4 && backend->Init(*mp4, &why));
    if (!mp4)
        return;
    const Mp4Track& t = mp4->track();
    const StreamFormat& f = backend->format();
    CHECK_EQ(f.coded_width, 144);
    CHECK_EQ(f.width, 144);
    CHECK_EQ(f.dpb_slots, 6);  // sps_max_dec_pic_buffering_minus1 4: five pictures, plus one
    CHECK(backend->start_samples() == (std::vector<uint32_t>{0, 10}));
    nri::VideoSessionParametersDesc desc;
    backend->FillSessionParameters(&desc);
    CHECK(desc.h265Parameters != nullptr);
    if (desc.h265Parameters) {
        const nri::VideoH265SessionParametersDesc& p = *desc.h265Parameters;
        CHECK_EQ(p.videoParameterSetNum, 1);
        CHECK_EQ(p.sequenceParameterSetNum, 1);
        CHECK_EQ(p.pictureParameterSetNum, 1);
        CHECK_EQ(p.videoParameterSets[0].profileTierLevel.generalLevelIdc, 30);
        CHECK_EQ(p.videoParameterSets[0].decPicBufMgr.maxDecPicBufferingMinus1[0], 4);
        const nri::VideoH265SequenceParameterSetDesc& sps = p.sequenceParameterSets[0];
        CHECK_EQ(sps.pictureWidthInLumaSamples, 144);
        CHECK_EQ(sps.pictureHeightInLumaSamples, 144);
        CHECK_EQ(sps.log2MaxPictureOrderCountLsbMinus4, 4);
        CHECK_EQ(sps.decPicBufMgr.maxDecPicBufferingMinus1[0], 4);
        CHECK_EQ(sps.decPicBufMgr.maxNumReorderPics[0], 2);
        CHECK_EQ(sps.log2MinLumaCodingBlockSizeMinus3, 0);
        CHECK_EQ(sps.log2DiffMaxMinLumaCodingBlockSize, 3);
        CHECK_EQ(sps.log2MinLumaTransformBlockSizeMinus2, 0);
        CHECK_EQ(sps.log2DiffMaxMinLumaTransformBlockSize, 3);
        CHECK_EQ(sps.numShortTermRefPicSets, 0);
        using B = nri::VideoH265SequenceParameterSetBits;
        CHECK(!!(sps.flags & B::SAMPLE_ADAPTIVE_OFFSET_ENABLED));
        CHECK(!!(sps.flags & B::TEMPORAL_MVP_ENABLED));
        CHECK(!!(sps.flags & B::STRONG_INTRA_SMOOTHING_ENABLED));
        CHECK(!(sps.flags & B::AMP_ENABLED));
        CHECK(!(sps.flags & B::PCM_ENABLED));
        const nri::VideoH265PictureParameterSetDesc& pps = p.pictureParameterSets[0];
        CHECK_EQ(pps.diffCuQpDeltaDepth, 1);
        using P = nri::VideoH265PictureParameterSetBits;
        CHECK(!!(pps.flags & P::SIGN_DATA_HIDING_ENABLED));
        CHECK(!!(pps.flags & P::CU_QP_DELTA_ENABLED));
        CHECK(!!(pps.flags & P::WEIGHTED_PRED));
        CHECK(!!(pps.flags & P::LOOP_FILTER_ACROSS_SLICES_ENABLED));
        CHECK(!(pps.flags & P::TILES_ENABLED));
    }
    std::vector<int32_t> poc(t.samples.size());
    std::vector<uint8_t> bytes;
    std::vector<PictureJob> jobs;
    bool saw_slice_rps = false;
    for (uint32_t s = 0; s < t.samples.size(); s++) {
        if (s == 0 || s == 10)
            backend->Reset();
        mp4->ReadSample(s, &bytes);
        CHECK(backend->ParseSample(s, bytes, &jobs, &why));
        CHECK_EQ(jobs.size(), 1);
        const H265Job* h = jobs.empty() ? nullptr : std::get_if<H265Job>(&jobs[0].codec);
        CHECK(h != nullptr);
        if (!h)
            continue;
        poc[s] = h->poc;
        CHECK_EQ(h->idr, t.samples[s].sync);
        CHECK(jobs[0].setup_slot >= 0 && jobs[0].setup_slot < 6);
        CHECK_EQ(h->slice_offsets.size(), 1);
        for (const H265Ref& r : h->refs) {
            CHECK(r.slot >= 0 && r.slot < 6 && r.slot != jobs[0].setup_slot);
            CHECK(r.poc != h->poc);
        }
        if (!h->idr) {
            CHECK(!h->short_term_ref_pic_set_sps);  // the SPS has no sets: every picture codes its own
            CHECK(h->num_bits_for_st_ref_pic_set_in_slice > 0);
            saw_slice_rps = true;
        }
    }
    CHECK(saw_slice_rps);
    TestPocOrder(t, poc, {0, 10});
}

// What is refused, with a reason that names it, at the point a player can still fall back to another decoder.
void TestRefusals() {
    std::string why;
    {
        auto mp4 = Mp4::Open(Path("av1_svt_128_grain.mp4"), &why);
        CHECK(mp4 != nullptr);
        auto backend = MakeAv1Backend();
        CHECK(mp4 && !backend->Init(*mp4, &why));
        CHECK(why.find("film grain") != std::string::npos);
    }
    {
        auto mp4 = Mp4::Open(Path("h264_interlaced_160.mp4"), &why);
        CHECK(mp4 != nullptr);
        auto backend = MakeH264Backend();
        CHECK(mp4 && !backend->Init(*mp4, &why));
        CHECK(why.find("interlaced") != std::string::npos);
    }
    {
        // A codec the container holds but a backend does not take (an H.265 file given to the H.264 backend is a
        // programming error; the media types decide which backend runs), and a file that is no MP4 at all.
        CHECK(Mp4::Open(Path("generate.sh"), &why) == nullptr);
        CHECK(!why.empty());
        CHECK(Mp4::Open(Path("no_such_file.mp4"), &why) == nullptr);
    }
}

void TestH265TenBit() {
    std::string why;
    auto mp4 = Mp4::Open(Path("h265_144_10bit.mp4"), &why);
    auto backend = MakeH265Backend();
    CHECK(mp4 && backend->Init(*mp4, &why));
    if (!mp4)
        return;
    CHECK_EQ(backend->format().bit_depth, 10);
    CHECK_EQ(mp4->track().samples.size(), 10);
}

// The displayed rectangle inside the decoded picture: H.264 frame cropping, the H.265 conformance window, AV1's odd sizes.
void TestCropping() {
    std::string why;
    struct Case {
        const char* name;
        std::unique_ptr<CodecBackend> backend;
        uint32_t coded_w, coded_h, w, h;
    };
    Case cases[] = {{"h264_crop_160x150.mp4", MakeH264Backend(), 160, 160, 160, 150},
                    {"h265_crop_144x130.mp4", MakeH265Backend(), 144, 136, 144, 130},
                    {"av1_odd_131x137.mp4", MakeAv1Backend(), 131, 137, 131, 137}};
    for (Case& c : cases) {
        auto mp4 = Mp4::Open(Path(c.name), &why);
        CHECK(mp4 && c.backend->Init(*mp4, &why));
        if (!mp4)
            continue;
        const StreamFormat& f = c.backend->format();
        CHECK_EQ(f.coded_width, c.coded_w);
        CHECK_EQ(f.coded_height, c.coded_h);
        CHECK_EQ(f.width, c.w);
        CHECK_EQ(f.height, c.h);
        CHECK_EQ(f.crop_x, 0);
        CHECK_EQ(f.crop_y, 0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: demux_test TESTDATA_DIR\n");
        return 2;
    }
    g_dir = argv[1];
    for (const ClipExpect& clip : kClips)
        TestMp4(clip);
    TestAv1("av1_svt_128.mp4", kAv1Svt, 8);
    TestAv1("av1_aom_128.mp4", kAv1Aom, 8);
    TestAv1("av1_svt_128_10bit.mp4", kAv1Svt10, 10);
    TestAv1("av1_aom_seg_160.mp4", kAv1AomSeg, 8, 160);
    TestAv1("av1_aom_tools_160.mp4", kAv1AomTools, 8, 160);
    TestAv1Backend("av1_svt_128.mp4");
    TestAv1Backend("av1_aom_128.mp4");
    TestH264("h264_bf_160.mp4");
    TestH265("h265_144.mp4");
    TestH265TenBit();
    TestRefusals();
    TestCropping();
    if (g_failures) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    std::printf("demux_test: ok\n");
    return 0;
}
