// The mode launcher (renderer/README.md "Modes"): what `bazel run //renderer` opens with no arguments. A small window with
// a card for each mode (the scene renderer, compare videos, browse captures), each with what it is and whether what it needs
// is here. It loads no scene, models, DLSS or decoder: only the window, a device to draw with and the UI. Choosing a mode
// (a click, Enter on the highlighted card, 1, 2 or 3) ends the launcher; main then starts the mode (mode_switch.h).

#pragma once

namespace renderer {

// Runs the launcher until it is closed or a mode was chosen. Options it does not know (a scene renderer's) are ignored.
// 0 on a normal end; RequestedMode() says which mode was chosen.
int RunLauncher(int argc, char** argv);

}  // namespace renderer
