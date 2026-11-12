// The recent-pairs registry of the compare viewer: which projects belong to which pair of recordings, newest first, so a pair
// chosen in the window finds its project (marks, offset, labels, logos, layout) again, and the folders the file dialogs start
// in. One JSON file in the renderer's cache folder; a missing or damaged file is an empty registry. Pure (no GPU, no
// OS apart from files), tested by pair_registry_test.cpp.
//
//   { "version": 1, "last_video_folder": "...", "last_project_folder": "...",
//     "pairs": [ { "a": "/abs/a.mp4", "b": "/abs/b.mp4", "project": "/abs/x.compare.json", "last_used": 1762700000 }, ... ] }
//
// A pair is keyed by the two absolute paths in order: A with B is not B with A (swapping them changes the offset's sign
// and the marks' times, so it is another project; the viewer's Swap moves the entry with `Rekey`).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace renderer::clips {

// The absolute, lexically normalised form of a path ("a/../b" and "b" are one file); files that do not exist are fine.
std::string AbsolutePath(const std::string& path);

struct RecentPair {
    std::string a, b;        // absolute
    std::string project;     // absolute: the project file of this pair
    int64_t last_used = 0;   // unix seconds
};

// Whether the files of an entry are still there.
struct PairStatus {
    bool a_exists = false, b_exists = false, project_exists = false;
    bool usable() const { return a_exists && b_exists; }
};
PairStatus StatusOf(const RecentPair& pair);

class PairRegistry {
public:
    static constexpr size_t MAX_ENTRIES = 30;

    // Reads `path`. A missing file gives an empty registry; a file that is not a registry gives an empty one too, with the
    // reason in `note` (the next Save replaces it). Entries that are not complete are skipped.
    static PairRegistry Load(const std::string& path, std::string* note = nullptr);
    // Writes it atomically (a temporary file, then a rename), creating the folder.
    bool Save(const std::string& path, std::string& error) const;

    // The pair was used now: added at the front, or moved there with its project and time updated. Paths are made
    // absolute; the oldest entries beyond MAX_ENTRIES go.
    void Touch(const std::string& a, const std::string& b, const std::string& project, int64_t now);
    const RecentPair* Find(const std::string& a, const std::string& b) const;
    bool Remove(const std::string& a, const std::string& b);
    // A project's pair changed (Swap A and B): its entry takes the new key and goes to the front. An entry the new key
    // already had is replaced. False when the old key is not in the registry (then nothing changes).
    bool Rekey(const std::string& old_a, const std::string& old_b, const std::string& new_a, const std::string& new_b,
               int64_t now);

    const std::vector<RecentPair>& entries() const { return entries_; }
    std::string last_video_folder, last_project_folder;

private:
    std::vector<RecentPair> entries_;
};

// "just now", "5 min ago", "3 h ago", "yesterday", "12 days ago", "4 months ago" for the Recent list.
std::string FormatAgo(int64_t now, int64_t then);

// The project file of a pair that has none: <folder>/<stem a>-vs-<stem b>-<8 hex digits of the two paths>.compare.json.
// The digits keep two pairs with the same file names in other folders apart; the stems are cut and made safe for any
// file system.
std::string ProjectFileFor(const std::string& folder, const std::string& a, const std::string& b);

}  // namespace renderer::clips
