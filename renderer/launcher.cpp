#include "renderer/launcher.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <thread>

#include <imgui.h>
#include "renderer/app.h"
#include "renderer/clips/captures/png_write.h"
#include "renderer/clips/captures/store.h"
#include "renderer/clips/gfx.h"
#include "renderer/clips/process.h"
#include "renderer/clips/swap_output.h"
#include "renderer/display.h"
#include "renderer/mode_switch.h"
#include "renderer/scene.h"
#include "device/log.h"
#include <spdlog/spdlog.h>

namespace renderer {
namespace fs = std::filesystem;
namespace {

const ImVec4 GOOD(0.55f, 0.9f, 0.6f, 1.0f);
const ImVec4 WARNING(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 BAD(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 ACCENT(0.35f, 0.65f, 1.0f, 1.0f);

// --- What each mode needs, looked up without loading anything ---------------------------------------------------------------

enum class Level { READY, NOTE, MISSING };  // runs / runs with limits / cannot start

struct Availability {
    Level level = Level::READY;
    std::string text;
};

bool IsFile(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

// ffmpeg or ffprobe: where clips::FindTool looks, then PATH.
bool ToolFound(const char* name, const char* variable) {
    const std::string found = clips::FindTool(name, variable);
    if (found.find('/') != std::string::npos || found.find('\\') != std::string::npos)
        return IsFile(found);
    const char* path = std::getenv("PATH");
    if (!path)
        return false;
#ifdef _WIN32
    const char separator = ';';
    const std::string file = std::string(name) + ".exe";
#else
    const char separator = ':';
    const std::string file = name;
#endif
    std::stringstream directories(path);
    for (std::string directory; std::getline(directories, directory, separator);)
        if (!directory.empty() && IsFile((fs::path(directory) / file).string()))
            return true;
    return false;
}

std::string Join(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (const std::string& part : parts)
        out += (out.empty() ? "" : separator) + part;
    return out;
}

Availability SceneAvailability() {
    Availability a;
    const SceneStatus status = CheckScene();
    if (!status.can_start) {
        a.level = Level::MISSING;
        a.text = "Cannot start: " + status.missing + ".";
    } else if (status.notes.empty()) {
        a.text = "Ready: the scene, the display model catalogue and the DLSS runtime are here.";
    } else {
        a.level = Level::NOTE;
        a.text = "Runs with limits: " + Join(status.notes, "; ") + ".";
    }
    return a;
}

Availability CompareAvailability() {
    Availability a;
    const bool ffmpeg = ToolFound("ffmpeg", "NR_FFMPEG"), ffprobe = ToolFound("ffprobe", "NR_FFPROBE");
    if (ffmpeg && ffprobe) {
        a.text = "Ready: the recordings are chosen in the window; ffmpeg and ffprobe are found.";
    } else {
        a.level = Level::NOTE;
        a.text = std::string(!ffmpeg && !ffprobe ? "ffmpeg and ffprobe are" : !ffmpeg ? "ffmpeg is" : "ffprobe is") +
                 " not found (NR_FFMPEG, NR_FFPROBE, beside the executable or on PATH): sync detection, the export and "
                 "decoding where the GPU cannot need them.";
    }
    return a;
}

// A recording (record.json) or a capture of a folder format.
bool IsCapture(const fs::path& folder) {
    if (IsFile((folder / "record.json").string()))
        return true;
    for (const clips::captures::FolderFormat& f : clips::captures::FolderFormats())
        if (IsFile((folder / f.marker).string()))
            return true;
    return false;
}

Availability CapturesAvailability() {
    Availability a;
    const std::vector<std::string> folders = clips::captures::DefaultCaptureFolders();
    int captures = 0;
    for (const std::string& folder : folders) {
        std::error_code ec;
        if (IsDirectory(folder))
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_directory(ec) && IsCapture(it->path()))
                    captures++;
    }
    // What recreating frames needs: nr-replay for recordings, and each folder format's tools
    clips::captures::Locations where = clips::captures::DefaultLocations(clips::WorkspacePath("."));
    where.executable = clips::ExecutablePath();
    std::vector<std::string> notes;
    if (clips::captures::ReplayTool(where).empty())
        notes.push_back("recreating frames of recordings needs nr-replay (NR_REPLAY, or beside the renderer)");
    for (const clips::captures::FolderFormat& f : clips::captures::FolderFormats())
        if (f.missing)
            if (const std::string missing = f.missing(where); !missing.empty())
                notes.push_back(missing);
    const std::string where_text = Join(folders, ", ");
    if (captures > 0 && notes.empty()) {
        a.text = "Ready: " + std::to_string(captures) + (captures == 1 ? " capture" : " captures") + " in " + where_text + ".";
    } else {
        a.level = Level::NOTE;
        if (captures == 0)
            notes.insert(notes.begin(), "no captures in " + where_text + " yet (nr-record's capture DLL writes them in a "
                                        "game; the tab can add other folders)");
        a.text = std::string(captures > 0 ? "Browsing works: " : "Opens empty: ") + Join(notes, "; ") + ".";
    }
    return a;
}

// The options the launcher takes; the rest of a command line that asked for it with --launcher is ignored. Returns the
// arguments to hand to the window code.
std::vector<std::string> LauncherArguments(int argc, char** argv, std::vector<std::string>& ignored) {
    static const char* with_value[] = {"--width", "--height", "--timeLimit", "--frameNum", "--script", "--capture",
                                       "--capture-frame", "--log-level"};
    static const char* without_value[] = {"--vsync", "--x11", "--alwaysActive", "--help"};
    std::vector<std::string> out = {argc > 0 ? argv[0] : "renderer"};
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool inline_value = arg.find('=') != std::string::npos;
        const std::string name = arg.substr(0, arg.find('='));
        bool takes_value = false, known = false;
        for (const char* option : without_value)
            known = known || name == option;
        for (const char* option : with_value)
            if (name == option)
                known = takes_value = true;
        if (known) {
            out.push_back(arg);
            if (takes_value && !inline_value && i + 1 < argc)
                out.push_back(argv[++i]);
        } else if (name != "--launcher") {
            std::string dropped = arg;
            if (!inline_value && i + 1 < argc && argv[i + 1][0] != '-')  // its value, if it has one
                dropped += " " + std::string(argv[++i]);
            ignored.push_back(dropped);
        }
    }
    return out;
}

// --- The window -------------------------------------------------------------------------------------------------------------

class Launcher : public App {
public:
    Launcher() {
        nr::InitLogging(/*to_stderr=*/false);
        m_Resizable = true;
        m_Vsync = true;
        m_AlwaysActive = true;
        m_OutputResolution = {1100, 400};  // a small window, in points (the same on a scaled display)
        m_SizeInPoints = true;
    }
    ~Launcher() override;

    void InitCmdLine(cmdline::parser& cmd) override {
        cmd.add<std::string>("script", 0, "actions at frames for checks: FRAME:ACTION, comma-separated (select=scene|compare|"
                             "captures, focus=N, capture=FILE.png, log, quit)", false, "");
        cmd.add<std::string>("capture", 0, "write the window's pixels of one frame to this PNG, then quit", false, "");
        cmd.add<int>("capture-frame", 0, "the frame --capture takes", false, 20);
        cmd.add<std::string>("log-level", 0, "trace, debug, info, warn or error", false, "info");
    }
    void ReadCmdLine(cmdline::parser& cmd) override {
        m_AlwaysActive = true;
        if (cmd.exist("width") || cmd.exist("height"))
            m_SizeInPoints = false;  // a size asked for is in pixels, as everywhere else
        spdlog::set_level(spdlog::level::from_str(cmd.get<std::string>("log-level")));
        capture_path_ = clips::UserPath(cmd.get<std::string>("capture"));
        capture_frame_ = cmd.get<int>("capture-frame");
        capture_quits_ = !capture_path_.empty();
        std::stringstream items(cmd.get<std::string>("script"));
        for (std::string item; std::getline(items, item, ',');) {
            const size_t colon = item.find(':');
            if (colon == std::string::npos) {
                spdlog::warn("--script: {} is not FRAME:ACTION", item);
                continue;
            }
            script_.push_back({(uint32_t)std::stoul(item.substr(0, colon)), item.substr(colon + 1)});
        }
    }
    bool Initialize(nri::GraphicsAPI, bool) override;
    void LatencySleep(uint32_t frame_index) override;
    void PrepareFrame(uint32_t frame_index) override;
    void RenderFrame(uint32_t frame_index) override;
    bool AppShouldClose() override { return close_; }

private:
    struct QueuedFrame {
        nri::CommandAllocator* allocator = nullptr;
        nri::CommandBuffer* command_buffer = nullptr;
    };

    void HandleResize();
    void ApplyUiScale();
    void SignalFrame(uint32_t frame_index);
    void RunScript(uint32_t frame_index);
    void DrawUi();
    void DrawCard(int index, ImVec2 size);
    float CardHeight(int index, float width) const;  // what its texts need at this width
    void Choose(Mode mode);

    clips::Gfx gfx_;
    clips::SwapOutput swap_;
    clips::SwapOptions swap_options_;
    nri::Streamer* streamer_ = nullptr;
    nri::Fence* frame_fence_ = nullptr;
    std::vector<QueuedFrame> queued_frames_;

    Availability availability_[MODE_COUNT];
    std::vector<int> shown_;  // the modes this build has, in card order
    Mode last_ = Mode::SCENE;
    int focus_ = 0;
    std::string version_, gpu_, notice_;
    bool notice_bad_ = false;
    bool close_ = false;
    float ui_scale_ = 1.0f;
    ImGuiStyle base_style_;
    ImFont* mono_ = nullptr;

    std::vector<std::pair<uint32_t, std::string>> script_;
    std::string capture_path_;
    int capture_frame_ = 20;
    bool capture_quits_ = false;
};

Launcher::~Launcher() {
    if (gfx_.NRI.HasCore()) {
        gfx_.NRI.DeviceWaitIdle(gfx_.device);
        swap_.Destroy();
        for (QueuedFrame& q : queued_frames_) {
            gfx_.NRI.DestroyCommandBuffer(q.command_buffer);
            gfx_.NRI.DestroyCommandAllocator(q.allocator);
        }
        if (frame_fence_)
            gfx_.NRI.DestroyFence(frame_fence_);
        if (streamer_)
            gfx_.NRI.DestroyStreamer(streamer_);
    }
    DestroyImgui();
    gfx_.Destroy();
}

bool Launcher::Initialize(nri::GraphicsAPI, bool) {
    std::string error;
    if (!gfx_.Create(/*window=*/true, error, /*video_decode=*/false)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        spdlog::critical("{}", error);
        return false;
    }
    auto& NRI = gfx_.NRI;
    nri::StreamerDesc streamer = {};
    streamer.dynamicBufferMemoryLocation = nri::MemoryLocation::HOST_UPLOAD;
    streamer.dynamicBufferDesc = {0, 0, nri::BufferUsageBits::VERTEX | nri::BufferUsageBits::INDEX};
    streamer.constantBufferMemoryLocation = nri::MemoryLocation::HOST_UPLOAD;
    streamer.queuedFrameNum = GetQueuedFrameNum();
    streamer.hostDataCapacity = IMGUI_HOST_DATA_CAPACITY;
    if (NRI.CreateStreamer(*gfx_.device, streamer, streamer_) != nri::Result::SUCCESS ||
        NRI.CreateFence(*gfx_.device, nri::FenceDesc{}, frame_fence_) != nri::Result::SUCCESS) {
        spdlog::critical("cannot create the streamer or the frame fence");
        return false;
    }
    queued_frames_.resize(GetQueuedFrameNum());
    for (QueuedFrame& q : queued_frames_)
        if (NRI.CreateCommandAllocator(*gfx_.queue, q.allocator) != nri::Result::SUCCESS ||
            NRI.CreateCommandBuffer(*q.allocator, q.command_buffer) != nri::Result::SUCCESS) {
            spdlog::critical("cannot create a command buffer");
            return false;
        }
    if (!gfx_.CreateDescriptorPool(8, 8)) {
        spdlog::critical("cannot create the descriptor pool");
        return false;
    }
    swap_options_.vsync = m_Vsync;
    swap_options_.wanted = SRGB8;
    swap_.QueryDisplay(gfx_, *this, m_Window, swap_options_);
    if (!swap_.Create(gfx_, *this, GetQueuedFrameNum(), error)) {
        spdlog::critical("{}", error);
        return false;
    }
    if (!InitImgui(*gfx_.device))
        return false;
    renderer::SetupUiStyle(base_style_, mono_);
    ApplyUiScale();

    // The line under the title: the build, and the GPU (the device exists anyway to draw this window).
    const nri::DeviceDesc& device = NRI.GetDeviceDesc(*gfx_.device);
#ifdef NDEBUG
    version_ = "Build: release";
#else
    version_ = "Build: debug";
#endif
#ifdef __clang__
    version_ += " | clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) + "." +
                std::to_string(__clang_patchlevel__);
#endif
    const int sdl = SDL_GetVersion();
    version_ += " | NRI " + std::to_string(NRI_VERSION) + " | SDL " + std::to_string(SDL_VERSIONNUM_MAJOR(sdl)) + "." +
                std::to_string(SDL_VERSIONNUM_MINOR(sdl)) + "." + std::to_string(SDL_VERSIONNUM_MICRO(sdl)) + " | " +
                (clips::METAL ? "Metal 4" : "Vulkan") + " | " + VideoDriver();
    char memory[48] = "";
    if (device.adapterDesc.videoMemorySize > 0)
        std::snprintf(memory, sizeof(memory), ", %.0f GB", (double)device.adapterDesc.videoMemorySize / (1024.0 * 1024.0 * 1024.0));
    gpu_ = std::string("GPU: ") + (device.adapterDesc.name[0] ? device.adapterDesc.name : gfx_.device_name.c_str()) + memory;

    for (int i = 0; i < MODE_COUNT; i++)
        if ((Mode)i != Mode::SCENE || SceneBuilt())
            shown_.push_back(i);
    if (SceneBuilt())
        availability_[(int)Mode::SCENE] = SceneAvailability();
    else
        availability_[(int)Mode::SCENE] = {Level::MISSING, "Not in this build."};
    availability_[(int)Mode::COMPARE] = CompareAvailability();
    availability_[(int)Mode::CAPTURES] = CapturesAvailability();
    last_ = LastMode();
    if (std::find(shown_.begin(), shown_.end(), (int)last_) == shown_.end())
        last_ = (Mode)shown_.front();
    focus_ = (int)last_;
    spdlog::info("mode: launcher (last chosen: {})", Info(last_).name);
    for (int i : shown_)
        spdlog::info("launcher: {}: {}", Info((Mode)i).name, availability_[i].text);
    return true;
}

void Launcher::ApplyUiScale() {
    ui_scale_ = renderer::AutoUiScale(*this);
    ImGuiStyle& style = ImGui::GetStyle();
    style = base_style_;
    style.ScaleAllSizes(ui_scale_);
    style.FontScaleMain = ui_scale_;
}

void Launcher::SignalFrame(uint32_t frame_index) {
    nri::FenceSubmitDesc done = {frame_fence_, 1 + (uint64_t)frame_index};
    nri::QueueSubmitDesc submit = {};
    submit.signalFences = &done;
    submit.signalFenceNum = 1;
    gfx_.NRI.QueueSubmit(*gfx_.queue, submit);
}

void Launcher::LatencySleep(uint32_t frame_index) {
    auto& NRI = gfx_.NRI;
    const uint32_t queued_num = GetQueuedFrameNum();
    NRI.Wait(*frame_fence_, frame_index >= queued_num ? 1 + frame_index - queued_num : 0);
    NRI.ResetCommandAllocator(*queued_frames_[frame_index % queued_num].allocator);
}

void Launcher::HandleResize() {
    const int fw = (int)PixelSize().x, fh = (int)PixelSize().y;
    if (fw > 0 && fh > 0 && (m_OutputResolution.x != (uint32_t)fw || m_OutputResolution.y != (uint32_t)fh)) {
        m_OutputResolution = uint2((uint32_t)fw, (uint32_t)fh);
        swap_.dirty = true;
        ImGui::GetIO().DisplaySize = ImVec2((float)fw, (float)fh);
        ApplyUiScale();
    }
    if (swap_.dirty && fw > 0 && fh > 0) {
        gfx_.NRI.DeviceWaitIdle(gfx_.device);
        swap_.Destroy();
        std::string error;
        if (!swap_.Create(gfx_, *this, GetQueuedFrameNum(), error)) {
            spdlog::critical("{}", error);
            close_ = true;
        }
    }
}

// A mode chosen: a click, Enter, a number key or the script. One that cannot start says why and stays.
void Launcher::Choose(Mode mode) {
    const Availability& a = availability_[(int)mode];
    if (a.level == Level::MISSING) {
        notice_ = std::string(Info(mode).title) + ": " + a.text;
        notice_bad_ = true;
        return;
    }
    std::string error;
    if (!RequestMode(mode, error)) {
        notice_ = error;
        notice_bad_ = true;
        return;
    }
    close_ = true;
}

void Launcher::RunScript(uint32_t frame_index) {
    for (const auto& [frame, action] : script_) {
        if (frame != frame_index)
            continue;
        const size_t equals = action.find('=');
        const std::string name = action.substr(0, equals), value = equals == std::string::npos ? "" : action.substr(equals + 1);
        if (name == "select") {
            if (const auto mode = ParseMode(value))
                Choose(*mode);
            else
                spdlog::warn("--script: select={}: scene, compare or captures", value);
        } else if (name == "focus") {
            const int mode = std::clamp(std::atoi(value.c_str()), 0, MODE_COUNT - 1);
            if (std::find(shown_.begin(), shown_.end(), mode) != shown_.end())
                focus_ = mode;
        } else if (name == "capture") {
            capture_path_ = clips::UserPath(value);
            capture_frame_ = (int)frame_index + 1;
        } else if (name == "log") {
            spdlog::info("state: launcher focus {} last {} notice \"{}\"", Info((Mode)focus_).name, Info(last_).name, notice_);
        } else if (name == "quit") {
            close_ = true;
        } else {
            spdlog::warn("--script: unknown action {}", action);
        }
    }
}

void Launcher::PrepareFrame(uint32_t frame_index) {
    RunScript(frame_index);
    HandleResize();
    DrawUi();
}

// --- The UI -----------------------------------------------------------------------------------------------------------------

float Launcher::CardHeight(int index, float width) const {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line = ImGui::GetFontSize();
    const float wrap = width - 2.0f * line * 0.9f;
    // the padding, the title row (a number at 2x), the last-used line, the gaps, the description and the availability
    float height = 2.0f * line * 0.8f + 2.0f * line + line + line * 0.6f + line * 0.5f + 5.0f * style.ItemSpacing.y;
    height += ImGui::CalcTextSize(AllModes()[(size_t)index].description, nullptr, false, wrap).y;
    height += ImGui::CalcTextSize(availability_[index].text.c_str(), nullptr, false, wrap).y;
    return height;
}

void Launcher::DrawCard(int index, ImVec2 size) {
    const ModeInfo& info = AllModes()[(size_t)index];
    const Availability& availability = availability_[index];
    ImGuiIO& io = ImGui::GetIO();
    const float line = ImGui::GetFontSize();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    if (hovered && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
        focus_ = index;  // the pointer moves the highlight; the keys move it too
    const bool focused = focus_ == index;
    const bool blocked = availability.level == Level::MISSING;
    ImGui::PushStyleColor(ImGuiCol_Border, focused ? ACCENT : ImGui::GetStyleColorVec4(ImGuiCol_Border));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, focused ? ImVec4(0.11f, 0.135f, 0.18f, 1.0f) : ImVec4(0.09f, 0.10f, 0.125f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, focused ? 2.0f : 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(line * 0.9f, line * 0.8f));
    ImGui::BeginChild(info.name, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
    ImGui::TextColored(ACCENT, "%d", (int)(std::find(shown_.begin(), shown_.end(), index) - shown_.begin()) + 1);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(info.title);
    ImGui::PopFont();
    if (last_ == info.mode)
        ImGui::TextColored(ACCENT, "last used");
    else
        ImGui::Dummy(ImVec2(0.0f, line));
    ImGui::Dummy(ImVec2(0.0f, line * 0.2f));
    ImGui::TextWrapped("%s", info.description);
    ImGui::Dummy(ImVec2(0.0f, line * 0.4f));
    ImGui::PushStyleColor(ImGuiCol_Text, availability.level == Level::READY ? GOOD : availability.level == Level::NOTE ? WARNING : BAD);
    ImGui::TextWrapped("%s", availability.text.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (blocked && hovered)
        ImGui::SetTooltip("%s", availability.text.c_str());
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        Choose(info.mode);
}

void Launcher::DrawUi() {
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    const float line = ImGui::GetFontSize();
    // Keys: 1, 2, 3 start a mode; the arrows and Tab move the highlight; Enter starts it; Esc quits.
    if (!io.WantTextInput) {
        const ImGuiKey numbers[MODE_COUNT] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3};
        const ImGuiKey keypad[MODE_COUNT] = {ImGuiKey_Keypad1, ImGuiKey_Keypad2, ImGuiKey_Keypad3};
        const int count = (int)shown_.size();
        for (int i = 0; i < count; i++)
            if (ImGui::IsKeyPressed(numbers[i], false) || ImGui::IsKeyPressed(keypad[i], false))
                Choose((Mode)shown_[(size_t)i]);
        const int at = (int)(std::find(shown_.begin(), shown_.end(), focus_) - shown_.begin());
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            focus_ = shown_[(size_t)((at + count - 1) % count)];
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_Tab))
            focus_ = shown_[(size_t)((at + 1) % count)];
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Space, false))
            Choose((Mode)focus_);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close_ = true;
    }
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.075f, 0.09f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(line * 1.6f, line * 1.3f));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.2f);
    ImGui::TextUnformatted("NR renderer");
    ImGui::PopFont();
    ImGui::TextDisabled("%s", version_.c_str());
    ImGui::TextDisabled("%s", gpu_.c_str());
    ImGui::Dummy(ImVec2(0.0f, line * 0.8f));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::TextUnformatted("Choose a mode");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, line * 0.2f));
    const float gap = ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const float available = ImGui::GetContentRegionAvail().x;
    const float count = (float)shown_.size();
    if (available >= line * 16.0f * count) {  // the cards in a row, as high as the longest text needs
        const float width = (available - (count - 1.0f) * gap) / count;
        float height = 0.0f;
        for (int i : shown_)
            height = std::max(height, CardHeight(i, width));
        for (int i : shown_) {
            if (i != shown_.front())
                ImGui::SameLine(0.0f, gap);
            DrawCard(i, ImVec2(width, height));
        }
    } else {  // a narrow window: one under the other
        for (int i : shown_)
            DrawCard(i, ImVec2(available, CardHeight(i, available)));
    }
    ImGui::Dummy(ImVec2(0.0f, line * 0.3f));
    if (!notice_.empty())
        ImGui::TextColored(notice_bad_ ? BAD : GOOD, "%s", notice_.c_str());
    else
        ImGui::TextDisabled("Click a card, press its number or Enter on the highlighted one. Esc quits. Every mode has a Mode "
                            "menu to come back here or switch.");
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::EndFrame();
    ImGui::Render();
}

