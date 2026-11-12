#include "renderer/display.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#ifndef __APPLE__
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>  // not vulkan.h: its Xlib header defines DisplayHeight, an App method
#endif

#include "renderer/app.h"
#include <spdlog/spdlog.h>

namespace renderer {

#ifdef __linux__
std::vector<std::pair<std::string, float>> EdidPeaks() {
    std::vector<std::pair<std::string, float>> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
        std::ifstream status(entry.path() / "status");
        std::string state;
        if (!(status >> state) || state != "connected")
            continue;
        std::ifstream file(entry.path() / "edid", std::ios::binary);
        const std::vector<uint8_t> e((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (e.size() < 256)
            continue;
        std::string name;
        for (int d = 0; d < 4; d++) {
            const uint8_t* b = e.data() + 54 + 18 * d;
            if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0xFC)
                for (int i = 5; i < 18 && b[i] != '\n'; i++)
                    name += (char)b[i];
        }
        float peak = 0.0f;
        for (size_t block = 128; block + 128 <= e.size(); block += 128) {
            const uint8_t* x = e.data() + block;
            if (x[0] != 0x02)
                continue;
            for (uint32_t i = 4; i < x[2] && i < 127; i += (x[i] & 31u) + 1) {
                const uint32_t tag = x[i] >> 5, length = x[i] & 31u;
                if (tag == 7 && length >= 4 && i + 4 < 128 && x[i + 1] == 6)
                    peak = 50.0f * std::pow(2.0f, (float)x[i + 4] / 32.0f);
            }
        }
        if (peak > 0.0f)
            out.push_back({name, peak});
    }
    return out;
}
#else
std::vector<std::pair<std::string, float>> EdidPeaks() {
    return {};
}
#endif

bool QueryHdrSurface(const nri::Window& window, void* instance_handle, void* physical_device, void* get_instance_proc,
                     bool& pq10, bool& scrgb) {
#ifdef __APPLE__
    // NRI's Metal backend: RGBA16F with extended linear sRGB, RGB10A2 with BT.2100 PQ, both as EDR layers
    (void)window, (void)instance_handle, (void)physical_device, (void)get_instance_proc;
    pq10 = scrgb = true;
    return true;
#else
    auto gipa = (PFN_vkGetInstanceProcAddr)get_instance_proc;
    const VkInstance instance = (VkInstance)instance_handle;
    // VkWaylandSurfaceCreateInfoKHR, VkXlibSurfaceCreateInfoKHR and VkWin32SurfaceCreateInfoKHR: two 64-bit handles
    struct SurfaceInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        void* first;
        uint64_t second;
    } info = {};
    const char* create_name = nullptr;
    if (window.wayland.surface) {
        info = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, nullptr, 0, window.wayland.display,
                (uint64_t)(uintptr_t)window.wayland.surface};
        create_name = "vkCreateWaylandSurfaceKHR";
    } else if (window.x11.window) {
        info = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, nullptr, 0, window.x11.dpy, window.x11.window};
        create_name = "vkCreateXlibSurfaceKHR";
    } else if (window.windows.hwnd) {
        info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, nullptr, (uint64_t)(uintptr_t)window.windows.hwnd};
        create_name = "vkCreateWin32SurfaceKHR";
    }
    using CreateSurface = VkResult(VKAPI_PTR*)(VkInstance, const SurfaceInfo*, const VkAllocationCallbacks*, VkSurfaceKHR*);
    auto create = create_name ? (CreateSurface)gipa(instance, create_name) : nullptr;
    auto formats_of = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    auto destroy = (PFN_vkDestroySurfaceKHR)gipa(instance, "vkDestroySurfaceKHR");
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (create && formats_of && destroy && create(instance, &info, nullptr, &surface) == VK_SUCCESS) {
        uint32_t n = 0;
        formats_of((VkPhysicalDevice)physical_device, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        formats_of((VkPhysicalDevice)physical_device, surface, &n, formats.data());
        for (const VkSurfaceFormatKHR& f : formats) {
            pq10 |= f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
            scrgb |= f.format == VK_FORMAT_R16G16B16A16_SFLOAT && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
        }
        destroy(instance, surface, nullptr);
        return true;
    }
    spdlog::warn("could not query the surface's formats; SDR only");
    return false;
#endif
}

void ReadEdidPeak(SDL_Window* window, HdrState& state) {
#ifdef __linux__
    const auto peaks = EdidPeaks();
    const char* display_name = SDL_GetDisplayName(SDL_GetDisplayForWindow(window));
    for (const auto& [name, peak] : peaks)
        if (peaks.size() == 1 || (display_name && !name.empty() && std::string(display_name).find(name) != std::string::npos)) {
            state.reported_peak = peak;
            state.peak_source = "EDID (" + name + ")";
        }
#else
    (void)window, (void)state;
#endif
}

void UpdateHdrDefaults(SDL_Window* window, HdrState& s, bool absolute_pq) {
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    const bool hdr = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    const float white = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
    const float headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    s.reported_white = hdr ? white * SCRGB_NITS : 0.0f;
    // SDL's headroom on Wayland is the compositor's luminance range (10000 nits for PQ), not the display's peak
    if (s.peak_source.rfind("EDID", 0) != 0 && hdr && headroom > 1.0f && headroom * white * SCRGB_NITS < 9999.0f) {
        s.reported_peak = headroom * white * SCRGB_NITS;
        s.peak_source = "SDL";
    }
#ifdef _WIN32
    absolute_pq = false;
#endif
    if (!s.paper_white_set)
        s.paper_white = absolute_pq || s.reported_white <= 0.0f ? 203.0f : s.reported_white;
    if (!s.peak_set)
        s.peak = s.reported_peak > 0.0f ? s.reported_peak : 1000.0f;
    s.peak = std::max(s.peak, s.paper_white);
}

void SetupUiStyle(ImGuiStyle& base, ImFont*& mono) {
    const float size = 15.0f;  // scaled by the UI scale
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : {"/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF/Roboto-Regular.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "C:/Windows/Fonts/segoeui.ttf"})
        if (std::filesystem::exists(path)) {
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, size)) {
                io.FontDefault = font;
                break;
            }
        }
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", "C:/Windows/Fonts/consola.ttf"})
        if (std::filesystem::exists(path)) {
            if ((mono = io.Fonts->AddFontFromFileTTF(path, size)) != nullptr)
                break;
        }
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = size;
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Border] = ImVec4(0.34f, 0.38f, 0.46f, 0.85f);
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.92f);
    c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.14f, 0.17f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.20f, 0.26f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.26f, 0.34f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.34f, 0.45f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.40f, 0.52f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.19f, 0.22f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.31f, 0.38f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.36f, 0.45f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.72f, 1.0f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.72f, 1.0f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.40f, 0.52f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.24f, 0.32f, 0.42f, 1.0f);
    base = style;
}

float AutoUiScale(const App& app) {
    const float content = app.DisplayScale();
    const float monitor = app.DisplayHeight() / 1440.0f;
    const float window = app.GetOutputResolution().y / app.PixelDensity() / 1080.0f;
    return content * std::clamp(std::min(monitor, window), 1.0f, 2.0f);
}

}  // namespace renderer
