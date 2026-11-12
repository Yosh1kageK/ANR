// The slot cache around the playhead: reserving, eviction order, pins, what the decoder needs next, restart or continue.
#include "renderer/clips/frame_cache.h"

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

void load(SlotCache& cache, int64_t frame, const FrameWindow& w) {
    const int slot = cache.Reserve(frame, w);
    CHECK(slot >= 0);
    if (slot >= 0)
        cache.MarkReady(slot);
}

void basics() {
    SlotCache cache(4);
    const FrameWindow w = {10, 20, 12};
    CHECK_EQ(cache.capacity(), 4);
    CHECK(!cache.IsReady(10));
    const int slot = cache.Reserve(10, w);
    CHECK(slot >= 0);
    CHECK(cache.Present(10));
    CHECK(!cache.IsReady(10));  // loading
    int pinned = -1;
    CHECK(!cache.Pin(10, pinned));
    cache.MarkReady(slot);
    CHECK(cache.IsReady(10));
    CHECK(cache.Pin(10, pinned));
    CHECK_EQ(pinned, slot);
    CHECK_EQ(cache.pins(slot), 1);
    cache.Unpin(slot);
    CHECK_EQ(cache.pins(slot), 0);
    CHECK_EQ(cache.Reserve(10, w), -1);  // already there
    CHECK_EQ(cache.ReadyCount(), 1);
    cache.Free(slot);
    CHECK(!cache.Present(10));
}

void eviction_order() {
    SlotCache cache(4);
    FrameWindow w = {0, 100, 50};
    for (int64_t f : {10, 20, 60, 70})
        load(cache, f, w);
    // all in the window: a new frame ahead of the playhead evicts the farthest one behind it (10), not one ahead
    load(cache, 80, w);
    CHECK(!cache.Present(10));
    CHECK(cache.Present(20) && cache.Present(60) && cache.Present(70) && cache.Present(80));
    // the window moves: frames outside it go first, the farthest from the playhead first
    w = {55, 120, 90};
    load(cache, 90, w);  // evicts 20 (outside, farthest)
    CHECK(!cache.Present(20));
    load(cache, 91, w);  // 60 is outside the window too (55..120 contains it): 60 is behind the playhead, evictable
    CHECK(cache.Present(91));
    // frames ahead of the playhead are never dropped for a frame further ahead
    SlotCache ahead(2);
    const FrameWindow v = {80, 120, 90};
    load(ahead, 95, v);
    load(ahead, 96, v);
    CHECK_EQ(ahead.Reserve(100, v), -1);
    CHECK(ahead.Present(95) && ahead.Present(96));
}

void pins_and_loading_are_kept() {
    SlotCache cache(2);
    FrameWindow w = {0, 10, 5};
    load(cache, 1, w);
    int pin = -1;
    CHECK(cache.Pin(1, pin));
    const int loading = cache.Reserve(2, w);
    CHECK(loading >= 0);
    // slot 1 is pinned and slot 2 loading: nothing may be taken, even outside the window
    w = {100, 110, 105};
    CHECK_EQ(cache.Reserve(105, w), -1);
    cache.Unpin(pin);
    cache.MarkReady(loading);
    CHECK(cache.Reserve(105, w) >= 0);
}

void next_missing() {
    SlotCache cache(16);
    const FrameWindow w = {10, 20, 14};
    CHECK_EQ(cache.NextMissing(w), 14);  // empty: from the playhead
    for (int64_t f : {14, 15, 16})
        load(cache, f, w);
    CHECK_EQ(cache.NextMissing(w), 17);
    for (int64_t f = 17; f <= 20; f++)
        load(cache, f, w);
    CHECK_EQ(cache.NextMissing(w), 13);  // the window ahead is complete: behind, nearest first
    load(cache, 13, w);
    CHECK_EQ(cache.NextMissing(w), 12);
    // frames still loading count as present
    CHECK(cache.Reserve(12, w) >= 0);
    CHECK_EQ(cache.NextMissing({12, 20, 14}), -1);
    CHECK_EQ(cache.NextMissing({10, 20, 14}), 11);
}

void decode_plan() {
    // nothing missing: idle
    CHECK(PlanDecode(100, -1, 5000, 12, 90).action == DecodePlan::Action::IDLE);
    // the stream is where the need is, or a little before it: continue
    CHECK(PlanDecode(100, 100, 5000, 12, 90).action == DecodePlan::Action::CONTINUE);
    CHECK(PlanDecode(100, 150, 5000, 12, 90).action == DecodePlan::Action::CONTINUE);
    // too far ahead of the stream, behind it, or no stream: restart a little before the need
    DecodePlan p = PlanDecode(100, 400, 5000, 12, 90);
    CHECK(p.action == DecodePlan::Action::RESTART);
    CHECK_EQ(p.restart_at, 388);
    p = PlanDecode(500, 400, 5000, 12, 90);
    CHECK(p.action == DecodePlan::Action::RESTART);
    p = PlanDecode(-1, 5, 5000, 12, 90);
    CHECK(p.action == DecodePlan::Action::RESTART);
    CHECK_EQ(p.restart_at, 0);
    // a need beyond the recording: nothing to do
    CHECK(PlanDecode(100, 5000, 5000, 12, 90).action == DecodePlan::Action::IDLE);
}

}  // namespace

int main() {
    basics();
    eviction_order();
    pins_and_loading_are_kept();
    next_missing();
    decode_plan();
    return check_result("frame_cache");
}
