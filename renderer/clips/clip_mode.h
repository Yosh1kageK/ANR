// The renderer binary's other modes (renderer.cpp's main hands them over once renderer/modes.h's rule has picked one):
// --compare (the clip viewer in a window), --captures (the same, on its Captures tab), --mode compare|captures,
// --compare-check N (a headless check of the lockstep playback), --compare-export (the offline export), --study (the 2AFC
// study viewer) and --render-test (its exactness test).

#pragma once

namespace renderer::clips {

int RunClipMode(int argc, char** argv);

}  // namespace renderer::clips
