// The renderer's dock layout through Dear ImGui's DockBuilder (imgui_internal.h, kept out of renderer.cpp, whose
// MathLib clashes with <math.h>'s lerp).
#pragma once

#include <imgui.h>

namespace renderer {

// The default layout: the image in the central node, `right` windows as tabs on the right (`panel` of the width),
// `bottom` windows as tabs along the bottom (`bottom_height` of the height).
void BuildDockLayout(ImGuiID dock, float panel, const char* const* right, int right_count, const char* const* bottom,
                     int bottom_count, float bottom_height = 0.28f);
bool DockLayoutExists(ImGuiID dock);
// The central node's position and size in ImGui's coordinates; false without one.
bool DockCentralNode(ImGuiID dock, ImVec2& pos, ImVec2& size);

}  // namespace renderer
