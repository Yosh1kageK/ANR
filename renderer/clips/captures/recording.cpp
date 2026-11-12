#include "renderer/clips/captures/recording.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "record/reader.h"

namespace renderer::clips::captures {
namespace fs = std::filesystem;

namespace {

// The Vulkan formats of decoded texels (record::Texture::upload) that the tab shows.
constexpr uint32_t VK_RGBA8_UNORM = 37, VK_RGBA8_SRGB = 43, VK_A2B10G10R10_UNORM = 64, VK_RG16_SFLOAT = 83,
                   VK_RGBA16_SFLOAT = 97, VK_R32_SFLOAT = 100, VK_RG32_SFLOAT = 103, VK_D16_UNORM = 124,
                   VK_D32_SFLOAT = 126;

// The DXGI format whose codes a recorded colour texture holds (CodeSteps): the game's in D3D12, else the equivalent of
// its Vulkan format.
int DxgiOf(const record::Texture& t) {
    if (t.dxgi)
        return (int)t.dxgi;
    switch (t.upload) {
        case VK_RGBA8_UNORM: return 28;
        case VK_RGBA8_SRGB: return 29;
        case VK_A2B10G10R10_UNORM: return 24;
        case VK_RGBA16_SFLOAT: return 10;
        default: return 0;
    }
}

// An open recording: its reader, a decoder pool, and its last decoded frames (each frame's textures decode together).
struct Opened {
    std::mutex mu;
    record::Recording rec;
    std::unique_ptr<record::Pool> pool;
    record::DecodeScratch scratch;
    std::deque<std::pair<int, std::map<std::string, std::vector<uint8_t>>>> decoded;  // newest last
};

constexpr size_t KEEP_RECORDINGS = 4, KEEP_FRAMES = 2;

std::mutex g_mu;
std::deque<std::pair<std::string, std::shared_ptr<Opened>>> g_open;  // most recently used last

std::shared_ptr<Opened> Get(const std::string& folder, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto it = g_open.begin(); it != g_open.end(); ++it)
        if (it->first == folder) {
            auto found = *it;
            g_open.erase(it);
            g_open.push_back(found);
            return found.second;
        }
    auto opened = std::make_shared<Opened>();
    if (!record::Open(folder, opened->rec, error))
        return nullptr;
    opened->pool = std::make_unique<record::Pool>(std::max(1u, std::thread::hardware_concurrency() / 2));
    g_open.push_back({folder, opened});
    while (g_open.size() > KEEP_RECORDINGS)
        g_open.pop_front();  // a reader still using it keeps it alive
    return opened;
}

// Calls `use` with a recorded texture of a real frame and its decoded texels.
bool WithTexels(const std::string& folder, int frame, const std::string& role,
                const std::function<bool(const record::Texture&, const std::vector<uint8_t>&)>& use, std::string& error) {
    std::shared_ptr<Opened> opened = Get(folder, error);
    if (!opened)
        return false;
    std::lock_guard<std::mutex> lock(opened->mu);
    const record::Texture* texture = opened->rec.Find(role);
    if (!texture) {
        error = "the recording has no " + role;
        return false;
    }
    if (frame < 0 || frame >= (int)opened->rec.index.size()) {
        error = "the recording has no frame " + std::to_string(frame);
        return false;
    }
    auto cached = std::find_if(opened->decoded.begin(), opened->decoded.end(), [&](const auto& d) { return d.first == frame; });
    if (cached == opened->decoded.end()) {
        std::map<std::string, std::vector<uint8_t>> textures;
        if (!record::Decode(opened->rec, opened->rec.index[(size_t)frame], *opened->pool, opened->scratch, textures, error))
            return false;
        opened->decoded.push_back({frame, std::move(textures)});
        while (opened->decoded.size() > KEEP_FRAMES)
            opened->decoded.pop_front();
        cached = std::prev(opened->decoded.end());
    }
    auto texels = cached->second.find(role);
    if (texels == cached->second.end() ||
        texels->second.size() != (size_t)texture->width * texture->height * texture->texel) {
        error = "frame " + std::to_string(frame) + "'s " + role + " did not decode to its size";
        return false;
    }
    return use(*texture, texels->second);
}

}  // namespace

