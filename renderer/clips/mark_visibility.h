// When the rectangle of a mark shows on the video: only near the playhead, fading in and out, so a long list of marks does not
// cover the picture. Pure (no GPU, no UI), tested by mark_visibility_test.cpp. The timeline's ticks and bars and the Areas
// list do not use it (they show every mark); the export draws no rectangles.
//
//   alpha
//   1 |            ┌────────────┐
//     |          ╱ │ the effect │ ╲
//   0 |_________╱   │            │   ╲_________
//       <- lead ->  t0         end  <- trail ->      (seconds of the playhead's timeline)
//
// The effect of a span is t0 to t1. The effect of a freeze mark is the frame's time to the end of its sequence, which is
// the frame's time plus the sequence's length in seconds (zoom in, hold, zoom out: edit_list.h's FreezeSequence::length()
// over the frame rate). The rectangle fades in over `lead` seconds before the effect starts (a smoothstep, 0 to 1), is
// whole while the playhead is inside it, and fades out over `trail` seconds after it ends.

#pragma once

#include <algorithm>

#include "renderer/clips/camera.h"

namespace renderer::clips {

enum class MarkShow { NEAR_PLAYHEAD, ALWAYS, NEVER };

// The "Show marks" setting of the Areas panel (a window setting, not stored in the project).
struct MarkShowSettings {
    MarkShow show = MarkShow::NEAR_PLAYHEAD;
    double lead = 1.0;   // seconds before a mark's time
    double trail = 1.0;  // seconds after the end of its effect
};

// The alpha of a mark's rectangle (0 to 1) for the playhead at `playhead` seconds. sequence_seconds is the length of a
// freeze mark's sequence (ignored for a span).
inline double mark_alpha(double playhead, AreaKind kind, double t0, double t1, double sequence_seconds, double lead,
                         double trail) {
    const double end = kind == AreaKind::FREEZE ? t0 + std::max(sequence_seconds, 0.0) : std::max(t1, t0);
    if (playhead >= t0 && playhead <= end)
        return 1.0;
    if (playhead < t0)
        return lead > 0.0 ? smoothstep(1.0 - (t0 - playhead) / lead) : 0.0;
    return trail > 0.0 ? smoothstep(1.0 - (playhead - end) / trail) : 0.0;
}

// The same by the setting: Always 1, Never 0, Near the playhead the ramp.
inline double mark_alpha(const MarkShowSettings& settings, double playhead, AreaKind kind, double t0, double t1,
                         double sequence_seconds) {
    switch (settings.show) {
        case MarkShow::ALWAYS:
            return 1.0;
        case MarkShow::NEVER:
            return 0.0;
        case MarkShow::NEAR_PLAYHEAD:
            break;
    }
    return mark_alpha(playhead, kind, t0, t1, sequence_seconds, settings.lead, settings.trail);
}

}  // namespace renderer::clips
