// Tests the SDL event boundary without a display or GPU. Native hardware identifies the source before this call.
#include "renderer/input.h"

#include <cstdio>
#include <SDL3/SDL.h>

// Internal SDL producers: the original wheel path and the NR source-preserving extension.
extern "C" void SDL_SendMouseWheel(Uint64, SDL_Window*, SDL_MouseID, float, float, SDL_MouseWheelDirection);
extern "C" void SDL_SendMouseWheelWithSource(Uint64, SDL_Window*, SDL_MouseID, float, float,
                                             SDL_MouseWheelDirection, bool);

bool CheckScroll(float x, float y, SDL_MouseWheelDirection direction, bool touchpad,
                 std::array<float, 2> expected) {
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    if (touchpad)
        SDL_SendMouseWheelWithSource(0, nullptr, 1, x, y, direction, true);
    else
        SDL_SendMouseWheel(0, nullptr, 1, x, y, direction);
    SDL_Event event = {};
    if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_MOUSE_WHEEL) != 1 ||
        event.wheel.touchpad != touchpad || event.wheel.x != x || event.wheel.y != y ||
        renderer::TrackpadDelta(event.wheel) != expected) {
        std::fprintf(stderr, "scroll source/delta mismatch: %g %g direction=%d touchpad=%d\n", x, y, direction, touchpad);
        return false;
    }
    return true;
}

int main() {
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const bool ok =
        CheckScroll(0.0f, 1.0f, SDL_MOUSEWHEEL_NORMAL, false, {0.0f, 0.0f}) &&
        CheckScroll(0.25f, -0.5f, SDL_MOUSEWHEEL_NORMAL, false, {0.0f, 0.0f}) &&
        CheckScroll(2.0f, -1.0f, SDL_MOUSEWHEEL_NORMAL, true, {2.0f, 1.0f}) &&
        CheckScroll(-2.0f, 1.0f, SDL_MOUSEWHEEL_FLIPPED, true, {2.0f, 1.0f}) &&
        CheckScroll(0.25f, -0.5f, SDL_MOUSEWHEEL_NORMAL, true, {0.25f, 0.5f}) &&
        // Switching back to a mouse must not retain the previous gesture source.
        CheckScroll(0.25f, -0.5f, SDL_MOUSEWHEEL_FLIPPED, false, {0.0f, 0.0f});
    SDL_Quit();
    return ok ? 0 : 1;
}
