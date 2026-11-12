// What is drawn on the pictures besides them: the labels of A and B, the divider line, the caption and zoom of the shown
// area, and (in the window only) the areas' outlines. Written with ImDrawList so the window (ImGui's background list)
// and the export (a list of its own, drawn into the offscreen target) draw it with the same code, like `decorate` in
// the compare tool's earlier Python version.

#pragma once

#include <string>
#include <vector>

#include <imgui.h>
#include "renderer/clips/camera.h"
#include "renderer/clips/logos.h"
#include "renderer/clips/view_math.h"

namespace renderer::clips {

struct OverlayInput {
    Layout layout = Layout::slider;
    Rect view;                  // the pictures' rectangle in the target (the whole target for an export)
    CompareLayout areas;        // the view areas of A and B
    Mapping mapping[2];         // the camera's mapping into each area
    int video_w = 0, video_h = 0;
    std::string label_a, label_b;
    double divider_x = 0.0;     // slider: screen column of the divider
    std::string caption;        // the shown area's label
    double zoom = 1.0;
    bool no_a = false, no_b = false;  // no recording of it at this time
    float size = 15.0f;         // font size in target pixels
    float line = 2.0f;          // divider line half width
};

// A pill: rounded dark translucent rectangle with white text. Anchored at its left (or right) edge. Returns its size.
ImVec2 DrawPill(ImDrawList* list, ImFont* font, float size, ImVec2 position, const std::string& text, bool right_align);

// Labels, divider and caption (everything an export shows). `pills`, when given, gets the rectangle of each label and
// caption pill in the target's pixels (what the logos keep clear of).
void DrawVideoOverlay(ImDrawList* list, ImFont* font, const OverlayInput& in, std::vector<IntRect>* pills = nullptr);

// The outline of an area in a view area's mapping, with its label above; selected ones thick. alpha (0 to 1) scales the
// outline's, the label's and its shadow's opacity (a mark fading in or out, mark_visibility.h).
void DrawAreaOutline(ImDrawList* list, ImFont* font, float size, const Mapping& mapping, int video_w, int video_h,
                     const Area& area, ImU32 color, bool selected, float alpha = 1.0f);
// A rectangle in normalized coordinates (being dragged).
void DrawNormalizedRect(ImDrawList* list, const Mapping& mapping, int video_w, int video_h, const double rect[4],
                        ImU32 color, bool selected);

// The colors of the areas (by their order in the list).
ImU32 AreaColor(size_t index);

// The visible part of a recording inside a view area, in the target's pixels: where its top-left corner is (the labels
// sit there).
ImVec2 PictureCorner(const Rect& area, const Mapping& mapping, int video_w, int video_h);

}  // namespace renderer::clips
