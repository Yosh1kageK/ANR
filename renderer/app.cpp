#include "renderer/app.h"
#include "renderer/input.h"
#include "renderer/window_fit.h"

#include <cmath>
#include <cstdio>

#include <imgui_impl_sdl3.h>
#include <spdlog/spdlog.h>

namespace renderer {
namespace {

// NRIFramework's Key (GLFW's key codes) from an SDL scancode: the physical key, as GLFW reports it.
Key ToKey(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z)
        return (Key)(GLFW_KEY_A + (s - SDL_SCANCODE_A));
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9)
        return (Key)(GLFW_KEY_1 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12)
        return (Key)(GLFW_KEY_F1 + (s - SDL_SCANCODE_F1));
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9)
        return (Key)(GLFW_KEY_KP_1 + (s - SDL_SCANCODE_KP_1));
    switch (s) {
        case SDL_SCANCODE_0: return Key::_0;
        case SDL_SCANCODE_GRAVE: return Key::Tilda;
        case SDL_SCANCODE_MINUS: return Key::Minus;
        case SDL_SCANCODE_EQUALS: return Key::Equals;
        case SDL_SCANCODE_BACKSPACE: return Key::Back;
        case SDL_SCANCODE_ESCAPE: return Key::Escape;
        case SDL_SCANCODE_PAUSE: return Key::Pause;
        case SDL_SCANCODE_KP_0: return Key::Num0;
        case SDL_SCANCODE_NUMLOCKCLEAR: return Key::NumLock;
        case SDL_SCANCODE_KP_MINUS: return Key::NumSub;
        case SDL_SCANCODE_KP_PLUS: return Key::NumAdd;
        case SDL_SCANCODE_KP_PERIOD: return Key::NumDel;
        case SDL_SCANCODE_KP_MULTIPLY: return Key::NumMul;
        case SDL_SCANCODE_KP_DIVIDE: return Key::NumDiv;
        case SDL_SCANCODE_KP_ENTER: return Key::NumEnter;
        case SDL_SCANCODE_LCTRL: return Key::LControl;
        case SDL_SCANCODE_RCTRL: return Key::RControl;
        case SDL_SCANCODE_LALT: return Key::LAlt;
        case SDL_SCANCODE_RALT: return Key::RAlt;
        case SDL_SCANCODE_LSHIFT: return Key::LShift;
        case SDL_SCANCODE_RSHIFT: return Key::RShift;
        case SDL_SCANCODE_LEFTBRACKET: return Key::LBracket;
        case SDL_SCANCODE_RIGHTBRACKET: return Key::RBracket;
        case SDL_SCANCODE_SCROLLLOCK: return Key::Scroll;
        case SDL_SCANCODE_TAB: return Key::Tab;
        case SDL_SCANCODE_RETURN: return Key::Return;
        case SDL_SCANCODE_SEMICOLON: return Key::Semicolon;
        case SDL_SCANCODE_APOSTROPHE: return Key::Apostrophe;
        case SDL_SCANCODE_BACKSLASH: return Key::BackSlash;
        case SDL_SCANCODE_COMMA: return Key::Comma;
        case SDL_SCANCODE_PERIOD: return Key::Period;
        case SDL_SCANCODE_SLASH: return Key::Slash;
        case SDL_SCANCODE_SPACE: return Key::Space;
        case SDL_SCANCODE_CAPSLOCK: return Key::Capital;
        case SDL_SCANCODE_INSERT: return Key::Insert;
        case SDL_SCANCODE_DELETE: return Key::Del;
        case SDL_SCANCODE_HOME: return Key::Home;
        case SDL_SCANCODE_END: return Key::End;
        case SDL_SCANCODE_PAGEUP: return Key::PageUp;
        case SDL_SCANCODE_PAGEDOWN: return Key::PageDown;
        case SDL_SCANCODE_LEFT: return Key::Left;
        case SDL_SCANCODE_RIGHT: return Key::Right;
        case SDL_SCANCODE_UP: return Key::Up;
        case SDL_SCANCODE_DOWN: return Key::Down;
        default: return Key::NUM;
    }
}

Button ToButton(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return Button::Left;
        case SDL_BUTTON_RIGHT: return Button::Right;
        case SDL_BUTTON_MIDDLE: return Button::Middle;
        default: return Button::NUM;
    }
}

}  // namespace

App::~App() {
    if (m_Gamepad)
        SDL_CloseGamepad(m_Gamepad);
    if (m_MetalView)
        SDL_Metal_DestroyView(m_MetalView);
    if (m_Window)
        SDL_DestroyWindow(m_Window);
    SDL_Quit();
}

