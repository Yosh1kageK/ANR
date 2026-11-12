// The renderer's files in the user's cache folder: $XDG_CACHE_HOME/nr-record-viewer (else ~/.cache/nr-record-viewer)
// on Linux and macOS, %LOCALAPPDATA%\nr-record-viewer on Windows.

#include "renderer/paths.h"

#include <cstdlib>

namespace renderer::paths {
namespace {

std::string Base() {
#ifdef _WIN32
    const char* local = std::getenv("LOCALAPPDATA");
    return std::string(local ? local : ".") + "/nr-record-viewer";
#else
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache && *cache)
        return std::string(cache) + "/nr-record-viewer";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.cache/nr-record-viewer";
#endif
}

}  // namespace

std::string State() {
    return Base();
}

std::string Captures() {
    return Base() + "/captures";
}

std::string CaptureFrames() {
    return Base() + "/captures/frames";
}

std::vector<std::string> CaptureFolders() {
    return {};
}

std::vector<std::string> LogoFolders() {
    return {Base() + "/logos"};
}

}  // namespace renderer::paths
