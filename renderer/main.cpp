// The renderer binary: the mode rule (modes.h) starts the launcher, the clip viewer's modes (compare, captures, the study
// viewer and the headless checks) or the scene renderer (scene.h). A mode chosen in a Mode menu or a launcher card
// restarts the program as that mode.

#include <cstdio>

#include "renderer/clips/clip_mode.h"
#include "renderer/launcher.h"
#include "renderer/mode_switch.h"
#include "renderer/modes.h"
#include "renderer/scene.h"

int main(int argc, char** argv) {
    renderer::SetSceneBuilt(renderer::SceneInThisBuild());
    // No arguments open the launcher; --mode NAME, --compare, --captures and --study pick one; any other argument is the
    // scene renderer's.
    const renderer::Decision decision = renderer::DecideMode(argc, argv);
    int code = 0;
    switch (decision.start) {
        case renderer::Start::INVALID:
            std::fprintf(stderr, "%s\n", decision.error.c_str());
            return 1;
        case renderer::Start::LAUNCHER:
            code = renderer::RunLauncher(argc, argv);
            break;
        case renderer::Start::COMPARE:
        case renderer::Start::CAPTURES:
        case renderer::Start::COMPARE_TOOL:
        case renderer::Start::STUDY:
            // The clip viewer's modes (renderer/clips, README.md "Compare mode"): --compare, --captures, --compare-check,
            // --compare-export, --study, --render-test. They load no scene, models or DLSS.
            code = renderer::clips::RunClipMode(argc, argv);
            break;
        case renderer::Start::SCENE:
            code = renderer::RunScene(argc, argv);
            break;
    }
    // A Mode menu or a launcher card chose a mode: the window and the device are gone, so the process becomes that mode
    // (exec, so that `bazel run` keeps waiting for it), or on Windows a new process starts and this one ends.
    if (renderer::RequestedMode()) {
        const int failed = renderer::StartRequestedMode();
        return failed ? failed : code;
    }
    return code;
}
