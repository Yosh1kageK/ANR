// The renderer's window, input and frame loop on SDL3, in place of NRIFramework's GLFW-based SampleBase (the
// renderer keeps NRIFramework's camera, timer, scene loader and NRI helpers):
//
//   class Renderer : public renderer::App { ... Initialize, PrepareFrame, RenderFrame ... };
//   if (app->Create(argc, argv, "NR renderer")) app->RenderLoop();
//
// The window is Wayland's or X11's on Linux (--x11 picks X11), Win32's on Windows and Cocoa's with a CAMetalLayer on
// macOS, sized in pixels. Input: keyboard and mouse state per frame (mouse motion is the sum of every motion event
// since the last frame, raw counts while the mouse is relative), and the first connected gamepad in SDL's mapping
// (Xbox names: south A, east B, west X, north Y). Dear ImGui gets its input through SDL's ImGui backend, in pixels.

#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>

#include <NRIFramework.h>

namespace renderer {

class App {
public:
    App() = default;
    virtual ~App();

    virtual void InitCmdLine(cmdline::parser&) {}
    virtual void ReadCmdLine(cmdline::parser&) {}
    virtual bool Initialize(nri::GraphicsAPI graphicsAPI, bool) = 0;
    virtual void LatencySleep(uint32_t) {}
    virtual void PrepareFrame(uint32_t) {}
    virtual void RenderFrame(uint32_t) = 0;
    virtual bool AppShouldClose() { return false; }
    // The window was closed or a quit was requested: true quits (the default); the study viewer asks the observer first.
    virtual bool OnCloseRequested() { return true; }
    // Every SDL event, after the UI got it and before the App's own handling, on the thread of the frame loop (the clip
    // viewer takes the drop events here). Mouse positions are in window points, as SDL gives them. The strings an event
    // points to (a dropped file's path) are valid until the next poll: copy what is kept.
    virtual void OnEvent(const SDL_Event&) {}

    // The command line, SDL, the window (shown before Initialize: Wayland maps a surface on its first frame), then
    // Initialize. False on --help, a bad command line or a failure.
    bool Create(int argc, char** argv, const char* title);
    void RenderLoop();

    // Dear ImGui through NRI's ImGui extension, with SDL's ImGui backend for input.
    bool InitImgui(nri::Device& device);
    void DestroyImgui();
    nri::ImguiRenderData CmdCopyImguiData(nri::CommandBuffer& commandBuffer, nri::Streamer& streamer);
    void CmdDrawImgui(nri::CommandBuffer& commandBuffer, const nri::ImguiRenderData& data, nri::Format format,
                      float sdrScale, bool isSrgb);
    bool HasUserInterface() const { return m_ImguiRenderer != nullptr; }

    // Keyboard and mouse. A toggle is a press (not a key repeat) since the key's last IsKeyToggled.
    bool IsKeyToggled(Key key);
    bool IsKeyPressed(Key key) const { return m_KeyState[(size_t)key]; }
    bool IsButtonPressed(Button button) const { return m_ButtonState[(size_t)button]; }
    float2 GetMouseDelta() const { return m_MouseDelta; }
    float GetMouseWheel() const { return m_MouseWheel; }
    // Trackpad scrolling since the last frame in the fingers' direction (x right, y down) whatever the system's
    // scroll direction (mouse wheels excluded), and the pinch's scale (above 1: fingers apart).
    float2 GetScroll() const { return m_Scroll; }
    float GetPinch() const { return m_Pinch; }
    void SetRelativeMouse(bool on);  // hidden cursor, raw motion

    // The first connected gamepad. Sticks -1..1 (y down), triggers 0..1.
    bool HasGamepad() const { return m_Gamepad != nullptr; }
    float GamepadAxis(SDL_GamepadAxis axis) const;
    bool IsGamepadButtonPressed(SDL_GamepadButton button) const;
    bool IsGamepadButtonToggled(SDL_GamepadButton button);
    std::string GamepadName() const;

    uint2 GetOutputResolution() const { return m_OutputResolution; }
    const nri::Window& GetWindow() const { return m_NRIWindow; }
    uint8_t GetQueuedFrameNum() const { return m_Vsync ? 2 : 3; }
    uint8_t GetOptimalSwapChainTextureNum() const { return GetQueuedFrameNum() + 1; }

    // The window: its pixel size from SDL's events, minimized, the window system ("wayland", "x11", "windows",
    // "cocoa"), its display's refresh rate, height in pixels and content scale.
    uint2 PixelSize() const { return m_PixelSize; }
    bool Minimized() const { return m_Minimized; }
    std::string VideoDriver() const;
    bool IsWayland() const { return VideoDriver() == "wayland"; }
    float RefreshRate() const;
    float DisplayHeight() const;  // in points, the OS's layout unit (pixels / pixel density)
    uint2 FullscreenSize() const;  // the window's display in pixels (its desktop mode)
    float DisplayScale() const;   // UI pixels per point: the pixel density times the OS content scale
    float PixelDensity() const;   // window pixels per point (2 on a Retina display)
    void SetFullscreen(bool on);
    static double Seconds();  // a monotonic clock

protected:
    std::string m_SceneFile;
    SDL_Window* m_Window = nullptr;
    Camera m_Camera;
    Timer m_Timer;
    uint2 m_OutputResolution = {1920, 1080};  // pixels (points with m_SizeInPoints, for the first window only)
    bool m_SizeInPoints = false;              // the requested size is in points, the OS's layout unit: the launcher's, so that it has the same size on a Retina or scaled display
    float m_MouseSensitivity = 1.0f;
    bool m_Vsync = false;
    bool m_AlwaysActive = false;
    bool m_Resizable = false;

private:
    void PollEvents();
    void OpenGamepad();

    nri::ImguiInterface m_iImgui = {};
    nri::Imgui* m_ImguiRenderer = nullptr;
    nri::Window m_NRIWindow = {};
    SDL_MetalView m_MetalView = nullptr;
    SDL_Gamepad* m_Gamepad = nullptr;

    std::array<bool, (size_t)Key::NUM> m_KeyState = {};
    std::array<bool, (size_t)Key::NUM> m_KeyToggled = {};
    std::array<bool, (size_t)Button::NUM> m_ButtonState = {};
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> m_GamepadToggled = {};
    float2 m_MouseDelta = {};
    float m_MouseWheel = 0.0f;
    float2 m_Scroll = {};
    float m_Pinch = 1.0f;
    bool m_Relative = false;

    uint2 m_PixelSize = {};
    bool m_Minimized = false;
    bool m_Quit = false;
    bool m_X11 = false;
    double m_TimeLimit = 1e38;
    uint32_t m_FrameNum = UINT32_MAX;
};

}  // namespace renderer