bool App::Create(int argc, char** argv, const char* title) {
    cmdline::parser cmd;
    cmd.add("help", '?', "print this message");
    cmd.add<std::string>("scene", 's', "scene (glTF)", false, m_SceneFile);
    cmd.add<uint32_t>("width", 'w', "window width in pixels", false, m_OutputResolution.x);
    cmd.add<uint32_t>("height", 'h', "window height in pixels", false, m_OutputResolution.y);
    cmd.add<uint32_t>("frameNum", 'f', "close after N frames", false, m_FrameNum);
    cmd.add<double>("timeLimit", 't', "close after N seconds", false, m_TimeLimit);
    cmd.add("vsync", 'v', "vertical sync");
    cmd.add("alwaysActive", 0, "continue to render if not in focus");
    cmd.add("x11", 0, "Linux: X11 (XWayland in a Wayland session) instead of Wayland");
    // Read by main (renderer/modes.h) before any mode starts; registered so that every mode's parser accepts them.
    cmd.add<std::string>("mode", 0, "start this mode: scene, compare or captures (no arguments: the launcher)", false, "");
    cmd.add("launcher", 0, "the mode launcher, with whatever else is on the command line ignored");
    InitCmdLine(cmd);
    const bool parsed = cmd.parse(argc, argv);
    if (cmd.exist("help")) {
        std::printf("\n%s", cmd.usage().c_str());
        return false;
    }
    if (!parsed) {
        std::printf("\n%s\n\n%s", cmd.error().c_str(), cmd.usage().c_str());
        return false;
    }
    m_SceneFile = cmd.get<std::string>("scene");
    m_OutputResolution = {cmd.get<uint32_t>("width"), cmd.get<uint32_t>("height")};
    m_FrameNum = cmd.get<uint32_t>("frameNum");
    m_TimeLimit = cmd.get<double>("timeLimit");
    m_Vsync = cmd.exist("vsync");
    m_AlwaysActive = cmd.exist("alwaysActive");
    m_X11 = cmd.exist("x11");
    ReadCmdLine(cmd);

#if defined(__linux__)
    // SDL honors the hint on every OS, and macOS and Windows have neither driver.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, m_X11 ? "x11" : "wayland,x11");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        spdlog::critical("SDL: {}", SDL_GetError());
        return false;
    }

    // The window: the requested size in pixels, scaled down with its aspect ratio kept until it and its decorations
    // fit the usable area of the display it opens on (renderer/window_fit.h), centred. Fitted once before it shows
    // and again after, when the system reports its decorations and the output's scale. F11's fullscreen takes the
    // display's own size.
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (m_Resizable)
        flags |= SDL_WINDOW_RESIZABLE;
#ifdef __APPLE__
    flags |= SDL_WINDOW_METAL;
    const nri::GraphicsAPI api = nri::GraphicsAPI::METAL;
#else
    const nri::GraphicsAPI api = nri::GraphicsAPI::VK;
