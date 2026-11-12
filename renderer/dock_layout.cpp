#include "renderer/dock_layout.h"

#include <imgui_internal.h>

namespace renderer {

void BuildDockLayout(ImGuiID dock, float panel, const char* const* right, int right_count, const char* const* bottom,
                     int bottom_count, float bottom_height) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dock, vp->WorkSize);
    ImGuiID centre = dock;
    const ImGuiID right_node = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, panel, nullptr, &centre);
    const ImGuiID bottom_node = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, bottom_height, nullptr, &centre);
    for (int i = 0; i < right_count; i++)
        ImGui::DockBuilderDockWindow(right[i], right_node);
    for (int i = 0; i < bottom_count; i++)
        ImGui::DockBuilderDockWindow(bottom[i], bottom_node);
    ImGui::DockBuilderFinish(dock);
}

bool DockLayoutExists(ImGuiID dock) {
    return ImGui::DockBuilderGetNode(dock) != nullptr;
}

bool DockCentralNode(ImGuiID dock, ImVec2& pos, ImVec2& size) {
    const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock);
    if (!central)
        return false;
    pos = central->Pos;
    size = central->Size;
    return true;
}

}  // namespace renderer
