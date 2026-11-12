// The player over two simulated sources that decode on their own threads in real time: slow, stalling, with pinned
// frames, and the pair it hands out is always one moment of both recordings.
#include "renderer/clips/player.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A source whose frames are numbers: a thread "decodes" frame by frame at decode_fps (with stall windows), up to the
// interest window's end, and keeps the ones inside it. Pins are counted.
class FakeSource : public VideoSource {
public:
    FakeSource(int64_t frames, double fps, double decode_fps) : decode_fps_(decode_fps) {
        info_.width = 64;
        info_.height = 36;
        info_.fps = fps;
        info_.frames = frames;
        thread_ = std::thread([this] { Run(); });
    }
    ~FakeSource() override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    struct Stall {
        double t0, t1;  // seconds after start()
    };
    std::vector<Stall> stalls;
    void start() { start_ = now_seconds(); }

    const VideoInfo& Info() const override { return info_; }
    void SetInterest(int64_t first, int64_t last, int64_t playhead, int) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            first_ = first;
            last_ = last;
            playhead_ = playhead;
        }
        cv_.notify_all();
    }
    bool Ready(int64_t frame) const override {
        std::lock_guard<std::mutex> lock(mu_);
        return ready_.count(frame) > 0;
    }
    bool Acquire(int64_t frame, FrameRef& out) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ready_.count(frame))
            return false;
        pins_[frame]++;
        pinned_total_++;
        out.index = frame;
        out.slot = (uint32_t)frame;
        return true;
    }
    void Release(const FrameRef& frame) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pins_.find(frame.index);
        CHECK(it != pins_.end());
        if (it != pins_.end() && --it->second == 0)
            pins_.erase(it);
        pinned_total_--;
    }
    DecodeStats Stats() const override {
        std::lock_guard<std::mutex> lock(mu_);
        DecodeStats s;
        s.decode_fps = measured_;
        s.cached = (int64_t)ready_.size();
        return s;
    }
    std::string Describe() const override { return "fake"; }
    int pinned() const {
        std::lock_guard<std::mutex> lock(mu_);
        return pinned_total_;
    }
    int64_t max_cached() const {
        std::lock_guard<std::mutex> lock(mu_);
        return max_cached_;
    }

private:
    bool Stalled(double t) const {
        for (const Stall& s : stalls)
            if (t >= s.t0 && t < s.t1)
                return true;
        return false;
    }
    void Run() {
        std::unique_lock<std::mutex> lock(mu_);
        double busy = 0.0;
        int64_t made = 0;
        while (!quit_) {
            // the next frame the window wants: from the playhead forward
            int64_t want = -1;
            for (int64_t f = std::max(playhead_, first_); f <= last_ && f < info_.frames; f++)
                if (!ready_.count(f)) {
                    want = f;
                    break;
                }
            if (want < 0 || start_ == 0.0) {
                cv_.wait_for(lock, std::chrono::milliseconds(2));
                continue;
            }
            if (Stalled(now_seconds() - start_)) {
                cv_.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }
            lock.unlock();
            const double t0 = now_seconds();
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / decode_fps_));
            const double took = now_seconds() - t0;
            lock.lock();
            ready_.insert(want);
            // drop what the window no longer holds, unless pinned
            for (auto it = ready_.begin(); it != ready_.end();)
                if ((*it < first_ || *it > last_) && !pins_.count(*it))
                    it = ready_.erase(it);
                else
                    ++it;
            max_cached_ = std::max<int64_t>(max_cached_, (int64_t)ready_.size());
            busy += took;
            made++;
            if (made % 8 == 0) {
                measured_ = 8.0 / busy;
                busy = 0.0;
            }
        }
    }

    VideoInfo info_;
    double decode_fps_;
    double start_ = 0.0;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    bool quit_ = false;
    int64_t first_ = 0, last_ = -1, playhead_ = 0;
    std::set<int64_t> ready_;
    std::map<int64_t, int> pins_;
    int pinned_total_ = 0;
    int64_t max_cached_ = 0;
    double measured_ = 0.0;
};

struct Result {
    uint64_t shown = 0, held_ticks = 0;
    int64_t last_pair = -1;
    int out_of_order = 0;
};

Result run_player(Player& player, double seconds, double hz) {
    Result r;
    const double start = now_seconds();
    std::shared_ptr<const Pair> previous;
    while (now_seconds() - start < seconds) {
        const Player::Tick t = player.Update(now_seconds());
        if (t.changed) {
            r.shown++;
            // one moment of both recordings
            const Timeline& tl = player.clock().timeline();
            CHECK_EQ(t.pair->has_a, tl.has_a(t.pair->n));
            CHECK_EQ(t.pair->has_b, tl.has_b(t.pair->n));
            if (t.pair->has_a)
                CHECK_EQ(t.pair->a.index, t.pair->n);
            if (t.pair->has_b)
                CHECK_EQ(t.pair->b.index, tl.b_frame(t.pair->n));
            if (r.last_pair >= 0 && t.pair->n <= r.last_pair)
                r.out_of_order++;
            r.last_pair = t.pair->n;
        }
        if (t.buffering)
            r.held_ticks++;
        previous = t.pair;
        std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / hz));
    }
    return r;
}

