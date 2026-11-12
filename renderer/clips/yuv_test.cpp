// The plane layout of a frame and the YUV to RGB constants against known code values.
#include "renderer/clips/yuv.h"

#include <cmath>

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

void layouts() {
    PlaneLayout p = MakePlaneLayout(3840, 2160, 8);
    CHECK_EQ(p.chroma_width, 1920u);
    CHECK_EQ(p.chroma_height, 1080u);
    CHECK_EQ(p.luma_row_bytes, 3840u);
    CHECK_EQ(p.chroma_row_bytes, 3840u);  // U and V interleaved: as wide as a luma row
    CHECK_EQ(p.luma_bytes, 3840u * 2160u);
    CHECK_EQ(p.frame_bytes(), 3840u * 2160u * 3u / 2u);  // 12.4 MB
    // odd sizes round the chroma plane up
    p = MakePlaneLayout(1921, 1081, 8);
    CHECK_EQ(p.chroma_width, 961u);
    CHECK_EQ(p.chroma_height, 541u);
    CHECK_EQ(p.chroma_row_bytes, 1922u);
    // deeper than 8 bits: two bytes a sample
    p = MakePlaneLayout(1920, 1080, 10);
    CHECK_EQ(p.bytes_per_sample, 2u);
    CHECK_EQ(p.luma_row_bytes, 3840u);
    CHECK_EQ(p.chroma_row_bytes, 3840u);
    CHECK_EQ(p.frame_bytes(), 1920u * 1080u * 3u);
}

struct Rgb {
    float r, g, b;
};

Rgb convert(const YuvParams& p, int y, int u, int v, int bits = 8) {
    const float top = (float)((1 << bits) - 1);
    const float shift = bits > 8 ? (float)(1 << (16 - bits)) : 1.0f;
    // the planes hold the code shifted into the high bits of 16 for deeper pictures, as a normalized value
    auto sample = [&](int code) { return bits > 8 ? (float)(code * shift) / 65535.0f : (float)code / top; };
    float rgb[3];
    YuvToRgb(p, sample(y), sample(u), sample(v), rgb);
    return {rgb[0], rgb[1], rgb[2]};
}

void bt709_limited() {
    const YuvParams p = MakeYuvParams(YuvMatrix::BT709, YuvRange::LIMITED, 8);
    CHECK_NEAR(p.kr, 0.2126, 1e-6);
    CHECK_NEAR(p.kb, 0.0722, 1e-6);
    // black, white, mid grey
    Rgb c = convert(p, 16, 128, 128);
    CHECK_NEAR(c.r, 0.0, 1e-5);
    CHECK_NEAR(c.g, 0.0, 1e-5);
    CHECK_NEAR(c.b, 0.0, 1e-5);
    c = convert(p, 235, 128, 128);
    CHECK_NEAR(c.r, 1.0, 1e-5);
    CHECK_NEAR(c.g, 1.0, 1e-5);
    CHECK_NEAR(c.b, 1.0, 1e-5);
    c = convert(p, 126, 128, 128);
    CHECK_NEAR(c.r, (126.0 - 16.0) / 219.0, 1e-5);
    // saturated red in BT.709 limited range: Y 63, Cb 102, Cr 240 (the standard values)
    c = convert(p, 63, 102, 240);
    CHECK_NEAR(c.r, 1.0, 0.01);
    CHECK_NEAR(c.g, 0.0, 0.01);
    CHECK_NEAR(c.b, 0.0, 0.01);
    // blue: Y 32, Cb 240, Cr 118
    c = convert(p, 32, 240, 118);
    CHECK_NEAR(c.b, 1.0, 0.01);
    CHECK_NEAR(c.r, 0.0, 0.01);
}

void other_matrices_and_ranges() {
    const YuvParams bt601 = MakeYuvParams(YuvMatrix::BT601, YuvRange::LIMITED, 8);
    CHECK_NEAR(bt601.kr, 0.299, 1e-6);
    // BT.601 red: Y 81, Cb 90, Cr 240
    Rgb c = convert(bt601, 81, 90, 240);
    CHECK_NEAR(c.r, 1.0, 0.01);
    CHECK_NEAR(c.g, 0.0, 0.01);
    // the same codes mean another color in BT.709 (the matrices differ)
    const YuvParams bt709 = MakeYuvParams(YuvMatrix::BT709, YuvRange::LIMITED, 8);
    CHECK(std::fabs(convert(bt709, 81, 90, 240).g - c.g) > 0.02);
    const YuvParams bt2020 = MakeYuvParams(YuvMatrix::BT2020, YuvRange::LIMITED, 8);
    CHECK_NEAR(bt2020.kr, 0.2627, 1e-6);
    // full range: 0 and 255 are black and white
    const YuvParams full = MakeYuvParams(YuvMatrix::BT709, YuvRange::FULL, 8);
    c = convert(full, 0, 128, 128);
    CHECK_NEAR(c.r, 0.0, 1e-5);
    c = convert(full, 255, 128, 128);
    CHECK_NEAR(c.g, 1.0, 1e-5);
    c = convert(full, 128, 128, 128);
    CHECK_NEAR(c.b, 128.0 / 255.0, 1e-5);
}

void deeper_pictures() {
    // 10 bits in the high bits of 16: limited black is 64, white 940, mid chroma 512
    const YuvParams p = MakeYuvParams(YuvMatrix::BT709, YuvRange::LIMITED, 10);
    CHECK_NEAR(p.sample_scale, 65535.0 / 65472.0, 1e-6);
    Rgb c = convert(p, 64, 512, 512, 10);
    CHECK_NEAR(c.r, 0.0, 2e-5);
    c = convert(p, 940, 512, 512, 10);
    CHECK_NEAR(c.g, 1.0, 2e-5);
    // the same picture as 8 bits gives the same colors to within a code value
    const YuvParams q = MakeYuvParams(YuvMatrix::BT709, YuvRange::LIMITED, 8);
    const Rgb a = convert(q, 100, 140, 90);
    const Rgb b = convert(p, 400, 560, 360, 10);
    CHECK_NEAR(a.r, b.r, 0.01);
    CHECK_NEAR(a.g, b.g, 0.01);
    CHECK_NEAR(a.b, b.b, 0.01);
    const YuvParams q12 = MakeYuvParams(YuvMatrix::BT709, YuvRange::LIMITED, 12);
    CHECK_NEAR(q12.sample_scale, 65535.0 / 65520.0, 1e-6);
}

}  // namespace

int main() {
    layouts();
    bt709_limited();
    other_matrices_and_ranges();
    deeper_pictures();
    return check_result("yuv");
}
