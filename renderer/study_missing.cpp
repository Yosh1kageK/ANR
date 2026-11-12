// The study viewer's stand-in (renderer/clips/study_entry.h) in a build without it.

#include <cstdio>

#include "renderer/clips/study_entry.h"

namespace renderer::clips {

int RunStudy(int, char**) {
    std::fprintf(stderr, "This build has no study viewer. Use --compare or --captures, or no arguments for the launcher.\n");
    return 1;
}

int RunRenderTest(int argc, char** argv) {
    return RunStudy(argc, argv);
}

}  // namespace renderer::clips
