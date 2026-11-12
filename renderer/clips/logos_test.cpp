// The logos' placement (corners, margins, scale, the pills they keep clear of, both layouts), the resampling of their
// bitmaps and the file lookup.
#include "renderer/clips/logos.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

LogoItem item(Corner corner, double height = LOGO_HEIGHT, double margin = LOGO_MARGIN) {
    LogoItem i;
    i.corner = corner;
    i.height = height;
    i.margin = margin;
    return i;
}

void corners_and_scale() {
    // 1920x1080, a logo of aspect 3.4: height 5 % = 54, width 184, margin 2 % = 22 (21.6 rounded)
    const double aspect = 3.4;
    LogoPlace p = place_logo(item(Corner::BOTTOM_LEFT), 1920, 1080, aspect);
    CHECK(p.logo == (IntRect{22, 1080 - 22 - 54, 184, 54}));
    CHECK(p.box == p.logo && !p.backdrop);
    p = place_logo(item(Corner::BOTTOM_RIGHT), 1920, 1080, aspect);
    CHECK(p.logo == (IntRect{1920 - 22 - 184, 1080 - 22 - 54, 184, 54}));
    p = place_logo(item(Corner::TOP_LEFT), 1920, 1080, aspect);
    CHECK(p.logo == (IntRect{22, 22, 184, 54}));
    p = place_logo(item(Corner::TOP_RIGHT), 1920, 1080, aspect);
    CHECK(p.logo == (IntRect{1920 - 22 - 184, 22, 184, 54}));
    // it scales with the height: 4K doubles every number, 720p takes two thirds
    p = place_logo(item(Corner::BOTTOM_LEFT), 3840, 2160, aspect);
    CHECK(p.logo == (IntRect{43, 2160 - 43 - 108, 367, 108}));
    p = place_logo(item(Corner::BOTTOM_LEFT), 1280, 720, aspect);
    CHECK(p.logo == (IntRect{14, 720 - 14 - 36, 122, 36}));
    // its own height and margin
    p = place_logo(item(Corner::BOTTOM_RIGHT, 0.1, 0.05), 1920, 1080, 6.0);
    CHECK(p.logo == (IntRect{1920 - 54 - 648, 1080 - 54 - 108, 648, 108}));
    // inside the frame at every corner and size
    for (Corner c : {Corner::BOTTOM_LEFT, Corner::BOTTOM_RIGHT, Corner::TOP_LEFT, Corner::TOP_RIGHT})
        for (int h : {360, 540, 720, 1080, 1440, 2160, 4320}) {
            const int w = h * 16 / 9;
            const LogoPlace q = place_logo(item(c), w, h, 6.1);
            CHECK(q.logo.x >= 0 && q.logo.y >= 0 && q.logo.x + q.logo.w <= w && q.logo.y + q.logo.h <= h);
        }
}

void backdrop() {
    LogoItem i = item(Corner::BOTTOM_LEFT);
    i.backdrop = true;
    const LogoPlace p = place_logo(i, 1920, 1080, 3.4);
    CHECK(p.backdrop);
    const int pad = 16;  // 30 % of 54
    CHECK(p.box == (IntRect{22, 1080 - 22 - 54 - 2 * pad, 184 + 2 * pad, 54 + 2 * pad}));  // the margin is to the backdrop
    CHECK(p.logo == (IntRect{22 + pad, p.box.y + pad, 184, 54}));
}

void clear_of_the_pills() {
    // The overlay's pills at 1080p: labels at the top corners, a caption centred at the bottom.
    const IntRect label_a = {40, 40, 120, 35}, label_b = {1920 - 40 - 100, 40, 100, 35};
    const IntRect caption = {960 - 150, 1080 - 40 - 35, 300, 35};
    const std::vector<IntRect> slider = {label_a, label_b, caption};
    // slider and side by side: the bottom logos are clear of a short caption, where they are without it
    for (Corner c : {Corner::BOTTOM_LEFT, Corner::BOTTOM_RIGHT}) {
        CHECK(place_logo(item(c), 1920, 1080, 3.4, slider).logo == place_logo(item(c), 1920, 1080, 3.4).logo);
    }
    // side by side: B's label sits at the second panel's corner
    const std::vector<IntRect> sbs = {label_a, {960 + 40, 40, 100, 35}, caption};
    CHECK(place_logo(item(Corner::BOTTOM_RIGHT), 1920, 1080, 6.1, sbs).logo == place_logo(item(Corner::BOTTOM_RIGHT), 1920, 1080, 6.1).logo);
    // a long caption reaches into the corners: the logos go up above it, clear of it
    const IntRect wide = {200, 1080 - 40 - 35, 1520, 35};
    for (Corner c : {Corner::BOTTOM_LEFT, Corner::BOTTOM_RIGHT}) {
        const LogoPlace p = place_logo(item(c), 1920, 1080, 6.1, {wide});
        CHECK(!p.box.intersects(wide));
        CHECK(p.box.y + p.box.h <= wide.y);
        CHECK(p.box.x == place_logo(item(c), 1920, 1080, 6.1).box.x);  // along the vertical only
    }
    // a logo at a top corner is moved below the label that sits there
    const LogoPlace top = place_logo(item(Corner::TOP_LEFT), 1920, 1080, 3.4, slider);
    CHECK(!top.box.intersects(label_a));
    CHECK(top.box.y >= label_a.y + label_a.h);
    CHECK(place_logo(item(Corner::TOP_RIGHT), 1920, 1080, 3.4, slider).box.y >= label_b.y + label_b.h);
    // two pills one over the other: clear of both
    const IntRect high = {200, 1080 - 130, 1520, 35};
    const LogoPlace both = place_logo(item(Corner::BOTTOM_LEFT), 1920, 1080, 3.4, {wide, high});
    CHECK(!both.box.intersects(wide) && !both.box.intersects(high));
}

