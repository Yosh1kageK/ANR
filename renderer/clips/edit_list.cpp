#include "renderer/clips/edit_list.h"

#include <algorithm>
#include <cmath>

namespace renderer::clips {

FreezeSequence make_freeze(const Area& area, const EditOptions& options) {
    FreezeSequence s;
    s.area_id = area.id;
    s.label = area.label;
    s.frame = (int64_t)std::llround(area.t0 * options.fps);
    const double ease = std::clamp(area.ease.value_or(options.ease), 0.0, 5.0);
    const double hold = std::clamp(area.hold.value_or(options.freeze_hold), 0.0, 60.0);
    s.ease_frames = (int64_t)std::llround(ease * options.fps);
    s.hold_frames = (int64_t)std::llround(hold * options.fps);
    s.view = view_for(area.rect);
    s.divider = area.divider;
    return s;
}

FreezePhase freeze_phase(const FreezeSequence& s, int64_t step) {
    if (step < s.ease_frames)
        return FreezePhase::ZOOMING_IN;
    return step <= s.ease_frames + s.hold_frames ? FreezePhase::HOLDING : FreezePhase::ZOOMING_OUT;
}

Shot freeze_shot(const FreezeSequence& s, const Shot& base, int64_t step) {
    const int64_t ease = s.ease_frames, hold = s.hold_frames;
    step = std::clamp<int64_t>(step, 0, s.length());
    double u = 1.0;  // the weight of the view over the base camera
    if (step < ease)
        u = smoothstep((double)step / (double)ease);
    else if (step > ease + hold)
        u = 1.0 - smoothstep((double)(step - ease - hold) / (double)ease);
    Shot out;
    out.camera = lerp_camera(base.camera, s.view, u);
    out.caption = (step >= ease && step <= ease + hold && !s.label.empty()) ? s.label : base.caption;
    double target = base.divider;
    if (s.divider.kind == AreaDivider::FIXED) {
        target = s.divider.fraction;
    } else if (s.divider.kind == AreaDivider::SWEEPING) {
        const double x = hold > 0 ? std::clamp((double)(step - ease) / (double)hold, 0.0, 1.0) : 0.0;
        target = s.divider.sweep_at(x);
    }
    out.divider = base.divider + (target - base.divider) * u;
    return out;
}

EditList::EditList(const std::vector<Area>& areas, const EditOptions& options) : options_(options) {
    spans_ = ordered_areas(areas);
    keys_ = camera_keys(spans_, options.ease);
    for (const Area& a : areas) {
        if (a.kind != AreaKind::FREEZE)
            continue;
        FreezeSequence s = make_freeze(a, options);
        if (s.frame < options.first || s.frame >= options.end)
            skipped_.push_back({a.id, "frame " + std::to_string((long long)s.frame) + " is outside the range"});
        else if (s.length() <= 0)
            skipped_.push_back({a.id, "no zoom and no hold"});
        else
            freezes_.push_back(std::move(s));
    }
    std::stable_sort(freezes_.begin(), freezes_.end(),
                     [](const FreezeSequence& a, const FreezeSequence& b) { return a.frame < b.frame; });
    int64_t inserted = 0;
    for (size_t i = 0; i < freezes_.size();) {
        Group g;
        g.pair = freezes_[i].frame;
        g.first = i;
        g.extra = 0;
        for (; i < freezes_.size() && freezes_[i].frame == g.pair; i++)
            g.extra += freezes_[i].length();
        g.count = i - g.first;
        g.out_start = (g.pair - options.first) + inserted;
        inserted += g.extra;
        groups_.push_back(g);
    }
    length_ = source_frames() + inserted;
}

Shot EditList::base_shot(int64_t pair) const {
    const double t = (double)pair / options_.fps;
    return {camera_at(keys_, t), divider_at(options_.divider, spans_, t), caption_at(spans_, t)};
}

int64_t EditList::first_output_of(int64_t pair) const {
    int64_t inserted = 0;
    for (const Group& g : groups_) {
        if (g.pair >= pair)
            break;
        inserted += g.extra;
    }
    return (pair - options_.first) + inserted;
}

EditEntry EditList::at(int64_t index) const {
    EditEntry e;
    if (length_ <= 0)
        return e;
    index = std::clamp<int64_t>(index, 0, length_ - 1);
    // The last group that starts at or before the index.
    auto it = std::upper_bound(groups_.begin(), groups_.end(), index,
                               [](int64_t i, const Group& g) { return i < g.out_start; });
    if (it != groups_.begin()) {
        const Group& g = *(it - 1);
        const int64_t s = index - g.out_start;
        if (s <= g.extra) {
            int64_t cum = 0;
            for (size_t k = 0; k < g.count; k++) {
                const FreezeSequence& f = freezes_[g.first + k];
                if (s <= cum + f.length()) {
                    e.pair = g.pair;
                    e.step = s - cum;
                    e.shot = freeze_shot(f, base_shot(g.pair), e.step);
                    e.frozen = s > 0;
                    e.freeze_area = f.area_id;
                    return e;
                }
                cum += f.length();
            }
        }
        // The group's last frame is out_start + extra; the one after it shows the next pair.
        e.pair = g.pair + (index - (g.out_start + g.extra));
    } else {
        e.pair = options_.first + index;
    }
    e.shot = base_shot(e.pair);
    return e;
}

}  // namespace renderer::clips
