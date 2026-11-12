#include "record/reader.h"

#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>

namespace record {
namespace {

std::vector<char> Read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

bool Open(const std::string& dir, Recording& r, std::string& error) {
    const std::vector<char> text = Read(dir + "/record.json");
    r.meta = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    const std::string format = r.meta.is_object() ? r.meta.value("format", "") : "";
    const int version = format.rfind("nr-record-", 0) == 0 && format.size() == 11 ? format.back() - '0' : 0;
    if (version < 1 || version > FORMAT_VERSION) {
        error = dir + "/record.json: not an nr-record folder (formats 1 to " + std::to_string(FORMAT_VERSION) + ")";
        return false;
    }
    r.format = version;
    r.coding = r.meta.value("coding", version >= 3 ? 3 : 1);
    if (r.coding != 1 && r.coding != 3) {
        error = dir + "/record.json: coding " + std::to_string(r.coding) + " is not known";
        return false;
    }
    r.blocks = version >= 2;
    r.feature = r.meta.value("feature", "fg");  // formats 1 to 3: DLSS-G's inputs in D3D12 games
    r.source = r.meta.value("source", "ngx-dlssg");
    r.graphics_api = r.meta.value("graphics_api", "d3d12");
    r.parameter_source = r.meta.value("parameter_source", "ngx");
    for (const auto& t : r.meta.value("textures", nlohmann::json::array())) {
        Texture x;
        x.name = t.contains("role") ? t["role"].get<std::string>() : t.value("name", "");
        x.reference = t.value("reference", nlohmann::json()).is_string() ? t["reference"].get<std::string>() : "";
        x.dxgi = t.value("dxgi_format", 0u);
        x.api_format = t.value("api_format", x.dxgi);
        x.vk = t.value("vk_format", 0u);
        x.width = t.value("width", 0u);
        x.height = t.value("height", 0u);
        x.channels = t.value("channels", 0u);
        x.kinds = t.value("kinds", 0u);
        x.table = t.value("table", 0u);
        x.depth = t.value("depth", false);
        x.srgb = t.value("srgb", false);
        if (x.name.empty() || !x.width || !x.height || x.channels < 1 || x.channels > 4) {
            error = dir + "/record.json: a texture without a role, size or channels";
            return false;
        }
        x.upload = x.vk == vk::B8G8R8A8_UNORM ? vk::R8G8B8A8_UNORM
                 : x.vk == vk::B8G8R8A8_SRGB ? vk::R8G8B8A8_SRGB
                 : x.vk == vk::D32_SFLOAT_S8_UINT ? vk::D32_SFLOAT : x.vk;
        Channels c;
        c.channels = x.channels;
        c.kinds = x.kinds;
        x.texel = x.vk == vk::B10G11R11_UFLOAT_PACK32 ? 4 : TexelBytes(c, x.vk == vk::A2B10G10R10_UNORM_PACK32);
        r.textures.push_back(x);
    }
    for (const auto& p : r.meta.value("parameters", nlohmann::json::array()))
        r.parameters.push_back(p);
    for (const auto& m : r.meta.value("matrices", nlohmann::json::array()))
        r.matrices.push_back(m);
    const std::vector<char> idx = Read(dir + "/frames.idx");
    r.index.resize(idx.size() / sizeof(IndexRecord));
    std::memcpy(r.index.data(), idx.data(), r.index.size() * sizeof(IndexRecord));
    const size_t call_bytes = r.meta.value("call_bytes", (size_t)0);
    if (call_bytes != CallBytes(r.parameters.size(), r.matrices.size())) {
        error = "params.bin's record size does not match its parameter lists";
        return false;
    }
    const std::vector<char> params = Read(dir + "/params.bin");
    for (size_t at = 0; at + call_bytes <= params.size(); at += call_bytes) {
        const char* p = params.data() + at;
        CallHeader h;
        std::memcpy(&h, p, sizeof(h));
        if (std::memcmp(h.magic, "NRP1", 4) != 0) {
            error = "params.bin: a record without its magic";
            return false;
        }
        Call c;
        c.index = h.index;
        c.count = h.count;
        c.flags = h.flags;
        c.frame = h.frame;
        c.qpc = h.qpc;
        p += sizeof(h);
        c.values.resize(r.parameters.size());
        std::memcpy(c.values.data(), p, PARAMETER_BYTES * r.parameters.size());
        p += PARAMETER_BYTES * r.parameters.size();
        for (size_t m = 0; m < r.matrices.size(); m++, p += MATRIX_BYTES) {
            uint32_t has = 0;
            std::array<float, 16> v;
            std::memcpy(&has, p, 4);
            std::memcpy(v.data(), p + 4, 64);
            c.matrices.push_back({has != 0, v});
        }
        r.calls.push_back(std::move(c));
    }
    r.frames.open(dir + "/frames.bin", std::ios::binary);
    if (!r.frames) {
        error = dir + "/frames.bin: cannot open";
        return false;
    }
    return true;
}

const Texture* Recording::Find(const std::string& role) const {
    for (const Texture& t : textures)
        if (t.name == role)
            return &t;
    return nullptr;
}

bool Recording::Unsigned(const Call& call, const std::string& name, uint32_t& out) const {
    for (size_t i = 0; i < parameters.size() && i < call.values.size(); i++)
        if (parameters[i] == name && (call.values[i][0] & 1)) {
            out = call.values[i][1];
            return true;
        }
    return false;
}

bool Recording::Float(const Call& call, const std::string& name, float& out) const {
    for (size_t i = 0; i < parameters.size() && i < call.values.size(); i++)
        if (parameters[i] == name && (call.values[i][0] & 2)) {
            std::memcpy(&out, &call.values[i][2], 4);
            return true;
        }
    return false;
}

const float* Recording::Matrix(const Call& call, const std::string& name) const {
    for (size_t i = 0; i < matrices.size() && i < call.matrices.size(); i++)
        if (matrices[i] == name && call.matrices[i].first)
            return call.matrices[i].second.data();
    return nullptr;
}

bool Inflate(Recording& r, const IndexRecord& ix, Pool& pool, DecodeScratch& s, std::string& error) {
    const uint64_t raw = r.blocks ? ix.raw_bytes : ix.bytes;
    if (std::memcmp(ix.magic, "NRI1", 4) != 0 || raw % 4) {
        error = "frames.idx: a bad record";
        return false;
    }
    std::vector<uint32_t>& words = s.words;
    words.resize(raw / 4);
    s.stored.resize(r.blocks ? ix.bytes : 0);
    const std::vector<char>& stored = s.stored;
    char* read_to = r.blocks ? s.stored.data() : (char*)words.data();
    r.frames.clear();
    r.frames.seekg((std::streamoff)ix.offset);
    if (!r.frames.read(read_to, (std::streamsize)ix.bytes)) {
        error = "frames.bin is shorter than its index";
        return false;
    }
    if (r.blocks) {  // the blocks into the slot's words
        BlocksHeader h;
        if (stored.size() < sizeof(h) || (std::memcpy(&h, stored.data(), sizeof(h)), std::memcmp(h.magic, "NRZ1", 4)) ||
            h.raw_bytes != raw || !h.block_bytes || h.blocks != (raw + h.block_bytes - 1) / h.block_bytes ||
            stored.size() < sizeof(h) + 4ull * h.blocks) {
            error = "frames.bin: a frame's block header does not match its index";
            return false;
        }
        std::vector<uint32_t> sizes(h.blocks);
        std::memcpy(sizes.data(), stored.data() + sizeof(h), 4ull * h.blocks);
        s.block_at.resize(h.blocks);
        uint64_t at = sizeof(h) + 4ull * h.blocks;
        for (uint32_t b = 0; b < h.blocks; b++) {
            const uint64_t n = sizes[b] & ~BLOCK_STORED, offset = (uint64_t)b * h.block_bytes;
            const uint64_t expected = std::min<uint64_t>(h.block_bytes, raw - offset);
            if (at + n > stored.size()) {
                error = "frames.bin: a block runs past its frame";
                return false;
            }
            if ((sizes[b] & BLOCK_STORED) && n != expected) {
                error = "frames.bin: a stored block's size";
                return false;
            }
            s.block_at[b] = at;
            at += n;
        }
        if (at != stored.size()) {
            error = "frames.bin: a frame's blocks do not fill its index entry";
            return false;
        }
        std::atomic<bool> bad{false};
        pool.For(h.blocks, [&](size_t b) {
            const uint64_t n = sizes[b] & ~BLOCK_STORED, offset = (uint64_t)b * h.block_bytes;
            const uint64_t expected = std::min<uint64_t>(h.block_bytes, raw - offset);
            char* to = (char*)words.data() + offset;
            if (sizes[b] & BLOCK_STORED)
                std::memcpy(to, stored.data() + s.block_at[b], n);
            else if (ZSTD_decompress(to, expected, stored.data() + s.block_at[b], n) != expected)
                bad = true;
        });
        if (bad) {
            error = "frames.bin: a block does not decompress to its size";
            return false;
        }
    }
    return true;
}

// Coding 3 predicts each texel from left + above - above-left within its tile row or from its reference texture's
// texel, so the residuals are read in parallel per tile row, then summed along each row and down the tile row's
// columns (modulo the channel's width; the texels keep its bits); coding 1 predicts from the left neighbour (in column
// 0 the one above): summed down column 0, then along each row. Textures with a reference come after the others.
bool Decode(Recording& r, const IndexRecord& ix, Pool& pool, DecodeScratch& s,
            std::map<std::string, std::vector<uint8_t>>& out, std::string& error) {
    if (!Inflate(r, ix, pool, s, error))
        return false;
    const std::vector<uint32_t>& words = s.words;
    const size_t count = r.textures.size();
    std::vector<int> reference(count, -1);
    for (size_t t = 0; t < count; t++)
        for (size_t u = 0; u < count; u++)
            if (!r.textures[t].reference.empty() && r.textures[u].name == r.textures[t].reference)
                reference[t] = (int)u;
    s.planes.resize(count);
    std::vector<uint8_t*> bytes(count);
    for (size_t t = 0; t < count; t++) {
        const Texture& x = r.textures[t];
        if (!x.reference.empty() && reference[t] < 0) {
            error = x.name + ": no texture " + x.reference;
            return false;
        }
        s.planes[t].resize(x.channels);
        for (std::vector<uint32_t>& plane : s.planes[t])
            plane.resize((size_t)x.width * x.height);
        std::vector<uint8_t>& o = out[x.name];
        o.resize((size_t)x.width * x.height * x.texel);  // every byte is written below
        bytes[t] = o.data();
    }
    std::mutex error_mutex;
    std::atomic<bool> failed{false};
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!failed.exchange(true))
            error = e;
    };
    // Coding 1: tile row ty of texture t into its planes: residuals, or with a reference the texels' codes.
    auto tile_row = [&](size_t t, uint32_t ty) {
        const Texture& x = r.textures[t];
        const uint32_t w = x.width, h = x.height, C = x.channels, tiles = (w + 7) / 8, rows = (h + 7) / 8;
        const uint32_t chunks = (tiles + 31) / 32;
        if (x.table + 2ull * rows * chunks > words.size()) {
            fail(x.name + ": the chunk table is outside the frame");
            return;
        }
        for (uint32_t cx = 0; cx < chunks; cx++) {
            const uint64_t entry = x.table + 2ull * ((uint64_t)ty * chunks + cx);
            const uint64_t base = words[entry], total = words[entry + 1];
            const uint32_t first = cx * 32, n = std::min(32u, tiles - first);
            const uint64_t header = ((uint64_t)n * C + 3) / 4;
            if (base + total > words.size() || total < header) {
                fail(x.name + ": a chunk outside the frame");
                return;
            }
            uint64_t at = base + header;
            for (uint32_t j = 0; j < n; j++)
                for (uint32_t c = 0; c < C; c++) {
                    const uint64_t k = (uint64_t)j * C + c;
                    const uint32_t b = (words[base + k / 4] >> (8 * (k % 4))) & 0xFF;
                    if (b > 32 || at + 2ull * b > base + total) {
                        fail(x.name + ": a tile's bits run past its chunk");
                        return;
                    }
                    uint32_t* plane = s.planes[t][c].data();
                    const uint32_t* ref = reference[t] >= 0 ? s.planes[reference[t]][c].data() : nullptr;
                    const uint32_t tx = first + j;
                    const uint64_t mask = b == 32 ? 0xFFFFFFFFull : ((1ull << b) - 1);
                    for (uint32_t i = 0; i < 64; i++) {
                        const uint32_t x0 = tx * 8 + i % 8, y0 = ty * 8 + i / 8;
                        if (x0 >= w || y0 >= h)
                            continue;
                        uint32_t z = 0;
                        if (b) {
                            const uint64_t bit = (uint64_t)i * b, wi = at + bit / 32, sh = bit % 32;
                            uint64_t pair = words[wi];
                            if (sh + b > 32)
                                pair |= (uint64_t)words[wi + 1] << 32;
                            z = (uint32_t)((pair >> sh) & mask);
                        }
                        const uint32_t residual = (z >> 1) ^ (0u - (z & 1));
                        const size_t p = (size_t)y0 * w + x0;
                        plane[p] = ref ? ref[p] + residual : residual;
                    }
                    at += 2ull * b;
                }
            if (at != base + total) {
                fail(x.name + ": a chunk's length does not match its bits");
                return;
            }
        }
    };
    // Coding 3: tile row ty of texture t into its planes (FORMAT.md "Coding").
    auto tile_row3 = [&](size_t t, uint32_t ty) {
        const Texture& x = r.textures[t];
        const uint32_t w = x.width, h = x.height, C = x.channels, tiles = (w + 7) / 8, rows = (h + 7) / 8;
        const uint32_t chunks = (tiles + 31) / 32;
        if (x.table + 2ull * rows * chunks > words.size() || C > 4) {
            fail(x.name + ": the chunk table is outside the frame");
            return;
        }
        auto bits_at = [&](uint64_t bit, uint32_t n) {  // n (1 to 32) bits from bit `bit` of the words
            const uint64_t wi = bit / 32, sh = bit % 32;
            uint64_t pair = words[wi];
            if (sh + n > 32)
                pair |= (uint64_t)words[wi + 1] << 32;
            return (uint32_t)((pair >> sh) & (n == 32 ? 0xFFFFFFFFull : (1ull << n) - 1));
        };
        std::array<uint32_t, 128> b, flag, e, at;
        for (uint32_t cx = 0; cx < chunks; cx++) {
            const uint64_t entry = x.table + 2ull * ((uint64_t)ty * chunks + cx);
            const uint64_t base = words[entry], total = words[entry + 1];
            const uint32_t first = cx * 32, n = std::min(32u, tiles - first), entries = n * C;
            const uint64_t header = (entries + 3) / 4;
            if (base + total > words.size() || total < header) {
                fail(x.name + ": a chunk outside the frame");
                return;
            }
            uint64_t fixed = 0;
            uint32_t flagged = 0;
            for (uint32_t k = 0; k < entries; k++) {
                const uint32_t byte = (words[base + k / 4] >> (8 * (k % 4))) & 0xFF;
                b[k] = byte & 0x7F;
                flag[k] = byte >> 7;
                if (b[k] + flag[k] > 32) {
                    fail(x.name + ": a tile's bit count");
                    return;
                }
                at[k] = (uint32_t)fixed;
                fixed += 2ull * (b[k] + flag[k]);
                flagged += flag[k];
            }
            const uint64_t list = base + header + fixed, end = base + total;
            uint64_t extra = 32 * (list + (flagged + 3) / 4);  // the next stored bits above b
            if (extra > 32 * end) {
                fail(x.name + ": a tile's bits run past its chunk");
                return;
            }
            for (uint32_t k = 0, f = 0; k < entries; k++)
                if (flag[k]) {
                    e[k] = (words[list + f / 4] >> (8 * (f % 4))) & 0xFF;
                    f++;
                    if (e[k] == 0 || b[k] + e[k] > 32) {
                        fail(x.name + ": a tile's stored bit count");
                        return;
                    }
                }
            for (uint32_t k = 0; k < entries; k++) {
                const uint32_t c = k % C, tx = first + k / C, bits = b[k] + flag[k];
                const uint32_t low = b[k] == 32 ? 0xFFFFFFFFu : (1u << b[k]) - 1;
                uint32_t* plane = s.planes[t][c].data();
                const uint32_t* ref = reference[t] >= 0 ? s.planes[reference[t]][c].data() : nullptr;
                const uint64_t fields = 32 * (base + header + at[k]);
                for (uint32_t i = 0; i < 64; i++) {
                    uint32_t z = 0;
                    if (bits) {
                        const uint32_t field = bits_at(fields + (uint64_t)i * bits, bits);
                        z = field & low;
                        if (flag[k] && ((field >> b[k]) & 1)) {
                            if (extra + e[k] > 32 * end) {
                                fail(x.name + ": a tile's stored bits run past its chunk");
                                return;
                            }
                            z |= bits_at(extra, e[k]) << b[k];
                            extra += e[k];
                        }
                    }
                    const uint32_t x0 = tx * 8 + i % 8, y0 = ty * 8 + i / 8;
                    if (x0 >= w || y0 >= h)
                        continue;
                    const uint32_t residual = (z >> 1) ^ (0u - (z & 1));
                    const size_t p = (size_t)y0 * w + x0;
                    plane[p] = ref ? ref[p] + residual : residual;
                }
            }
            if ((extra + 31) / 32 != end) {
                fail(x.name + ": a chunk's length does not match its bits");
                return;
            }
        }
    };
    // Row y of texture t from its planes into its texels.
    auto pack = [&](size_t t, uint32_t y) {
        const Texture& x = r.textures[t];
        const std::vector<std::vector<uint32_t>>& v = s.planes[t];
        const size_t w = x.width, p0 = (size_t)y * w;
        uint8_t* row = bytes[t] + p0 * x.texel;
        if (x.vk == vk::A2B10G10R10_UNORM_PACK32) {
            uint32_t* o = (uint32_t*)row;
            for (size_t p = 0; p < w; p++)  // coding 3's sums keep the codes' bits only
                o[p] = (v[0][p0 + p] & 0x3FF) | (v[1][p0 + p] & 0x3FF) << 10 | (v[2][p0 + p] & 0x3FF) << 20 |
                       v[3][p0 + p] << 30;
            return;
        }
        if (x.vk == vk::B10G11R11_UFLOAT_PACK32) {  // coded as FP16: the top 11, 11 and 10 bits of each half
            uint32_t* o = (uint32_t*)row;
            auto half = [](uint32_t v) { return (v ^ ((v & 0x8000) ? 0x8000u : 0xFFFFu)) & 0xFFFF; };
            for (size_t p = 0; p < w; p++)
                o[p] = (half(v[0][p0 + p]) >> 4 & 0x7FF) | (half(v[1][p0 + p]) >> 4 & 0x7FF) << 11 |
                       (half(v[2][p0 + p]) >> 5 & 0x3FF) << 22;
            return;
        }
        size_t offset = 0;
        for (uint32_t c = 0; c < x.channels; c++) {
            const uint32_t kind = (x.kinds >> (8 * c)) & 0xFF, type = kind >> 5;
            const uint32_t size = type == 2 ? 4 : type == 1 ? 2 : (kind & 31) <= 8 ? 1 : 2;
            const uint32_t* src = v[c].data() + p0;
            for (size_t p = 0; p < w; p++) {
                uint32_t value = src[p];
                if (type == 2)  // the order-preserving integer back to float bits
                    value ^= (value & 0x80000000u) ? 0x80000000u : 0xFFFFFFFFu;
                else if (type == 1)
                    value = (value ^ ((value & 0x8000) ? 0x8000u : 0xFFFFu)) & 0xFFFF;
                std::memcpy(row + p * x.texel + offset, &value, size);
            }
            offset += size;
        }
    };
    std::vector<size_t> plain, referenced;
    for (size_t t = 0; t < count; t++)
        (reference[t] < 0 ? plain : referenced).push_back(t);
    auto read_tile_row = [&](size_t t, uint32_t ty) { r.coding >= 3 ? tile_row3(t, ty) : tile_row(t, ty); };
    for (size_t t : plain)
        pool.For((r.textures[t].height + 7) / 8, [&](size_t ty) { read_tile_row(t, (uint32_t)ty); });
    if (failed)
        return false;
    for (size_t t : plain) {
        const Texture& x = r.textures[t];
        if (r.coding >= 3) {  // per tile row: along each row, then down its columns
            pool.For((x.height + 7) / 8, [&](size_t ty) {
                const size_t y0 = ty * 8, y1 = std::min<size_t>(y0 + 8, x.height);
                for (std::vector<uint32_t>& plane : s.planes[t])
                    for (size_t y = y0; y < y1; y++) {
                        uint32_t* row = plane.data() + y * x.width;
                        for (size_t p = 1; p < x.width; p++)
                            row[p] += row[p - 1];
                        if (y > y0)
                            for (size_t p = 0; p < x.width; p++)
                                row[p] += row[p - x.width];
                    }
                for (size_t y = y0; y < y1; y++)
                    pack(t, (uint32_t)y);
            });
            continue;
        }
        for (std::vector<uint32_t>& plane : s.planes[t])  // column 0, top to bottom
            for (size_t y = 1; y < x.height; y++)
                plane[y * x.width] += plane[(y - 1) * x.width];
        pool.For(x.height, [&](size_t y) {
            for (std::vector<uint32_t>& plane : s.planes[t]) {  // along the row
                uint32_t* row = plane.data() + y * x.width;
                for (size_t p = 1; p < x.width; p++)
                    row[p] += row[p - 1];
            }
            pack(t, (uint32_t)y);
        });
    }
    for (size_t t : referenced) {
        const Texture& x = r.textures[t];
        pool.For((x.height + 7) / 8, [&](size_t ty) {
            read_tile_row(t, (uint32_t)ty);
            for (uint32_t y = (uint32_t)ty * 8; y < std::min(x.height, (uint32_t)ty * 8 + 8); y++)
                pack(t, y);
        });
        if (failed)
            return false;
    }
    return true;
}


