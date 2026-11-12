// The capture DLLs' log: one file per game session beside the DLL, nr_record-<date>-<time>.log, so that a crash's log
// survives the next start; the oldest beyond `keep` are deleted.
#pragma once

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace record {

inline FILE* OpenSessionLog(const std::string& dir, size_t keep = 10) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> logs;
    for (const auto& entry : fs::directory_iterator(fs::path(dir), ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("nr_record-", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0)
            logs.push_back(entry.path());
    }
    std::sort(logs.begin(), logs.end());  // the names sort by time
    for (size_t i = 0; keep > 0 && i + keep <= logs.size(); i++)  // room for this session's
        fs::remove(logs[i], ec);
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_s(&local, &now);
    char name[48];
    std::strftime(name, sizeof(name), "nr_record-%Y%m%d-%H%M%S.log", &local);
    return std::fopen((dir + "\\" + name).c_str(), "w");
}

}  // namespace record
