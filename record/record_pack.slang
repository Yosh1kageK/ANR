// The GPU coding of nr-record (FORMAT.md "Coding 3"), compiled to DXIL for D3D12 (d3d12_packer.cpp) and run on the
// game's command list: per real frame record_init, then record_pack once per texture, then record_done.
// - record_init: word 0 of the slot (the next free word) starts at the slot's data start.
// - record_pack: one texture of the frame into chunks of words; word 0 allocates each chunk's words, and the texture's
//   chunk table (rp.table) gets each chunk's first word and word count.
// - record_done: the used word count and the frame's serial into the status buffer, which the host copies to memory
//   it reads to see that the frame's pack has finished.

struct RecordParams
{
    uint width, height;  // the texture's size
    uint kinds;          // per channel (8 bits each, red low) bits | type << 5 (REC_*)
    uint flags;          // REC_DEPTH, REC_REFERENCE, REC_SRGB
    uint table;          // the slot word of the texture's chunk table
    uint channels;       // 1 to 4
    uint value;          // record_init: the data start; record_done: the frame's serial
    uint slot;           // record_done: the slot's status entry
};
ConstantBuffer<RecordParams> rp : register(b0);

Texture2D<float4> src : register(t0);   // the texture (a depth format's depth plane in x)
Texture2D<float4> src2 : register(t1);  // with REC_REFERENCE: the texture it is coded against
RWStructuredBuffer<uint> record_slot : register(u0);
RWStructuredBuffer<uint> record_status : register(u1);  // per slot: used words, serial

