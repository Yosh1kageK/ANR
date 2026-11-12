#include "renderer/clips/pair_pick.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "renderer/clips/pair_registry.h"

namespace renderer::clips {
namespace fs = std::filesystem;
namespace {

std::string Lower(std::string text) {
    for (char& c : text)
        c = (char)std::tolower((unsigned char)c);
    return text;
}

std::string Name(const std::string& path) {
    return fs::path(path).filename().string();
}

std::string Rate(double fps) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", fps);
    std::string out = text;
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out;
}

std::string Describe(const MediaFacts& m) {
    return std::to_string(m.width) + "x" + std::to_string(m.height) + " at " + Rate(m.fps) + " fps";
}

}  // namespace

FileKind ClassifyFile(const std::string& path) {
    const std::string extension = Lower(fs::path(path).extension().string());
    for (const char* video : {".mp4", ".mov", ".m4v", ".mkv", ".webm", ".avi"})
        if (extension == video)
            return FileKind::VIDEO;
    if (extension == ".json")
        return FileKind::PROJECT;
    return FileKind::OTHER;
}

bool IsVideoFile(const std::string& path) {
    return ClassifyFile(path) == FileKind::VIDEO;
}

bool SameFile(const std::string& a, const std::string& b) {
    if (AbsolutePath(a) == AbsolutePath(b))
        return true;
    std::error_code ec;
    return fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec);
}

std::string CompatibilityProblem(const MediaFacts& a, const MediaFacts& b, const std::string& changed) {
    const bool same_size = a.width == b.width && a.height == b.height;
    const bool same_rate = std::fabs(a.fps - b.fps) <= 1e-3;
    if (same_size && same_rate)
        return {};
    const MediaFacts& fresh = changed == "B" ? b : a;
    const MediaFacts& other = changed == "B" ? a : b;
    std::string message;
    if (changed.empty()) {
        message = "A (" + Name(a.path) + ") is " + Describe(a) + " and B (" + Name(b.path) + ") is " + Describe(b);
    } else {
        message = "cannot use " + Name(fresh.path) + " as " + changed + ": it is " + Describe(fresh) + " and " +
                  Name(other.path) + " (" + (changed == "B" ? "A" : "B") + ") is " + Describe(other);
    }
    return message + "; the two videos need the same size and frame rate";
}

int DropSide(double x) {
    return x < 0.5 ? 0 : 1;
}

DropDecision DecideDrop(const std::vector<std::string>& files, double x) {
    DropDecision decision;
    if (files.empty())
        return decision;
    auto reject = [&](const std::string& why) {
        decision.action = DropDecision::Action::REJECT;
        decision.message = why;
        return decision;
    };
    for (const std::string& file : files)
        if (ClassifyFile(file) == FileKind::OTHER)
            return reject(Name(file) + " is not a video (mp4, mov, m4v, mkv, webm, avi) or a project (.compare.json)");
    size_t videos = 0, projects = 0;
    for (const std::string& file : files)
        (ClassifyFile(file) == FileKind::VIDEO ? videos : projects)++;
    if (projects > 0) {
        if (projects == 1 && videos == 0) {
            decision.action = DropDecision::Action::OPEN_PROJECT;
            decision.project = files[0];
            return decision;
        }
        return reject("drop one project, or one or two videos, not a mix");
    }
    if (videos > 2)
        return reject("drop one or two videos (" + std::to_string(videos) + " were dropped)");
    if (videos == 2) {
        decision.action = DropDecision::Action::SET_BOTH;
        decision.a = files[0];
        decision.b = files[1];
        return decision;
    }
    if (DropSide(x) == 0) {
        decision.action = DropDecision::Action::SET_A;
        decision.a = files[0];
    } else {
        decision.action = DropDecision::Action::SET_B;
        decision.b = files[0];
    }
    return decision;
}

void DropCollector::Begin() {
    hovering_ = true;
    x_ = -1.0;
    files_.clear();
}

void DropCollector::Position(double x) {
    hovering_ = true;
    x_ = x;
}

void DropCollector::File(const std::string& path, double x) {
    hovering_ = true;
    if (x >= 0.0)
        x_ = x;
    files_.push_back(path);
}

DropDecision DropCollector::Complete() {
    // No position at all (a system that sends none): the left half, where the first video goes.
    const DropDecision decision = DecideDrop(files_, x_ < 0.0 ? 0.0 : x_);
    hovering_ = false;
    x_ = -1.0;
    files_.clear();
    return decision;
}

}  // namespace renderer::clips
