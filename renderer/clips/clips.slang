// Shaders of the clip viewer (clips/): the video pass, which shows one or two decoded recordings (or raw clips) in up to
// two screen regions of one pass, and the UI layer's composite for an HDR10 swap chain. Registers follow HLSL (b, t, u,
// s per space); slangc shifts them to NRI's Vulkan binding offsets (renderer/clips/BUILD.bazel). The mapping of
// each region comes from view_math.h: screen pixel p shows source position (p + 0.5 - origin) / scale.
//
// Exactness: from a scale of 1 (FILTER_NEAREST) every screen pixel is one source pixel's converted value, picked with
// floor(); a source pixel is Y'CbCr to R'G'B' with the chroma sample of its 2x2 block (nearest, no chroma filtering),
// no dithering, no tone curve: the codes of the recording. Below 1 the footprint of a screen pixel is averaged
// (FILTER_BOX). The code values go to the swap chain through the renderer's display path: sRGB codes on an SDR swap
// chain (or decoded to linear for an sRGB target that encodes itself), the same codes placed at the paper white on an
// HDR one. Raw clips (the study) hold their codes already in the swap chain's encoding where it is HDR10.

#include "../display_common.slang"

static const uint FILTER_NEAREST = 0, FILTER_BOX = 1, FILTER_BILINEAR = 2, FILTER_BICUBIC = 3;

struct RegionData
{
    int4 rect;   // x0, y0, x1, y1 (screen pixels, exclusive end)
    float4 map;  // origin x, origin y, scale
    int4 mode;   // x: version (0 A, 1 B, 2 A left of the split and B from it), y: FILTER_*, z: bit 0 A exists, bit 1 B exists
};

struct VideoData
{
    float4 a;    // kr, kb, y_black, y_scale
    float4 b;    // c_mid, c_scale, sample_scale, unused
    uint4 size;  // luma width, height
};

struct VideoConstants
{
    RegionData region[2];
    VideoData video[2];
    int4 header;    // x: region count, y: split column, z: the target's encoding, w: the codes' encoding
    float4 target;  // x: paper white (nits), y: 1 when the target is an sRGB format that encodes itself, z: the gray
                    // where a recording does not exist (code value)
};
ConstantBuffer<VideoConstants> vc : register(b0, space0);

Texture2D<float> luma_a : register(t0, space1);
Texture2D<float2> chroma_a : register(t1, space1);
Texture2D<float> luma_b : register(t2, space1);
Texture2D<float2> chroma_b : register(t3, space1);

struct FullscreenOut
{
    float4 position : SV_Position;
};

[shader("vertex")]
FullscreenOut fullscreen_vs(uint vertex : SV_VertexID)
{
    FullscreenOut o;
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// R'G'B' (unclamped) of source pixel p of recording `which`, the chroma of its 2x2 block.
float3 pixel_rgb(uint which, int2 p)
{
    VideoData v = vc.video[which];
    float y;
    float2 c;
    if (which == 0)
    {
        y = luma_a.Load(int3(p, 0));
        c = chroma_a.Load(int3(p >> 1, 0));
    }
    else
    {
        y = luma_b.Load(int3(p, 0));
        c = chroma_b.Load(int3(p >> 1, 0));
    }
    float kr = v.a.x, kb = v.a.y;
    float yy = (y * v.b.z - v.a.z) * v.a.w;
    float cb = (c.x * v.b.z - v.b.x) * v.b.y, cr = (c.y * v.b.z - v.b.x) * v.b.y;
    float r = yy + 2.0 * (1.0 - kr) * cr;
    float b = yy + 2.0 * (1.0 - kb) * cb;
    float g = (yy - kr * r - kb * b) / (1.0 - kr - kb);
    return saturate(float3(r, g, b));
}

float3 pixel_clamped(uint which, int2 p)
{
    int2 size = int2(vc.video[which].size.xy);
    return pixel_rgb(which, clamp(p, int2(0, 0), size - 1));
}

float cubic_weight(float x)  // Catmull-Rom (Keys, a = -0.5): PIL's BICUBIC
{
    x = abs(x);
    if (x < 1.0)
        return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0)
        return (((-0.5 * x + 2.5) * x) - 4.0) * x + 2.0;
    return 0.0;
}

float3 sample_video(uint which, float2 src, float2 pixel_origin, float scale, int filter)
{
    // src: the source position of the screen pixel's centre; pixel_origin: of its top-left corner (box)
    int2 size = int2(vc.video[which].size.xy);
    if (filter == FILTER_NEAREST)
        return pixel_rgb(which, clamp(int2(floor(src)), int2(0, 0), size - 1));
    if (filter == FILTER_BOX)
    {
        float footprint = 1.0 / scale;
        int n = clamp(int(ceil(footprint)), 1, 8);
        float3 sum = float3(0.0, 0.0, 0.0);
        for (int iy = 0; iy < n; iy++)
            for (int ix = 0; ix < n; ix++)
            {
                float2 at = pixel_origin + (float2(ix, iy) + 0.5) / n * footprint;
                sum += pixel_clamped(which, int2(floor(at)));
            }
        return sum / float(n * n);
    }
    float2 f = src - 0.5;
    int2 base = int2(floor(f));
    float2 t = f - float2(base);
    if (filter == FILTER_BILINEAR)
    {
        float3 a = pixel_clamped(which, base), b = pixel_clamped(which, base + int2(1, 0));
        float3 c = pixel_clamped(which, base + int2(0, 1)), d = pixel_clamped(which, base + int2(1, 1));
        return lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
    }
    float3 sum = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (int j = -1; j <= 2; j++)
        for (int i = -1; i <= 2; i++)
        {
            float w = cubic_weight(float(i) - t.x) * cubic_weight(float(j) - t.y);
            sum += w * pixel_clamped(which, base + int2(i, j));
            total += w;
        }
    return saturate(sum / total);
}

