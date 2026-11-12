// The nr-record format (FORMAT.md): the binary records of a recording folder, the channel kinds of the lossless GPU
// coding, and the coded size bounds a recorder allocates by. The recorder (recorder.h) writes these records and the
// reader (reader.h) reads them; both include this file, so the layouts exist once.
#pragma once

#include <algorithm>
#include <cstdint>

namespace record {

// record.json "format": "nr-record-<FORMAT_VERSION>". Readers take 1 to FORMAT_VERSION.
constexpr int FORMAT_VERSION = 4;
// record.json "coding" (from format 4; formats 1 to 3 imply 1, 1 and 3): 1 predicts each value from its left
// neighbour (in column 0 the one above) with one bit count per tile and channel; 3 predicts left + above - above-left
// within each 8-row tile row, with per tile and channel a bit count and the wider values' upper bits stored apart.
constexpr int CODING_VERSION = 3;

// frames.idx: one per recorded real frame, in recording order.
struct IndexRecord {
    char magic[4] = {'N', 'R', 'I', '1'};
    uint32_t sequence = 0;           // recorded real frames before this one
    uint64_t frame = 0;              // the source's real frame number (its calls in params.bin carry it)
    uint64_t offset = 0, bytes = 0;  // in frames.bin: the frame's BlocksHeader and blocks (format 1: its words)
    int64_t qpc = 0;                 // QueryPerformanceCounter at its first call (record.json qpc_frequency)
    uint32_t dropped = 0;            // real frames dropped right before it (a full ring): replay resets here
    float pack_gpu_ms = 0, copy_ms = 0;
    uint32_t raw_bytes = 0;          // from format 2: the slot's coded words before compression
    float compress_ms = 0;           // from format 2: its blocks' compression (or copy) time, summed over threads
    uint32_t reserved = 0;
};
static_assert(sizeof(IndexRecord) == 64);

// frames.bin, from format 2: per real frame this, then per block its stored size (BLOCK_STORED set: the block as it
// is, else one zstd frame), then the blocks. Blocks are block_bytes of the slot's words, the last one shorter.
struct BlocksHeader {
    char magic[4] = {'N', 'R', 'Z', '1'};
    uint32_t blocks = 0;
    uint64_t raw_bytes = 0;
    uint32_t block_bytes = 0;
    int32_t level = 0;  // zstd's; 0: stored
};
static_assert(sizeof(BlocksHeader) == 24);
constexpr uint32_t BLOCK_BYTES = 2u << 20, BLOCK_STORED = 0x80000000u;

// params.bin: per call this, then per parameter (record.json "parameters") 12 bytes: has (1: an unsigned value,
// 2: a float value), the unsigned value, the float's bits; then per matrix ("matrices") 68 bytes: has (1), 16 floats.
struct CallHeader {
    char magic[4] = {'N', 'R', 'P', '1'};
    uint32_t index = 0, count = 0, flags = 0;  // FG: the generated frame's index (1 to count) and the count
    uint64_t frame = 0;                        // the real frame number (IndexRecord::frame)
    int64_t qpc = 0;
};
static_assert(sizeof(CallHeader) == 32);
constexpr uint32_t PARAMETER_BYTES = 12, MATRIX_BYTES = 68;
constexpr uint64_t CallBytes(uint64_t parameters, uint64_t matrices) {
    return sizeof(CallHeader) + PARAMETER_BYTES * parameters + MATRIX_BYTES * matrices;
}

// Channel kinds: per channel 8 bits (red lowest), bits | type << 5: UNORM codes of `bits` bits, or float bits.
enum Kind : uint32_t { KIND_UNORM = 0, KIND_F16 = 1, KIND_F32 = 2 };
constexpr uint32_t F16 = KIND_F16 << 5, F32 = KIND_F32 << 5;

// How one texture is coded. vk_format: the Vulkan format of its texels as recorded (a D3D12 format's equivalent);
// depth: the depth plane of a depth (and stencil) format.
struct Channels {
    uint32_t vk_format = 0, width = 0, height = 0, channels = 0, kinds = 0;
    bool depth = false, srgb = false;
};

constexpr uint32_t KindOf(const Channels& c, uint32_t channel) { return (c.kinds >> (8 * channel)) & 0xFF; }

// A channel's width in bits: the values are integers of this width, residuals modulo 2^width.
constexpr uint32_t ChannelBits(uint32_t kind) {
    return (kind >> 5) == KIND_F32 ? 32 : (kind >> 5) == KIND_F16 ? 16 : std::max(kind & 31, 1u);
}

// The chunk table of a texture: 2 words (first word, word count) per chunk of 32 8x8 tiles, tile row by tile row.
constexpr uint64_t TableWords(const Channels& c) {
    const uint64_t rows = (c.height + 7) / 8, tiles = (c.width + 7) / 8, chunks = (tiles + 31) / 32;
    return 2 * rows * chunks;
}

// The most words coding 3 writes for a texture: per chunk its header's, bytes' and stored bits' padding words, then
// the headers and the fields at the channels' full widths with a flag bit each.
constexpr uint64_t WorstWords(const Channels& c) {
    const uint64_t rows = (c.height + 7) / 8, tiles = (c.width + 7) / 8, chunks = (tiles + 31) / 32;
    uint64_t bits = 0;
    for (uint32_t ch = 0; ch < c.channels; ch++)
        bits += ChannelBits(KindOf(c, ch));
    return rows * (3 * chunks + (tiles * c.channels + 3) / 4 + tiles * 2 * bits);
}

// Bytes of one texel as a reader stores it decoded (the recorded format's texel; A2B10G10R10 4).
constexpr uint32_t TexelBytes(const Channels& c, bool packed_10_10_10_2) {
    if (packed_10_10_10_2)
        return 4;
    uint32_t bytes = 0;
    for (uint32_t ch = 0; ch < c.channels; ch++) {
        const uint32_t kind = KindOf(c, ch), type = kind >> 5;
        bytes += type == KIND_F32 ? 4 : type == KIND_F16 ? 2 : (kind & 31) <= 8 ? 1 : 2;
    }
    return bytes;
}

}  // namespace record
