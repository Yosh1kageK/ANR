// The scene renderer's stand-in (scene.h) in a build without it: the launcher and the Mode menu leave the mode out, and
// a command line that asks for it says so.

#include <cstdio>

#include "renderer/scene.h"

namespace renderer {

bool SceneInThisBuild() {
    return false;
}

SceneStatus CheckScene() {
    SceneStatus status;
    status.can_start = false;
    status.missing = "this build has no scene renderer";
    return status;
}

int RunScene(int, char**) {
    std::fprintf(stderr, "This build has no scene renderer. Use --compare or --captures, or no arguments for the launcher.\n");
    return 1;
}

}  // namespace renderer