void Launcher::RenderFrame(uint32_t frame_index) {
    auto& NRI = gfx_.NRI;
    const uint32_t queued = frame_index % GetQueuedFrameNum();
    QueuedFrame& q = queued_frames_[queued];
    if (Minimized() || swap_.dirty) {
        SignalFrame(frame_index);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }
    clips::SwapOutput::Frame frame;
    if (!swap_.Acquire(frame_index, frame)) {
        SignalFrame(frame_index);
        return;
    }
    nri::CommandBuffer& cb = *q.command_buffer;
    NRI.BeginCommandBuffer(cb, gfx_.descriptor_pool);
    // A capture: the swap chain's texture is copied to a buffer after the frame is drawn.
    nri::Buffer* capture = nullptr;
    nri::Memory* capture_memory = nullptr;
    uint32_t capture_pitch = 0;
    if (!capture_path_.empty() && (int)frame_index >= capture_frame_) {
        const uint32_t align = NRI.GetDeviceDesc(*gfx_.device).memoryAlignment.uploadBufferTextureRow;
        capture_pitch = (swap_.size().x * 4 + align - 1) / align * align;
        nri::BufferDesc desc = {};
        desc.size = (uint64_t)capture_pitch * swap_.size().y;
        if (NRI.CreateBuffer(*gfx_.device, desc, capture) == nri::Result::SUCCESS) {
            nri::ResourceGroupDesc group = {};
            group.memoryLocation = nri::MemoryLocation::HOST_READBACK;
            group.buffers = &capture;
            group.bufferNum = 1;
            if (NRI.AllocateAndBindMemory(*gfx_.device, group, &capture_memory) != nri::Result::SUCCESS) {
                NRI.DestroyBuffer(capture);
                capture = nullptr;
            }
        }
    }
    swap_.Record(cb, *streamer_, frame, [](nri::Format) {}, true, capture, capture_pitch);  // the UI on black
    NRI.EndCommandBuffer(cb);
    const bool presented = swap_.Present(cb, frame_index, frame);
    NRI.EndStreamerFrame(*streamer_);
    if (!presented)
        swap_.dirty = true;
    SignalFrame(frame_index);
    if (capture) {
        NRI.QueueWaitIdle(gfx_.queue);
        const uint32_t width = swap_.size().x, height = swap_.size().y;
        const char* bytes = (const char*)NRI.MapBuffer(*capture, 0, (uint64_t)capture_pitch * height);
        const nri::Format format = swap_.format();
        const bool bgra = format == nri::Format::BGRA8_UNORM || format == nri::Format::BGRA8_SRGB;
        std::vector<uint8_t> rgba((size_t)width * height * 4);
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t* in = (const uint8_t*)bytes + (uint64_t)y * capture_pitch + (uint64_t)x * 4;
                uint8_t* out = &rgba[((size_t)y * width + x) * 4];
                out[0] = in[bgra ? 2 : 0];
                out[1] = in[1];
                out[2] = in[bgra ? 0 : 2];
                out[3] = 255;
            }
        NRI.UnmapBuffer(*capture);
        NRI.DestroyBuffer(capture);
        NRI.FreeMemory(capture_memory);
        std::string error;
        if (clips::captures::WritePng8(capture_path_, width, height, 4, rgba.data(), (int)width * 4, error))
            spdlog::info("capture {}: frame {}, {}x{}", capture_path_, frame_index, width, height);
        else
            spdlog::error("capture {}: {}", capture_path_, error);
        capture_path_.clear();
        if (capture_quits_)
            close_ = true;
    }
}

}  // namespace

int RunLauncher(int argc, char** argv) {
    std::vector<std::string> ignored;
    std::vector<std::string> arguments = LauncherArguments(argc, argv, ignored);
    if (!ignored.empty())
        std::fprintf(stderr, "the launcher ignores: %s\n", Join(ignored, ", ").c_str());
    std::vector<char*> pointers;
    for (std::string& argument : arguments)
        pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    // (NRIFramework's SAMPLE_MAIN deletes the sample in an atexit handler, after the Vulkan driver's: the app is destroyed
    // before main returns, as the renderer's is.)
    Launcher* app = new Launcher;
    const bool ok = app->Create((int)arguments.size(), pointers.data(), "NR renderer");
    if (ok)
        app->RenderLoop();
    delete app;
    spdlog::shutdown();
    return ok ? 0 : 1;
}

}  // namespace renderer