bool Fg(const Recording& r, const Call& call, FgCall& out, std::string& error) {
    if (r.feature != "fg" || r.parameter_source != "ngx") {
        error = "no FG view of " + r.feature + " parameters from " + r.parameter_source;
        return false;
    }
    out = {};
    out.index = call.index;
    out.count = call.count;
    out.frame = call.frame;
    auto u = [&](const char* name, uint32_t fallback = 0) {
        uint32_t v = fallback;
        r.Unsigned(call, std::string("DLSSG.") + name, v);
        return v;
    };
    auto f = [&](const char* name, float fallback = 0) {
        float v = fallback;
        r.Float(call, std::string("DLSSG.") + name, v);
        return v;
    };
    auto m = [&](const char* name, float* to, bool& has) {
        if (const float* v = r.Matrix(call, std::string("DLSSG.") + name)) {
            std::memcpy(to, v, 64);
            has = true;
        }
    };
    auto rect = [&](const char* prefix, uint32_t* to) {
        const std::string p = prefix;
        to[0] = u((p + "SubrectBaseX").c_str());
        to[1] = u((p + "SubrectBaseY").c_str());
        to[2] = u((p + "SubrectWidth").c_str());
        to[3] = u((p + "SubrectHeight").c_str());
    };
    out.reset = u("Reset") != 0;
    out.depth_inverted = u("DepthInverted") != 0;
    out.hdr = u("ColorBuffersHDR") != 0;
    out.motion_jittered = u("MvecJittered") != 0;
    out.camera_motion_included = u("CameraMotionIncluded") != 0;
    out.orthographic = u("OrthoProjection") != 0;
    out.jitter[0] = f("JitterOffsetX");
    out.jitter[1] = f("JitterOffsetY");
    out.motion_scale[0] = f("MvecScaleX", 1);
    out.motion_scale[1] = f("MvecScaleY", 1);
    out.near_plane = f("CameraNear");
    out.far_plane = f("CameraFar");
    out.fov = f("CameraFOV");
    out.aspect = f("CameraAspectRatio");
    const char* axes[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        out.camera_pos[i] = f((std::string("CameraPos") + axes[i]).c_str());
        out.camera_up[i] = f((std::string("CameraUp") + axes[i]).c_str());
        out.camera_right[i] = f((std::string("CameraRight") + axes[i]).c_str());
        out.camera_fwd[i] = f((std::string("CameraFwd") + axes[i]).c_str());
    }
    m("ClipToPrevClip", out.clip_to_prev_clip, out.has_clip_to_prev_clip);
    m("PrevClipToClip", out.prev_clip_to_clip, out.has_prev_clip_to_clip);
    m("CameraViewToClip", out.view_to_clip, out.has_view_to_clip);
    m("ClipToCameraView", out.clip_to_view, out.has_clip_to_view);
    rect("Backbuffer", out.backbuffer_rect);
    rect("HUDLess", out.hudless_rect);
    rect("Depth", out.depth_rect);
    rect("MVecs", out.motion_rect);
    return true;
}

