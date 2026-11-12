// Worker threads for For(n, f): f(i) for every i in [0, n), on the workers and the calling thread. Several threads may
// call For at once (a decoder and video writers share one pool).
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace record {

class Pool {
public:
    explicit Pool(unsigned threads) {
        for (unsigned i = 0; i < threads; i++)
            workers_.emplace_back([this] { Work(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        work_.notify_all();
        for (std::thread& t : workers_)
            t.join();
    }
    template <typename F>
    void For(size_t n, F&& f) {
        if (!n)
            return;
        Job job;
        job.n = n;
        job.run = [&f](size_t i) { f(i); };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(&job);
        }
        work_.notify_all();
        Run(job);
        std::unique_lock<std::mutex> lock(mutex_);
        jobs_.erase(std::find(jobs_.begin(), jobs_.end(), &job));
        done_.wait(lock, [&] { return job.active == 0; });  // every index is claimed: the workers in it finish them
    }

private:
    struct Job {
        size_t n = 0;
        std::function<void(size_t)> run;
        std::atomic<size_t> next{0};
        int active = 0;  // workers in it, under mutex_
    };
    static void Run(Job& job) {
        for (size_t i; (i = job.next.fetch_add(1)) < job.n;)
            job.run(i);
    }
    void Work() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            Job* job = nullptr;
            work_.wait(lock, [&] {
                for (Job* j : jobs_)
                    if (j->next.load() < j->n) {
                        job = j;
                        break;
                    }
                return stop_ || job;
            });
            if (stop_)
                return;
            job->active++;
            lock.unlock();
            Run(*job);
            lock.lock();
            job->active--;
            done_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable work_, done_;
    std::vector<Job*> jobs_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

}  // namespace record
