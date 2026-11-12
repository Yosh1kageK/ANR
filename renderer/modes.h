// The renderer binary's modes and the rule that picks one from the command line (renderer/README.md "Modes"). Pure: no
// window, no OS code, tested by modes_test.cpp.
//
//   no arguments            the launcher (launcher.cpp)
//   --launcher              the launcher, whatever else is given
//   --mode scene|compare|captures   that mode
//   --compare, --captures   as before (the clip viewer on its Videos or Captures tab)
//   --study, --render-test, --compare-check, --compare-export   as before (the study viewer, the headless checks)
//   any other argument      the scene renderer, as before: every script and README example that passes options works unchanged

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace renderer {

// The three modes the launcher and the Mode menu offer.
enum class Mode { SCENE, COMPARE, CAPTURES };
inline constexpr int MODE_COUNT = 3;

struct ModeInfo {
    Mode mode;
    const char* name;         // --mode NAME, and the remembered last choice
    const char* title;        // the card and the menu entry
    const char* description;  // one line
};

const std::array<ModeInfo, MODE_COUNT>& AllModes();
const ModeInfo& Info(Mode mode);
std::optional<Mode> ParseMode(const std::string& name);  // "scene", "compare" or "captures", surrounding blanks ignored

// What the process starts as.
enum class Start {
    LAUNCHER,
    SCENE,
    COMPARE,       // the clip viewer on its Videos tab
    CAPTURES,      // the clip viewer on its Captures tab
    COMPARE_TOOL,  // --compare-check, --compare-export: the compare mode's headless checks and export
    STUDY,         // --study, --render-test: the 2AFC study viewer
    INVALID,       // the command line contradicts itself: `error` says how
};

struct Decision {
    Start start = Start::LAUNCHER;
    std::string error;
};

// argv[0] is the program. The rules are in the header comment above; a mode flag that contradicts another (--mode scene
// with --compare) is INVALID, not a guess.
Decision DecideMode(int argc, const char* const* argv);

// Whether this build has the scene renderer (main sets it from scene.h): the launcher and the Mode menu offer the mode
// only then.
void SetSceneBuilt(bool built);
bool SceneBuilt();

// The arguments, after the executable, that start a mode in a new process: --mode scene, --compare or --captures. Only the
// mode: nothing of the running mode's options goes along.
std::vector<std::string> ModeArguments(Mode mode);

}  // namespace renderer
