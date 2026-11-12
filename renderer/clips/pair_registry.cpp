#include "renderer/clips/pair_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace renderer::clips {
namespace fs = std::filesystem;
namespace {

using json = nlohmann::json;

// The key two spellings of one path share: Windows paths do not differ by case.
std::string Key(const std::string& path) {
    std::string key = AbsolutePath(path);
#ifdef _WIN32
    for (char& c : key)
        c = (char)std::tolower((unsigned char)c);
#endif
    return key;
}

bool SamePair(const RecentPair& entry, const std::string& a, const std::string& b) {
    return Key(entry.a) == Key(a) && Key(entry.b) == Key(b);
}

std::string Text(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A stem as a file name part: letters, digits, '.', '_' and '-' stay, the rest becomes '_'; at most `limit` characters.
std::string SafeStem(const std::string& stem, size_t limit) {
    std::string out;
    for (unsigned char c : stem)
        out += (std::isalnum(c) || c == '.' || c == '_' || c == '-') ? (char)c : '_';
    if (out.size() > limit)
        out.resize(limit);
    return out.empty() ? "video" : out;
}

}  // namespace

std::string AbsolutePath(const std::string& path) {
    if (path.empty())
        return path;
    std::error_code ec;
    const fs::path absolute = fs::absolute(fs::path(path), ec);
    return (ec ? fs::path(path) : absolute).lexically_normal().string();
}

PairStatus StatusOf(const RecentPair& pair) {
    std::error_code ec;
    PairStatus status;
    status.a_exists = fs::is_regular_file(pair.a, ec);
    status.b_exists = fs::is_regular_file(pair.b, ec);
    status.project_exists = !pair.project.empty() && fs::is_regular_file(pair.project, ec);
    return status;
}

PairRegistry PairRegistry::Load(const std::string& path, std::string* note) {
    PairRegistry registry;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return registry;  // none yet
    json document;
    try {
        document = json::parse(in);
    } catch (const std::exception& e) {
        if (note)
            *note = path + " is not valid JSON (" + e.what() + "): starting with an empty list";
        return registry;
    }
    if (!document.is_object()) {
        if (note)
            *note = path + " is not a registry: starting with an empty list";
        return registry;
    }
    registry.last_video_folder = Text(document, "last_video_folder");
    registry.last_project_folder = Text(document, "last_project_folder");
    auto pairs = document.find("pairs");
    if (pairs == document.end() || !pairs->is_array())
        return registry;
    for (const json& item : *pairs) {
        if (!item.is_object())
            continue;
        RecentPair entry;
        entry.a = Text(item, "a");
        entry.b = Text(item, "b");
        entry.project = Text(item, "project");
        auto used = item.find("last_used");
        if (used != item.end() && used->is_number())
            entry.last_used = used->get<int64_t>();
        if (entry.a.empty() || entry.b.empty())
            continue;
        entry.a = AbsolutePath(entry.a);
        entry.b = AbsolutePath(entry.b);
        if (registry.Find(entry.a, entry.b))
            continue;  // the first is the newest
        registry.entries_.push_back(entry);
    }
    if (registry.entries_.size() > MAX_ENTRIES)
        registry.entries_.resize(MAX_ENTRIES);
    return registry;
}

bool PairRegistry::Save(const std::string& path, std::string& error) const {
    json pairs = json::array();
    for (const RecentPair& entry : entries_)
        pairs.push_back({{"a", entry.a}, {"b", entry.b}, {"project", entry.project}, {"last_used", entry.last_used}});
    const json document = {{"version", 1},
                           {"last_video_folder", last_video_folder},
                           {"last_project_folder", last_project_folder},
                           {"pairs", pairs}};
    std::error_code ec;
    const fs::path target(path);
    if (!target.parent_path().empty())
        fs::create_directories(target.parent_path(), ec);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << document.dump(1) << "\n";
        if (!out) {
            error = "cannot write " + temporary;
            return false;
        }
    }
    fs::rename(temporary, target, ec);
    if (ec) {
        fs::remove(target, ec);  // Windows does not replace on rename
        ec.clear();
        fs::rename(temporary, target, ec);
        if (ec) {
            error = "cannot replace " + path + ": " + ec.message();
            return false;
        }
    }
    return true;
}

void PairRegistry::Touch(const std::string& a, const std::string& b, const std::string& project, int64_t now) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const RecentPair& e) { return SamePair(e, a, b); }),
                   entries_.end());
    RecentPair entry;
    entry.a = AbsolutePath(a);
    entry.b = AbsolutePath(b);
    entry.project = AbsolutePath(project);
    entry.last_used = now;
    entries_.insert(entries_.begin(), entry);
    if (entries_.size() > MAX_ENTRIES)
        entries_.resize(MAX_ENTRIES);
}

const RecentPair* PairRegistry::Find(const std::string& a, const std::string& b) const {
    for (const RecentPair& entry : entries_)
        if (SamePair(entry, a, b))
            return &entry;
    return nullptr;
}

bool PairRegistry::Remove(const std::string& a, const std::string& b) {
    const size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const RecentPair& e) { return SamePair(e, a, b); }),
                   entries_.end());
    return entries_.size() != before;
}

bool PairRegistry::Rekey(const std::string& old_a, const std::string& old_b, const std::string& new_a,
                         const std::string& new_b, int64_t now) {
    const RecentPair* found = Find(old_a, old_b);
    if (!found)
        return false;
    const std::string project = found->project;  // Touch erases entries: copy first
    Remove(old_a, old_b);
    Touch(new_a, new_b, project, now);
    return true;
}

std::string FormatAgo(int64_t now, int64_t then) {
    if (then <= 0)
        return "never";
    const int64_t seconds = std::max<int64_t>(now - then, 0);
    char text[48];
    if (seconds < 60)
        return "just now";
    if (seconds < 3600) {
        std::snprintf(text, sizeof(text), "%lld min ago", (long long)(seconds / 60));
    } else if (seconds < 86400) {
        std::snprintf(text, sizeof(text), "%lld h ago", (long long)(seconds / 3600));
    } else if (seconds < 2 * 86400) {
        return "yesterday";
    } else if (seconds < 100 * 86400) {
        std::snprintf(text, sizeof(text), "%lld days ago", (long long)(seconds / 86400));
    } else {
        std::snprintf(text, sizeof(text), "%lld months ago", (long long)(seconds / (30 * 86400)));
    }
    return text;
}

std::string ProjectFileFor(const std::string& folder, const std::string& a, const std::string& b) {
    // FNV-1a over the two keys, with a separator no path holds.
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : Key(a) + '\n' + Key(b)) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%08x", (unsigned)(hash ^ (hash >> 32)));
    const std::string name = SafeStem(fs::path(a).stem().string(), 40) + "-vs-" + SafeStem(fs::path(b).stem().string(), 40) +
                             "-" + digits + ".compare.json";
    return (fs::path(folder) / name).string();
}

}  // namespace renderer::clips