LogoImage solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    LogoImage image;
    image.w = w;
    image.h = h;
    image.rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        image.rgba[i * 4 + 0] = r;
        image.rgba[i * 4 + 1] = g;
        image.rgba[i * 4 + 2] = b;
        image.rgba[i * 4 + 3] = a;
    }
    return image;
}

void resampling() {
    // a flat image stays flat at any size, up or down
    for (auto [w, h] : {std::pair<int, int>{7, 5}, {64, 64}, {200, 30}, {13, 77}}) {
        const LogoImage out = resample_logo(solid(97, 41, 200, 100, 50, 180), w, h);
        CHECK_EQ(out.w, w);
        CHECK_EQ(out.h, h);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            CHECK_EQ(out.rgba[i * 4 + 0], 200);
            CHECK_EQ(out.rgba[i * 4 + 1], 100);
            CHECK_EQ(out.rgba[i * 4 + 2], 50);
            CHECK_EQ(out.rgba[i * 4 + 3], 180);
        }
    }
    // the same size gives the image back
    LogoImage ramp = solid(16, 4, 255, 255, 255, 255);
    for (int x = 0; x < 16; x++)
        for (int y = 0; y < 4; y++)
            ramp.rgba[((size_t)y * 16 + x) * 4 + 3] = (uint8_t)(x * 17);
    const LogoImage same = resample_logo(ramp, 16, 4);
    for (size_t i = 0; i < 16 * 4; i++)
        CHECK(std::abs((int)same.rgba[i * 4 + 3] - (int)ramp.rgba[i * 4 + 3]) <= 1);
    // a 1-pixel checkerboard reduced by 16 is the mean, with no aliasing pattern left (no pixel far from 50 %; the
    // border is skipped: it repeats the edge pixel)
    LogoImage checker = solid(256, 256, 255, 255, 255, 0);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++)
            checker.rgba[((size_t)y * 256 + x) * 4 + 3] = ((x + y) & 1) ? 255 : 0;
    const LogoImage small = resample_logo(checker, 16, 16);
    for (int y = 3; y < 13; y++)
        for (int x = 3; x < 13; x++) {
            const size_t i = (size_t)y * 16 + x;
            CHECK(std::abs((int)small.rgba[i * 4 + 3] - 128) <= 8);
            CHECK_EQ(small.rgba[i * 4 + 0], 255);  // straight colour stays white: the alpha carries the coverage
        }
    // a white disc on a transparent ground: white where there is coverage, alpha between 0 and 255 at the edge, never
    // above the source's range (no overshoot from the filter's negative lobes)
    LogoImage disc = solid(400, 400, 255, 255, 255, 0);
    for (int y = 0; y < 400; y++)
        for (int x = 0; x < 400; x++)
            if ((x - 200) * (x - 200) + (y - 200) * (y - 200) < 150 * 150)
                disc.rgba[((size_t)y * 400 + x) * 4 + 3] = 255;
    const LogoImage ds = resample_logo(disc, 40, 40);
    int partial = 0;
    for (size_t i = 0; i < 40 * 40; i++) {
        const uint8_t a = ds.rgba[i * 4 + 3];
        if (a > 0)
            CHECK_EQ(ds.rgba[i * 4 + 0], 255);
        if (a > 5 && a < 250)
            partial++;
    }
    CHECK(partial > 20 && partial < 400);
    CHECK_EQ(ds.rgba[((size_t)20 * 40 + 20) * 4 + 3], 255);
    CHECK_EQ(ds.rgba[3], 0);
}

void files() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "nr-clips-logos-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "one");
    fs::create_directories(dir / "two");
    std::ofstream((dir / "two" / "apex.png").string()) << "x";
    std::ofstream((dir / "one" / "other.png").string()) << "x";
    const std::vector<std::string> folders = {(dir / "one").string(), (dir / "two").string()};
    CHECK(find_logo_file("apex.png", folders) == (dir / "two" / "apex.png").string());
    CHECK(find_logo_file("other.png", folders) == (dir / "one" / "other.png").string());
    CHECK(find_logo_file("missing.png", folders).empty());
    CHECK(find_logo_file((dir / "two" / "apex.png").string(), {}) == (dir / "two" / "apex.png").string());
    CHECK(find_logo_file((dir / "two" / "none.png").string(), folders).empty());
    CHECK(find_logo_file("", folders).empty());
    CHECK(!logo_folders("/p").empty() && logo_folders("/p").back() == "/p");
    fs::remove_all(dir);
    Corner c = Corner::BOTTOM_LEFT;
    CHECK(parse_corner("top-right", c) && c == Corner::TOP_RIGHT);
    CHECK(!parse_corner("middle", c));
    const std::vector<LogoItem> defaults = default_logos();
    CHECK_EQ(defaults.size(), (size_t)2);
    CHECK(defaults[0].id == "apex" && defaults[0].corner == Corner::BOTTOM_LEFT);
    CHECK(defaults[1].id == "nvidia" && defaults[1].corner == Corner::BOTTOM_RIGHT);
    CHECK_NEAR(defaults[0].height, 0.05, 1e-12);
    CHECK_NEAR(defaults[0].margin, 0.02, 1e-12);
    CHECK_NEAR(defaults[0].opacity, 0.9, 1e-12);
    CHECK(!defaults[0].backdrop);
}

}  // namespace

int main() {
    corners_and_scale();
    backdrop();
    clear_of_the_pills();
    resampling();
    files();
    return check_result("logos");
}
