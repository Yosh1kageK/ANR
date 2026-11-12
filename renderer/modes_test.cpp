// renderer::DecideMode: what each command line starts. Every way the renderer is started today (README examples, the
// scripts and the checks all pass arguments) must still start what it started.
#include "renderer/modes.h"

#include <initializer_list>
#include <string>
#include <vector>

#include "renderer/clips/check.h"

namespace {

renderer::Decision Decide(std::initializer_list<const char*> args) {
    std::vector<const char*> argv = {"renderer"};
    argv.insert(argv.end(), args.begin(), args.end());
    return renderer::DecideMode((int)argv.size(), argv.data());
}

}  // namespace

int main() {
    using renderer::Start;

    // No arguments: the launcher. --launcher: the launcher, whatever else is on the line.
    CHECK(Decide({}).start == Start::LAUNCHER);
    CHECK(Decide({"--launcher"}).start == Start::LAUNCHER);
    CHECK(Decide({"--launcher", "--vsync", "--technique", "native"}).start == Start::LAUNCHER);
    CHECK(Decide({"--vsync", "--launcher"}).start == Start::LAUNCHER);
    CHECK(Decide({"--launcher", "--mode", "scene"}).start == Start::LAUNCHER);

    // --mode
    CHECK(Decide({"--mode", "scene"}).start == Start::SCENE);
    CHECK(Decide({"--mode", "compare"}).start == Start::COMPARE);
    CHECK(Decide({"--mode", "captures"}).start == Start::CAPTURES);
    CHECK(Decide({"--mode=scene"}).start == Start::SCENE);
    CHECK(Decide({"--mode=captures"}).start == Start::CAPTURES);
    CHECK(Decide({"--mode", "scene", "--vsync", "--technique", "dlss-g"}).start == Start::SCENE);
    CHECK(Decide({"--vsync", "--mode", "compare"}).start == Start::COMPARE);
    CHECK(Decide({"--mode", "bogus"}).start == Start::INVALID);
    CHECK(Decide({"--mode"}).start == Start::INVALID);
    CHECK(Decide({"--mode="}).start == Start::INVALID);
    CHECK(Decide({"--mode", "study"}).start == Start::INVALID);
    CHECK(!Decide({"--mode", "bogus"}).error.empty());

    // The existing flags keep their meaning
    CHECK(Decide({"--compare"}).start == Start::COMPARE);
    CHECK(Decide({"--captures"}).start == Start::CAPTURES);
    CHECK(Decide({"--compare", "--captures"}).start == Start::CAPTURES);
    CHECK(Decide({"--compare", "--project", "x.compare.json"}).start == Start::COMPARE);
    CHECK(Decide({"--compare", "--compare-check", "600", "--project", "x"}).start == Start::COMPARE_TOOL);
    CHECK(Decide({"--compare", "--compare-export", "x", "--out", "y.mp4"}).start == Start::COMPARE_TOOL);
    CHECK(Decide({"--study", "DIR"}).start == Start::STUDY);
    CHECK(Decide({"--render-test"}).start == Start::STUDY);

    // Any other argument without a mode flag: the scene renderer, as before
    CHECK(Decide({"--vsync"}).start == Start::SCENE);
    CHECK(Decide({"--rendered-fps", "60"}).start == Start::SCENE);
    CHECK(Decide({"--technique", "dlss-g"}).start == Start::SCENE);
    CHECK(Decide({"--dump", "/tmp/renderer-pair", "--scene-options", "none"}).start == Start::SCENE);
    CHECK(Decide({"--timeLimit=120", "--fixed-step-ms=16.667"}).start == Start::SCENE);
    CHECK(Decide({"--bundled", "--models", "", "--technique", "display-v1"}).start == Start::SCENE);
    CHECK(Decide({"--script", "0:pause,10:quit"}).start == Start::SCENE);
    CHECK(Decide({"--help"}).start == Start::SCENE);
    CHECK(Decide({"stray"}).start == Start::SCENE);

    // Contradictions are refused, not guessed
    CHECK(Decide({"--mode", "scene", "--compare"}).start == Start::INVALID);
    CHECK(Decide({"--mode", "scene", "--captures"}).start == Start::INVALID);
    CHECK(Decide({"--mode", "scene", "--compare-check", "5"}).start == Start::INVALID);
    CHECK(Decide({"--mode", "scene", "--study"}).start == Start::INVALID);
    CHECK(Decide({"--mode", "compare", "--study"}).start == Start::INVALID);
    CHECK(Decide({"--mode", "compare", "--captures"}).start == Start::INVALID);
    CHECK(Decide({"--mode", "compare", "--compare"}).start == Start::COMPARE);
    CHECK(Decide({"--mode", "captures", "--captures"}).start == Start::CAPTURES);
    CHECK(Decide({"--mode", "captures", "--compare"}).start == Start::CAPTURES);
    CHECK(Decide({"--mode", "compare", "--compare-check", "5", "--project", "x"}).start == Start::COMPARE_TOOL);

    // The names, and what a mode switch starts: only the mode flag
    CHECK(renderer::ParseMode("scene") == renderer::Mode::SCENE);
    CHECK(renderer::ParseMode("compare\n") == renderer::Mode::COMPARE);
    CHECK(renderer::ParseMode(" captures ") == renderer::Mode::CAPTURES);
    CHECK(!renderer::ParseMode("").has_value());
    CHECK(!renderer::ParseMode("Scene").has_value());
    for (const renderer::ModeInfo& info : renderer::AllModes()) {
        CHECK(renderer::ParseMode(info.name) == info.mode);
        std::vector<const char*> argv = {"renderer"};
        const std::vector<std::string> args = renderer::ModeArguments(info.mode);
        for (const std::string& arg : args)
            argv.push_back(arg.c_str());
        const renderer::Decision decision = renderer::DecideMode((int)argv.size(), argv.data());  // what a switch starts is itself
        CHECK(decision.start == (info.mode == renderer::Mode::SCENE     ? Start::SCENE
                                 : info.mode == renderer::Mode::COMPARE ? Start::COMPARE
                                                                        : Start::CAPTURES));
    }
    CHECK_EQ(renderer::ModeArguments(renderer::Mode::SCENE).size(), 2);
    CHECK_EQ(renderer::ModeArguments(renderer::Mode::COMPARE).size(), 1);
    CHECK_EQ(renderer::ModeArguments(renderer::Mode::CAPTURES).size(), 1);
    return check_result("modes");
}
