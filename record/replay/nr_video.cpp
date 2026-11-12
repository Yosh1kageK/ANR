// nr-video (README.md): SDR videos, side-by-side videos, PNG images and compare projects from nr-replay's outputs.
// A source is a replay folder with raw frames (nr-replay --raw) or a frame stream (nr-replay --stream: a FIFO, a file,
// or - for standard input). ffmpeg encodes.
//
//   nr-video video SOURCE --out FILE.mp4 [--fps F] [--codec auto|hevc_nvenc|libx265] [--qp N] [--ffmpeg PATH]
//   nr-video side-by-side SOURCE_A SOURCE_B --out FILE.mp4 [--video-a FILE] [--video-b FILE] [as video]
//   nr-video png SOURCE --frames N,N,... --out DIR [--ffmpeg PATH]
//   nr-video project VIDEO_A VIDEO_B --out FILE.compare.json [--labels A B] [--title TEXT]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "record/pool.h"
#include "record/replay/frames.h"

namespace {

namespace fs = std::filesystem;
using replay::Image;

constexpr const char* USAGE =
    "usage: nr-video video SOURCE --out FILE.mp4 [--fps F] [--codec auto|hevc_nvenc|libx265] [--qp N]\n"
    "                 [--ffmpeg PATH]\n"
    "       nr-video side-by-side SOURCE_A SOURCE_B --out FILE.mp4 [--video-a FILE] [--video-b FILE] [as video]\n"
    "       nr-video png SOURCE --frames N,N,... --out DIR [--ffmpeg PATH]\n"
    "       nr-video project VIDEO_A VIDEO_B --out FILE.compare.json [--labels A B] [--title TEXT]\n"
    "SOURCE: a replay folder with raw frames (nr-replay --raw), or a frame stream (nr-replay --stream: a FIFO,\n"
    "a file, or - for standard input)\n";

int Fail(const std::string& message) {
    std::fprintf(stderr, "nr-video: %s\n", message.c_str());
    return 1;
}

// A child process with its standard input as a pipe (or none), and its exit code.
class Child {
public:
    ~Child() { Wait(); }
    bool Start(const std::vector<std::string>& argv, bool pipe_input, bool quiet, std::string& error) {
#ifdef _WIN32
        std::wstring line;
        for (const std::string& a : argv) {  // CommandLineToArgvW's quoting
            std::string q = "\"";
            size_t slashes = 0;
            for (char c : a) {
                if (c == '\\') {
                    slashes++;
                    continue;
                }
                q.append(c == '"' ? 2 * slashes + 1 : slashes, '\\');
                slashes = 0;
                q += c;
            }
            q.append(2 * slashes, '\\');
            q += "\"";
            const int n = MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, nullptr, 0);
            std::wstring w(n - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, w.data(), n);
            line += (line.empty() ? L"" : L" ") + w;
        }
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        HANDLE null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe_input) {
            if (!CreatePipe(&read, &write, &sa, 1 << 20)) {
                error = "cannot make a pipe";
                return false;
            }
            SetHandleInformation(write, HANDLE_FLAG_INHERIT, 0);
        }
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = pipe_input ? read : null;
        si.hStdOutput = quiet ? null : GetStdHandle(STD_ERROR_HANDLE);
        si.hStdError = quiet ? null : GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi = {};
        const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        if (read)
            CloseHandle(read);
        CloseHandle(null);
        if (!ok) {
            if (write)
                CloseHandle(write);
            error = "cannot start " + argv[0];
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        if (pipe_input)
            input_ = _fdopen(_open_osfhandle((intptr_t)write, 0), "wb");
#else
        int fds[2] = {-1, -1};
        // close-on-exec: an encoder started later must not hold this pipe open, or this one never sees its end
        if (pipe_input && pipe2(fds, O_CLOEXEC)) {
            error = "cannot make a pipe";
            return false;
        }
        pid_ = fork();
        if (pid_ == 0) {
            if (pipe_input) {
                dup2(fds[0], 0);
                close(fds[0]);
                close(fds[1]);
            } else {
                const int in = open("/dev/null", O_RDONLY);
                dup2(in, 0);
            }
            const int out = quiet ? open("/dev/null", O_WRONLY) : 2;
            dup2(out, 1);
            dup2(out, 2);
            std::vector<char*> args;
            for (const std::string& a : argv)
                args.push_back((char*)a.c_str());
            args.push_back(nullptr);
            execvp(args[0], args.data());
            _exit(127);
        }
        if (pipe_input)
            close(fds[0]);
        if (pid_ < 0) {
            error = "cannot start " + argv[0];
            return false;
        }
        if (pipe_input)
            input_ = fdopen(fds[1], "wb");
#endif
        if (input_)
            std::setvbuf(input_, nullptr, _IOFBF, 4 << 20);
        return true;
    }
    std::FILE* input() { return input_; }
    // Closes the input and waits; the exit code (-1 if it did not start).
    int Wait() {
        if (input_)
            std::fclose(input_);
        input_ = nullptr;
#ifdef _WIN32
        if (!process_)
            return code_;
        WaitForSingleObject(process_, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        CloseHandle(process_);
        process_ = nullptr;
        code_ = (int)code;
#else
        if (pid_ <= 0)
            return code_;
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
        code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
        return code_;
    }

private:
    std::FILE* input_ = nullptr;
    int code_ = -1;
#ifdef _WIN32
    HANDLE process_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

// A replay's frames in order: from its folder's raw frames, or from its stream.
class Source {
public:
    bool Open(const std::string& path, std::string& error) {
        path_ = path;
        if (path != "-" && fs::is_directory(path)) {
            folder_ = true;
            std::ifstream in(fs::path(path) / "replay.json");
            header_ = nlohmann::json::parse(in, nullptr, false);
            if (!header_.is_object() || !header_.contains("frames")) {
                error = path + ": no replay.json";
                return false;
            }
            for (const auto& f : header_["frames"])
                if (f.value("file", nlohmann::json()).is_string())
                    entries_.push_back(f);
            if (entries_.empty()) {
                error = path + ": the replay wrote no raw frames (nr-replay --raw)";
                return false;
            }
            return true;
        }
        if (!stream_.Open(path, error))
            return false;
        header_ = stream_.header();
        return true;
    }
    const nlohmann::json& header() const { return header_; }
    std::string Label() const {
        return header_.contains("generator") ? header_["generator"].value("label", path_) : path_;
    }
    // The next frame and its entry (kind, real, a, b, index, t); false at the end (error empty) or on an error.
    bool Next(Image& image, nlohmann::json& entry, std::string& error) {
        if (folder_) {
            if (at_ >= entries_.size())
                return false;
            entry = entries_[at_++];
            return replay::ReadRaw((fs::path(path_) / entry["file"].get<std::string>()).string(), image, error);
        }
        replay::StreamFrame f;
        if (!stream_.Next(f, image, error))
            return false;
        const char* kind = f.kind == 2 ? "output" : f.kind ? "generated" : "real";
        entry = {{"kind", kind}, {"real", f.b}, {"a", f.a}, {"b", f.b}, {"index", f.index}, {"t", (double)f.t},
                 {"file", nullptr}};
        return true;
    }
    std::string Stem(const nlohmann::json& e) const {
        return replay::FrameStem(e["kind"], e["a"], e["b"], e["t"]);
    }

private:
    std::string path_;
    bool folder_ = false;
    nlohmann::json header_;
    std::vector<nlohmann::json> entries_;
    size_t at_ = 0;
    replay::StreamReader stream_;
};

struct Encoding {
    std::string ffmpeg = "ffmpeg", codec = "auto";
    int qp = 16;
    double fps = 0;
};

// hevc_nvenc where ffmpeg has it and a GPU encodes with it, else libx265.
std::string ChooseCodec(const Encoding& e) {
    if (e.codec != "auto")
        return e.codec;
    Child probe;
    std::string error;
    if (probe.Start({e.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                     "color=size=256x256:rate=1", "-frames:v", "1", "-c:v", "hevc_nvenc", "-profile:v", "main10",
                     "-pix_fmt", "p010le", "-f", "null", "-"},
                    false, true, error) &&
        probe.Wait() == 0)
        return "hevc_nvenc";
    return "libx265";
}

// One video's encoder: P010 frames into ffmpeg's standard input.
class Encoder {
public:
    bool Open(const Encoding& e, const std::string& codec, uint32_t w, uint32_t h, const std::string& out,
              const std::string& title, std::string& error) {
        const std::vector<std::string> colors = {"-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace",
                                                 "bt709", "-color_range", "tv"};
        char rate[32];
        std::snprintf(rate, sizeof(rate), "%.6g", e.fps);
        std::vector<std::string> argv = {e.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
                                         "-pix_fmt", "p010le", "-s", std::to_string(w) + "x" + std::to_string(h),
                                         "-r", rate};
        argv.insert(argv.end(), colors.begin(), colors.end());
        argv.insert(argv.end(), {"-i", "-"});
        if (codec == "hevc_nvenc")
            argv.insert(argv.end(), {"-c:v", "hevc_nvenc", "-profile:v", "main10", "-preset", "p5", "-tune", "hq",
                                     "-rc", "constqp", "-qp", std::to_string(e.qp), "-pix_fmt", "p010le"});
        else
            argv.insert(argv.end(), {"-c:v", codec, "-preset", "medium", "-crf", std::to_string(e.qp), "-pix_fmt",
                                     "yuv420p10le", "-x265-params", "log-level=error"});
        argv.insert(argv.end(), colors.begin(), colors.end());
        argv.insert(argv.end(), {"-tag:v", "hvc1", "-movflags", "+faststart", "-metadata", "title=" + title, out});
        out_ = out;
        return child_.Start(argv, true, false, error);
    }
    bool Write(const std::vector<uint16_t>& p010) {
        frames_++;
        return std::fwrite(p010.data(), 2, p010.size(), child_.input()) == p010.size();
    }
    bool Close(std::string& error) {
        const int code = child_.Wait();
        if (code != 0) {
            error = "ffmpeg failed (exit " + std::to_string(code) + ") writing " + out_;
            return false;
        }
        return true;
    }
    uint64_t frames() const { return frames_; }

private:
    Child child_;
    std::string out_;
    uint64_t frames_ = 0;
};

// Images through the SDR curve into P010 (each image's even part, side by side); rows in parallel.
class Converter {
public:
    explicit Converter(record::Pool& pool) : pool_(pool) {}
    void Convert(const std::vector<const Image*>& images, const std::vector<const replay::SdrCurve*>& curves,
                 uint32_t height, std::vector<uint16_t>& p010) {
        uint32_t width = 0;
        for (const Image* i : images)
            width += i->width & ~1u;
        height &= ~1u;
        p010.resize((size_t)width * height * 3 / 2);
        uint16_t* y = p010.data();
        uint16_t* uv = p010.data() + (size_t)width * height;
        pool_.For(height / 2, [&](size_t pair) {
            thread_local std::vector<float> rows[2];
            for (auto& r : rows)
                r.resize((size_t)3 * width);
            for (int r = 0; r < 2; r++) {
                const size_t row = 2 * pair + r;
                size_t x = 0;
                for (size_t i = 0; i < images.size(); i++) {
                    const Image& im = *images[i];
                    curves[i]->Row(im.texels.data() + row * im.width * im.format.texel, im.width & ~1u,
                                   rows[r].data() + 3 * x);
                    x += im.width & ~1u;
                }
            }
            replay::ToP010(rows[0].data(), rows[1].data(), width, y + 2 * pair * width, y + (2 * pair + 1) * width,
                           uv + pair * width);
        });
    }

private:
    record::Pool& pool_;
};

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> options;
    std::string Get(const std::string& k, const std::string& fallback = "") const {
        auto it = options.find(k);
        return it == options.end() || it->second.empty() ? fallback : it->second[0];
    }
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 2; i < argc; i++) {
        const std::string k = argv[i];
        if (k.rfind("--", 0) == 0 && k.size() > 2) {
            const int n = k == "--labels" ? 2 : 1;
            auto& values = a.options[k];
            for (int j = 0; j < n && i + 1 < argc; j++)
                values.push_back(argv[++i]);
        } else {
            a.positional.push_back(k);
        }
    }
    return a;
}

