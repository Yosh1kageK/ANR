// The Captures tab's native dialogs (SDL_ShowOpenFolderDialog, SDL_ShowOpenFileDialog, SDL_ShowSaveFileDialog): a folder of
// captures, a generator's folder or DLL, a file name for a saved image, a folder for "Save all". One at a time; the result is
// taken in the frame loop (SDL may call back from another thread). A failure to show a dialog comes back as an error
// result: the UI then offers a path field.

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct SDL_Window;

namespace renderer::clips::captures {

class Dialogs {
public:
    enum class Kind { CAPTURE_FOLDER, EXPORT_FOLDER, NVIDIA_DLL, PROVIDER_DLL, SAVE_IMAGE, SAVE_FOLDER };
    struct Result {
        Kind kind = Kind::CAPTURE_FOLDER;
        bool error = false;
        std::string message;
        std::string path;  // empty: cancelled
    };

    Dialogs();
    bool Show(Kind kind, SDL_Window* window, const std::string& location);
    bool busy() const;
    std::optional<Result> Take();

private:
    struct State {
        std::mutex mutex;
        bool busy = false;
        std::optional<Result> done;
    };
    std::shared_ptr<State> state_;
};

}  // namespace renderer::clips::captures
