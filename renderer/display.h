// The display code the renderer (renderer.cpp) and the clip viewer (clips/) share: the swap chain encodings and
// their names, the HDR surface query, the display's peak from its EDID (Linux), the paper white and peak defaults, and
// the docked UI's style. Extracted from renderer.cpp; README.md "Display and HDR" says what each does.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <NRIFramework.h>
#include <imgui.h>

namespace renderer {

class App;

// Display encodings (display_common.slang ENCODING_*, in the order of the frame generation display protocol's encodings):
// the swap chain's, the scene renderer's frame generation images and the compare views.
enum Encoding : uint32_t { SRGB8, PQ10, SCRGB16, ENCODINGS };
inline const char* const ENCODING_NAMES[ENCODINGS] = {"srgb8", "pq10", "scrgb16"};
inline const char* const ENCODING_LABELS[ENCODINGS] = {"sRGB 8-bit", "PQ 10-bit", "scRGB FP16"};
inline const char* const SWAP_NAMES[ENCODINGS] = {"sdr", "hdr10", "scrgb"};
inline const char* const SWAP_LABELS[ENCODINGS] = {"SDR (sRGB 8-bit)", "HDR10 (PQ 10-bit, Rec. 2020)",
                                                   "scRGB (FP16, linear Rec. 709)"};
// The swap chain's VkColorSpaceKHR per mode: SRGB_NONLINEAR, HDR10_ST2084, EXTENDED_SRGB_LINEAR (NRI picks no other for
// the HDR modes: third_party/nri_hdr.patch)
inline constexpr uint32_t SWAP_COLOR_SPACES[ENCODINGS] = {0, 1000104008, 1000104002};
inline constexpr float SCRGB_NITS = 80.0f;  // scRGB 1.0

inline nri::Format EncodingFormat(uint32_t e) {
    return e == PQ10 ? nri::Format::R10_G10_B10_A2_UNORM : e == SCRGB16 ? nri::Format::RGBA16_SFLOAT : nri::Format::RGBA8_UNORM;
}

inline uint32_t EncodingVkFormat(uint32_t e) {  // VK_FORMAT_A2B10G10R10_UNORM_PACK32, R16G16B16A16_SFLOAT, R8G8B8A8_UNORM
    return e == PQ10 ? 64 : e == SCRGB16 ? 97 : 37;
}

inline uint32_t EncodingBytes(uint32_t e) {
    return e == SCRGB16 ? 8 : 4;
}

inline int EncodingByName(const std::string& name, const char* const* names) {
    for (int i = 0; i < (int)ENCODINGS; i++)
        if (name == names[i])
            return i;
    return -1;
}

// The connected monitors' EDID HDR static metadata (CTA-861 data block, extended tag 6): the desired content maximum
// luminance in nits, by the monitor's name (display descriptor 0xFC). Linux only (empty elsewhere).
std::vector<std::pair<std::string, float>> EdidPeaks();

// Which HDR swap chains a window's surface offers, from a Vulkan surface of our own made before NRI creates its swap
// chain. instance, physical_device and get_instance_proc are nr::VulkanDevice's handles. False when the surface could
// not be queried (SDR only).
bool QueryHdrSurface(const nri::Window& window, void* instance, void* physical_device, void* get_instance_proc,
                     bool& pq10, bool& scrgb);

// Paper white and peak in nits, and what the display reports.
struct HdrState {
    float paper_white = 203.0f, peak = 1000.0f;
    bool paper_white_set = false, peak_set = false;  // by --paper-white, --peak or the UI
    float reported_white = 0.0f, reported_peak = 0.0f;
    std::string peak_source;  // where reported_peak comes from
};

// The display's peak from the EDID of the monitor the window is on (Linux), sets reported_peak and peak_source.
void ReadEdidPeak(SDL_Window* window, HdrState& state);

// Paper white and peak, unless set: the display's where it reports them, else 203 and 1000. PQ on Linux and macOS takes
// 203 nits, the reference white the compositor maps to the desktop's SDR white (Wayland's color management: PQ's default
// reference luminance). scRGB on macOS: EDR's 1.0 is the system's SDR white (SDL reports 1.0, 80 nits). absolute_pq:
// the swap chain (or the frames) are PQ.
void UpdateHdrDefaults(SDL_Window* window, HdrState& state, bool absolute_pq);

// The docked UI's fonts and style (the renderer's look), in `base` unscaled; mono is the monospace font where one is
// found. Call after the ImGui context exists.
void SetupUiStyle(ImGuiStyle& base, ImFont*& mono);

// The UI scale that follows the window: 1x for a 1080-line window, up to 1.5x on a 4K monitor when the window is as large,
// times the OS's scale.
float AutoUiScale(const App& app);

}  // namespace renderer
