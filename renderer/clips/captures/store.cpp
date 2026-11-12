#include "renderer/clips/captures/store.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>
#include "renderer/clips/captures/recording.h"
#include "renderer/paths.h"

namespace renderer::clips::captures {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

uint64_t Fnv(uint64_t hash, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Hex(uint64_t v) {
    char text[20];
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)v);
    return text;
}

std::string Lower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

bool Exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string Home() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = std::getenv("USERPROFILE");
#endif
    return home ? home : "";
}

std::string Sanitize(const std::string& s) {
    std::string out;
    for (char c : s)
        out += (std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') ? c : '_';
    return out;
}

}  // namespace

std::string Hash64(const std::string& text) {
    return Hex(Fnv(14695981039346656037ull, text.data(), text.size()));
}

std::string HashFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "";
    std::error_code ec;
    const uint64_t size = (uint64_t)fs::file_size(path, ec);
    uint64_t hash = Fnv(14695981039346656037ull, &size, sizeof(size));
    std::vector<char> buffer(1 << 20);
    auto feed = [&](uint64_t bytes) {
        while (bytes > 0 && in) {
            in.read(buffer.data(), (std::streamsize)std::min<uint64_t>(bytes, buffer.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0)
                break;
            hash = Fnv(hash, buffer.data(), (size_t)got);
            bytes -= (uint64_t)got;
        }
    };
    constexpr uint64_t WHOLE = 256ull << 20, EDGE = 1ull << 20;
    if (size <= WHOLE) {
        feed(size);
    } else {
        feed(EDGE);
        in.seekg((std::streamoff)(size - EDGE));
        feed(EDGE);
    }
    return Hex(hash);
}

bool GamesRunning() {
#ifdef __linux__
    static const char* NAMES[] = {"tlou-ii.exe", "hogwartslegacy.exe"};
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator("/proc", ec)) {
        const std::string pid = entry.path().filename().string();
        if (pid.empty() || !std::isdigit((unsigned char)pid[0]))
            continue;
        std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
        std::string argv0;
        std::getline(cmdline, argv0, '\0');
        std::ifstream comm_file(entry.path() / "comm");
        std::string comm;
        std::getline(comm_file, comm);
        std::error_code link_error;
        const std::string exe = fs::read_symlink(entry.path() / "exe", link_error).string();
        if (argv0.empty() && comm.empty())
            continue;
        std::string normal = argv0;
        std::replace(normal.begin(), normal.end(), '\\', '/');
        const size_t slash = normal.find_last_of('/');
        const std::string name = Lower(slash == std::string::npos ? normal : normal.substr(slash + 1));
        const std::string lcomm = Lower(comm), paths = Lower(argv0 + "\n" + exe);
        for (const char* game : NAMES)
            if (name == game || lcomm == game || (lcomm.size() >= 15 && std::strncmp(game, lcomm.c_str(), lcomm.size()) == 0))
                return true;
        if (paths.find("steamapps/common/") != std::string::npos || paths.find("steamapps\\common\\") != std::string::npos)
            return true;
    }
#endif
    return false;
}

// --- Folders ----------------------------------------------------------------------------------------------------------

Locations DefaultLocations(const std::string& workspace) {
    Locations where;
    where.workspace = workspace;
    where.browser = paths::Captures();
    where.frames = paths::CaptureFrames();
    where.cache = paths::State() + "/captures";
    return where;
}

std::string RecordingsFolder() {
#ifdef _WIN32
    const char* local = std::getenv("LOCALAPPDATA");
    return local ? std::string(local) + "/nr-record" : std::string();
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data)
        return std::string(data) + "/nr-record";
    const std::string home = Home();
    return home.empty() ? std::string() : home + "/.local/share/nr-record";
#endif
}

std::vector<std::string> DefaultCaptureFolders() {
    std::vector<std::string> folders;
    if (const std::string recordings = RecordingsFolder(); !recordings.empty())
        folders.push_back(recordings);
    for (const std::string& folder : paths::CaptureFolders())
        folders.push_back(folder);
    return folders;
}