void healthy() {
    FakeSource a(600, 60.0, 400.0), b(600, 60.0, 400.0);
    a.start();
    b.start();
    Player player(&a, &b, 5);
    player.clock().Seek(5);
    // the decoders get a head start
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    player.clock().Play();
    const Result r = run_player(player, 2.0, 240.0);
    CHECK(r.shown > 100 && r.shown < 140);  // about 2 s of 60 fps
    CHECK_EQ(r.out_of_order, 0);
    CHECK_EQ(player.mismatched(), 0u);
    CHECK_EQ(player.acquire_failures(), 0u);
    CHECK_EQ(player.clock().stats().stalls, 0u);
    // the cache stays within what was asked for: behind 120 + ahead 45 + the playhead
    CHECK(a.max_cached() <= 120 + 46 + 5);
}

void stalling() {
    FakeSource a(600, 60.0, 300.0), b(600, 60.0, 300.0);
    b.stalls.push_back({0.8, 2.4});
    a.start();
    b.start();
    Player player(&a, &b, 5);
    player.clock().Seek(5);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    player.clock().Play();
    const Result r = run_player(player, 4.0, 240.0);
    CHECK_EQ(r.out_of_order, 0);
    CHECK_EQ(player.mismatched(), 0u);
    CHECK(r.held_ticks > 20);  // the ring ran out during the stall: the pair was held, buffering
    CHECK(player.clock().stats().stalls >= 1u);
    CHECK(r.shown > 100);
}

void slow_decoders() {
    // 24 pairs per second against 60 fps: playback settles at a rate the decoders sustain
    FakeSource a(600, 60.0, 30.0), b(600, 60.0, 24.0);
    a.start();
    b.start();
    Player player(&a, &b, 0);
    player.clock().Play();
    const Result r = run_player(player, 4.0, 240.0);
    CHECK_EQ(r.out_of_order, 0);
    CHECK_EQ(player.mismatched(), 0u);
    CHECK(player.clock().decode_limited());
    CHECK(player.clock().rate() < 0.55);
    // it settled near what the slower decoder (24 pairs a second: 0.4 of real time) sustains, not at the floor
    CHECK(player.clock().stats().rate_min >= 0.2);
}

void pins_are_balanced() {
    FakeSource a(300, 60.0, 400.0), b(300, 60.0, 400.0);
    a.start();
    b.start();
    {
        Player player(&a, &b, 3);
        player.clock().Seek(3);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        player.clock().Play();
        std::vector<std::shared_ptr<const Pair>> held;  // the viewer's frames in flight
        const double start = now_seconds();
        while (now_seconds() - start < 0.8) {
            const Player::Tick t = player.Update(now_seconds());
            if (t.changed) {
                held.push_back(t.pair);
                if (held.size() > 3)
                    held.erase(held.begin());
            }
            CHECK(a.pinned() <= 5 && b.pinned() <= 5);  // the pairs in flight and the current one
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    // everything released once the player and the held pairs are gone
    CHECK_EQ(a.pinned(), 0);
    CHECK_EQ(b.pinned(), 0);
}

void changing_the_offset() {
    FakeSource a(300, 60.0, 400.0), b(300, 60.0, 400.0);
    a.start();
    b.start();
    Player player(&a, &b, 10);
    player.clock().Seek(100);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Player::Tick t = player.Update(now_seconds());
    for (int i = 0; i < 100 && !t.pair; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        t = player.Update(now_seconds());
    }
    CHECK(t.pair != nullptr);
    CHECK_EQ(t.pair->n, 100);
    CHECK_EQ(t.pair->b.index, 90);
    player.SetOffset(11);
    bool changed = false;
    for (int i = 0; i < 100 && !changed; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        t = player.Update(now_seconds());
        changed = t.changed;
    }
    CHECK(changed);
    CHECK_EQ(t.pair->n, 100);
    CHECK_EQ(t.pair->b.index, 89);
}

// An offset map: B's decoder window follows B's moving position (a pause that moves B 45 frames ahead, K rising a frame, and a
// jump of 150 that takes B back past what a decoder reads through), and every pair is one moment of both recordings.
void drifting_offsets() {
    const std::vector<OffsetPoint> points = {{0, 5.0}, {299, 5.0}, {300, -40.0}, {599, -38.0}, {600, 112.0}};
    for (int64_t start : {285, 585}) {
        FakeSource a(900, 60.0, 400.0), b(900, 60.0, 400.0);
        a.start();
        b.start();
        Player player(&a, &b, OffsetMap(points));
        player.clock().Seek(start);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        player.clock().Play();
        const Result r = run_player(player, 1.0, 240.0);
        CHECK(r.shown > 30);
        CHECK(r.last_pair > start + 30);  // through the jump at 300 or at 600
        CHECK_EQ(r.out_of_order, 0);
        CHECK_EQ(player.mismatched(), 0u);
        CHECK_EQ(player.acquire_failures(), 0u);
        // the sources never held much more than the windows asked for, jump or not
        CHECK(a.max_cached() <= 120 + 46 + 5);
        CHECK(b.max_cached() <= 120 + 46 + 5 + 50);
    }
}

}  // namespace

int main() {
    healthy();
    stalling();
    slow_decoders();
    pins_are_balanced();
    changing_the_offset();
    drifting_offsets();
    return check_result("player");
}