float3 linear_to_srgb(float3 l)
{
    l = saturate(l);
    return select(l <= 0.0031308, l * 12.92, 1.055 * pow(l, 1.0 / 2.4) - 0.055);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void record_init(uint3 id : SV_DispatchThreadID)
{
    record_slot[0] = rp.value;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void record_done(uint3 id : SV_DispatchThreadID)
{
    record_status[2 * rp.slot] = record_slot[0];
    record_status[2 * rp.slot + 1] = rp.value;
}

// The coding (FORMAT.md "Coding 3"): a value's residual is the value minus left + above - above-left within its 8-row
// tile row (or minus the reference texture's value), modulo 2^width as the integer nearest 0, zigzagged. Per 8x8 tile
// and channel (an entry) the residuals take the bit count b with the fewest bits, the few wider residuals' upper bits
// stored apart; a chunk is up to 32 tiles of a tile row, its record laid out as FORMAT.md says.
static const uint REC_UNORM = 0, REC_F16 = 1, REC_F32 = 2;
static const uint REC_DEPTH = 1, REC_REFERENCE = 2, REC_SRGB = 4;
static const uint REC_CHUNK = 32, REC_ENTRIES = REC_CHUNK * 4;
// A chunk's words after its header: at most 64 x 16 bits per entry with 4 channels (32 bits with 2), as an entry takes
// flags only when they (with its byte) make it smaller, and a padding word each for the bytes and the stored bits.
static const uint REC_STAGE = REC_CHUNK * 4 * 16 * 2 + 2;
// rec_mem: first per entry the count of its residuals of each bit count (33 words each); then per entry and row the
// row's flagged residuals (REC_ROWS words), and the staged words after them.
static const uint REC_ROWS = REC_ENTRIES * 8, REC_MEM = REC_ROWS + REC_STAGE;  // above REC_ENTRIES * 33
groupshared uint rec_mem[REC_MEM];
groupshared uint rec_info[REC_ENTRIES];   // per entry its residuals' bits ORed, then b | flags << 8 | (B - b) << 16
groupshared uint rec_fixed[REC_ENTRIES];  // per entry its field words, then their inclusive prefix sums
groupshared uint rec_extra[REC_ENTRIES];  // ... its stored bits above b, the same
groupshared uint rec_flag[REC_ENTRIES];   // ... 1 if it has flags, the same
groupshared uint rec_base;

uint rec_code(float v, uint c)
{
    uint kind = (rp.kinds >> (8 * c)) & 0xFF, bits = kind & 31, type = kind >> 5;
    if (type == REC_F16)
    {
        uint h = f32tof16(v) & 0xFFFF;
        return h ^ ((h & 0x8000) != 0 ? 0xFFFFu : 0x8000u);
    }
    if (type == REC_F32)
    {
        uint u = asuint(v);
        return u ^ ((u & 0x80000000u) != 0 ? 0xFFFFFFFFu : 0x80000000u);
    }
    if ((rp.flags & REC_SRGB) != 0 && c < 3)  // the view decoded sRGB: encoded again before rounding
        v = linear_to_srgb(float3(v, 0.0, 0.0)).x;
    return uint(round(saturate(v) * float((1u << bits) - 1u)));
}

uint4 rec_codes(int2 p, bool reference)
{
    float4 t = (rp.flags & REC_DEPTH) != 0 ? float4(src.Load(int3(p, 0)).x, 0.0, 0.0, 0.0)
             : reference ? src2.Load(int3(p, 0)) : src.Load(int3(p, 0));
    return uint4(rec_code(t.x, 0), rec_code(t.y, 1), rec_code(t.z, 2), rec_code(t.w, 3));
}

// Channel c's width in bits.
uint rec_width(uint c)
{
    const uint kind = (rp.kinds >> (8 * c)) & 0xFF, type = kind >> 5;
    return type == REC_F32 ? 32 : type == REC_F16 ? 16 : max(kind & 31, 1u);
}

// r modulo 2^width as the integer nearest 0, zigzagged.
uint rec_zigzag(uint r, uint width)
{
    if (width < 32)
    {
        const uint mask = (1u << width) - 1u;
        r &= mask;
        if (r >= (1u << (width - 1)))
            r |= ~mask;
    }
    return (r << 1) ^ uint(int(r) >> 31);
}

uint rec_bits(uint z)
{
    return z == 0 ? 0 : uint(firstbithigh(z)) + 1;
}

// bits (1 to 32) of v at bit `at` of the staged words.
void rec_put(uint at, uint v, uint bits)
{
    const uint word = REC_ROWS + at / 32, shift = at % 32;
    InterlockedOr(rec_mem[word], v << shift);
    if (shift != 0 && shift + bits > 32)
        InterlockedOr(rec_mem[word + 1], v >> (32 - shift));
}

// One workgroup per chunk (group x) of a tile row (group y): 8 threads per tile, one per row of it. Each thread keeps
// its row's residuals; the entries' widths come from shared bit counts, their offsets from prefix sums. Each row's
// fields start at a byte (8 fields of w bits), so the threads OR them into the staged words with the stored bits;
// then the chunk's words go out in order, one per thread.
[shader("compute")]
[numthreads(256, 1, 1)]
void record_pack(uint3 group : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint cx = group.x, ty = group.y, C = rp.channels;
    const uint tiles = (rp.width + 7) / 8, chunks = (tiles + REC_CHUNK - 1) / REC_CHUNK;
    const uint j = gi / 8, row = gi % 8, t = cx * REC_CHUNK + j, n = min(REC_CHUNK, tiles - cx * REC_CHUNK);
    const uint entries = n * C;
    const bool reference = (rp.flags & REC_REFERENCE) != 0;
    // This thread's row of 8 texels from (x0, y): each value minus its left neighbour's (or the reference's); below a
    // tile row's first row minus the row above's differences too (the thread before's, through rec_mem); zigzagged.
    // 0 outside the texture.
    const int x0 = int(t * 8), y = int(ty * 8 + row);
    const bool inside = t < tiles && y < int(rp.height);
    uint4 z[8];
    [unroll]
    for (int dx = 0; dx < 8; dx++)
        z[dx] = uint4(0);
    if (inside)
    {
        uint4 left = !reference && x0 > 0 ? rec_codes(int2(x0 - 1, y), false) : uint4(0);
        [unroll]
        for (int dx = 0; dx < 8; dx++)
            if (x0 + dx < int(rp.width))
            {
                const int2 p = int2(x0 + dx, y);
                const uint4 v = rec_codes(p, false);
                z[dx] = v - (reference ? rec_codes(p, true) : left);
                left = v;
            }
    }
    if (!reference)
        [unroll]
        for (uint c = 0; c < 4; c++)
            if (c < C)
            {
                [unroll]
                for (int dx = 0; dx < 8; dx++)
                    rec_mem[dx * 256 + gi] = z[dx][c];
                GroupMemoryBarrierWithGroupSync();
                if (row != 0)
                    [unroll]
                    for (int dx = 0; dx < 8; dx++)
                        z[dx][c] -= rec_mem[dx * 256 + gi - 1];
                GroupMemoryBarrierWithGroupSync();
            }
    uint4 m = uint4(0);
    [unroll]
    for (int dx = 0; dx < 8; dx++)
    {
        [unroll]
        for (uint c = 0; c < 4; c++)
            if (c < C)
                z[dx][c] = rec_zigzag(z[dx][c], rec_width(c));
        m |= z[dx];
    }
    for (uint w = gi; w < entries * 33; w += 256)
        rec_mem[w] = 0;
    if (gi < REC_ENTRIES)
        rec_info[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint c = 0; c < 4; c++)
        if (j < n && c < C)
        {
            const uint k = j * C + c;
            if (m[c] != 0)
                InterlockedOr(rec_info[k], m[c]);
            [unroll]
            for (int dx = 0; dx < 8; dx++)
            {
                const uint bits = rec_bits(z[dx][c]);
                if (bits != 0)
                    InterlockedAdd(rec_mem[k * 33 + bits], 1);
            }
        }
    GroupMemoryBarrierWithGroupSync();
    // Each entry's b: B, or the b < B whose fields, flagged bits and byte take fewer bits.
    if (gi < entries)
    {
        const uint B = rec_bits(rec_info[gi]);
        uint best = 64 * B, b = B, above = 0;
        for (int k = int(B) - 1; k >= 0; k--)
        {
            above += rec_mem[gi * 33 + k + 1];
            const uint cost = 64 * (uint(k) + 1) + above * (B - uint(k)) + 8;
            if (cost < best)
            {
                best = cost;
                b = uint(k);
            }
        }
        rec_info[gi] = b | (b < B ? 1u << 8 : 0u) | ((B - b) << 16);
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint c = 0; c < 4; c++)  // per entry and row the row's flagged residuals
        if (j < n && c < C)
        {
            const uint k = j * C + c, info = rec_info[k], b = info & 0xFF;
            uint count = 0;
            if ((info & 0x100) != 0)
                [unroll]
                for (int dx = 0; dx < 8; dx++)
                    count += rec_bits(z[dx][c]) > b ? 1 : 0;
            rec_mem[k * 8 + row] = count;
        }
    GroupMemoryBarrierWithGroupSync();
    if (gi < REC_ENTRIES)
    {
        uint fixed = 0, extra = 0, flag = 0;
        if (gi < entries)
        {
            const uint info = rec_info[gi];
            flag = (info >> 8) & 1;
            fixed = 2 * ((info & 0xFF) + flag);
            if (flag != 0)
                for (uint r = 0; r < 8; r++)
                    extra += rec_mem[gi * 8 + r] * (info >> 16);
        }
        rec_fixed[gi] = fixed;
        rec_extra[gi] = extra;
        rec_flag[gi] = flag;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1; step < REC_ENTRIES; step <<= 1)  // inclusive prefix sums over the entries
    {
        uint3 add = uint3(0);
        if (gi < REC_ENTRIES && gi >= step)
            add = uint3(rec_fixed[gi - step], rec_extra[gi - step], rec_flag[gi - step]);
        GroupMemoryBarrierWithGroupSync();
        if (gi < REC_ENTRIES)
        {
            rec_fixed[gi] += add.x;
            rec_extra[gi] += add.y;
            rec_flag[gi] += add.z;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    const uint header = (entries + 3) / 4, fixed_words = rec_fixed[REC_ENTRIES - 1];
    const uint list_words = (rec_flag[REC_ENTRIES - 1] + 3) / 4, extra_bits = rec_extra[REC_ENTRIES - 1];
    const uint total = header + fixed_words + list_words + (extra_bits + 31) / 32;
    for (uint w = REC_ROWS + gi; w < REC_ROWS + total - header; w += 256)
        rec_mem[w] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        uint base;
        InterlockedAdd(record_slot[0], total, base);
        rec_base = base;
        const uint entry = rp.table + 2 * (ty * chunks + cx);
        record_slot[entry] = base;
        record_slot[entry + 1] = total;
    }
    if (gi < entries && (rec_info[gi] & 0x100) != 0)  // the entry's byte, B - b
    {
        const uint i = rec_flag[gi] - 1;
        InterlockedOr(rec_mem[REC_ROWS + fixed_words + i / 4], (rec_info[gi] >> 16) << (8 * (i % 4)));
    }
    // This row's fields of each channel into the staged words: the entry's words start after the earlier entries',
    // the row's 8 fields of w bits after the earlier rows'; its flagged residuals' upper bits after the earlier
    // entries' and rows'.
    [unroll]
    for (uint c = 0; c < 4; c++)
        if (j < n && c < C && (rec_info[j * C + c] & 0x1FF) != 0)
        {
            const uint k = j * C + c, info = rec_info[k], b = info & 0xFF, flag = (info >> 8) & 1, e = info >> 16;
            const uint w = b + flag;
            const uint at = k == 0 ? 0 : rec_fixed[k - 1];
            uint extra_at = 32 * (fixed_words + list_words) + (k == 0 ? 0 : rec_extra[k - 1]);
            for (uint r = 0; r < row; r++)
                extra_at += rec_mem[k * 8 + r] * e;
            const uint low = b == 32 ? 0xFFFFFFFFu : (1u << b) - 1u;
            uint word = at + (8 * w * row) / 32, shift = (8 * w * row) % 32, acc = 0;
            [unroll]
            for (int dx = 0; dx < 8; dx++)
            {
                uint v = z[dx][c] & low;
                if (flag != 0 && rec_bits(z[dx][c]) > b)
                {
                    v |= 1u << b;
                    rec_put(extra_at, z[dx][c] >> b, e);
                    extra_at += e;
                }
                acc |= v << shift;
                if (shift + w >= 32)
                {
                    InterlockedOr(rec_mem[REC_ROWS + word], acc);
                    word++;
                    const uint placed = 32 - shift;
                    acc = placed < 32 ? v >> placed : 0;
                    shift = shift + w - 32;
                }
                else
                    shift += w;
            }
            if (shift > 0)
                InterlockedOr(rec_mem[REC_ROWS + word], acc);
        }
    GroupMemoryBarrierWithGroupSync();
    const uint base = rec_base;
    for (uint w = gi; w < total; w += 256)
    {
        uint word = 0;
        if (w < header)
            for (uint k = 0; k < 4; k++)
            {
                const uint i = 4 * w + k;
                if (i < entries)
                    word |= ((rec_info[i] & 0xFF) | ((rec_info[i] >> 1) & 0x80)) << (8 * k);
            }
        else
            word = rec_mem[REC_ROWS + w - header];
        record_slot[base + w] = word;
    }
}
