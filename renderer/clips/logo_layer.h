// The logos as ImGui textures: the bitmaps resampled to the size they have in the frame (logos.h), drawn at whole pixels,
// one texel per pixel, with the same code in the window (the background draw list) and in the export (its own list).
// A texture is an ImTextureData the renderer backends upload; one that is no longer wanted is let go through ImGui's
// own protocol (WantDestroyNextFrame, then freed once the backends are done: CollectRetired, once per UI frame).

#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>
#include "renderer/clips/logos.h"

namespace renderer::clips {

class LogoLayer {
public:
    LogoLayer() = default;
    LogoLayer(const LogoLayer&) = delete;
    LogoLayer& operator=(const LogoLayer&) = delete;
    ~LogoLayer();

    // The logos wanted for a frame of out_w x out_h pixels (none: empty items). Files are looked for in `folders`; a file
    // that is missing or unreadable is logged once and left out. The bitmaps are made at the size the frame needs and
    // kept while it stays; with `settle` > 0 a change of size waits that many calls (frames) for the size to stay, and
    // the old bitmaps are drawn scaled until then (a window being resized).
    void Set(const std::vector<LogoItem>& items, const std::vector<std::string>& folders, int out_w, int out_h, int settle = 0);

    bool empty() const;
    // How many logos will be drawn.
    int count() const;
    // The folders and file names that were not found, for a message (one line each).
    const std::vector<std::string>& problems() const { return problems_; }

    // Draws them with the frame's origin at `origin` (in the target's pixels); `pills` are the label and caption pills in
    // frame pixels that they keep clear of.
    void Draw(ImDrawList* list, ImVec2 origin, const std::vector<IntRect>& pills) const;

    // Frees textures the backends have let go of. Call once per UI frame.
    static void CollectRetired();

private:
    struct Entry {
        LogoItem item;
        ImTextureData* texture = nullptr;
        int w = 0, h = 0;             // the bitmap's size
        double aspect = 1.0;
        std::string key;              // file, size
    };
    ImTextureData* MakeTexture(const LogoImage& image);

    std::vector<Entry> entries_;
    std::map<std::string, std::shared_ptr<LogoImage>> sources_;
    std::vector<std::string> problems_;
    std::string signature_;              // the items and folders the bitmaps were made for
    int frame_w_ = 0, frame_h_ = 0;      // the frame's size now (what Draw places by)
    int built_w_ = 0, built_h_ = 0;      // the size the bitmaps were made for
    int pending_w_ = 0, pending_h_ = 0, pending_calls_ = 0;
};

}  // namespace renderer::clips