// Code values of encoding `from` to the target's: the same encoding passes as it is; another goes through display
// light, the SDR codes at the paper white; an sRGB target that encodes itself takes linear values.
float3 to_swap_chain(float3 codes, uint from)
{
    uint to = uint(vc.header.z);
    float paper_white = vc.target.x;
    if (from != to)
        codes = encode_nits(decode_nits(codes, from, paper_white), to, paper_white);
    if (to == ENCODING_SRGB8 && vc.target.y > 0.5)
        codes = decode_srgb(codes);
    return codes;
}

[shader("fragment")]
float4 video_fs(FullscreenOut i) : SV_Target0
{
    float2 p = floor(i.position.xy);
    float3 codes = float3(0.0, 0.0, 0.0);
    for (int r = 0; r < vc.header.x; r++)
    {
        RegionData region = vc.region[r];
        if (p.x < region.rect.x || p.y < region.rect.y || p.x >= region.rect.z || p.y >= region.rect.w)
            continue;
        uint which = region.mode.x == 1 ? 1u : 0u;
        if (region.mode.x == 2 && int(p.x) >= vc.header.y)
            which = 1;
        float scale = region.map.z;
        float2 origin = region.map.xy;
        float2 corner = (p - origin) / scale;
        float2 src = (p + 0.5 - origin) / scale;
        int2 size = int2(vc.video[which].size.xy);
        if ((region.mode.z & (1 << int(which))) == 0)
        {
            if (src.x >= 0.0 && src.y >= 0.0 && src.x < float(size.x) && src.y < float(size.y))
                codes = float3(vc.target.z, vc.target.z, vc.target.z);
        }
        else if (src.x >= 0.0 && src.y >= 0.0 && src.x < float(size.x) && src.y < float(size.y))
        {
            codes = sample_video(which, src, corner, scale, region.mode.y);
        }
        else
        {
            return float4(to_swap_chain(float3(0.0, 0.0, 0.0), uint(vc.header.w)), 1.0);
        }
        break;
    }
    return float4(to_swap_chain(codes, uint(vc.header.w)), 1.0);
}

// Raw clips: the frames' RGB codes (the study's stored 10-bit codes), as textures of 8 or 10 bits per channel.
Texture2D<float4> raw_a : register(t0, space1);
Texture2D<float4> raw_b : register(t1, space1);

[shader("fragment")]
float4 raw_fs(FullscreenOut i) : SV_Target0
{
    float2 p = floor(i.position.xy);
    float3 codes = float3(0.0, 0.0, 0.0);
    for (int r = 0; r < vc.header.x; r++)
    {
        RegionData region = vc.region[r];
        if (p.x < region.rect.x || p.y < region.rect.y || p.x >= region.rect.z || p.y >= region.rect.w)
            continue;
        uint which = region.mode.x == 1 ? 1u : 0u;
        if (region.mode.x == 2 && int(p.x) >= vc.header.y)
            which = 1;
        float2 src = (p + 0.5 - region.map.xy) / region.map.z;
        int2 size = int2(vc.video[which].size.xy);
        if (src.x >= 0.0 && src.y >= 0.0 && src.x < float(size.x) && src.y < float(size.y))
        {
            int3 at = int3(int2(floor(src)), 0);
            codes = which == 0 ? raw_a.Load(at).rgb : raw_b.Load(at).rgb;
        }
        break;
    }
    return float4(to_swap_chain(codes, uint(vc.header.w)), 1.0);
}

// The UI layer (ImGui's premultiplied colors and coverage, linear, 1 at its white) over a PQ swap chain at the paper
// white: blended by coverage in the target's code values. Uncovered pixels keep the frame's values exactly.
struct UiConstants
{
    uint encoding;
    float paper_white;
    float2 pad;
};
[[vk::push_constant]] ConstantBuffer<UiConstants> ui_constants : register(b0, space0);
Texture2D<float4> ui_layer : register(t0, space1);

[shader("fragment")]
float4 ui_layer_fs(FullscreenOut i) : SV_Target0
{
    float4 u = ui_layer.Load(int3(int2(i.position.xy), 0));
    if (u.a <= 0.0)
        discard;
    float3 c = max(u.rgb / u.a, 0.0);
    return float4(encode_nits(c * ui_constants.paper_white, ui_constants.encoding, ui_constants.paper_white), saturate(u.a));
}

// The Captures tab's picture pass (still_fs).
#include "captures/stills.slang"