State LoadState(const Locations& where) {
    State state;
    std::ifstream in(where.StateFile());
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return state;
    for (const char* key : {"capture_folders", "export_folders"}) {
        std::vector<std::string>& list = key[0] == 'c' ? state.capture_folders : state.export_folders;
        if (j.contains(key) && j[key].is_array())
            for (const json& v : j[key])
                if (v.is_string())
                    list.push_back(v.get<std::string>());
    }
    if (j.contains("dlls") && j["dlls"].is_array())
        for (const json& v : j["dlls"])
            if (v.is_object() && v.value("path", "") != "")
                state.dlls.push_back({v.value("kind", "provider"), v.value("path", "")});
    return state;
}

bool SaveState(const Locations& where, const State& state) {
    std::error_code ec;
    fs::create_directories(fs::path(where.StateFile()).parent_path(), ec);
    const std::string temporary = where.StateFile() + ".part";
    {
        json dlls = json::array();
        for (const State::Dll& d : state.dlls)
            dlls.push_back({{"kind", d.kind}, {"path", d.path}});
        std::ofstream out(temporary);
        out << json({{"capture_folders", state.capture_folders}, {"export_folders", state.export_folders}, {"dlls", dlls}}).dump(1)
            << "\n";
        if (!out)
            return false;
    }
    fs::rename(temporary, where.StateFile(), ec);
    return !ec;
}

// --- Folder formats -------------------------------------------------------------------------------------------------------

namespace {

std::vector<FolderFormat>& Formats() {
    static std::vector<FolderFormat> formats;
    return formats;
}

bool IsCapture(const fs::path& folder) {
    if (IsRecording(folder.string()))
        return true;
    for (const FolderFormat& f : Formats())
        if (Exists((folder / f.marker).string()))
            return true;
    return false;
}

}  // namespace

void AddFolderFormat(FolderFormat format) {
    Formats().push_back(std::move(format));
}

const std::vector<FolderFormat>& FolderFormats() {
    return Formats();
}

const FolderFormat* FormatOf(const CaptureInfo& capture) {
    for (const FolderFormat& f : Formats())
        if (f.name == capture.format)
            return &f;
    return nullptr;
}

std::string JobSpec::Title() const {
    std::string what = all ? "every pair" : pairs.size() == 1 ? pairs[0].Stem() : std::to_string(pairs.size()) + " pairs";
    return generator.label + ": " + what;
}

bool CanMake(const Generator& g, const PairSpec& pair) {
    return !g.replay || pair.kind == PairKind::REAL;
}

int JobFrameCount(const JobSpec& spec, const CaptureInfo& capture) {
    if (!spec.all)
        return (int)spec.pairs.size();
    int n = 0;
    for (const PairSpec& p : EnumeratePairs(capture))
        n += CanMake(spec.generator, p) ? 1 : 0;
    return n;
}

std::vector<std::string> ReplayCommand(const std::string& replay, const JobSpec& spec, const std::string& result_dir) {
    std::vector<std::string> args;
    if (replay.size() > 3 && replay.compare(replay.size() - 3, 3, ".py") == 0)
        args = {"python3", "-I"};  // the Linux runner (standard library only)
    args.insert(args.end(), {replay, "--record", spec.capture_folder, "--generator",
                             spec.generator.kind == Generator::DLSSG ? "nvidia" : "provider", "--dll", spec.generator.dll,
                             "--out", result_dir, "--raw"});
    if (!spec.all && !spec.pairs.empty()) {
        // Real frames a to b: the replay resets at its first frame, and frame b's calls make the frames between a and b
        int a = spec.pairs[0].a, b = spec.pairs[0].b;
        for (const PairSpec& p : spec.pairs) {
            a = std::min(a, p.a);
            b = std::max(b, p.b);
        }
        args.insert(args.end(), {"--frames", std::to_string(a) + ":" + std::to_string(b + 1)});
    }
    return args;
}

std::vector<std::string> JobCommand(const Locations& where, const JobSpec& spec, const std::string& result_dir) {
    if (spec.generator.replay) {
        const std::string replay = ReplayTool(where);
        return replay.empty() ? std::vector<std::string>() : ReplayCommand(replay, spec, result_dir);
    }
    for (const FolderFormat& f : FolderFormats())
        if (f.command) {
            std::vector<std::string> command = f.command(where, spec, result_dir);
            if (!command.empty())
                return command;
        }
    return {};
}