nlohmann::json ToJson(const FgCall& c) {
    auto v = [](const float* p, int n) { return std::vector<float>(p, p + n); };
    auto rect = [](const uint32_t* p) { return std::vector<uint32_t>(p, p + 4); };
    nlohmann::json j = {
        {"index", c.index}, {"count", c.count}, {"frame", c.frame}, {"reset", c.reset},
        {"depth_inverted", c.depth_inverted}, {"hdr", c.hdr}, {"motion_jittered", c.motion_jittered},
        {"camera_motion_included", c.camera_motion_included}, {"orthographic", c.orthographic},
        {"jitter", v(c.jitter, 2)}, {"motion_scale", v(c.motion_scale, 2)}, {"near", c.near_plane},
        {"far", c.far_plane}, {"fov", c.fov}, {"aspect", c.aspect}, {"camera_pos", v(c.camera_pos, 3)},
        {"camera_up", v(c.camera_up, 3)}, {"camera_right", v(c.camera_right, 3)},
        {"camera_fwd", v(c.camera_fwd, 3)}, {"backbuffer_rect", rect(c.backbuffer_rect)},
        {"hudless_rect", rect(c.hudless_rect)}, {"depth_rect", rect(c.depth_rect)},
        {"motion_rect", rect(c.motion_rect)}};
    if (c.has_clip_to_prev_clip) j["clip_to_prev_clip"] = v(c.clip_to_prev_clip, 16);
    if (c.has_prev_clip_to_clip) j["prev_clip_to_clip"] = v(c.prev_clip_to_clip, 16);
    if (c.has_view_to_clip) j["view_to_clip"] = v(c.view_to_clip, 16);
    if (c.has_clip_to_view) j["clip_to_view"] = v(c.clip_to_view, 16);
    return j;
}

