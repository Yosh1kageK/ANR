#include "renderer/clips/captures/jobs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "renderer/clips/process.h"
#include <spdlog/spdlog.h>

namespace renderer::clips::captures {
namespace fs = std::filesystem;

namespace {

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AddLog(std::vector<std::string>& log, const std::string& line) {
    log.push_back(line);
    if (log.size() > 40)
        log.erase(log.begin(), log.begin() + (long)(log.size() - 40));
}

std::vector<std::string> TailOf(const std::string& path, size_t lines) {
    std::ifstream in(path);
    std::deque<std::string> tail;
    for (std::string line; std::getline(in, line);) {
        if (line.empty())
            continue;
        tail.push_back(line.substr(0, 400));
        if (tail.size() > lines)
            tail.pop_front();
    }
    return {tail.begin(), tail.end()};
}

}  // namespace

const char* JobStateName(JobState state) {
    switch (state) {
        case JobState::QUEUED: return "queued";
        case JobState::WAITING_GAME: return "waiting for the game to close";
        case JobState::RUNNING: return "running";
        case JobState::DONE: return "done";
        case JobState::FAILED: return "failed";
        case JobState::CANCELLED: return "cancelled";
    }
    return "";
}

std::string FailedMarker(const std::string& result_dir, const std::string& stem) {
    return result_dir + "/" + stem + ".failed";
}

JobQueue::JobQueue(Locations where) : where_(std::move(where)) {
    worker_ = std::thread([this] { Run(); });
}

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        if (process_)
            process_->Terminate();
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

int JobQueue::Add(const JobSpec& spec, const CaptureInfo& capture) {
    Job job;
    job.spec = spec;
    job.total = JobFrameCount(spec, capture);
    job.result_dir = ResultDir(where_, spec.capture_name, spec.generator);
    std::lock_guard<std::mutex> lock(mu_);
    job.id = next_id_++;
    job.status = "queued";
    jobs_.push_back(job);
    ++revision_;
    cv_.notify_all();
    return job.id;
}

int JobQueue::Retry(int id, const CaptureInfo& capture) {
    JobSpec spec;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.id == id; });
        if (it == jobs_.end() || it->state == JobState::QUEUED || it->state == JobState::RUNNING || it->state == JobState::WAITING_GAME)
            return 0;
        spec = it->spec;
    }
    return Add(spec, capture);
}

void JobQueue::Cancel(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Job& job : jobs_)
        if (job.id == id) {
            if (job.state == JobState::QUEUED || job.state == JobState::WAITING_GAME) {
                job.state = JobState::CANCELLED;
                job.status = "cancelled";
            } else if (job.state == JobState::RUNNING && running_id_ == id) {
                job.status = "cancelling";
                cancel_id_ = id;
                if (process_)
                    process_->Terminate();  // the tool's own process; a program it started finishes by itself
            }
        }
    ++revision_;
}

std::vector<Job> JobQueue::Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {jobs_.begin(), jobs_.end()};
}

bool JobQueue::Active() const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::any_of(jobs_.begin(), jobs_.end(), [](const Job& j) {
        return j.state == JobState::QUEUED || j.state == JobState::WAITING_GAME || j.state == JobState::RUNNING;
    });
}

void JobQueue::Update(int id, const std::function<void(Job&)>& change) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Job& job : jobs_)
        if (job.id == id) {
            if (job.state != JobState::CANCELLED)  // a job cancelled while it waited keeps its state
                change(job);
            break;
        }
    ++revision_;
}

void JobQueue::Run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] {
                return stop_ || std::any_of(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.state == JobState::QUEUED; });
            });
            if (stop_)
                return;
            job = *std::find_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.state == JobState::QUEUED; });
            running_id_ = job.id;
        }
        Execute(job);
        std::lock_guard<std::mutex> lock(mu_);
        running_id_ = 0;
    }
}

