// Changing mode while the renderer runs (renderer/README.md "Modes"): the launcher's cards and every mode's Mode menu come
// here. A mode switch is a restart of the same executable with only the new mode's flag, so the old mode's window, device
// and state are gone before the new one starts:
//
//   1. the app calls RequestMode(mode) and ends its frame loop (it closes its window and destroys its device as on any quit);
//   2. main sees RequestedMode() and calls StartRequestedMode(): on Linux and macOS it replaces the process (exec), so
//      `bazel run` keeps waiting for the new mode; on Windows it starts a new process and the old one exits.
//
// The last mode chosen is remembered in paths::State()'s last-mode (the launcher highlights it).

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "renderer/modes.h"

namespace renderer {

// The mode last chosen in the launcher or a Mode menu (the scene renderer when none was).
Mode LastMode();
void RememberMode(Mode mode);

// Asks to start `mode` once this one ends. False, with the reason, where the renderer's own executable cannot be found (it
// is checked here, before the window goes). On true the caller ends its loop and returns from main.
bool RequestMode(Mode mode, std::string& error);
std::optional<Mode> RequestedMode();
// Starts the requested mode (see above). Returns only when it could not (the exit code, after a message on stderr) or on
// Windows, where it returns 0 once the new process runs.
int StartRequestedMode();

// The Mode menu: "Mode" in the main menu bar with the three modes, the current one ticked. `loses(target)` says what
// switching to `target` would end that is not saved ("A video export is running"), or is empty. The chosen mode comes back
// at once when nothing is lost, else after DrawModeConfirm's window was answered.
std::optional<Mode> DrawModeMenu(Mode current, const std::function<std::string(Mode)>& loses = nullptr);
// The confirmation window of a switch that loses something; once a frame, outside the menus.
std::optional<Mode> DrawModeConfirm();
// A confirmation is on screen: the app's Esc and other keys wait.
bool ModeConfirmOpen();

}  // namespace renderer
