// nr-record: reads nr-record folders (FORMAT.md). Each command prints one JSON line.
//
//   nr-record info DIR                          # format, source, textures, frames, calls, gaps
//   nr-record decode DIR --frame N --out OUT    # recorded real frame N's textures as raw texels, OUT/f<N>_<role>.bin
//   nr-record verify DIR                        # decodes every frame: the folder is complete and consistent
//   nr-record params DIR [--call K]             # call K's parameters by name and, for FG, in FORMAT.md's units

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "record/reader.h"

namespace {

int Fail(const std::string& message) {
    std::fprintf(stderr, "nr-record: %s\n", message.c_str());
    return 1;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: nr-record info DIR\n"
                 "       nr-record decode DIR --frame N --out OUT\n"
                 "       nr-record verify DIR\n"
                 "       nr-record params DIR [--call K]\n");
    return 2;
}

nlohmann::json Textures(const record::Recording& rec) {
    nlohmann::json out = nlohmann::json::array();
    for (const record::Texture& t : rec.textures)
        out.push_back({{"role", t.name}, {"width", t.width}, {"height", t.height}, {"api_format", t.api_format},
                       {"vk_format", t.vk}, {"decoded_vk_format", t.upload}, {"texel_bytes", t.texel},
                       {"reference", t.reference.empty() ? nlohmann::json() : nlohmann::json(t.reference)}});
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3)
        return Usage();
    const std::string command = argv[1], dir = argv[2];
    std::map<std::string, std::string> options;
    for (int i = 3; i + 1 < argc; i += 2)
        options[argv[i]] = argv[i + 1];
    record::Recording rec;
    std::string error;
    if (!record::Open(dir, rec, error))
        return Fail(error);
    if (command == "info") {
        uint64_t dropped = 0;
        for (const record::IndexRecord& ix : rec.index)
            dropped += ix.dropped;
        nlohmann::json j = {{"format", rec.format}, {"coding", rec.coding}, {"feature", rec.feature},
                            {"source", rec.source}, {"graphics_api", rec.graphics_api},
                            {"game", rec.meta.value("game", "")}, {"time", rec.meta.value("time", "")},
                            {"width", rec.meta.value("width", 0)}, {"height", rec.meta.value("height", 0)},
                            {"status", rec.meta.value("status", "")}, {"frames", rec.index.size()},
                            {"dropped_between_frames", dropped}, {"calls", rec.calls.size()},
                            {"parameter_source", rec.parameter_source}, {"parameters", rec.parameters.size()},
                            {"matrices", rec.matrices.size()}, {"textures", Textures(rec)}};
        for (const char* key : {"multiplier", "encoding", "hdr", "mode", "recorder"})
            if (rec.meta.contains(key))
                j[key] = rec.meta[key];
        std::printf("%s\n", j.dump().c_str());
        return 0;
    }
    record::Pool pool(std::max(1u, std::thread::hardware_concurrency()));
    record::DecodeScratch scratch;
    if (command == "decode") {
        if (!options.count("--frame") || !options.count("--out"))
            return Usage();
        const long long n = std::stoll(options["--frame"]);
        if (n < 0 || (size_t)n >= rec.index.size())
            return Fail("no recorded frame " + std::to_string(n));
        std::map<std::string, std::vector<uint8_t>> textures;
        if (!record::Decode(rec, rec.index[n], pool, scratch, textures, error))
            return Fail(error);
        const std::string out = options["--out"];
        std::filesystem::create_directories(out);
        nlohmann::json files = nlohmann::json::object();
        for (const record::Texture& t : rec.textures) {
            const std::string path = out + "/f" + std::to_string(n) + "_" + t.name + ".bin";
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (!f || std::fwrite(textures[t.name].data(), 1, textures[t.name].size(), f) != textures[t.name].size())
                return Fail("cannot write " + path);
            std::fclose(f);
            files[t.name] = {{"file", path}, {"vk_format", t.upload}, {"width", t.width}, {"height", t.height},
                             {"texel_bytes", t.texel}};
        }
        std::printf("%s\n", nlohmann::json({{"frame", n}, {"real_frame", rec.index[n].frame}, {"files", files}})
                                .dump().c_str());
        return 0;
    }
    if (command == "verify") {
        const auto start = std::chrono::steady_clock::now();
        size_t decoded = 0;
        uint64_t raw = 0;
        std::map<std::string, std::vector<uint8_t>> textures;
        for (const record::IndexRecord& ix : rec.index) {
            if (!record::Decode(rec, ix, pool, scratch, textures, error))
                return Fail("frame " + std::to_string(decoded) + ": " + error);
            decoded++;
            for (const auto& [role, bytes] : textures)
                raw += bytes.size();
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%s\n", nlohmann::json({{"frames", decoded}, {"texel_bytes", raw}, {"seconds", s},
                                            {"status", rec.meta.value("status", "")}}).dump().c_str());
        return 0;
    }
    if (command == "params") {
        const long long k = options.count("--call") ? std::stoll(options["--call"]) : 0;
        if (k < 0 || (size_t)k >= rec.calls.size())
            return Fail("no call " + std::to_string(k));
        const record::Call& c = rec.calls[k];
        nlohmann::json values = nlohmann::json::object();
        for (size_t i = 0; i < rec.parameters.size(); i++) {
            nlohmann::json v = nlohmann::json::object();
            uint32_t u = 0;
            float f = 0;
            if (rec.Unsigned(c, rec.parameters[i], u))
                v["u"] = u;
            if (rec.Float(c, rec.parameters[i], f))
                v["f"] = f;
            if (!v.empty())
                values[rec.parameters[i]] = v;
        }
        for (const std::string& name : rec.matrices)
            if (const float* m = rec.Matrix(c, name))
                values[name] = std::vector<float>(m, m + 16);
        nlohmann::json j = {{"call", k}, {"index", c.index}, {"count", c.count}, {"frame", c.frame},
                            {"values", values}};
        record::FgCall fg;
        std::string why;
        if (record::Fg(rec, c, fg, why))
            j["fg"] = record::ToJson(fg);
        std::printf("%s\n", j.dump().c_str());
        return 0;
    }
    return Usage();
}
