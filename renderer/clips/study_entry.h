// The study viewer's entries (--study, --render-test), which clip_mode.cpp starts. The binary links them from
// renderer/clips/study, or in a build without the study viewer from a stand-in that says so.

#pragma once

namespace renderer::clips {

int RunStudy(int argc, char** argv);
int RunRenderTest(int argc, char** argv);

}  // namespace renderer::clips
