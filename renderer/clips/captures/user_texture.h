// An ImGui texture the application owns (a thumbnail): made from RGBA8 pixels and registered with ImGui, which tells its
// renderer backends to create the GPU copy. Kept apart from the studio: imgui_internal.h and the NRI headers do not mix
// (MathLib's lerp clashes with <math.h>'s).

#pragma once

#include <cstdint>

struct ImTextureData;

namespace renderer::clips::captures {

// A texture of width x height RGBA8 pixels; never released (the thumbnails live as long as the window).
ImTextureData* MakeUserTexture(int width, int height, const uint8_t* rgba);

}  // namespace renderer::clips::captures
