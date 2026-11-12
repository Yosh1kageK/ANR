// The recent-pairs registry: adding, updating, dedupe by absolute path, missing files, removal, a damaged file, the
// rekey of a swap, the Recent list's times and the project file names of new pairs.
#include "renderer/clips/pair_registry.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "renderer/clips/check.h"

using namespace renderer::clips;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
    const char* test_tmp = std::getenv("TEST_TMPDIR");  // Bazel's, else the system's
    const fs::path dir = (test_tmp ? fs::path(test_tmp) : fs::temp_directory_path()) / (std::string("nr_pair_registry_") + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void touch_adds_updates_and_orders() {
    PairRegistry r;
    CHECK(r.entries().empty());
    r.Touch("/v/a.mp4", "/v/b.mp4", "/p/ab.compare.json", 100);
    r.Touch("/v/c.mp4", "/v/d.mp4", "/p/cd.compare.json", 200);
    CHECK_EQ((int)r.entries().size(), 2);
    CHECK(r.entries()[0].a == "/v/c.mp4");  // newest first
    // used again: moved to the front with its project and time updated, not added twice
    r.Touch("/v/a.mp4", "/v/b.mp4", "/p/ab2.compare.json", 300);
    CHECK_EQ((int)r.entries().size(), 2);
    CHECK(r.entries()[0].a == "/v/a.mp4" && r.entries()[0].project == "/p/ab2.compare.json");
    CHECK_EQ(r.entries()[0].last_used, 300);
    const RecentPair* found = r.Find("/v/c.mp4", "/v/d.mp4");
    CHECK(found != nullptr);
    CHECK(found && found->project == "/p/cd.compare.json");
    CHECK(r.Find("/v/a.mp4", "/v/d.mp4") == nullptr);
}

void pairs_are_keyed_by_absolute_paths_in_order() {
    PairRegistry r;
    r.Touch("/v/x/../a.mp4", "/v//b.mp4", "/p/1.compare.json", 1);
    CHECK(r.Find("/v/a.mp4", "/v/b.mp4") != nullptr);  // one file however it is spelled
    r.Touch("/v/a.mp4", "/v/b.mp4", "/p/2.compare.json", 2);
    CHECK_EQ((int)r.entries().size(), 1);
    // a relative path is made absolute against the working directory
    r.Touch("rel/a.mp4", "rel/b.mp4", "/p/3.compare.json", 3);
    CHECK(r.Find((fs::current_path() / "rel/a.mp4").string(), (fs::current_path() / "rel/b.mp4").string()) != nullptr);
    CHECK(r.entries()[0].a == (fs::current_path() / "rel/a.mp4").lexically_normal().string());
    // B with A is another pair
    r.Touch("/v/b.mp4", "/v/a.mp4", "/p/4.compare.json", 4);
    CHECK_EQ((int)r.entries().size(), 3);
    CHECK(r.Find("/v/a.mp4", "/v/b.mp4")->project == "/p/2.compare.json");
    CHECK(r.Find("/v/b.mp4", "/v/a.mp4")->project == "/p/4.compare.json");
}

void the_list_is_capped() {
    PairRegistry r;
    for (int i = 0; i < 50; i++)
        r.Touch("/v/a" + std::to_string(i) + ".mp4", "/v/b.mp4", "/p/x.compare.json", i);
    CHECK_EQ((int)r.entries().size(), (int)PairRegistry::MAX_ENTRIES);
    CHECK(r.entries().front().a == "/v/a49.mp4");
    CHECK(r.Find("/v/a0.mp4", "/v/b.mp4") == nullptr);  // the oldest went
    CHECK(r.Find("/v/a20.mp4", "/v/b.mp4") != nullptr);
}

void remove_and_rekey() {
    PairRegistry r;
    r.Touch("/v/a.mp4", "/v/b.mp4", "/p/ab.compare.json", 10);
    r.Touch("/v/c.mp4", "/v/d.mp4", "/p/cd.compare.json", 20);
    CHECK(!r.Remove("/v/a.mp4", "/v/z.mp4"));
    CHECK(r.Remove("/v/a.mp4", "/v/b.mp4"));
    CHECK_EQ((int)r.entries().size(), 1);
    CHECK(!r.Remove("/v/a.mp4", "/v/b.mp4"));
    // a swap: the project keeps its file and takes the new key
    r.Touch("/v/a.mp4", "/v/b.mp4", "/p/ab.compare.json", 30);
    CHECK(r.Rekey("/v/a.mp4", "/v/b.mp4", "/v/b.mp4", "/v/a.mp4", 40));
    CHECK(r.Find("/v/a.mp4", "/v/b.mp4") == nullptr);
    const RecentPair* swapped = r.Find("/v/b.mp4", "/v/a.mp4");
    CHECK(swapped && swapped->project == "/p/ab.compare.json" && swapped->last_used == 40);
    CHECK(r.entries()[0].a == "/v/b.mp4");
    // an entry the new key already had is replaced, and an unknown old key changes nothing
    r.Touch("/v/c.mp4", "/v/d.mp4", "/p/other.compare.json", 50);
    r.Touch("/v/d.mp4", "/v/c.mp4", "/p/old-dc.compare.json", 51);
    CHECK(r.Rekey("/v/c.mp4", "/v/d.mp4", "/v/d.mp4", "/v/c.mp4", 60));
    CHECK(r.Find("/v/d.mp4", "/v/c.mp4")->project == "/p/other.compare.json");
    CHECK(r.Find("/v/c.mp4", "/v/d.mp4") == nullptr);
    const size_t count = r.entries().size();
    CHECK(!r.Rekey("/v/q.mp4", "/v/r.mp4", "/v/r.mp4", "/v/q.mp4", 70));
    CHECK_EQ((int)r.entries().size(), (int)count);
}

void missing_files_show_in_the_status() {
    const fs::path dir = TempDir("status");
    std::ofstream(dir / "a.mp4") << "a";
    std::ofstream(dir / "b.mp4") << "b";
    std::ofstream(dir / "p.compare.json") << "{}";
    RecentPair pair;
    pair.a = (dir / "a.mp4").string();
    pair.b = (dir / "b.mp4").string();
    pair.project = (dir / "p.compare.json").string();
    PairStatus s = StatusOf(pair);
    CHECK(s.a_exists && s.b_exists && s.project_exists && s.usable());
    fs::remove(dir / "b.mp4");
    s = StatusOf(pair);
    CHECK(s.a_exists && !s.b_exists && !s.usable() && s.project_exists);
    fs::remove(dir / "p.compare.json");
    CHECK(!StatusOf(pair).project_exists);
    pair.project.clear();
    CHECK(!StatusOf(pair).project_exists);
    fs::remove_all(dir);
}

void save_and_load_round_trip() {
    const fs::path dir = TempDir("round");
    const std::string file = (dir / "sub" / "recent.json").string();  // the folder does not exist yet
    PairRegistry r;
    r.last_video_folder = "/videos";
    r.last_project_folder = "/projects";
    r.Touch("/v/a.mp4", "/v/b.mp4", "/p/ab.compare.json", 111);
    r.Touch("/v/c.mp4", "/v/d.mp4", "/p/cd.compare.json", 222);
    std::string error;
    CHECK(r.Save(file, error));
    CHECK(error.empty());
    CHECK(!fs::exists(file + ".tmp"));
    std::string note;
    const PairRegistry back = PairRegistry::Load(file, &note);
    CHECK(note.empty());
    CHECK_EQ((int)back.entries().size(), 2);
    CHECK(back.entries()[0].a == "/v/c.mp4" && back.entries()[0].b == "/v/d.mp4");
    CHECK(back.entries()[0].project == "/p/cd.compare.json" && back.entries()[0].last_used == 222);
    CHECK(back.entries()[1].a == "/v/a.mp4");
    CHECK(back.last_video_folder == "/videos" && back.last_project_folder == "/projects");
    // saving again replaces the file
    PairRegistry other;
    other.Touch("/v/x.mp4", "/v/y.mp4", "/p/xy.compare.json", 5);
    CHECK(other.Save(file, error));
    CHECK_EQ((int)PairRegistry::Load(file).entries().size(), 1);
    fs::remove_all(dir);
}

void a_missing_or_damaged_file_is_tolerated() {
    const fs::path dir = TempDir("damaged");
    std::string note;
    PairRegistry r = PairRegistry::Load((dir / "none.json").string(), &note);
    CHECK(r.entries().empty() && note.empty());  // nothing yet is not an error
    auto write = [&](const char* name, const std::string& text) {
        std::ofstream(dir / name, std::ios::binary) << text;
        return (dir / name).string();
    };
    r = PairRegistry::Load(write("garbage.json", "{not json at all"), &note);
    CHECK(r.entries().empty());
    CHECK(note.find("not valid JSON") != std::string::npos);
    note.clear();
    r = PairRegistry::Load(write("array.json", "[1, 2, 3]"), &note);
    CHECK(r.entries().empty() && !note.empty());
    note.clear();
    r = PairRegistry::Load(write("empty.json", ""), &note);
    CHECK(r.entries().empty() && !note.empty());
    // the right shape with wrong parts: bad entries are skipped, good ones stay, a duplicate keeps the newest
    note.clear();
    r = PairRegistry::Load(write("partial.json", R"({"version": 1, "last_video_folder": 5, "pairs": [
        7, {"a": "/v/a.mp4"}, {"a": 1, "b": 2},
        {"a": "/v/a.mp4", "b": "/v/b.mp4", "project": "/p/new.compare.json", "last_used": 9},
        {"a": "/v/a.mp4", "b": "/v/b.mp4", "project": "/p/old.compare.json", "last_used": 3},
        {"a": "/v/c.mp4", "b": "/v/d.mp4", "last_used": "yesterday"}]})"),
                           &note);
    CHECK(note.empty());
    CHECK_EQ((int)r.entries().size(), 2);
    CHECK(r.entries()[0].project == "/p/new.compare.json");
    CHECK(r.entries()[1].project.empty() && r.entries()[1].last_used == 0);
    CHECK(r.last_video_folder.empty());
    // a damaged registry is replaced by the next save
    PairRegistry fresh;
    fresh.Touch("/v/a.mp4", "/v/b.mp4", "/p/x.compare.json", 1);
    std::string error;
    CHECK(fresh.Save((dir / "garbage.json").string(), error));
    CHECK_EQ((int)PairRegistry::Load((dir / "garbage.json").string(), &note).entries().size(), 1);
    fs::remove_all(dir);
}