void JobQueue::Execute(Job& job) {
    const int id = job.id;
    const double started = Now();
    auto cancelled = [&] {
        std::lock_guard<std::mutex> lock(mu_);
        for (const Job& j : jobs_)
            if (j.id == id)
                return stop_ || cancel_id_ == id || j.state == JobState::CANCELLED;
        return true;
    };
    // A game running: wait (never stop it).
    bool waited = false;
    while (GamesRunning()) {
        if (cancelled())
            return;
        waited = true;
        Update(id, [](Job& j) {
            j.state = JobState::WAITING_GAME;
            j.status = "waiting for the game to close";
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    if (cancelled())
        return;
    (void)waited;
    std::error_code ec;
    fs::create_directories(job.result_dir, ec);
    const std::string log_dir = where_.Jobs() + "/" + job.spec.capture_name + "-" + std::to_string(id) + "-" +
                                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                   std::chrono::system_clock::now().time_since_epoch()).count());
    fs::create_directories(log_dir, ec);
    const std::string log_path = log_dir + "/stderr.log";
    for (const PairSpec& p : job.spec.pairs)
        fs::remove(FailedMarker(job.result_dir, p.Stem()), ec);
    const std::vector<std::string> command = JobCommand(where_, job.spec, job.result_dir);
    if (command.empty() || command[0].empty()) {
        Update(id, [](Job& j) {
            j.state = JobState::FAILED;
            j.status = "no tool for this generator here";
        });
        return;
    }
    // The tool's name: the script after an interpreter (python -I tool.py ...), else the program
    std::string tool = command[0];
    for (size_t i = 1; i < command.size() && i < 3; i++)
        if (command[i].size() > 3 && command[i].compare(command[i].size() - 3, 3, ".py") == 0)
            tool = command[i];
    std::string line = "$";
    for (const std::string& arg : command)
        line += " " + arg;
    Update(id, [&](Job& j) {
        j.state = JobState::RUNNING;
        j.status = "running " + fs::path(tool).filename().string();
        j.log_path = log_path;
        AddLog(j.log, line);
    });
    auto process = std::make_unique<Process>();
    ProcessOptions options;
    options.pipe_stdout = true;
    options.stderr_path = log_path;
    std::string error;
    Process* running = process.get();
    if (!process->Start(command, options, &error)) {
        Update(id, [&](Job& j) {
            j.state = JobState::FAILED;
            j.status = "cannot start the tool: " + error;
        });
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        process_ = std::move(process);
    }
    // stdout: one line per frame the tool wrote (its stem)
    std::string pending;
    char buffer[4096];
    for (;;) {
        const size_t got = running->Read(buffer, sizeof(buffer));
        if (got == 0)
            break;
        pending.append(buffer, got);
        size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (line.empty())
                continue;
            Update(id, [&](Job& j) {
                AddLog(j.log, line);
                j.done = std::min(j.done + 1, j.total);
                j.seconds = Now() - started;
            });
        }
    }
    const int code = running->Wait();
    {
        std::lock_guard<std::mutex> lock(mu_);
        process_.reset();
    }
    const bool stopped = cancelled();
    const std::vector<std::string> tail = code == 0 ? std::vector<std::string>() : TailOf(log_path, 12);
    Update(id, [&](Job& j) {
        j.seconds = Now() - started;
        if (stopped) {
            j.state = JobState::CANCELLED;
            j.status = "cancelled";
        } else if (code == 0) {
            j.state = JobState::DONE;
            j.done = j.total;
            char text[64];
            std::snprintf(text, sizeof(text), "done in %.1f s", j.seconds);
            j.status = text;
        } else {
            j.state = JobState::FAILED;
            j.status = "failed (exit " + std::to_string(code) + ")";
            for (const std::string& line : tail)
                AddLog(j.log, line);
        }
    });
    if (!stopped && code != 0) {  // marks the frames the job should have made, for the next session
        std::ofstream marker;
        for (const PairSpec& p : job.spec.pairs) {
            marker.open(FailedMarker(job.result_dir, p.Stem()));
            for (const std::string& line : tail)
                marker << line << "\n";
            marker.close();
        }
    }
    spdlog::info("capture job {} ({}): exit {}", id, job.spec.Title(), code);
}

}  // namespace renderer::clips::captures
