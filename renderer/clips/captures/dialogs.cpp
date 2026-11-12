#include "renderer/clips/captures/dialogs.h"

#include <SDL3/SDL.h>

namespace renderer::clips::captures {
namespace {

const SDL_DialogFileFilter IMAGE_FILTERS[] = {{"PNG images", "png"}, {"All files", "*"}};
const SDL_DialogFileFilter DLL_FILTERS[] = {{"DLLs", "dll"}, {"All files", "*"}};

struct Pending {
    std::shared_ptr<void> state;
    Dialogs::Kind kind;
};

}  // namespace

Dialogs::Dialogs() : state_(std::make_shared<State>()) {}

bool Dialogs::busy() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->busy;
}

std::optional<Dialogs::Result> Dialogs::Take() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::optional<Result> result = std::move(state_->done);
    state_->done.reset();
    return result;
}

bool Dialogs::Show(Kind kind, SDL_Window* window, const std::string& location) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->busy)
            return false;
        state_->busy = true;
    }
    auto* pending = new Pending{state_, kind};
    auto callback = [](void* userdata, const char* const* files, int) {
        std::unique_ptr<Pending> p(static_cast<Pending*>(userdata));
        auto state = std::static_pointer_cast<State>(p->state);
        Result result;
        result.kind = p->kind;
        if (!files) {
            result.error = true;
            result.message = SDL_GetError();
        } else if (*files) {
            result.path = *files;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done = std::move(result);
        state->busy = false;
    };
    const char* where = location.empty() ? nullptr : location.c_str();
    if (kind == Kind::SAVE_IMAGE)
        SDL_ShowSaveFileDialog(callback, pending, window, IMAGE_FILTERS, 2, where);
    else if (kind == Kind::NVIDIA_DLL || kind == Kind::PROVIDER_DLL)
        SDL_ShowOpenFileDialog(callback, pending, window, DLL_FILTERS, 2, where, false);
    else
        SDL_ShowOpenFolderDialog(callback, pending, window, where, false);
    return true;
}

}  // namespace renderer::clips::captures
