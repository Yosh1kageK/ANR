// The Captures tab's picture pass (included by clips.slang): one or two still images (RGBA16F textures holding a capture's
// colour values as the game wrote them: UNORM code / steps, floats as they are) in up to two screen regions of one pass, or
// their difference. A source is PQ10 (BT.2020 PQ codes), sRGB8 (sRGB codes, of 8 or 10 bits), scRGB16 (linear BT.709, 1.0 =
// 80 nits) or VIZ (false colour as sRGB codes). The mapping of a region is view_math.h's: screen pixel p shows source position
// (p + 0.5 - origin) / scale; from a scale of 1 every pixel is one source pixel (floor, no filtering), below it the pixels
// a screen pixel covers are averaged after the display transform.
//
// Display transform (to the target's encoding):
//   SDR target (sRGB codes): sRGB and VIZ sources as they are; PQ and scRGB sources through the comparison images' curve
//   (curves.h): PQ to nits, BT.2020 to BT.709 (a rounded matrix), 203 nits = 1.0, Reinhard x / (1 + x) per channel, sRGB.
//   PQ10 target: PQ sources unchanged (the codes), sRGB and VIZ sources at the paper white, scRGB converted.
//   scRGB target: everything converted to linear BT.709 over 80 nits.
// Difference: the largest channel's |A - B| in code units (steps of the source), times 8, in magma (curves.h Magma), a
// display-referred colour placed like an sRGB source.

static const uint CODING_SRGB8 = 0, CODING_PQ10 = 1, CODING_SCRGB16 = 2, CODING_VIZ = 3;

struct StillRegionData
{
    int4 rect;   // x0, y0, x1, y1 (screen pixels, exclusive end)
    float4 map;  // origin x, origin y, scale
    int4 mode;   // x: version (0 A, 1 B, 2 A left of the split and B from it), y: filter (0 nearest, 1 box), z: bit 0 A exists, bit 1 B exists
};

struct StillConstants
{
    StillRegionData region[2];
    int4 header;    // x: region count, y: split column, z: target encoding (ENCODING_*), w: view (0 pictures, 1 difference)
    int4 coding;    // x: A's CODING_*, y: B's
    uint4 size;     // A width, height, B width, height
    float4 target;  // x: paper white (nits), y: 1 when the target is an sRGB format that encodes itself, z: the gray where a source is missing (sRGB value)
    float4 steps;   // x: A's UNORM steps (0: float values), y: B's, z: the difference's code unit for float sources
};
ConstantBuffer<StillConstants> sc : register(b0, space0);

Texture2D<float4> pic_a : register(t0, space1);
Texture2D<float4> pic_b : register(t1, space1);

static const float SDR_WHITE_NITS = 203.0;

float3 sdr_from_pq(float3 pq)
{
    float3 nits = pq_decode(pq);
    float3 x = max(float3(dot(float3(1.6605, -0.5876, -0.0728), nits), dot(float3(-0.1246, 1.1329, -0.0083), nits),
                          dot(float3(-0.0182, -0.1006, 1.1187), nits)),
                   0.0) / SDR_WHITE_NITS;
    return encode_srgb(saturate(x / (1.0 + x)));
}

float3 sdr_from_scrgb(float3 v)
{
    float3 x = max(v, 0.0) * (SCRGB_NITS / SDR_WHITE_NITS);
    return encode_srgb(saturate(x / (1.0 + x)));
}

// A display-referred sRGB colour (SDR content, false colour, heat) on the target.
float3 srgb_on_target(float3 srgb)
{
    uint to = uint(sc.header.z);
    if (to == ENCODING_SRGB8)
        return sc.target.y > 0.5 ? decode_srgb(srgb) : srgb;
    float3 nits = decode_srgb(srgb) * sc.target.x;
    return to == ENCODING_PQ10 ? pq_encode(rec709_to_rec2020(nits)) : nits / SCRGB_NITS;
}

