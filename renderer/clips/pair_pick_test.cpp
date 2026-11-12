// Taking two recordings from the user: file kinds, the drop target's decision (left and right half, two files, a project, a
// file that is not a video), the batching of the system's drop events, the size and frame rate rule, and the same file.
#include "renderer/clips/pair_pick.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "renderer/clips/check.h"

using namespace renderer::clips;
namespace fs = std::filesystem;
using Action = DropDecision::Action;

namespace {

void files_are_told_by_their_extension() {
    for (const char* video : {"/v/a.mp4", "/v/a.MP4", "/v/a.mov", "/v/a.m4v", "/v/a.mkv", "/v/a.webm", "/v/a.avi", "a.Mov"})
        CHECK(ClassifyFile(video) == FileKind::VIDEO);
    CHECK(IsVideoFile("/v/clip.mp4") && !IsVideoFile("/v/clip.json"));
    CHECK(ClassifyFile("/p/x.compare.json") == FileKind::PROJECT);
    CHECK(ClassifyFile("/p/x.JSON") == FileKind::PROJECT);
    for (const char* other : {"/v/a.txt", "/v/a", "/v/a.mp4.part", "/v/.mp4", "/v/a.png", ""})
        CHECK(ClassifyFile(other) == FileKind::OTHER);
}

void a_drop_on_a_half_sets_a_or_b() {
    DropDecision d = DecideDrop({"/v/a.mp4"}, 0.1);
    CHECK(d.action == Action::SET_A && d.a == "/v/a.mp4" && d.b.empty());
    d = DecideDrop({"/v/b.mp4"}, 0.9);
    CHECK(d.action == Action::SET_B && d.b == "/v/b.mp4" && d.a.empty());
    // the middle goes to the right, as a half is x < 0.5
    CHECK(DecideDrop({"/v/b.mp4"}, 0.5).action == Action::SET_B);
    CHECK(DecideDrop({"/v/b.mp4"}, 0.4999).action == Action::SET_A);
    CHECK(DecideDrop({"/v/b.mp4"}, 0.0).action == Action::SET_A);
    CHECK(DecideDrop({"/v/b.mp4"}, 1.0).action == Action::SET_B);
    CHECK_EQ(DropSide(0.2), 0);
    CHECK_EQ(DropSide(0.8), 1);
}

void two_videos_set_both_in_order() {
    // wherever they land: the first is A, the second B
    for (double x : {0.05, 0.5, 0.95}) {
        const DropDecision d = DecideDrop({"/v/first.mp4", "/v/second.mkv"}, x);
        CHECK(d.action == Action::SET_BOTH && d.a == "/v/first.mp4" && d.b == "/v/second.mkv");
    }
    const DropDecision three = DecideDrop({"/v/1.mp4", "/v/2.mp4", "/v/3.mp4"}, 0.2);
    CHECK(three.action == Action::REJECT && three.message.find("one or two videos") != std::string::npos);
}

void a_project_opens_and_other_files_are_refused() {
    DropDecision d = DecideDrop({"/p/x.compare.json"}, 0.9);
    CHECK(d.action == Action::OPEN_PROJECT && d.project == "/p/x.compare.json");
    d = DecideDrop({"/p/x.compare.json", "/v/a.mp4"}, 0.1);
    CHECK(d.action == Action::REJECT);
    d = DecideDrop({"/p/x.compare.json", "/p/y.compare.json"}, 0.1);
    CHECK(d.action == Action::REJECT);
    // a file that is neither: the line names it and says what a drop can be
    d = DecideDrop({"/v/notes.txt"}, 0.1);
    CHECK(d.action == Action::REJECT);
    CHECK(d.message.find("notes.txt") != std::string::npos && d.message.find("not a video") != std::string::npos);
    CHECK(d.message.find("mp4") != std::string::npos && d.message.find(".compare.json") != std::string::npos);
    d = DecideDrop({"/v/a.mp4", "/v/cover.png"}, 0.1);  // one bad file refuses the whole drop and names it
    CHECK(d.action == Action::REJECT && d.message.find("cover.png") != std::string::npos);
    d = DecideDrop({}, 0.1);
    CHECK(d.action == Action::NONE);
}

void drop_events_are_batched() {
    DropCollector c;
    CHECK(!c.hovering());
    // begin, positions while the drag moves, the files, complete
    c.Begin();
    CHECK(c.hovering() && c.files() == 0);
    c.Position(0.3);
    c.Position(0.8);
    CHECK_NEAR(c.x(), 0.8, 1e-12);
    c.File("/v/a.mp4");
    CHECK_EQ((int)c.files(), 1);
    DropDecision d = c.Complete();
    CHECK(d.action == Action::SET_B && d.b == "/v/a.mp4");  // the last position was on the right
    CHECK(!c.hovering() && c.files() == 0);
    // two files in one drop
    c.Begin();
    c.File("/v/a.mp4", 0.2);
    c.File("/v/b.mp4", 0.2);
    d = c.Complete();
    CHECK(d.action == Action::SET_BOTH && d.a == "/v/a.mp4" && d.b == "/v/b.mp4");
    // the drag left the window: complete without files does nothing and ends the hover
    c.Begin();
    c.Position(0.4);
    CHECK(c.hovering());
    d = c.Complete();
    CHECK(d.action == Action::NONE && !c.hovering());
    // a system without positions: the file's own x, or the left half
    c.Begin();
    c.File("/v/a.mp4", 0.9);
    CHECK(c.Complete().action == Action::SET_B);
    c.Begin();
    c.File("/v/a.mp4");
    CHECK(c.Complete().action == Action::SET_A);
    // files without a Begin (a system that sends none) still count
    c.File("/v/a.mp4", 0.7);
    CHECK(c.Complete().action == Action::SET_B);
    // nothing carries over from one drop to the next
    c.Begin();
    c.Position(0.9);
    c.Complete();
    c.Begin();
    c.File("/v/a.mp4");
    CHECK(c.Complete().action == Action::SET_A);
}

void the_same_size_and_rate_rule() {
    const MediaFacts a = {"/v/apex.mp4", 3840, 2160, 60.0};
    CHECK(CompatibilityProblem(a, {"/w/dlssg.mp4", 3840, 2160, 60.0}).empty());
    CHECK(CompatibilityProblem(a, {"/w/dlssg.mp4", 3840, 2160, 60.0004}).empty());   // rounding of a rate
    CHECK(CompatibilityProblem({"/v/x.mp4", 1920, 1080, 30000.0 / 1001.0}, {"/v/y.mp4", 1920, 1080, 29.97}).empty());
    // another size: the line names both files with their sizes and rates
    std::string why = CompatibilityProblem(a, {"/w/small.mp4", 1280, 720, 60.0}, "B");
    CHECK(why.find("cannot use small.mp4 as B") != std::string::npos);
    CHECK(why.find("1280x720 at 60 fps") != std::string::npos);
    CHECK(why.find("apex.mp4 (A) is 3840x2160 at 60 fps") != std::string::npos);
    CHECK(why.find("same size and frame rate") != std::string::npos);
    // a frame rate that differs: 59.94 is not 60
    why = CompatibilityProblem(a, {"/w/ntsc.mp4", 3840, 2160, 60000.0 / 1001.0}, "B");
    CHECK(!why.empty() && why.find("59.94 fps") != std::string::npos);
    // the changed side is A: the message says so, with B as the other
    why = CompatibilityProblem({"/w/small.mp4", 1280, 720, 60.0}, {"/v/apex.mp4", 3840, 2160, 60.0}, "A");
    CHECK(why.find("cannot use small.mp4 as A") != std::string::npos && why.find("apex.mp4 (B)") != std::string::npos);
    // neither is new (a project was opened)
    why = CompatibilityProblem(a, {"/w/small.mp4", 1280, 720, 30.0});
    CHECK(why.rfind("A (apex.mp4) is 3840x2160 at 60 fps and B (small.mp4) is 1280x720 at 30 fps", 0) == 0);
}

void the_same_file_is_found() {
    const char* test_tmp = std::getenv("TEST_TMPDIR");
    const fs::path dir = (test_tmp ? fs::path(test_tmp) : fs::temp_directory_path()) / "nr_pair_pick";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    std::ofstream(dir / "a.mp4") << "a";
    std::ofstream(dir / "b.mp4") << "b";
    CHECK(SameFile((dir / "a.mp4").string(), (dir / "a.mp4").string()));
    CHECK(SameFile((dir / "a.mp4").string(), (dir / "sub" / ".." / "a.mp4").string()));
    CHECK(!SameFile((dir / "a.mp4").string(), (dir / "b.mp4").string()));
    std::error_code ec;
    fs::create_symlink(dir / "a.mp4", dir / "link.mp4", ec);
    if (!ec)
        CHECK(SameFile((dir / "a.mp4").string(), (dir / "link.mp4").string()));  // a link to it
    CHECK(!SameFile("/nowhere/a.mp4", "/nowhere/b.mp4"));
    CHECK(SameFile("/nowhere/a.mp4", "/nowhere/x/../a.mp4"));  // the same path, even where the file is not
    fs::remove_all(dir);
}

}  // namespace

int main() {
    files_are_told_by_their_extension();
    a_drop_on_a_half_sets_a_or_b();
    two_videos_set_both_in_order();
    a_project_opens_and_other_files_are_refused();
    drop_events_are_batched();
    the_same_size_and_rate_rule();
    the_same_file_is_found();
    return check_result("pair_pick");
}
