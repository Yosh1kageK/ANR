// The native file dialogs of the compare viewer (SDL_ShowOpenFileDialog and SDL_ShowSaveFileDialog): one at a time, never
// blocking the render loop. SDL calls the callback from another thread on some systems (the Linux portal, zenity), so it
// only queues the result; the viewer takes it in its frame loop with Take.
//
// Backends in SDL 3.4: Windows' IFileDialog, macOS' NSOpenPanel and NSSavePanel, and on Linux the XDG desktop portal (over
// DBus) or the zenity program. A system with neither (or any other failure) calls back with an error: Take returns it
// with error set, and the viewer shows a path field instead.

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace renderer::clips {

class FileDialog {
public:
    enum class Kind { VIDEO_A, VIDEO_B, PROJECT_OPEN, PROJECT_SAVE };

    struct Result {
        Kind kind = Kind::VIDEO_A;
        bool error = false;               // the dialog could not be shown or failed: `message` says why
        std::string message;
        std::vector<std::string> files;   // empty: cancelled
    };

    FileDialog();

    // Shows a dialog that starts in `folder` (and, for a save, suggests `file_name`). False when one is open already
    // (the buttons are disabled then) or when SDL fails at once (then the error result is queued as well).
    bool Show(Kind kind, SDL_Window* window, const std::string& folder, const std::string& file_name = "");
    bool busy() const;
    // The result of a finished dialog, once: from the main thread.
    std::optional<Result> Take();

private:
    struct State {
        std::mutex mutex;
        bool busy = false;
        std::vector<Result> done;
    };
    std::shared_ptr<State> state_;  // shared with a callback that may arrive after the viewer is gone
};

}  // namespace renderer::clips
