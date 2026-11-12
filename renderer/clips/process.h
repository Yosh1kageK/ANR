// Child processes with pipes (ffmpeg and ffprobe): posix_spawn on Linux and macOS, CreateProcess on Windows.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace renderer::clips {

struct ProcessOptions {
    bool pipe_stdout = false;      // the child's stdout is a pipe the parent reads (else the null device)
    bool pipe_stdin = false;       // the child's stdin is a pipe the parent writes (else the null device)
    std::string stderr_path;       // the child's stderr goes to this file (empty: the parent's)
    int stdout_pipe_bytes = 1 << 20;  // the pipe's capacity where the system lets it be set
};

class Process {
public:
    Process();
    ~Process();  // kills a child that still runs
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // args[0] is the program: a path, or a name looked up on PATH.
    bool Start(const std::vector<std::string>& args, const ProcessOptions& options, std::string* error = nullptr);
    bool started() const;

    // Blocking reads from the child's stdout. Read returns what it got (0 at the end or on an error); ReadFull
    // returns false when the child ended before `bytes` arrived.
    size_t Read(void* data, size_t bytes);
    bool ReadFull(void* data, size_t bytes);
    // Writes to the child's stdin; false when it is gone.
    bool Write(const void* data, size_t bytes);
    void CloseStdin();
    void CloseStdout();

    // Terminate only signals the child (safe while another thread reads its stdout: the read then ends). Kill stops
    // it, closes the pipes and waits for it. Wait returns its exit code (after it ended on its own, or -1).
    void Terminate();
    void Kill();
    int Wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The running executable's path (empty where it cannot be found).
std::string ExecutablePath();

// ffmpeg and ffprobe: the environment variable (NR_FFMPEG, NR_FFPROBE), the folder of the executable, then PATH. A
// bare name when none of the first two exists (the spawn searches PATH).
std::string FindTool(const std::string& name, const std::string& environment_variable);

// Runs a program to completion and returns its stdout (empty on failure; `ok` says whether it exited with 0).
std::string RunCapture(const std::vector<std::string>& args, bool* ok = nullptr, size_t limit = 64u << 20);

}  // namespace renderer::clips