#endif
    m_Window = SDL_CreateWindow(title, 640, 360, flags);
    if (!m_Window) {
        spdlog::critical("SDL: {}", SDL_GetError());
        return false;
    }
    const uint2 requested = m_OutputResolution;
    std::string fitted;
    auto fit_window = [&]() {
        SDL_DisplayID display = SDL_GetDisplayForWindow(m_Window);
        if (!display)
            display = SDL_GetPrimaryDisplay();
        SDL_Rect usable = {};
        SDL_GetDisplayUsableBounds(display, &usable);
        int top = 0, left = 0, bottom = 0, right = 0;
        if (!SDL_GetWindowBordersSize(m_Window, &top, &left, &bottom, &right) || !(top | left | bottom | right))
            top = 40, left = right = bottom = 0;  // a title bar where the system reports no decorations
        const float density = SDL_GetWindowPixelDensity(m_Window);
        const float to_pixels = m_SizeInPoints ? density : 1.0f;
        const WindowFit fit = FitWindow((int)std::lround(requested.x * to_pixels), (int)std::lround(requested.y * to_pixels),
                                        density, usable.w, usable.h, left, right, top, bottom);
        SDL_SetWindowSize(m_Window, fit.width, fit.height);
        SDL_SetWindowPosition(m_Window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        fitted = std::to_string(fit.width) + "x" + std::to_string(fit.height) + (fit.clamped ? " (scaled down)" : "") +
                 " in a usable " + std::to_string(usable.w) + "x" + std::to_string(usable.h) + " at density " +
                 std::to_string(density) + ", content scale " + std::to_string(SDL_GetDisplayContentScale(display)) +
                 ", decorations " + std::to_string(left) + " " + std::to_string(top) + " " + std::to_string(right) +
                 " " + std::to_string(bottom);
    };
    fit_window();
    SDL_ShowWindow(m_Window);
    SDL_SyncWindow(m_Window);
    fit_window();
    SDL_SyncWindow(m_Window);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(m_Window, &pw, &ph);
    m_OutputResolution = uint2((uint32_t)pw, (uint32_t)ph);
    m_PixelSize = m_OutputResolution;
    spdlog::info("window {}x{} pixels for a requested {}x{}: {} (window coordinates)", pw, ph, requested.x, requested.y,
                 fitted);

    const SDL_PropertiesID props = SDL_GetWindowProperties(m_Window);
    const std::string driver = VideoDriver();
    if (driver == "wayland") {
        m_NRIWindow.wayland.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        m_NRIWindow.wayland.surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else if (driver == "x11") {
        m_NRIWindow.x11.dpy = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        m_NRIWindow.x11.window = (uint64_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else if (driver == "windows") {
        m_NRIWindow.windows.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else if (driver == "cocoa") {
        m_MetalView = SDL_Metal_CreateView(m_Window);
        m_NRIWindow.metal.caMetalLayer = m_MetalView ? SDL_Metal_GetLayer(m_MetalView) : nullptr;
    }
    OpenGamepad();
    return Initialize(api, true);
}

void App::OpenGamepad() {
    if (m_Gamepad)
        return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0) {
        m_Gamepad = SDL_OpenGamepad(ids[0]);
        if (m_Gamepad)
            spdlog::info("gamepad: {}", GamepadName());
    }
    SDL_free(ids);
}

void App::PollEvents() {
    const float density = m_Window ? SDL_GetWindowPixelDensity(m_Window) : 1.0f;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (HasUserInterface()) {
            // ImGui works in pixels (its display size is the swap chain's); SDL's mouse positions are in points.
            SDL_Event scaled = e;
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                scaled.motion.x *= density;
                scaled.motion.y *= density;
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                scaled.button.x *= density;
                scaled.button.y *= density;
            }
            ImGui_ImplSDL3_ProcessEvent(&scaled);
        }
        OnEvent(e);
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (OnCloseRequested())
                    m_Quit = true;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                m_PixelSize = {(uint32_t)e.window.data1, (uint32_t)e.window.data2};
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
                m_Minimized = true;
                break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_MAXIMIZED:
                m_Minimized = false;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const Key key = ToKey(e.key.scancode);
                if (key != Key::NUM) {
                    m_KeyState[(size_t)key] = e.key.down;
                    if (e.key.down && !e.key.repeat)
                        m_KeyToggled[(size_t)key] = true;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const Button button = ToButton(e.button.button);
                if (button != Button::NUM)
                    m_ButtonState[(size_t)button] = e.button.down;
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:  // every event's motion counts, however many arrive per frame
                m_MouseDelta.x += e.motion.xrel;
                m_MouseDelta.y += e.motion.yrel;
                break;
            case SDL_EVENT_MOUSE_WHEEL: {
                m_MouseWheel += e.wheel.y;
                const auto scroll = TrackpadDelta(e.wheel);
                m_Scroll.x += scroll[0];
                m_Scroll.y += scroll[1];
                break;
            }
            case SDL_EVENT_PINCH_UPDATE:
                m_Pinch *= e.pinch.scale;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                OpenGamepad();
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (m_Gamepad && SDL_GetGamepadID(m_Gamepad) == e.gdevice.which) {
                    spdlog::info("gamepad removed: {}", GamepadName());
                    SDL_CloseGamepad(m_Gamepad);
                    m_Gamepad = nullptr;
                    m_GamepadToggled = {};
                    OpenGamepad();
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (m_Gamepad && SDL_GetGamepadID(m_Gamepad) == e.gbutton.which && e.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT)
                    m_GamepadToggled[e.gbutton.button] = true;
                break;
            default:
                break;
        }
    }
}

void App::RenderLoop() {
    double active = 0.0;
    for (uint32_t i = 0; i < m_FrameNum; i++) {
        const double start = Seconds();
        LatencySleep(i);
        PollEvents();
        if (m_Quit || AppShouldClose())
            break;
        if (HasUserInterface()) {
            ImGui_ImplSDL3_NewFrame();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)m_OutputResolution.x, (float)m_OutputResolution.y);
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            // The backend may have queued a position in points (outside the window, while dragging): the pixel one
            // goes last.
            float x = 0.0f, y = 0.0f;
            if (!m_Relative && SDL_GetMouseFocus() == m_Window) {
                SDL_GetMouseState(&x, &y);
                const float density = SDL_GetWindowPixelDensity(m_Window);
                io.AddMousePosEvent(x * density, y * density);
            }
        }
        const bool focused = (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        if (!m_AlwaysActive && (!focused || m_Minimized)) {
            i--;
            SDL_WaitEventTimeout(nullptr, 50);
            continue;
        }
        PrepareFrame(i);
        RenderFrame(i);
        m_MouseWheel = 0.0f;
        m_MouseDelta = float2(0.0f);
        m_Scroll = float2(0.0f);
        m_Pinch = 1.0f;
        m_Timer.UpdateFrameTime();
        active += Seconds() - start;
        if (i > 8 && active > m_TimeLimit)
            break;
    }
}

bool App::InitImgui(nri::Device& device) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    const float scale = DisplayScale();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameBorderSize = 1;
    style.WindowBorderSize = 1;
    style.ScaleAllSizes(scale);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    ImFontConfig font = {};
    font.SizePixels = std::floor(13.0f * scale);
    io.Fonts->AddFontDefaultVector(&font);
    if (!ImGui_ImplSDL3_InitForOther(m_Window))
        return false;
    if (nri::nriGetInterface(device, NRI_INTERFACE(nri::ImguiInterface), &m_iImgui) != nri::Result::SUCCESS)
        return false;
    nri::ImguiDesc desc = {};
    return m_iImgui.CreateImgui(device, desc, m_ImguiRenderer) == nri::Result::SUCCESS;
}

