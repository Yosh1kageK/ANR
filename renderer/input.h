// Trackpad deltas from SDL's native scroll-source information (third_party/sdl3_scroll_source.patch).
#pragma once

#include <array>
#include <SDL3/SDL_events.h>

namespace renderer {

inline std::array<float, 2> TrackpadDelta(const SDL_MouseWheelEvent& wheel) {
    if (!wheel.touchpad)
        return {};
    // Keep the gesture direction independent of the OS's natural-scroll setting.
    const float sign = wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
    return {sign * wheel.x, -sign * wheel.y};
}

}  // namespace renderer
