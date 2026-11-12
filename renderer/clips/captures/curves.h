// The SDR curve of the comparison images on the CPU (thumbnails and tests; the shader has the same one, stills.slang):
// PQ to nits, BT.2020 to BT.709 with a rounded matrix, 203 nits as 1.0, Reinhard x / (1 + x) per channel, sRGB. The
// constants are the comparison tools' (not display_common.slang's exact matrices) so an exported image equals theirs.

#pragma once

#include <algorithm>
#include <cmath>

namespace renderer::clips::captures {

inline constexpr float SDR_WHITE = 203.0f;  // nits
inline constexpr float SCRGB_NITS = 80.0f;

struct Rgb {
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

inline float PqToNits(float v) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

inline float SrgbEncode(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

inline float SrgbDecode(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// The curve of PQ codes (BT.2020 PQ): sRGB-encoded display values in 0..1.
inline Rgb SdrFromPq(float pr, float pg, float pb) {
    const float r = PqToNits(pr), g = PqToNits(pg), b = PqToNits(pb);
    const float x[3] = {std::max(1.6605f * r - 0.5876f * g - 0.0728f * b, 0.0f) / SDR_WHITE,
                        std::max(-0.1246f * r + 1.1329f * g - 0.0083f * b, 0.0f) / SDR_WHITE,
                        std::max(-0.0182f * r - 0.1006f * g + 1.1187f * b, 0.0f) / SDR_WHITE};
    return {SrgbEncode(x[0] / (1.0f + x[0])), SrgbEncode(x[1] / (1.0f + x[1])), SrgbEncode(x[2] / (1.0f + x[2]))};
}

// The same for scRGB values (BT.709, 1.0 = 80 nits): Reinhard on the nits over the SDR white.
inline Rgb SdrFromScrgb(float r, float g, float b) {
    const float s = SCRGB_NITS / SDR_WHITE;
    const float x[3] = {std::max(r, 0.0f) * s, std::max(g, 0.0f) * s, std::max(b, 0.0f) * s};
    return {SrgbEncode(x[0] / (1.0f + x[0])), SrgbEncode(x[1] / (1.0f + x[1])), SrgbEncode(x[2] / (1.0f + x[2]))};
}

// The difference heat map: an error in codes (times 8, 1023 at the top) as magma, sRGB values 0..1.
inline Rgb Magma(float error_codes) {
    static const float stops[9][3] = {{0, 0, 4},     {28, 16, 68},   {79, 18, 123},  {129, 37, 129}, {181, 54, 122},
                                      {229, 80, 100}, {251, 135, 97}, {254, 194, 135}, {252, 253, 191}};
    const float x = std::clamp(error_codes * 8.0f / 1023.0f, 0.0f, 1.0f) * 8.0f;
    const int i = std::min((int)x, 7);
    const float f = x - (float)i;
    auto channel = [&](int c) { return (stops[i][c] * (1.0f - f) + stops[i + 1][c] * f) / 255.0f; };
    return {channel(0), channel(1), channel(2)};
}

}  // namespace renderer::clips::captures