bool LoadCapture(const std::string& folder, CaptureInfo& capture, std::string& error) {
    if (IsRecording(folder))
        return LoadRecording(folder, capture, error);
    for (const FolderFormat& f : Formats())
        if (Exists(folder + "/" + f.marker)) {
            capture = CaptureInfo();
            capture.folder = folder;
            capture.name = fs::path(folder).filename().string();
            if (capture.name.empty())
                capture.name = fs::path(folder).parent_path().filename().string();
            capture.format = f.name;
            return f.load(folder, capture, error);
        }
    error = folder + " is not a capture (no record.json)";
    return false;
}

std::vector<Entry> ScanCaptures(const std::vector<std::string>& roots) {
    std::vector<Entry> entries;
    std::set<std::string> seen;
    auto add = [&](const fs::path& folder, const std::string& root) {
        std::error_code ec;
        const std::string canonical = fs::weakly_canonical(folder, ec).string();
        if (!seen.insert(canonical).second)
            return;
        Entry entry;
        std::string error;
        if (!LoadCapture(folder.string(), entry.info, error))
            return;
        entry.source = root;
        for (const fs::directory_entry& f : fs::directory_iterator(folder, ec))
            if (f.path().extension() == ".bin") {
                std::error_code size_error;
                entry.bytes += (uint64_t)f.file_size(size_error);
            }
        entries.push_back(std::move(entry));
    };
    for (const std::string& root : roots) {
        std::error_code ec;
        if (root.empty() || !fs::is_directory(root, ec))
            continue;
        if (IsCapture(root)) {
            add(root, root);
            continue;
        }
        for (const fs::directory_entry& child : fs::directory_iterator(root, ec))
            if (child.is_directory(ec) && IsCapture(child.path()))
                add(child.path(), root);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.info.epoch != b.info.epoch ? a.info.epoch > b.info.epoch : a.info.name > b.info.name;
    });
    return entries;
}

SteamFiles ScanSteam() {
    std::vector<std::string> libraries;
    const std::string home = Home();
    for (const std::string& steam : {home + "/.local/share/Steam", home + "/.steam/steam", home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam"}) {
        if (!Exists(steam + "/steamapps"))
            continue;
        libraries.push_back(steam);
        std::ifstream vdf(steam + "/steamapps/libraryfolders.vdf");
        for (std::string line; std::getline(vdf, line);) {
            const size_t key = line.find("\"path\"");
            if (key == std::string::npos)
                continue;
            const size_t open = line.find('"', key + 6), close = open == std::string::npos ? open : line.find('"', open + 1);
            if (close != std::string::npos)
                libraries.push_back(line.substr(open + 1, close - open - 1));
        }
    }
    SteamFiles found;
    std::set<std::string> seen;
    auto once = [&](const fs::path& path) {
        std::error_code ec;
        return seen.insert(fs::weakly_canonical(path, ec).string()).second;
    };
    for (const std::string& library : libraries) {
        std::error_code ec;
        const fs::path common = fs::path(library) / "steamapps" / "common";
        if (!fs::is_directory(common, ec))
            continue;
        for (const fs::directory_entry& game : fs::directory_iterator(common, ec)) {
            const std::string name = game.path().filename().string();
            // OptiScaler sits beside the game's executable: the game's folder, or deeper (Unreal Engine games:
            // <game>/<project>/Binaries/Win64, Hogwarts Legacy's Phoenix/Binaries/Win64). Folders up to 4 levels down,
            // without descending into an OptiScaler folder; each provider folder under its custom folder.
            std::vector<std::pair<fs::path, int>> stack = {{game.path(), 0}};
            while (!stack.empty()) {
                const auto [dir, depth] = stack.back();
                stack.pop_back();
                std::error_code walk;
                for (const fs::directory_entry& entry : fs::directory_iterator(dir, walk)) {
                    if (entry.is_symlink(walk))
                        continue;
                    if (!entry.is_directory(walk)) {
                        if (Lower(entry.path().filename().string()) == "nvngx_dlssg.dll" && once(entry.path())) {
                            found.dlls.push_back({"nvidia", entry.path().string()});
                            found.dll_games.push_back(name);
                        }
                        continue;
                    }
                    if (entry.path().filename() == "OptiScaler") {
                        for (const fs::directory_entry& provider : fs::directory_iterator(entry.path() / "custom", walk)) {
                            const fs::path captures = provider.path() / "dlssg" / "captures";
                            if (fs::is_directory(captures, ec) && once(captures))
                                found.capture_folders.push_back(captures.string());
                            const fs::path dll = provider.path() / "dlssg" / "nvngx_dlssg.dll";
                            if (fs::is_regular_file(dll, ec) && once(dll)) {
                                found.dlls.push_back({"provider", dll.string()});
                                found.dll_games.push_back(name);
                            }
                        }
                    } else if (depth + 1 < 4) {
                        stack.push_back({entry.path(), depth + 1});
                    }
                }
            }
        }
    }
    return found;
}

// --- Generators -------------------------------------------------------------------------------------------------------

std::string Generator::Dir() const {
    return replay ? Sanitize(id) : Sanitize(id) + "-" + Sanitize(version);  // a replay generator's id holds its version
}

std::string ReplayTool(const Locations& where) {
    if (const char* given = std::getenv("NR_REPLAY"); given && *given)
        return given;
    std::vector<std::string> candidates;
    const fs::path beside = fs::path(where.executable).parent_path();
#ifdef _WIN32
    const char* names[] = {"nr-replay.exe"};
    const char* built[] = {"bazel-bin/record/replay/nr-replay.exe"};
#else
    // Linux: the runner that starts the Windows executable under Wine (record/replay/README.md "Linux")
    const char* names[] = {"nr_replay_wine.py"};
    const char* built[] = {"record/replay/nr_replay_wine.py"};
#endif
    if (!where.executable.empty())
        for (const char* name : names)
            candidates.push_back((beside / name).string());
    if (!where.workspace.empty())
        for (const char* path : built)
            candidates.push_back(where.workspace + "/" + path);
    for (const std::string& c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec))
            return c;
    }
    return "";
}

