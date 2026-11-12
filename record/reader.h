// Reads an nr-record folder (FORMAT.md), formats 1 to 4: record.json, the frame index, every call's parameters, and
// each recorded real frame's textures decoded on the CPU to their recorded texels, bit for bit.
//
//   record::Recording rec;
//   if (!record::Open(dir, rec, error)) ...
//   record::Pool pool(std::thread::hardware_concurrency());
//   record::DecodeScratch scratch;
//   std::map<std::string, std::vector<uint8_t>> textures;  // by role
//   record::Decode(rec, rec.index[n], pool, scratch, textures, error);
#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "record/format.h"
#include "record/pool.h"

namespace record {

// The Vulkan formats a reader maps (vulkan_core.h's values; the reader does not need Vulkan's headers).
namespace vk {
constexpr uint32_t R8G8B8A8_UNORM = 37, R8G8B8A8_SRGB = 43, B8G8R8A8_UNORM = 44, B8G8R8A8_SRGB = 50,
                   A2B10G10R10_UNORM_PACK32 = 64, R16G16B16A16_SFLOAT = 97, D32_SFLOAT = 126,
                   D32_SFLOAT_S8_UINT = 130;
}

struct Texture {
    std::string name;       // its role (FORMAT.md, per feature)
    std::string reference;  // the role it is coded against, or empty
    uint32_t api_format = 0, dxgi = 0, vk = 0, width = 0, height = 0, channels = 0, kinds = 0, table = 0;
    bool depth = false, srgb = false;
    // As decoded: the Vulkan format of its texels (BGRA8 as RGBA8, D32S8's depth plane as D32) and bytes per texel.
    uint32_t upload = 0;
    uint32_t texel = 0;
};

struct Call {
    uint32_t index = 0, count = 0, flags = 0;
    uint64_t frame = 0;
    int64_t qpc = 0;
    std::vector<std::array<uint32_t, 3>> values;  // per parameter: has (1 u, 2 f), u, f's bits
    std::vector<std::pair<bool, std::array<float, 16>>> matrices;
};

struct Recording {
    nlohmann::json meta;
    int format = 1;  // nr-record-<format>
    int coding = 1;  // FORMAT.md "Coding"
    bool blocks = false;  // frames.bin holds blocks (format 2 on); else each frame's words
    std::string feature, source, graphics_api, parameter_source;
    std::vector<Texture> textures;
    std::vector<IndexRecord> index;
    std::vector<std::string> parameters, matrices;
    std::vector<Call> calls;
    std::ifstream frames;

    const Texture* Find(const std::string& role) const;
    // A call's parameter by name; false if the recording has no such parameter or the call did not set it that way.
    bool Unsigned(const Call& call, const std::string& name, uint32_t& out) const;
    bool Float(const Call& call, const std::string& name, float& out) const;
    const float* Matrix(const Call& call, const std::string& name) const;  // 16 floats, or null
};

bool Open(const std::string& dir, Recording& r, std::string& error);

// Buffers a decoder keeps between frames.
struct DecodeScratch {
    std::vector<char> stored;
    std::vector<uint32_t> words;
    std::vector<uint64_t> block_at;
    std::vector<std::vector<std::vector<uint32_t>>> planes;  // per texture (Recording::textures' order), per channel
};

// One recorded real frame's coded words (its slot) into s.words: read, and from format 2 its blocks decompressed on
// the pool.
bool Inflate(Recording& r, const IndexRecord& ix, Pool& pool, DecodeScratch& s, std::string& error);

// One recorded real frame's textures as their texels (Texture::upload's layout, row by row), by role.
bool Decode(Recording& r, const IndexRecord& ix, Pool& pool, DecodeScratch& s,
            std::map<std::string, std::vector<uint8_t>>& out, std::string& error);

// FG's parameters of one call in the recording's units (FORMAT.md "FG"), from the source's names (ngx-dlssg).
struct FgCall {
    uint32_t index = 0, count = 0;  // generated frame index (1 to count) of this real frame's count
    uint64_t frame = 0;
    bool reset = false, depth_inverted = false, hdr = false, motion_jittered = false, camera_motion_included = false,
         orthographic = false;
    float jitter[2] = {0, 0};        // render pixels
    float motion_scale[2] = {1, 1};  // stored motion times this: pixels of the motion texture
    float near_plane = 0, far_plane = 0, fov = 0, aspect = 0;
    float camera_pos[3] = {}, camera_up[3] = {}, camera_right[3] = {}, camera_fwd[3] = {};
    // 4x4, as the source gives them (row-major for NGX); has_* false if not given.
    float clip_to_prev_clip[16] = {}, prev_clip_to_clip[16] = {}, view_to_clip[16] = {}, clip_to_view[16] = {};
    bool has_clip_to_prev_clip = false, has_prev_clip_to_clip = false, has_view_to_clip = false,
         has_clip_to_view = false;
    // x, y, width, height of the part in use (0 0 0 0: the whole texture).
    uint32_t backbuffer_rect[4] = {}, hudless_rect[4] = {}, depth_rect[4] = {}, motion_rect[4] = {};
};
bool Fg(const Recording& r, const Call& call, FgCall& out, std::string& error);
nlohmann::json ToJson(const FgCall& c);

}  // namespace record
