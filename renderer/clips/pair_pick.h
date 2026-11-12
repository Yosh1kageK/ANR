// How the compare viewer takes its two recordings from the user: what a dropped or chosen file is, which of A and B a drop
// sets, whether two recordings can be compared, and the batching of the system's drop events. Pure (no GPU, no OS apart
// from files), tested by pair_pick_test.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace renderer::clips {

enum class FileKind { VIDEO, PROJECT, OTHER };

// By the extension, in any case: mp4, mov, m4v, mkv, webm and avi are videos; .json (a project is *.compare.json) is a
// project.
FileKind ClassifyFile(const std::string& path);
bool IsVideoFile(const std::string& path);

// A and B are the same file (the same path, or a link to it).
bool SameFile(const std::string& a, const std::string& b);

// What the viewer needs to know of a recording to compare it with another.
struct MediaFacts {
    std::string path;
    uint32_t width = 0, height = 0;
    double fps = 0.0;
};

// Two recordings are compared frame by frame: they need the same size and the same frame rate (to 0.001 fps). Empty:
// they do. Else one line that names both files with their sizes and rates. `changed` is "A" or "B" when that one is the
// recording being set (the line starts "cannot use <file> as B"), else empty.
std::string CompatibilityProblem(const MediaFacts& a, const MediaFacts& b, const std::string& changed = "");

// What a drop does. x is where it landed as a fraction of the window's width: the left half sets A, the right half B.
struct DropDecision {
    enum class Action { NONE, SET_A, SET_B, SET_BOTH, OPEN_PROJECT, REJECT } action = Action::NONE;
    std::string a, b;     // SET_A: a; SET_B: b; SET_BOTH: both (in the order dropped)
    std::string project;  // OPEN_PROJECT
    std::string message;  // REJECT: one line that says what is wrong and what a drop can be
};
DropDecision DecideDrop(const std::vector<std::string>& files, double x);

// 0 for the left half of the window, 1 for the right (a drop's or a hover's side).
int DropSide(double x);

// The system's drop events as one drop: Begin (a drag entered the window), Position (it moves), File (one per dropped
// file), Complete (dropped, or the drag left: no files then). Some systems send no Position, so a File carries its own
// x; a negative one means unknown and the last Position holds.
class DropCollector {
public:
    void Begin();
    void Position(double x);
    void File(const std::string& path, double x = -1.0);
    // The decision for the files collected (NONE when there were none), and the state is back to idle.
    DropDecision Complete();

    bool hovering() const { return hovering_; }
    double x() const { return x_; }  // the last position, a fraction of the window's width; negative: none yet
    size_t files() const { return files_.size(); }

private:
    bool hovering_ = false;
    double x_ = -1.0;
    std::vector<std::string> files_;
};

}  // namespace renderer::clips