Encoding EncodingOf(const Args& a, const nlohmann::json& header) {
    Encoding e;
    e.ffmpeg = a.Get("--ffmpeg", "ffmpeg");
    e.codec = a.Get("--codec", "auto");
    e.qp = std::stoi(a.Get("--qp", "16"));
    e.fps = std::stod(a.Get("--fps", "0"));
    if (e.fps <= 0)
        e.fps = header.value("fps", 0.0);
    if (e.fps <= 0)
        e.fps = 60;
    return e;
}

// The sidecar nr-video writes next to each video, which `project` reads.
void WriteSidecar(const std::string& video, const nlohmann::json& info) {
    std::ofstream(video + ".json") << info.dump(1) << "\n";
}

nlohmann::json SourceInfo(const Source& s) {
    nlohmann::json h = s.header(), out = nlohmann::json::object();
    for (const char* k : {"recording", "game", "generator", "gpu", "encoding", "hdr", "multiplier", "mode", "fps"})
        if (h.contains(k))
            out[k] = h[k];
    return out;
}

int Videos(const std::string& command, const Args& a) {
    const bool pair = command == "side-by-side";
    const std::string out = a.Get("--out");
    if (a.positional.size() != (pair ? 2u : 1u) || out.empty()) {
        std::fprintf(stderr, "%s", USAGE);
        return 2;
    }
    std::vector<std::unique_ptr<Source>> sources;
    std::string error;
    for (const std::string& p : a.positional) {
        sources.push_back(std::make_unique<Source>());
        if (!sources.back()->Open(p, error))
            return Fail(error);
    }
    const nlohmann::json& h0 = sources[0]->header();
    const Encoding enc = EncodingOf(a, h0);
    const std::string codec = ChooseCodec(enc);
    record::Pool pool(std::max(2u, std::thread::hardware_concurrency()));
    Converter converter(pool);

    std::vector<Image> images(sources.size());
    std::vector<std::unique_ptr<replay::SdrCurve>> curves(sources.size());
    std::vector<nlohmann::json> entries(sources.size());
    // The first frames give the sizes and formats.
    for (size_t i = 0; i < sources.size(); i++) {
        if (!sources[i]->Next(images[i], entries[i], error))
            return Fail(a.positional[i] + ": " + (error.empty() ? "no frames" : error));
        curves[i] = std::make_unique<replay::SdrCurve>(images[i].format, sources[i]->header().value("hdr", false));
    }
    if (pair && images[0].height != images[1].height)
        return Fail("side by side needs two replays of one height");
    const uint32_t height = images[0].height & ~1u;
    struct Output {
        Encoder encoder;
        std::vector<size_t> parts;  // the sources it shows, left to right
        std::string path;
    };
    std::vector<std::unique_ptr<Output>> outputs;
    auto add = [&](const std::string& path, std::vector<size_t> parts) -> bool {
        auto o = std::make_unique<Output>();
        o->parts = std::move(parts);
        o->path = path;
        uint32_t w = 0;
        std::string title;
        for (size_t p : o->parts) {
            w += images[p].width & ~1u;
            title += (title.empty() ? "" : " | ") + sources[p]->Label();
        }
        if (!o->encoder.Open(enc, codec, w, height, path, title, error))
            return false;
        outputs.push_back(std::move(o));
        return true;
    };
    if (!add(out, pair ? std::vector<size_t>{0, 1} : std::vector<size_t>{0}))
        return Fail(error);
    if (pair && !a.Get("--video-a").empty() && !add(a.Get("--video-a"), {0}))
        return Fail(error);
    if (pair && !a.Get("--video-b").empty() && !add(a.Get("--video-b"), {1}))
        return Fail(error);

    std::vector<uint16_t> p010;
    uint64_t frames = 0;
    std::string mismatch;
    for (;;) {
        if (pair && (entries[0]["kind"] != entries[1]["kind"] || entries[0]["b"] != entries[1]["b"] ||
                     entries[0]["index"] != entries[1]["index"]) && mismatch.empty())
            mismatch = "frame " + std::to_string(frames) + ": " + sources[0]->Stem(entries[0]) + " beside " +
                       sources[1]->Stem(entries[1]);
        for (auto& o : outputs) {
            std::vector<const Image*> parts;
            std::vector<const replay::SdrCurve*> part_curves;
            for (size_t p : o->parts) {
                parts.push_back(&images[p]);
                part_curves.push_back(curves[p].get());
            }
            converter.Convert(parts, part_curves, height, p010);
            if (!o->encoder.Write(p010))
                return Fail("ffmpeg stopped taking frames for " + o->path);
        }
        frames++;
        bool more = true;
        for (size_t i = 0; i < sources.size(); i++)
            if (!sources[i]->Next(images[i], entries[i], error)) {
                if (!error.empty())
                    return Fail(a.positional[i] + ": " + error);
                more = false;
            }
        if (!more)
            break;
    }
    nlohmann::json info = {{"frames", frames}, {"fps", enc.fps}, {"codec", codec}, {"qp", enc.qp},
                           {"curve", curves[0]->Describe()}};
    for (auto& o : outputs) {
        if (!o->encoder.Close(error))
            return Fail(error);
        nlohmann::json sidecar = info, shown = nlohmann::json::array(), labels = nlohmann::json::array();
        uint32_t w = 0;
        for (size_t p : o->parts) {
            shown.push_back(SourceInfo(*sources[p]));
            labels.push_back(sources[p]->Label());
            w += images[p].width & ~1u;
        }
        sidecar["video"] = fs::path(o->path).filename().string();
        sidecar["size"] = {w, height};
        sidecar["layout"] = o->parts.size() > 1 ? "side-by-side" : "single";
        sidecar["labels"] = labels;
        sidecar["sources"] = shown;
        WriteSidecar(o->path, sidecar);
    }
    nlohmann::json report = {{"videos", nlohmann::json::array()}, {"frames", frames}, {"codec", codec},
                             {"fps", enc.fps}};
    for (auto& o : outputs)
        report["videos"].push_back(o->path);
    if (!mismatch.empty())
        report["mismatch"] = mismatch;
    std::printf("%s\n", report.dump().c_str());
    return 0;
}

