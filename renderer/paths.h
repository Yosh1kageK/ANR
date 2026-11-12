// Where the renderer keeps its own files, in one place, so that each build can put them where it keeps such things.
// Relative paths in these are against bazel run's workspace where there is one (BUILD_WORKSPACE_DIRECTORY), else the
// working directory.

#pragma once

#include <string>
#include <vector>

namespace renderer::paths {

// The renderer's state: the last mode, the window layouts, the compare tool's recent pairs and projects, the Captures
// tab's thumbnails and its added folders.
std::string State();

// The Captures tab's work: the marks of each pair, the jobs' logs and saved images.
std::string Captures();
// The frames the Captures tab's jobs make, a folder per capture.
std::string CaptureFrames();

// Folders the Captures tab lists without being told, besides the nr-record recordings folder (store.h).
std::vector<std::string> CaptureFolders();

// Folders the export's logo files are looked for in (after NR_LOGO_DIR, before the project's folder).
std::vector<std::string> LogoFolders();

}  // namespace renderer::paths
