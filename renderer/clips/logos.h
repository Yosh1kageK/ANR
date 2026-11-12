// Logos on the exported video (and on the live view to judge them): the settings kept in the project, where each logo
// goes in a frame of a given size, and the resampling of the logo's bitmap to exactly that size. Pure (no GPU, no OS
// apart from the files the loader reads), tested by logos_test.cpp. The drawing is overlay.cpp's, with the bitmap as an
// ImGui texture.
//
// A logo is placed at an output corner: its height is a fraction of the output height, the margin to the frame's edge a
// fraction of the output height too. The bitmap is resampled once to the size it has on the screen (premultiplied
// alpha, a Lanczos filter widened by the reduction, so a 4K logo turned to 50 pixels loses no more than a
// box average would) and drawn at whole pixels with one texel per pixel, so it is the same in every frame and neither
// shimmers nor aliases.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace renderer::clips {

enum class Corner { BOTTOM_LEFT, BOTTOM_RIGHT, TOP_LEFT, TOP_RIGHT };

const char* corner_name(Corner corner);                         // "bottom-left", ...
bool parse_corner(const std::string& name, Corner& corner);

inline constexpr double LOGO_HEIGHT = 0.05;   // of the output height
inline constexpr double LOGO_MARGIN = 0.02;   // of the output height
inline constexpr double LOGO_OPACITY = 0.9;

struct LogoItem {
    std::string id;
    std::string path;                 // a file name looked for in the logo folders, or a path
    Corner corner = Corner::BOTTOM_LEFT;
    double height = LOGO_HEIGHT;
    double margin = LOGO_MARGIN;
    double opacity = LOGO_OPACITY;
    bool backdrop = false;            // a translucent dark rounded rectangle behind it
};

// The project's "logos": `enabled` is unset until the box is touched (then the logos are on when their files exist),
// `items` are the defaults until the project stores its own.
struct LogoSettings {
    std::optional<bool> enabled;
    std::vector<LogoItem> items;      // meaningful when has_items
    bool has_items = false;           // the project lists its logos (else the defaults apply)
    bool stored = false;              // the project has (or gets) a "logos" object
    bool items_dirty = false;         // the items were edited: written back

    std::vector<LogoItem> effective() const;
    // The items to edit: the effective ones, now the project's own.
    std::vector<LogoItem>& edit();
};

// Apex bottom-left (A, the Apex side, is left of the divider), NVIDIA bottom-right (B, the DLSS-G side, is right of it).
std::vector<LogoItem> default_logos();

struct IntRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool intersects(const IntRect& o) const { return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
    bool operator==(const IntRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct LogoPlace {
    IntRect logo;                     // the bitmap's pixels in the output
    IntRect box;                      // the backdrop's (the logo's when there is none)
    bool backdrop = false;
};

// Where a logo goes in an output of out_w x out_h pixels: the height from the fraction, the width from the bitmap's
// aspect (width / height), both rounded to pixels; the margin from the frame's edge to the logo (to the backdrop when
// there is one). A logo that would cover one of `avoid` (the caption and the labels' pills) moves along the frame's
// edge direction (up from the bottom, down from the top) until it is clear of them.
LogoPlace place_logo(const LogoItem& item, int out_w, int out_h, double aspect, const std::vector<IntRect>& avoid = {});

struct LogoImage {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;        // straight alpha, w * h * 4
};

// The bitmap at w x h pixels: resampled with premultiplied alpha, Lanczos (a = 3) widened by the reduction.
LogoImage resample_logo(const LogoImage& source, int w, int h);

// A logo's file: an absolute or ~ path as it is, else the first folder of `folders` that holds it. Empty: not found.
std::string find_logo_file(const std::string& path, const std::vector<std::string>& folders);

// The folders logos are looked for in: NR_LOGO_DIR, paths::LogoFolders(), then the project's folder.
std::vector<std::string> logo_folders(const std::string& project_folder);
// Those folders as one line for the UI (without the project's).
std::string LogoFolderText();

// Reads a PNG (or another stb_image format) as straight RGBA8. False: why not.
bool load_logo_image(const std::string& path, LogoImage& image, std::string& error);

}  // namespace renderer::clips
