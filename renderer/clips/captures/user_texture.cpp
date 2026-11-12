#include "renderer/clips/captures/user_texture.h"

#include <cstring>

#include <imgui_internal.h>  // RegisterUserTexture: how ImGui is told about a texture the application owns

namespace renderer::clips::captures {

ImTextureData* MakeUserTexture(int width, int height, const uint8_t* rgba) {
    static int id = 2000;  // logo_layer.cpp's ids cycle through 1 to 1000
    ImTextureData* texture = IM_NEW(ImTextureData)();
    texture->Create(ImTextureFormat_RGBA32, width, height);
    std::memcpy(texture->Pixels, rgba, (size_t)width * height * 4);
    texture->UseColors = true;
    texture->UsedRect.x = texture->UsedRect.y = 0;
    texture->UsedRect.w = (unsigned short)width;
    texture->UsedRect.h = (unsigned short)height;
    texture->UniqueID = ++id;
    ImGui::RegisterUserTexture(texture);
    return texture;
}

}  // namespace renderer::clips::captures