int Png(const Args& a) {
    const std::string out = a.Get("--out"), list = a.Get("--frames");
    if (a.positional.size() != 1 || out.empty() || list.empty()) {
        std::fprintf(stderr, "%s", USAGE);
        return 2;
    }
    std::set<size_t> wanted;
    std::set<std::string> stems;
    for (size_t at = 0; at < list.size();) {
        const size_t comma = std::min(list.find(',', at), list.size());
        const std::string item = list.substr(at, comma - at);
        if (item.find('_') != std::string::npos)
            stems.insert(item);
        else if (!item.empty())
            wanted.insert(std::stoul(item));
        at = comma + 1;
    }
    Source source;
    std::string error;
    if (!source.Open(a.positional[0], error))
        return Fail(error);
    fs::create_directories(out);
    const std::string ffmpeg = a.Get("--ffmpeg", "ffmpeg");
    Image image;
    nlohmann::json entry, written = nlohmann::json::array();
    for (size_t n = 0;; n++) {
        if (!source.Next(image, entry, error)) {
            if (!error.empty())
                return Fail(error);
            break;
        }
        const std::string stem = source.Stem(entry);
        if (!wanted.count(n) && !stems.count(stem))
            continue;
        stems.erase(stem);
        replay::SdrCurve curve(image.format, source.header().value("hdr", false));
        std::vector<float> rgb((size_t)3 * image.width);
        std::vector<uint8_t> bytes((size_t)3 * image.width * image.height);
        for (uint32_t y = 0; y < image.height; y++) {
            curve.Row(image.texels.data() + (size_t)y * image.width * image.format.texel, image.width, rgb.data());
            for (size_t i = 0; i < rgb.size(); i++)
                bytes[(size_t)y * rgb.size() + i] = (uint8_t)std::lround(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f);
        }
        const std::string path = (fs::path(out) / (stem + ".png")).string();
        Child child;
        if (!child.Start({ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                          "-s", std::to_string(image.width) + "x" + std::to_string(image.height), "-i", "-",
                          "-frames:v", "1", path},
                         true, false, error))
            return Fail(error);
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), child.input()) == bytes.size();
        if (child.Wait() != 0 || !ok)
            return Fail("ffmpeg failed writing " + path);
        written.push_back({{"frame", n}, {"stem", stem}, {"png", path}});
        if (stems.empty() && (wanted.empty() || n >= *wanted.rbegin()))
            break;
    }
    std::printf("%s\n", nlohmann::json({{"images", written}, {"curve", replay::SdrCurve(image.format,
                                         source.header().value("hdr", false)).Describe()}}).dump().c_str());
    return written.empty() ? 1 : 0;
}

