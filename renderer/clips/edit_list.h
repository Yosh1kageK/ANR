// The edit list of a comparison video: which pair of the recordings, which camera, divider and caption each output frame
// shows. The player (a freeze mark's sequence) and the export (every frame) both take their shots from here, so what the
// window shows is what the export writes. Pure (no GPU, no OS), tested by edit_list_test.cpp.
//
// Span areas zoom while the video plays: their camera, divider and caption are functions of the pair's time
// (camera.h). A freeze area stops the video at one frame n (the A frame of its t0) and runs a sequence of
// length() = ease + hold + ease frames in which the pair n stays on screen:
//
//     output:  ... n-1   n   n   n   n   n   n   n   n+1 ...
//     step j:           0   1 ..............  L   (L = 2 ease + hold, in frames)
//     camera:        base  zoom in   hold   zoom out  base
//
// Step 0 is the normal display of frame n; steps 1 to L are the frames the freeze inserts, so the output is longer than
// the source range by the sum of the sequences. The camera goes from the base camera (what the spans show at n) to the
// rectangle's view (view_for) with a smoothstep and a zoom in log space (steps 0 to ease), holds the view (steps ease to
// ease + hold), and returns to the base camera (steps ease + hold to L), so the frame after the sequence continues
// from the camera it had before. Rules:
//
//  - Frames: ease and hold are rounded to whole frames each (the area's own `ease` and `hold` or the project's
//    defaults, clamped to 0..5 s and 0..60 s). A freeze of no length is skipped.
//  - A freeze inside a span's window keeps the span's camera as its base, caption and divider: the sequence zooms from the
//    span's camera to the freeze's view and back to the span's camera at that frame, and the span goes on after it
//    (spans follow the pair's time, which stands still during the sequence). During the hold the freeze's label replaces
//    the span's caption.
//  - Freezes never overlap or merge: each sequence runs to its end, in the order of their frames (marks at the same frame
//    in the order of the list). A mark less than a zoom after the previous one therefore starts after the camera is back
//    at its base, a few frames or none later.
//  - The first and the last frame of the range may hold a freeze: the video starts, or ends, with its sequence.
//  - A freeze whose frame is outside [first, end) (outside the overlap of the recordings, or outside the exported range)
//    is skipped: there is no pair of both recordings to freeze.
//  - The divider of a freeze moves from the base divider to its own (fixed fraction, or the sweep) with the camera's
//    weight, so it never jumps; it sweeps once during the hold, as a span's divider does over its window.
//  - Frames after the first of a sequence are `frozen`: the same pair again. Everything else shows its own pair, once.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "renderer/clips/camera.h"

namespace renderer::clips {

struct EditOptions {
    double fps = 60.0;
    int64_t first = 0, end = 0;           // the pairs the edit shows: A frames [first, end)
    double ease = DEFAULT_EASE;           // seconds of zoom in, and of zoom out (the project's)
    double freeze_hold = DEFAULT_FREEZE_HOLD;  // seconds a freeze waits at its zoom (the project's)
    double divider = 0.5;                 // the project's divider (a fraction of the width)
};

// One freeze mark resolved to frames.
struct FreezeSequence {
    int area_id = 0;
    std::string label;
    int64_t frame = 0;                    // A frame number of the pair that is held
    int64_t ease_frames = 0, hold_frames = 0;
    Camera view;                          // the rectangle's view
    AreaDivider divider;
    int64_t length() const { return 2 * ease_frames + hold_frames; }  // frames the freeze inserts
};

enum class FreezePhase { ZOOMING_IN, HOLDING, ZOOMING_OUT };

// A freeze area as frames: frame = round(t0 * fps), ease and hold from the area or the options.
FreezeSequence make_freeze(const Area& area, const EditOptions& options);

FreezePhase freeze_phase(const FreezeSequence& sequence, int64_t step);

// The shot at step j (0 to length()) of a sequence, over the base shot (the camera, divider and caption the video has at
// the frame without the freeze).
Shot freeze_shot(const FreezeSequence& sequence, const Shot& base, int64_t step);

struct EditEntry {
    int64_t pair = 0;       // A frame number; B's is pair - K
    Shot shot;
    bool frozen = false;    // the pair again: a frame a freeze inserted
    int freeze_area = 0;    // the freeze's area id while a sequence runs (step 0 included); 0 outside sequences
    int64_t step = 0;       // the step within the sequence
};

class EditList {
public:
    EditList(const std::vector<Area>& areas, const EditOptions& options);

    int64_t length() const { return length_; }  // output frames
    int64_t source_frames() const { return options_.end > options_.first ? options_.end - options_.first : 0; }
    EditEntry at(int64_t index) const;           // index clamped to 0..length() - 1
    // The freezes that run, in play order, and the marks left out with the reason.
    const std::vector<FreezeSequence>& freezes() const { return freezes_; }
    struct Skipped {
        int area_id;
        std::string why;
    };
    const std::vector<Skipped>& skipped() const { return skipped_; }
    // The output index of the first display of an A frame of the range (step 0 of a sequence at it).
    int64_t first_output_of(int64_t pair) const;
    // The base shot (no freeze) at a pair: what the spans show.
    Shot base_shot(int64_t pair) const;
    const EditOptions& options() const { return options_; }

private:
    struct Group {          // the freezes at one frame
        int64_t pair, out_start;
        size_t first, count;  // in freezes_
        int64_t extra;      // frames they insert
    };
    EditOptions options_;
    std::vector<Area> spans_;
    std::vector<CameraKey> keys_;
    std::vector<FreezeSequence> freezes_;
    std::vector<Skipped> skipped_;
    std::vector<Group> groups_;
    int64_t length_ = 0;
};

}  // namespace renderer::clips