bool IsRecording(const std::string& folder) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(folder) / "record.json", ec);
}

bool LoadRecording(const std::string& folder, CaptureInfo& c, std::string& error) {
    record::Recording rec;
    if (!record::Open(folder, rec, error)) {
        error = folder + ": " + error;
        return false;
    }
    if (!rec.feature.empty() && rec.feature != "fg") {
        error = folder + ": a recording of " + rec.feature + " (the tab shows frame generation)";
        return false;
    }
    c = CaptureInfo();
    c.folder = folder;
    c.name = fs::path(folder).filename().string();
    if (c.name.empty())
        c.name = fs::path(folder).parent_path().filename().string();
    c.format = RECORDING_FORMAT;
    const nlohmann::json& m = rec.meta;
    c.game_path = m.value("game", "");
    c.game = GameName(c.game_path);
    c.time = m.value("time", "");
    c.epoch = ParseTime(c.time);
    c.width = m.value("width", 0u);
    c.height = m.value("height", 0u);
    c.encoding = m.value("encoding", "");
    c.coding = CodingOf(c.encoding);
    if (m.contains("recorder"))
        c.in_game = m["recorder"].is_string() ? m["recorder"].get<std::string>() : m["recorder"].dump();
    std::map<uint64_t, std::vector<const record::Call*>> calls;  // by the source's real frame number
    int multiplier = m.value("multiplier", 0);
    for (const record::Call& call : rec.calls) {
        calls[call.frame].push_back(&call);
        if (!m.contains("multiplier"))
            multiplier = std::max(multiplier, (int)call.count + 1);
    }
    c.multiplier = std::max(multiplier, 2);
    for (size_t i = 0; i < rec.index.size(); i++) {
        const record::IndexRecord& ix = rec.index[i];
        FrameInfo f;
        f.real_frame = (int64_t)ix.frame;
        f.after_gap = i > 0 && ix.dropped > 0;
        for (const record::Texture& t : rec.textures)
            f.files[t.name] = TextureInfo{t.width, t.height, DxgiOf(t), "recorded"};
        for (const record::Call* call : calls[ix.frame]) {
            CallInfo ci;
            ci.index = (int)call->index;
            ci.t = call->count ? (double)call->index / (call->count + 1) : (double)call->index / c.multiplier;
            f.calls.push_back(ci);
        }
        c.frames.push_back(std::move(f));
    }
    if (c.frames.empty() || c.width == 0 || c.height == 0) {
        error = folder + ": no recorded frames or no size";
        return false;
    }
    return true;
}

bool ReadRecordedColor(const std::string& folder, int frame, const std::string& role, Image& image, std::string& error) {
    return WithTexels(folder, frame, role, [&](const record::Texture& t, const std::vector<uint8_t>& texels) {
        image = Image();
        image.width = t.width;
        image.height = t.height;
        image.dxgi = DxgiOf(t);
        const size_t n = (size_t)t.width * t.height;
        image.rgba.resize(n * 4);
        switch (t.upload) {
            case VK_RGBA8_UNORM:
            case VK_RGBA8_SRGB:
                for (size_t i = 0; i < n * 4; i++)
                    image.rgba[i] = FloatToHalf((float)texels[i] / 255.0f);
                return true;
            case VK_A2B10G10R10_UNORM:
                for (size_t i = 0; i < n; i++) {
                    uint32_t w;
                    std::memcpy(&w, texels.data() + i * 4, 4);
                    image.rgba[i * 4 + 0] = FloatToHalf((float)(w & 1023u) / 1023.0f);
                    image.rgba[i * 4 + 1] = FloatToHalf((float)((w >> 10) & 1023u) / 1023.0f);
                    image.rgba[i * 4 + 2] = FloatToHalf((float)((w >> 20) & 1023u) / 1023.0f);
                    image.rgba[i * 4 + 3] = FloatToHalf((float)(w >> 30) / 3.0f);
                }
                return true;
            case VK_RGBA16_SFLOAT:
                std::memcpy(image.rgba.data(), texels.data(), n * 8);
                return true;
            default:
                error = role + ": its Vulkan format " + std::to_string(t.upload) + " is not shown here";
                return false;
        }
    }, error);
}

