// Display-encoding helpers shared by the renderer's shaders (renderer.slang) and the clip viewer's
// (clips/clips.slang): sRGB and PQ coding, Rec. 709 and Rec. 2020 light, and the conversions between an encoding's code
// values and display light in nits. Included, not imported: the renderer's registers and entry points stay in
// their own file.

static const uint ENCODING_SRGB8 = 0, ENCODING_PQ10 = 1, ENCODING_SCRGB16 = 2;
static const float SCRGB_NITS = 80.0;

float3 encode_srgb(float3 c)
{
    return select(c <= 0.0031308, 12.92 * c, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float3 decode_srgb(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float3 rec709_to_rec2020(float3 c)
{
    return float3(dot(float3(0.627403895934699, 0.329283038377884, 0.043313065687417), c),
                  dot(float3(0.069097289358232, 0.919540395075459, 0.011362315566309), c),
                  dot(float3(0.016391438875150, 0.088013307877226, 0.895595253247624), c));
}

float3 rec2020_to_rec709(float3 c)
{
    return float3(dot(float3(1.660491002108435, -0.587641138788550, -0.072849863319885), c),
                  dot(float3(-0.124550474521591, 1.132899897125960, -0.008349422604369), c),
                  dot(float3(-0.018150763354905, -0.100578898008007, 1.118729661362913), c));
}

float3 pq_encode(float3 nits)
{
    float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

float3 pq_decode(float3 code)
{
    float3 p = pow(saturate(code), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

// Display light in Rec. 709 nits to an encoding's code values (scRGB: its values), and back. SDR: sRGB with its
// white at the paper white (viewing conversions only; the SDR tone curve is display_fs's).
float3 encode_nits(float3 nits, uint encoding, float paper_white)
{
    if (encoding == ENCODING_SRGB8)
        return encode_srgb(saturate(nits / paper_white));
    if (encoding == ENCODING_PQ10)
        return pq_encode(max(rec709_to_rec2020(nits), 0.0));
    return nits / SCRGB_NITS;
}

float3 decode_nits(float3 code, uint encoding, float paper_white)
{
    if (encoding == ENCODING_SRGB8)
        return decode_srgb(saturate(code)) * paper_white;
    if (encoding == ENCODING_PQ10)
        return rec2020_to_rec709(pq_decode(code));
    return code * SCRGB_NITS;
}