void times_read_like_a_list() {
    const int64_t now = 1'000'000'000;
    CHECK(FormatAgo(now, now) == "just now");
    CHECK(FormatAgo(now, now - 59) == "just now");
    CHECK(FormatAgo(now, now - 60) == "1 min ago");
    CHECK(FormatAgo(now, now - 59 * 60) == "59 min ago");
    CHECK(FormatAgo(now, now - 3600) == "1 h ago");
    CHECK(FormatAgo(now, now - 23 * 3600) == "23 h ago");
    CHECK(FormatAgo(now, now - 86400) == "yesterday");
    CHECK(FormatAgo(now, now - 3 * 86400) == "3 days ago");
    CHECK(FormatAgo(now, now - 150 * 86400) == "5 months ago");
    CHECK(FormatAgo(now, now + 500) == "just now");  // a clock that went back
    CHECK(FormatAgo(now, 0) == "never");
}

void project_files_of_new_pairs() {
    const std::string a = ProjectFileFor("/cache/compare", "/v/apex.mp4", "/v/dlssg.mp4");
    CHECK(a.rfind("/cache/compare/apex-vs-dlssg-", 0) == 0);
    CHECK(a.size() > 14 && a.substr(a.size() - 13) == ".compare.json");
    CHECK(ProjectFileFor("/cache/compare", "/v/apex.mp4", "/v/dlssg.mp4") == a);  // the same pair, the same file
    CHECK(ProjectFileFor("/cache/compare", "/v/x/../apex.mp4", "/v/dlssg.mp4") == a);
    // the same names in another folder, or in the other order, are other files
    std::set<std::string> names = {a, ProjectFileFor("/cache/compare", "/w/apex.mp4", "/v/dlssg.mp4"),
                                   ProjectFileFor("/cache/compare", "/v/dlssg.mp4", "/v/apex.mp4"),
                                   ProjectFileFor("/cache/compare", "/v/apex.mp4", "/w/dlssg.mp4")};
    CHECK_EQ((int)names.size(), 4);
    // stems are made safe and short
    const std::string odd = ProjectFileFor("/c", "/v/my clip: 4K?.mp4", "/v/" + std::string(100, 'x') + ".mkv");
    const std::string name = fs::path(odd).filename().string();
    CHECK(name.rfind("my_clip__4K_-vs-" + std::string(40, 'x') + "-", 0) == 0);
    CHECK(name.find(' ') == std::string::npos && name.find(':') == std::string::npos && name.find('?') == std::string::npos);
    CHECK(fs::path(odd).parent_path() == "/c");
}

}  // namespace

int main() {
    touch_adds_updates_and_orders();
    pairs_are_keyed_by_absolute_paths_in_order();
    the_list_is_capped();
    remove_and_rekey();
    missing_files_show_in_the_status();
    save_and_load_round_trip();
    a_missing_or_damaged_file_is_tolerated();
    times_read_like_a_list();
    project_files_of_new_pairs();
    return check_result("pair_registry");
}