bool Sr(const Recording& r, const Call& call, SrCall& out, std::string& error) {
    if ((r.feature != "sr" && r.feature != "rr") || r.parameter_source != "ngx") {
        error = "no SR view of " + r.feature + " parameters from " + r.parameter_source;
        return false;
    }
    out = {};
    out.frame = call.frame;
    const nlohmann::json create = r.meta.value("create", nlohmann::json::object());
    auto u = [&](const char* name, uint32_t fallback = 0) {
        uint32_t v = fallback;
        if (!r.Unsigned(call, name, v) && create.contains(name) && create[name].contains("u"))
            v = create[name]["u"].get<uint32_t>();
        return v;
    };
    auto f = [&](const char* name, float fallback = 0) {
        float v = fallback;
        if (!r.Float(call, name, v) && create.contains(name) && create[name].contains("f"))
            v = create[name]["f"].get<float>();
        return v;
    };
    // NGX DLSS feature flags (nvsdk_ngx_defs.h NVSDK_NGX_DLSS_Feature_Flags).
    out.create_flags = u("DLSS.Feature.Create.Flags");
    out.hdr = out.create_flags & 1;
    out.motion_low_res = out.create_flags & 2;
    out.motion_jittered = out.create_flags & 4;
    out.depth_inverted = out.create_flags & 8;
    out.sharpening = out.create_flags & 32;
    out.auto_exposure = out.create_flags & 64;
    out.quality = u("PerfQualityValue");
    static const char* const PRESETS[] = {"DLAA", "Quality", "Balanced", "Performance", "UltraPerformance",
                                          "UltraQuality"};
    static const uint32_t QUALITIES[] = {5, 2, 1, 0, 3, 4};  // NVSDK_NGX_PerfQuality_Value of each preset hint
    for (int i = 0; i < 6; i++)
        if (QUALITIES[i] == out.quality)
            out.preset = u((std::string("DLSS.Hint.Render.Preset.") + PRESETS[i]).c_str());
    out.reset = u("Reset") != 0;
    out.jitter[0] = f("Jitter.Offset.X");
    out.jitter[1] = f("Jitter.Offset.Y");
    out.motion_scale[0] = f("MV.Scale.X", 1);
    out.motion_scale[1] = f("MV.Scale.Y", 1);
    out.render_size[0] = u("DLSS.Render.Subrect.Dimensions.Width", u("Width"));
    out.render_size[1] = u("DLSS.Render.Subrect.Dimensions.Height", u("Height"));
    out.output_size[0] = u("OutWidth", r.meta.value("width", 0u));
    out.output_size[1] = u("OutHeight", r.meta.value("height", 0u));
    out.pre_exposure = f("DLSS.Pre.Exposure", 1);
    out.exposure_scale = f("DLSS.Exposure.Scale", 1);
    out.sharpness = f("Sharpness");
    out.near_plane = f("FSR.cameraNear");
    out.far_plane = f("FSR.cameraFar");
    out.fov = f("FSR.cameraFovAngleVertical");
    out.frame_time_ms = f("FrameTimeDeltaInMsec", f("FSR.frameTimeDelta"));
    out.color_base[0] = u("DLSS.Input.Color.Subrect.Base.X");
    out.color_base[1] = u("DLSS.Input.Color.Subrect.Base.Y");
    out.depth_base[0] = u("DLSS.Input.Depth.Subrect.Base.X");
    out.depth_base[1] = u("DLSS.Input.Depth.Subrect.Base.Y");
    out.motion_base[0] = u("DLSS.Input.MV.Subrect.Base.X");
    out.motion_base[1] = u("DLSS.Input.MV.Subrect.Base.Y");
    out.output_base[0] = u("DLSS.Output.Subrect.Base.X");
    out.output_base[1] = u("DLSS.Output.Subrect.Base.Y");
    out.denoise_mode = u("DLSS.Denoise.Mode");
    out.roughness_mode = u("DLSS.Roughness.Mode");
    if (const float* m = r.Matrix(call, "WorldToViewMatrix")) {
        std::memcpy(out.world_to_view, m, 64);
        out.has_world_to_view = true;
    }
    if (const float* m = r.Matrix(call, "ViewToClipMatrix")) {
        std::memcpy(out.view_to_clip, m, 64);
        out.has_view_to_clip = true;
    }
    return true;
}

