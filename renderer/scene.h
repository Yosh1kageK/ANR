// The scene renderer (renderer/README.md "Scene renderer"), where this build has it: a glTF scene with live frame
// generation. main.cpp starts it; the launcher shows whether it can start. A build without it links a stand-in that
// says so.

#pragma once

#include <string>
#include <vector>

namespace renderer {

// Whether this build has the scene renderer at all.
bool SceneInThisBuild();

// What the launcher's card says, looked up without loading anything: whether it can start (else `missing` says why) and
// what it runs without (`notes`).
struct SceneStatus {
    bool can_start = true;
    std::string missing;
    std::vector<std::string> notes;
};
SceneStatus CheckScene();

// The scene renderer's window and frame loop, until it is closed; the exit code.
int RunScene(int argc, char** argv);

}  // namespace renderer
