// What the Captures tab keeps and finds on disk: the capture folders (nr-record recordings, the folders the build lists,
// the ones the user added, the Steam games' own), the browser's state, the generators (DLSS-G and frame generation
// providers that nr-replay runs, and those of the folder formats a build adds) and where each generator's frames for a
// capture are cached. Pure apart from files and /proc, tested in part by captures_test.cpp.
//
// Layout (renderer/paths.h):
//   RecordingsFolder()/<date>-<time>-<game>/                 nr-record recordings (read only here: nothing is written
//                                                            into a capture)
//   paths::CaptureFrames()/<capture>/<generator>-<version>-<tool revision>/
//                                                            the frames a job made (.nrraw) and the tool's record
//   paths::Captures()/projects/<capture>__<stem>.json        the marks, labels and logos of one capture and pair
//   paths::Captures()/jobs/<capture>-<n>/                    a job's log
//   paths::Captures()/saved/                                 saved images
//   paths::State()/captures/                                 thumbnails, state.json (the folders and DLLs the user added)

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "renderer/clips/captures/capture_info.h"

namespace renderer::clips::captures {

// 16 hex digits of FNV-1a 64 over a string; of a file's bytes (the whole file up to 256 MiB, else its size and 1 MiB at each end).
std::string Hash64(const std::string& text);
std::string HashFile(const std::string& path);

// A Steam game, or the Windows executable under Proton, runs on this machine: a process whose executable lies in a Steam
// library (and two games by name). False off Linux.
bool GamesRunning();

struct Locations {
    std::string workspace;  // bazel run's workspace, else the working directory
    std::string executable; // this program (ReplayTool looks beside it)
    std::string browser;    // paths::Captures()
    std::string frames;     // paths::CaptureFrames()
    std::string cache;      // paths::State()/captures
    std::string Results(const std::string& capture) const { return frames + "/" + capture; }
    std::string Project(const std::string& capture, const std::string& stem) const {
        return browser + "/projects/" + capture + "__" + stem + ".json";
    }
    std::string Jobs() const { return browser + "/jobs"; }
    std::string Saved() const { return browser + "/saved"; }
    std::string Thumbs() const { return cache + "/thumbs"; }
    std::string StateFile() const { return cache + "/state.json"; }
};
Locations DefaultLocations(const std::string& workspace);

// Where nr-record writes recordings: $XDG_CACHE_HOME/nr-record (else ~/.cache/nr-record) on Linux, also under Proton;
// %LOCALAPPDATA%\Temp\nr-record on Windows.
std::string RecordingsFolder();
// The folders the browser lists without being told: the recordings folder and the build's own (paths::CaptureFolders).
std::vector<std::string> DefaultCaptureFolders();

struct State {
    std::vector<std::string> capture_folders;  // added by the user (a capture, or a folder of captures)
    std::vector<std::string> export_folders;   // added by the user, for a folder format's generators
    struct Dll {
        std::string kind;  // "nvidia" (NVIDIA's nvngx_dlssg.dll) or "provider" (a DLSS-G provider such as ANR FG's)
        std::string path;
    };
    std::vector<Dll> dlls;  // added by the user, for nr-replay
};
State LoadState(const Locations& where);
bool SaveState(const Locations& where, const State& state);

// One capture in the browser (what it says about itself, without its textures).
struct Entry {
    CaptureInfo info;
    std::string source;     // the folder it was found under ("default", the path the user added, a game's)
    uint64_t bytes = 0;     // the size of its files
};

// A capture folder: an nr-record recording, else one of the folder formats.
bool LoadCapture(const std::string& folder, CaptureInfo& capture, std::string& error);
// Capture folders under `roots` (each a capture itself, or a folder with captures in it), newest first.
std::vector<Entry> ScanCaptures(const std::vector<std::string>& roots);

// What the Steam games installed here hold for the tab (libraries from libraryfolders.vdf), in each game's folder or up to
// 4 levels below it (Unreal Engine games: <game>/<project>/Binaries/Win64):
// - capture folders: <OptiScaler>/custom/<provider>/dlssg/captures;
// - DLLs nr-replay runs: the providers' <OptiScaler>/custom/<provider>/dlssg/nvngx_dlssg.dll, and the games' own
//   nvngx_dlssg.dll (NVIDIA's DLSS-G).
struct SteamFiles {
    std::vector<std::string> capture_folders;
    std::vector<State::Dll> dlls;
    std::vector<std::string> dll_games;  // the game of each DLL
};
SteamFiles ScanSteam();

// --- Generators ---------------------------------------------------------------------------------------------------------

struct Generator {
    enum Kind { DLSSG, APEX } kind = APEX;  // NVIDIA's DLSS-G, or ANR FG and other providers
    std::string id;           // "nvidia:<version>", "provider:<version>", or the folder format's
    std::string label;        // "DLSS-G (Cyberpunk 2077)", "ANR FG (fbv)"
    std::string version;      // what identifies its frames: a DLL's hash, an NGX version, a model's id
    std::string detail;       // one line for its tooltip
    bool replay = false;      // nr-replay runs it (nr-record recordings)
    std::string dll;          // replay: the DLL
    std::string model;        // a folder format's: its tool's model argument
    std::string export_dir;   // a folder format's: the folder it was added from
    bool curated = false, is_default = false;
    bool available = true;
    std::string unavailable;  // why not
    std::string Dir() const;  // "<id>-<version>" in the cache (replay: "<kind>_<version>"), without the tool revision
};

// The replay host (record/replay): nr-replay.exe on Windows, its Wine runner nr_replay_wine.py on Linux; NR_REPLAY, else
// beside this executable, else in the workspace. Empty: not found.
std::string ReplayTool(const Locations& where);
// What identifies a DLL's frames: 12 hex digits over its size and its first and last 64 KiB.
std::string DllVersion(const std::string& path);

// nr-replay's generators for a recording: the DLLs the user added and those found in Steam games, one per kind and
// version. `nvidia` / `device`: the GPU, for DLSS-G's availability.
std::vector<Generator> ReplayGenerators(const Locations& where, const State& state, const SteamFiles& steam,
                                        const std::string& device, bool nvidia);
// The generators of a capture: nr-replay's for recordings, else its folder format's.
std::vector<Generator> ListGenerators(const Locations& where, const State& state, const SteamFiles& steam,
                                      const CaptureInfo& capture, const std::string& device, bool nvidia);

// The revision of the tool a generator's frames came from: 8 hex digits over its files.
std::string ToolRevision(const Locations& where, const Generator& generator);
// The cache folder of a generator's frames for a capture (with the tool revision).
std::string ResultDir(const Locations& where, const std::string& capture, const Generator& generator);

// A generator's frames of a capture that exist: by pair stem (PairSpec::Stem), the frames in the folder of the current tool
// revision, and in the folders of older revisions or versions of the same generator (stale). nr-replay's generated frames
// ("generated_f4-f5_t0.333.nrraw") count as their real pair's.
struct Cached {
    std::string stem, file;
    bool stale = false;
    std::string folder_name;  // the cache folder it is in
};
std::vector<Cached> CachedFrames(const Locations& where, const std::string& capture, const Generator& generator);

// --- Jobs and folder formats -----------------------------------------------------------------------------------------------

struct JobSpec {
    std::string capture_folder, capture_name;
    Generator generator;
    std::vector<PairSpec> pairs;  // empty with `all`
    bool all = false;             // every pair of the capture the generator can make
    std::string Title() const;
};

// Capture folders of a format other than nr-record, and the generators that run on them as tools: a build adds a format
// with AddFolderFormat (from a static initializer of a library linked with alwayslink).
struct FolderFormat {
    std::string name;    // CaptureInfo::format
    std::string marker;  // the file that makes a folder one of these
    std::function<bool(const std::string& folder, CaptureInfo& capture, std::string& error)> load;
    std::function<std::vector<Generator>(const Locations& where, const State& state, const std::string& device,
                                         bool nvidia)> generators;
    // A job's command line: its tool on the capture, the frames into result_dir, and one line on stdout per frame made.
    std::function<std::vector<std::string>(const Locations& where, const JobSpec& spec, const std::string& result_dir)>
        command;
    std::function<std::string(const Locations& where, const Generator& generator)> revision;
    // What recreating frames needs that is not here (the launcher's card); empty when nothing is missing.
    std::function<std::string(const Locations& where)> missing;
    // The Generators panel's words on how its jobs run.
    std::string about;
    // The Generators panel's button that adds a generator folder (State::export_folders; empty: none), its tooltip, the
    // file a chosen folder must hold, and where the dialog starts (relative to the workspace).
    std::string add_label, add_hint, add_needs, add_location;
};
void AddFolderFormat(FolderFormat format);
const std::vector<FolderFormat>& FolderFormats();
// The format of a capture (null: an nr-record recording, or a format this build does not have).
const FolderFormat* FormatOf(const CaptureInfo& capture);

// The tool's command line for a job (also for tests); empty when there is no tool for it.
std::vector<std::string> JobCommand(const Locations& where, const JobSpec& spec, const std::string& result_dir);
// nr-replay's: REPLAY --record REC --generator nvidia|provider --dll DLL --out DIR --raw [--frames A:B] (python3 -I first
// for the Linux runner, nr_replay_wine.py). A pair's job replays the real frames a to b (--frames a:b+1): the replay
// starts without history at a, and b's calls make the generated frames between them.
std::vector<std::string> ReplayCommand(const std::string& replay, const JobSpec& spec, const std::string& result_dir);
// How many frames a job asks for.
int JobFrameCount(const JobSpec& spec, const CaptureInfo& capture);
// Whether a generator can make a pair: nr-replay makes the recorded real pairs, not skip-ones.
bool CanMake(const Generator& generator, const PairSpec& pair);

}  // namespace renderer::clips::captures