// A source's values on the target.
float3 shown(uint coding, float3 v)
{
    uint to = uint(sc.header.z);
    if (coding == CODING_SRGB8 || coding == CODING_VIZ)
        return srgb_on_target(saturate(v));
    if (to == ENCODING_SRGB8)
    {
        float3 srgb = coding == CODING_PQ10 ? sdr_from_pq(v) : sdr_from_scrgb(v);
        return sc.target.y > 0.5 ? decode_srgb(srgb) : srgb;
    }
    if (to == ENCODING_PQ10)
        return coding == CODING_PQ10 ? saturate(v) : pq_encode(rec709_to_rec2020(max(v, 0.0) * SCRGB_NITS));
    return coding == CODING_PQ10 ? rec2020_to_rec709(pq_decode(v)) / SCRGB_NITS : v;
}

float3 magma(float err)  // err in codes; curves.h Magma
{
    static const float3 stops[9] = {float3(0, 0, 4), float3(28, 16, 68), float3(79, 18, 123), float3(129, 37, 129), float3(181, 54, 122),
                                    float3(229, 80, 100), float3(251, 135, 97), float3(254, 194, 135), float3(252, 253, 191)};
    float x = saturate(err * 8.0 / 1023.0) * 8.0;
    int i = min(int(x), 7);
    float f = x - float(i);
    return (stops[i] * (1.0 - f) + stops[i + 1] * f) / 255.0;
}

float3 pic_value(uint which, int2 p)
{
    int2 size = int2(which == 0 ? sc.size.xy : sc.size.zw);
    p = clamp(p, int2(0, 0), size - 1);
    return (which == 0 ? pic_a.Load(int3(p, 0)) : pic_b.Load(int3(p, 0))).rgb;
}

// The source pixel of `which` under position src in A's pixels (B is scaled when its size differs).
float3 still_pixel(uint which, float2 src)
{
    if (which == 1 && (sc.size.z != sc.size.x || sc.size.w != sc.size.y))
        src = src * float2(sc.size.zw) / float2(sc.size.xy);
    return pic_value(which, int2(floor(src)));
}

float3 display_of(uint which, float2 src)
{
    if (sc.header.w == 1)
    {
        float3 a = pic_value(0, int2(floor(src)));
        float3 b = still_pixel(1, src);
        float sa = sc.steps.x, sb = sc.steps.y;
        float3 da = sa > 0.0 ? round(a * sa) : a * sc.steps.z;
        float3 db = sb > 0.0 ? round(b * sb) : b * sc.steps.z;
        float3 e = abs(da - db);
        return srgb_on_target(magma(max(e.x, max(e.y, e.z))));
    }
    return shown(uint(which == 0 ? sc.coding.x : sc.coding.y), still_pixel(which, src));
}

[shader("fragment")]
float4 still_fs(FullscreenOut i) : SV_Target0
{
    float2 p = floor(i.position.xy);
    float3 out_value = float3(0.0, 0.0, 0.0);
    for (int r = 0; r < sc.header.x; r++)
    {
        StillRegionData region = sc.region[r];
        if (p.x < region.rect.x || p.y < region.rect.y || p.x >= region.rect.z || p.y >= region.rect.w)
            continue;
        uint which = region.mode.x == 1 ? 1u : 0u;
        if (region.mode.x == 2 && int(p.x) >= sc.header.y)
            which = 1;
        float scale = region.map.z;
        float2 origin = region.map.xy;
        float2 src = (p + 0.5 - origin) / scale;
        int2 size = int2(sc.size.xy);
        bool inside = src.x >= 0.0 && src.y >= 0.0 && src.x < float(size.x) && src.y < float(size.y);
        if (!inside)
            return float4(0.0, 0.0, 0.0, 1.0);
        if ((region.mode.z & (1 << int(which))) == 0 && sc.header.w == 0)
            return float4(srgb_on_target(float3(sc.target.z, sc.target.z, sc.target.z)), 1.0);
        if (region.mode.y == 1 && scale < 1.0 - 1e-6)
        {
            float footprint = 1.0 / scale;
            float2 corner = (p - origin) / scale;
            int n = clamp(int(ceil(footprint)), 1, 8);
            float3 sum = float3(0.0, 0.0, 0.0);
            for (int iy = 0; iy < n; iy++)
                for (int ix = 0; ix < n; ix++)
                    sum += display_of(which, corner + (float2(ix, iy) + 0.5) / float(n) * footprint);
            out_value = sum / float(n * n);
        }
        else
        {
            out_value = display_of(which, src);
        }
        break;
    }
    return float4(out_value, 1.0);
}