std::string DllVersion(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::error_code ec;
    const uint64_t size = (uint64_t)fs::file_size(path, ec);
    if (!in || ec)
        return "";
    constexpr uint64_t EDGE = 64 << 10;
    std::vector<char> buffer((size_t)std::min(size, EDGE));
    uint64_t hash = Fnv(14695981039346656037ull, &size, sizeof(size));
    in.read(buffer.data(), (std::streamsize)buffer.size());
    hash = Fnv(hash, buffer.data(), (size_t)in.gcount());
    if (size > EDGE) {
        in.clear();
        in.seekg((std::streamoff)(size - std::min(size, EDGE)));
        in.read(buffer.data(), (std::streamsize)buffer.size());
        hash = Fnv(hash, buffer.data(), (size_t)in.gcount());
    }
    return Hex(hash).substr(0, 12);
}

namespace {

// "Apex Neural Rendering FG" from ".../OptiScaler/custom/Apex Neural Rendering FG/dlssg/nvngx_dlssg.dll", else the
// DLL's folder.
std::string ProviderName(const std::string& dll) {
    const fs::path path(dll);
    for (fs::path p = path.parent_path(); !p.empty() && p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
        if (p.parent_path().filename() == "custom")
            return p.filename().string();
    return path.parent_path().filename().string();
}

}  // namespace

std::vector<Generator> ReplayGenerators(const Locations& where, const State& state, const SteamFiles& steam,
                                        const std::string& device, bool nvidia) {
    std::vector<Generator> list;
    const std::string tool = ReplayTool(where);
    std::set<std::string> ids;
    auto add = [&](const State::Dll& d, const std::string& found_in) {
        Generator g;
        g.replay = true;
        g.dll = d.path;
        g.kind = d.kind == "nvidia" ? Generator::DLSSG : Generator::APEX;
        g.version = DllVersion(d.path);
        g.id = d.kind + ":" + (g.version.empty() ? Hash64(d.path).substr(0, 12) : g.version);
        if (!ids.insert(g.id).second)
            return;
        g.label = (d.kind == "nvidia" ? std::string("DLSS-G") : ProviderName(d.path)) + " (" + found_in + ")";
        g.detail = d.kind == "nvidia" ? "NVIDIA's DLSS-G from this DLL, through NVIDIA's NGX (nr-replay --generator nvidia)"
                                      : "a DLSS-G provider DLL, called as a game calls it (nr-replay --generator provider)";
        if (g.version.empty()) {
            g.available = false;
            g.unavailable = "the DLL is not here: " + d.path;
        } else if (tool.empty()) {
            g.available = false;
            g.unavailable = "nr-replay is not found: NR_REPLAY names it, or put it beside the renderer";
        } else if (d.kind == "nvidia" && !nvidia) {
            g.available = false;
            g.unavailable = "NVIDIA's DLSS-G needs an NVIDIA GPU (this one is " + (device.empty() ? std::string("not NVIDIA") : device) + ")";
        }
        list.push_back(g);
    };
    for (const State::Dll& d : state.dlls)
        add(d, "added");
    for (size_t i = 0; i < steam.dlls.size(); i++)
        add(steam.dlls[i], i < steam.dll_games.size() ? steam.dll_games[i] : "Steam");
    // ANR FG left of the divider, DLSS-G right: providers first
    std::stable_sort(list.begin(), list.end(), [](const Generator& a, const Generator& b) { return a.kind > b.kind; });
    return list;
}

std::vector<Generator> ListGenerators(const Locations& where, const State& state, const SteamFiles& steam,
                                      const CaptureInfo& capture, const std::string& device, bool nvidia) {
    if (capture.format == RECORDING_FORMAT)
        return ReplayGenerators(where, state, steam, device, nvidia);
    const FolderFormat* format = FormatOf(capture);
    return format && format->generators ? format->generators(where, state, device, nvidia) : std::vector<Generator>();
}

std::string ToolRevision(const Locations& where, const Generator& g) {
    static std::mutex mu;
    static std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> cache;
    const auto now = std::chrono::steady_clock::now();
    const std::string key = where.workspace + "|" + (g.replay ? std::string("replay") : g.id);
    {
        std::lock_guard<std::mutex> lock(mu);
        auto found = cache.find(key);
        if (found != cache.end() && now - found->second.second < std::chrono::seconds(5))
            return found->second.first;
    }
    std::string revision;
    if (g.replay) {
        // The runner and the executable it starts: beside it, else the workspace's Windows build
        const std::string tool = ReplayTool(where);
        std::string text = HashFile(tool);
        if (fs::path(tool).filename() != "nr-replay.exe")
            for (const fs::path& exe : {fs::path(tool).parent_path() / "nr-replay.exe",
                                        fs::path(where.workspace) / "bazel-windows-bin/record/replay/nr-replay.exe"}) {
                std::error_code ec;
                if (fs::is_regular_file(exe, ec)) {
                    text += HashFile(exe.string());
                    break;
                }
            }
        revision = Hash64(text).substr(0, 8);
    } else {
        for (const FolderFormat& f : Formats())
            if (f.revision && revision.empty())
                revision = f.revision(where, g);
    }
    std::lock_guard<std::mutex> lock(mu);
    cache[key] = {revision, now};
    return revision;
}

std::string ResultDir(const Locations& where, const std::string& capture, const Generator& g) {
    return where.Results(capture) + "/" + g.Dir() + "-" + ToolRevision(where, g);
}

std::vector<Cached> CachedFrames(const Locations& where, const std::string& capture, const Generator& g) {
    std::vector<Cached> frames;
    std::set<std::string> have;
    const std::string current = fs::path(ResultDir(where, capture, g)).filename().string();
    std::error_code ec;
    std::vector<fs::path> folders;
    for (const fs::directory_entry& d : fs::directory_iterator(where.Results(capture), ec))
        if (d.is_directory(ec) && d.path().filename().string().rfind(g.Dir() + "-", 0) == 0)
            folders.push_back(d.path());
    // The current folder first, so its frames win over an older revision's of the same stem
    std::sort(folders.begin(), folders.end(), [&](const fs::path& a, const fs::path& b) {
        return (a.filename() == current) > (b.filename() == current);
    });
    for (const fs::path& folder : folders) {
        const bool stale = folder.filename() != current;
        for (const fs::directory_entry& f : fs::directory_iterator(folder, ec)) {
            if (f.path().extension() != ".nrraw")
                continue;
            std::string stem = f.path().stem().string();
            unsigned a = 0, b = 0;
            double t = 0.0;
            if (stem.rfind("generated_", 0) == 0) {  // nr-replay's name of a generated frame
                if (std::sscanf(stem.c_str(), "generated_f%u-f%u_t%lf", &a, &b, &t) != 3)
                    continue;
                PairSpec pair;
                pair.a = (int)a;
                pair.b = (int)b;
                pair.t = t;
                stem = pair.Stem();
            } else if (stem.rfind("real_", 0) == 0) {  // nr-replay's copy of a real frame
                continue;
            }
            if (have.insert(stem).second)
                frames.push_back({stem, f.path().string(), stale, folder.filename().string()});
        }
    }
    return frames;
}

}  // namespace renderer::clips::captures
