#include "renderer/mode_switch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <imgui.h>
#include "renderer/clips/process.h"
#include "renderer/paths.h"
#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace renderer {
namespace fs = std::filesystem;
namespace {

std::optional<Mode> g_requested;

// What a switch that loses something asks about: the target and why, until it is answered.
struct Question {
    Mode target = Mode::SCENE;
    std::string reason;
    int frames = 0;       // since it was asked
    bool shown = false;   // the window was on screen
};
std::optional<Question> g_question;
bool g_open_question = false;

constexpr const char* POPUP = "Switch mode?###mode-switch";

std::string LastModePath() {
    return paths::State() + "/last-mode";
}

}  // namespace

Mode LastMode() {
    std::ifstream file(LastModePath());
    std::string text;
    std::getline(file, text);
    return ParseMode(text).value_or(Mode::SCENE);
}

void RememberMode(Mode mode) {
    const std::string path = LastModePath();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream file(path, std::ios::trunc);
    if (file)
        file << Info(mode).name << "\n";
    else
        spdlog::warn("cannot remember the mode in {}", path);
}

bool RequestMode(Mode mode, std::string& error) {
    const std::string exe = clips::ExecutablePath();
    std::error_code ec;
    if (exe.empty() || !fs::exists(exe, ec)) {
        error = "cannot find the renderer's own executable to start " + std::string(Info(mode).title);
        return false;
    }
    RememberMode(mode);
    g_requested = mode;
#ifdef _WIN32
    const char* how = "a new process";
#else
    const char* how = "this process (exec)";
#endif
    spdlog::info("mode: switching to {}: this window closes, then it starts in {}", Info(mode).name, how);
    return true;
}

std::optional<Mode> RequestedMode() {
    return g_requested;
}

int StartRequestedMode() {
    if (!g_requested)
        return 0;
    const Mode mode = *g_requested;
    g_requested.reset();
    const std::string exe = clips::ExecutablePath();
    std::vector<std::string> args = ModeArguments(mode);
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    // The mode's flags are plain ASCII and a Windows path holds no quote: one quoted path and the flags.
    std::wstring command = L"\"" + fs::path(exe).wstring() + L"\"";
    for (const std::string& arg : args)
        command += L" " + std::wstring(arg.begin(), arg.end());
    // The new process writes to the same console or log file (a run.cmd that redirects the log): it gets this one's standard
    // handles, when it has all three.
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    const HANDLE standard[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    const bool share = standard[0] && standard[0] != INVALID_HANDLE_VALUE && standard[1] && standard[1] != INVALID_HANDLE_VALUE &&
                       standard[2] && standard[2] != INVALID_HANDLE_VALUE;
    if (share) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = standard[0];
        startup.hStdOutput = standard[1];
        startup.hStdError = standard[2];
    }
    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, share ? TRUE : FALSE, 0, nullptr, nullptr, &startup, &info)) {
        std::fprintf(stderr, "cannot start %s: error %lu\n", exe.c_str(), (unsigned long)GetLastError());
        return 1;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return 0;
#else
    // Nothing the old mode left open goes into the new one (a driver's device file, a socket).
    const long limit = std::min<long>(sysconf(_SC_OPEN_MAX), 4096);
    for (int fd = 3; fd < limit; fd++) {
        const int flags = fcntl(fd, F_GETFD);
        if (flags >= 0)
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    std::vector<char*> argv = {const_cast<char*>(exe.c_str())};
    for (std::string& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    execv(exe.c_str(), argv.data());
    std::fprintf(stderr, "cannot start %s: %s\n", exe.c_str(), std::strerror(errno));
    return 1;
#endif
}

// --- The Mode menu -------------------------------------------------------------------------------------------------------

std::optional<Mode> DrawModeMenu(Mode current, const std::function<std::string(Mode)>& loses) {
    std::optional<Mode> picked;
    if (ImGui::BeginMenu("Mode")) {
        for (const ModeInfo& info : AllModes()) {
            if (info.mode == Mode::SCENE && !SceneBuilt())
                continue;
            if (ImGui::MenuItem(info.title, nullptr, info.mode == current) && info.mode != current)
                picked = info.mode;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", info.description);
        }
        ImGui::EndMenu();
    }
    if (!picked)
        return std::nullopt;
    const std::string reason = loses ? loses(*picked) : std::string();
    if (reason.empty())
        return picked;
    g_question = Question{*picked, reason};
    g_open_question = true;
    return std::nullopt;
}

std::optional<Mode> DrawModeConfirm() {
    if (!g_question)
        return std::nullopt;
    if (g_open_question) {
        ImGui::OpenPopup(POPUP);
        g_open_question = false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    std::optional<Mode> answer;
    bool close = false;
    g_question->frames++;
    if (ImGui::BeginPopupModal(POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        g_question->shown = true;
        ImGui::TextUnformatted(g_question->reason.c_str());
        ImGui::Text("Switching to %s ends it.", Info(g_question->target).title);
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.4f));
        if (ImGui::Button("Stay") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;
        ImGui::SameLine();
        if (ImGui::Button((std::string("Switch to ") + Info(g_question->target).title).c_str())) {
            answer = g_question->target;
            close = true;
        }
        if (close)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    } else if (g_question->shown || g_question->frames > 3) {
        close = true;  // closed some other way, or it never opened
    }
    if (close)
        g_question.reset();
    return answer;
}

bool ModeConfirmOpen() {
    return g_question.has_value();
}

}  // namespace renderer