void App::DestroyImgui() {
    if (!HasUserInterface())
        return;
    m_iImgui.DestroyImgui(m_ImguiRenderer);
    m_ImguiRenderer = nullptr;
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

nri::ImguiRenderData App::CmdCopyImguiData(nri::CommandBuffer& commandBuffer, nri::Streamer& streamer) {
    nri::ImguiRenderData data = {};
    if (!HasUserInterface())
        return data;
    const ImDrawData& draw = *ImGui::GetDrawData();
    nri::CopyImguiDataDesc desc = {};
    desc.drawLists = draw.CmdLists.Data;
    desc.drawListNum = draw.CmdLists.Size;
    desc.textures = draw.Textures->Data;
    desc.textureNum = draw.Textures->Size;
    m_iImgui.CmdCopyImguiData(commandBuffer, streamer, *m_ImguiRenderer, desc, data);
    return data;
}

void App::CmdDrawImgui(nri::CommandBuffer& commandBuffer, const nri::ImguiRenderData& data, nri::Format format,
                       float sdrScale, bool isSrgb) {
    if (!HasUserInterface())
        return;
    const ImDrawData& draw = *ImGui::GetDrawData();
    nri::DrawImguiDesc desc = {};
    desc.displaySize = {(nri::Dim_t)draw.DisplaySize.x, (nri::Dim_t)draw.DisplaySize.y};
    desc.hdrScale = sdrScale;
    desc.attachmentFormat = format;
    desc.linearColor = !isSrgb;
    m_iImgui.CmdDrawImgui(commandBuffer, data, desc);
}

bool App::IsKeyToggled(Key key) {
    const bool toggled = m_KeyToggled[(size_t)key];
    m_KeyToggled[(size_t)key] = false;
    return toggled;
}

void App::SetRelativeMouse(bool on) {
    if (on != m_Relative && SDL_SetWindowRelativeMouseMode(m_Window, on))
        m_Relative = on;
}

float App::GamepadAxis(SDL_GamepadAxis axis) const {
    if (!m_Gamepad)
        return 0.0f;
    const int16_t v = SDL_GetGamepadAxis(m_Gamepad, axis);
    return v < 0 ? v / 32768.0f : v / 32767.0f;
}

bool App::IsGamepadButtonPressed(SDL_GamepadButton button) const {
    return m_Gamepad && SDL_GetGamepadButton(m_Gamepad, button);
}

bool App::IsGamepadButtonToggled(SDL_GamepadButton button) {
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT)
        return false;
    const bool toggled = m_GamepadToggled[button];
    m_GamepadToggled[button] = false;
    return toggled;
}

std::string App::GamepadName() const {
    const char* name = m_Gamepad ? SDL_GetGamepadName(m_Gamepad) : nullptr;
    return name ? name : "";
}

std::string App::VideoDriver() const {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver ? driver : "";
}

float App::RefreshRate() const {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_Window));
    return mode && mode->refresh_rate > 0.0f ? mode->refresh_rate : 60.0f;
}

float App::DisplayHeight() const {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_Window));
    return mode ? (float)mode->h : 1080.0f;
}

uint2 App::FullscreenSize() const {
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(m_Window));
    if (!mode)
        return {0, 0};
    const float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
    return {(uint32_t)std::lround(mode->w * density), (uint32_t)std::lround(mode->h * density)};
}

float App::PixelDensity() const {
    const float density = m_Window ? SDL_GetWindowPixelDensity(m_Window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

float App::DisplayScale() const {
    const float scale = m_Window ? SDL_GetWindowDisplayScale(m_Window) : 1.0f;
    return scale > 0.0f ? scale : 1.0f;
}

void App::SetFullscreen(bool on) {
    SDL_SetWindowFullscreen(m_Window, on);  // borderless, at the display's mode
}

double App::Seconds() {
    return (double)SDL_GetTicksNS() * 1e-9;
}

}  // namespace renderer
