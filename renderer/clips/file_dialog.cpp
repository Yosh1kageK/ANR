#include "renderer/clips/file_dialog.h"

#include <SDL3/SDL.h>

#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace {

// SDL keeps the filters until the callback runs: static storage. The patterns are extensions without dots (a "." inside
// one, as in compare.json, is allowed and matches the double extension where the system filters by glob).
const SDL_DialogFileFilter VIDEO_FILTERS[] = {{"Video files", "mp4;mov;m4v;mkv;webm;avi"}, {"All files", "*"}};
const SDL_DialogFileFilter PROJECT_FILTERS[] = {
    {"Compare projects (*.compare.json)", "compare.json"}, {"JSON files", "json"}, {"All files", "*"}};

struct Pending {
    std::shared_ptr<void> state;  // FileDialog::State, kept alive until the callback
    FileDialog::Kind kind;
};

}  // namespace

FileDialog::FileDialog() : state_(std::make_shared<State>()) {}

bool FileDialog::busy() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->busy;
}

std::optional<FileDialog::Result> FileDialog::Take() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->done.empty())
        return std::nullopt;
    Result result = std::move(state_->done.front());
    state_->done.erase(state_->done.begin());
    return result;
}

bool FileDialog::Show(Kind kind, SDL_Window* window, const std::string& folder, const std::string& file_name) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->busy)
            return false;
        state_->busy = true;  // before SDL runs: it may call back at once, from this thread
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
        } else {
            for (; *files; files++)
                result.files.push_back(*files);
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done.push_back(std::move(result));
        state->busy = false;
    };
    // A save dialog starts at a file (the folder and the suggested name), an open one at the folder.
    std::string location = folder;
    if (kind == Kind::PROJECT_SAVE && !file_name.empty())
        location = folder.empty() ? file_name : folder + "/" + file_name;
    const char* where = location.empty() ? nullptr : location.c_str();
    switch (kind) {
        case Kind::VIDEO_A:
        case Kind::VIDEO_B:
            SDL_ShowOpenFileDialog(callback, pending, window, VIDEO_FILTERS, 2, where, false);
            break;
        case Kind::PROJECT_OPEN:
            SDL_ShowOpenFileDialog(callback, pending, window, PROJECT_FILTERS, 3, where, false);
            break;
        case Kind::PROJECT_SAVE:
            SDL_ShowSaveFileDialog(callback, pending, window, PROJECT_FILTERS, 3, where);
            break;
    }
    return true;
}

}  // namespace renderer::clips