nlohmann::json ToJson(const SrCall& c) {
    auto v = [](const auto* p, int n) { return std::vector<std::decay_t<decltype(*p)>>(p, p + n); };
    nlohmann::json j = {
        {"frame", c.frame}, {"reset", c.reset}, {"hdr", c.hdr}, {"depth_inverted", c.depth_inverted},
        {"motion_jittered", c.motion_jittered}, {"motion_low_res", c.motion_low_res},
        {"auto_exposure", c.auto_exposure}, {"sharpening", c.sharpening}, {"create_flags", c.create_flags},
        {"quality", c.quality}, {"preset", c.preset}, {"jitter", v(c.jitter, 2)},
        {"motion_scale", v(c.motion_scale, 2)}, {"render_size", v(c.render_size, 2)},
        {"output_size", v(c.output_size, 2)}, {"pre_exposure", c.pre_exposure},
        {"exposure_scale", c.exposure_scale}, {"sharpness", c.sharpness}, {"near", c.near_plane},
        {"far", c.far_plane}, {"fov", c.fov}, {"frame_time_ms", c.frame_time_ms},
        {"color_base", v(c.color_base, 2)}, {"depth_base", v(c.depth_base, 2)},
        {"motion_base", v(c.motion_base, 2)}, {"output_base", v(c.output_base, 2)},
        {"denoise_mode", c.denoise_mode}, {"roughness_mode", c.roughness_mode}};
    if (c.has_world_to_view) j["world_to_view"] = v(c.world_to_view, 16);
    if (c.has_view_to_clip) j["view_to_clip"] = v(c.view_to_clip, 16);
    return j;
}

}  // namespace record
