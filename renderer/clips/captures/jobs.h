// The Captures tab's background jobs: each runs a generator's tool, one at a time in the order they were added: nr-replay
// for recordings (record/replay: store.h ReplayCommand), else the tool of the capture's folder format. A job waits while a Steam game runs ("waiting for the game to close"; nothing is stopped),
// and a cancel ends the tool's own process only. The frames land in the cache folder of the generator (store.h
// ResultDir); the viewer lists what is there.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "renderer/clips/captures/store.h"

namespace renderer::clips {
class Process;
}

namespace renderer::clips::captures {

enum class JobState { QUEUED, WAITING_GAME, RUNNING, DONE, FAILED, CANCELLED };
const char* JobStateName(JobState state);

struct Job {
    int id = 0;
    JobSpec spec;
    JobState state = JobState::QUEUED;
    std::string status;                  // one line: what it is doing
    std::vector<std::string> log;        // the tail of the tool's output (stdout lines, then stderr's last lines on a failure)
    int done = 0, total = 0;             // frames written of frames asked for
    double seconds = 0.0;                // running time
    std::string result_dir, log_path;
};

class JobQueue {
public:
    explicit JobQueue(Locations where);
    ~JobQueue();

    // Queues a job; `capture` says how many frames "all" is. Returns its id.
    int Add(const JobSpec& spec, const CaptureInfo& capture);
    void Cancel(int id);
    // A copy of a failed or cancelled job, queued again.
    int Retry(int id, const CaptureInfo& capture);
    std::vector<Job> Snapshot() const;
    bool Active() const;
    // Counts up whenever a job changes state or writes a frame: the viewer re-reads the cache when it moves.
    uint64_t revision() const { return revision_; }

private:
    void Run();
    void Execute(Job& job);
    void Update(int id, const std::function<void(Job&)>& change);

    Locations where_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::unique_ptr<Process> process_;  // the running job's tool
    int running_id_ = 0, cancel_id_ = 0;
    int next_id_ = 1;
    bool stop_ = false;
    std::atomic<uint64_t> revision_{0};
    std::thread worker_;
};

// <result_dir>/<stem>.failed: the log tail of a frame that a tool failed on (shown until a run succeeds).
std::string FailedMarker(const std::string& result_dir, const std::string& stem);

}  // namespace renderer::clips::captures