// The renderer's compare project (renderer/clips/project.h): two videos with labels, size and rate.
int Project(const Args& a) {
    const std::string out = a.Get("--out");
    if (a.positional.size() != 2 || out.empty()) {
        std::fprintf(stderr, "%s", USAGE);
        return 2;
    }
    std::vector<std::string> paths, labels;
    nlohmann::json sidecars[2];
    for (int i = 0; i < 2; i++) {
        std::error_code ec;
        paths.push_back(fs::absolute(a.positional[i], ec).string());
        std::ifstream in(a.positional[i] + ".json");
        sidecars[i] = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
        std::string label = fs::path(a.positional[i]).stem().string();
        if (sidecars[i].is_object() && sidecars[i].contains("labels") && !sidecars[i]["labels"].empty())
            label = sidecars[i]["labels"][0];
        labels.push_back(label);
    }
    auto it = a.options.find("--labels");
    if (it != a.options.end() && it->second.size() == 2)
        labels = it->second;
    nlohmann::ordered_json p;
    p["version"] = 1;
    p["title"] = a.Get("--title", labels[0] + " vs " + labels[1]);
    p["a"] = {{"path", paths[0]}};
    p["b"] = {{"path", paths[1]}};
    p["labels"] = {{"a", labels[0]}, {"b", labels[1]}};
    p["layout"] = "slider";
    p["divider"] = 0.5;
    p["offset_frames"] = 0;
    if (sidecars[0].is_object()) {
        p["fps"] = sidecars[0].value("fps", 0.0);
        p["size"] = sidecars[0].value("size", nlohmann::json::array({0, 0}));
    }
    std::error_code ec;
    if (fs::path(out).has_parent_path())
        fs::create_directories(fs::path(out).parent_path(), ec);
    std::ofstream(out) << p.dump(1) << "\n";
    std::printf("%s\n", nlohmann::json({{"project", out}, {"labels", labels}}).dump().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);  // a stopped ffmpeg makes writes fail instead
#endif
    if (argc < 2) {
        std::fprintf(stderr, "%s", USAGE);
        return 2;
    }
    const std::string command = argv[1];
    const Args args = ParseArgs(argc, argv);
    try {
        if (command == "video" || command == "side-by-side")
            return Videos(command, args);
        if (command == "png")
            return Png(args);
        if (command == "project")
            return Project(args);
    } catch (const std::exception& e) {
        return Fail(e.what());
    }
    std::fprintf(stderr, "%s", USAGE);
    return 2;
}