bool ReadRecordedDepth(const std::string& folder, int frame, const std::string& role, std::vector<float>& depth,
                       uint32_t& width, uint32_t& height, std::string& error) {
    return WithTexels(folder, frame, role, [&](const record::Texture& t, const std::vector<uint8_t>& texels) {
        width = t.width;
        height = t.height;
        const size_t n = (size_t)t.width * t.height;
        depth.resize(n);
        if (t.upload == VK_D32_SFLOAT || t.upload == VK_R32_SFLOAT) {
            std::memcpy(depth.data(), texels.data(), n * 4);
        } else if (t.upload == VK_D16_UNORM) {
            for (size_t i = 0; i < n; i++) {
                uint16_t code;
                std::memcpy(&code, texels.data() + i * 2, 2);
                depth[i] = (float)code / 65535.0f;
            }
        } else {
            error = role + ": its Vulkan format " + std::to_string(t.upload) + " is not a depth format shown here";
            return false;
        }
        return true;
    }, error);
}

bool ReadRecordedMotion(const std::string& folder, int frame, const std::string& role, std::vector<float>& motion,
                        uint32_t& width, uint32_t& height, std::string& error) {
    return WithTexels(folder, frame, role, [&](const record::Texture& t, const std::vector<uint8_t>& texels) {
        width = t.width;
        height = t.height;
        const size_t n = (size_t)t.width * t.height * 2;
        motion.resize(n);
        if (t.upload == VK_RG16_SFLOAT) {
            for (size_t i = 0; i < n; i++) {
                uint16_t h;
                std::memcpy(&h, texels.data() + i * 2, 2);
                motion[i] = HalfToFloat(h);
            }
        } else if (t.upload == VK_RG32_SFLOAT) {
            std::memcpy(motion.data(), texels.data(), n * 4);
        } else {
            error = role + ": its Vulkan format " + std::to_string(t.upload) + " is not a motion format shown here";
            return false;
        }
        return true;
    }, error);
}

bool ReadSource(const Source& s, uint32_t out_w, uint32_t out_h, Image& image, std::string& error) {
    if (s.raw)
        return ReadRawFrame(s.path, image, error);
    uint32_t w = s.info.width, h = s.info.height;
    if (s.kind == SourceKind::DEPTH) {
        std::vector<float> depth;
        const bool ok = s.recorded ? ReadRecordedDepth(s.path, s.frame, s.role, depth, w, h, error)
                                   : ReadDepth(s.path, s.info, depth, error);
        if (ok)
            image = DepthImage(depth, w, h, out_w, out_h);
        return ok;
    }
    if (s.kind == SourceKind::MOTION) {
        std::vector<float> motion;
        const bool ok = s.recorded ? ReadRecordedMotion(s.path, s.frame, s.role, motion, w, h, error)
                                   : ReadMotion(s.path, s.info, motion, error);
        if (ok)
            image = MotionImage(motion, w, h, out_w, out_h);
        return ok;
    }
    return s.recorded ? ReadRecordedColor(s.path, s.frame, s.role, image, error) : ReadColor(s.path, s.info, image, error);
}

bool ReadFrameColor(const CaptureInfo& c, int frame, const std::string& name, Image& image, std::string& error) {
    if (c.format == RECORDING_FORMAT)
        return ReadRecordedColor(c.folder, frame, name, image, error);
    const FrameInfo* f = c.Frame(frame);
    if (!f || !f->files.count(name)) {
        error = "frame " + std::to_string(frame) + " has no " + name;
        return false;
    }
    return ReadColor(c.folder + "/" + TextureFile(frame, name), f->files.at(name), image, error);
}

}  // namespace renderer::clips::captures
