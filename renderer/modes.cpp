#include "renderer/modes.h"

#include <algorithm>
#include <cctype>

namespace renderer {
namespace {

const std::array<ModeInfo, MODE_COUNT> MODES = {{
    {Mode::SCENE, "scene", "Scene renderer",
     "Fly through a glTF scene with live frame generation: our models and DLSS-G on the same frames."},
    {Mode::COMPARE, "compare", "Compare videos",
     "Two screen recordings in lockstep with exact pixel zoom, marks and an export of the comparison."},
    {Mode::CAPTURES, "captures", "Browse captures",
     "Game recordings and captures: compare their frames, recreate generated frames with DLSS-G or ANR FG."},
}};

bool scene_built = true;

bool Has(const std::vector<std::string>& args, const char* flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

}  // namespace

void SetSceneBuilt(bool built) {
    scene_built = built;
}

bool SceneBuilt() {
    return scene_built;
}

const std::array<ModeInfo, MODE_COUNT>& AllModes() {
    return MODES;
}

const ModeInfo& Info(Mode mode) {
    return MODES[(size_t)mode];
}

std::optional<Mode> ParseMode(const std::string& text) {
    size_t first = 0, last = text.size();
    while (first < last && std::isspace((unsigned char)text[first]))
        first++;
    while (last > first && std::isspace((unsigned char)text[last - 1]))
        last--;
    const std::string name = text.substr(first, last - first);
    for (const ModeInfo& info : MODES)
        if (name == info.name)
            return info.mode;
    return std::nullopt;
}

Decision DecideMode(int argc, const char* const* argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++)
        args.push_back(argv[i]);
    Decision decision;
    if (args.empty() || Has(args, "--launcher"))
        return decision;  // the launcher

    // --mode NAME or --mode=NAME
    bool has_mode = false;
    std::string value;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--mode") {
            has_mode = true;
            value = i + 1 < args.size() ? args[i + 1] : std::string();
        } else if (args[i].rfind("--mode=", 0) == 0) {
            has_mode = true;
            value = args[i].substr(7);
        }
    }
    std::optional<Mode> chosen;
    if (has_mode) {
        chosen = ParseMode(value);
        if (!chosen) {
            decision.start = Start::INVALID;
            decision.error = value.empty() ? "--mode needs a value: scene, compare or captures"
                                           : "--mode " + value + ": use scene, compare or captures";
            return decision;
        }
    }

    // The flags that already name a mode (they keep working as they are)
    Start legacy = Start::SCENE;  // none
    std::string legacy_flag;
    auto flag = [&](const char* name, Start start) {
        if (Has(args, name) && legacy_flag.empty()) {
            legacy = start;
            legacy_flag = name;
        }
    };
    flag("--study", Start::STUDY);
    flag("--render-test", Start::STUDY);
    flag("--compare-export", Start::COMPARE_TOOL);
    flag("--compare-check", Start::COMPARE_TOOL);
    flag("--captures", Start::CAPTURES);  // with --compare too: the Captures tab, as before
    flag("--compare", Start::COMPARE);

    if (!has_mode) {
        decision.start = legacy;  // SCENE when no flag names a mode: any other argument is the scene renderer's
        return decision;
    }
    const std::string given = "--mode " + std::string(Info(*chosen).name);
    if (legacy == Start::STUDY) {
        decision.start = Start::INVALID;
        decision.error = given + " conflicts with " + legacy_flag + " (the study viewer is not one of the three modes)";
    } else if (legacy != Start::SCENE && *chosen == Mode::SCENE) {
        decision.start = Start::INVALID;
        decision.error = given + " conflicts with " + legacy_flag;
    } else if (legacy == Start::CAPTURES && *chosen == Mode::COMPARE) {
        decision.start = Start::INVALID;
        decision.error = given + " conflicts with " + legacy_flag;
    } else if (legacy == Start::COMPARE_TOOL) {
        decision.start = Start::COMPARE_TOOL;
    } else {
        decision.start = *chosen == Mode::SCENE ? Start::SCENE : *chosen == Mode::COMPARE ? Start::COMPARE : Start::CAPTURES;
    }
    return decision;
}

std::vector<std::string> ModeArguments(Mode mode) {
    switch (mode) {
        case Mode::COMPARE:
            return {"--compare"};
        case Mode::CAPTURES:
            return {"--captures"};
        case Mode::SCENE:
        default:
            return {"--mode", "scene"};
    }
}

}  // namespace renderer
