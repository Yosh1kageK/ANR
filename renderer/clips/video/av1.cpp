#include "renderer/clips/video/av1.h"

#include <algorithm>
#include <cstring>

#include "renderer/clips/video/bitreader.h"

namespace renderer::clips::video::av1 {

namespace {

enum ObuType : int {
    kObuSequenceHeader = 1,
    kObuTemporalDelimiter = 2,
    kObuFrameHeader = 3,
    kObuTileGroup = 4,
    kObuMetadata = 5,
    kObuFrame = 6,
    kObuRedundantFrameHeader = 7,
    kObuTileList = 8,
    kObuPadding = 15,
};

uint32_t Uvlc(BitReader& r) {
    int leading = 0;
    while (leading < 32 && !r.Flag() && !r.overrun())
        leading++;
    if (leading >= 32)
        return 0xffffffffu;
    return r.Bits(leading) + ((1u << leading) - 1);
}

bool Leb128(const uint8_t* data, size_t size, uint64_t* value, size_t* bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && i < size; i++) {
        v |= uint64_t(data[i] & 0x7f) << (i * 7);
        if (!(data[i] & 0x80)) {
            *value = v;
            *bytes = i + 1;
            return true;
        }
    }
    return false;
}

uint32_t TileLog2(uint32_t blk_size, uint32_t target) {
    uint32_t k = 0;
    while ((blk_size << k) < target)
        k++;
    return k;
}

int32_t InverseRecenter(int32_t r, int32_t v) {
    if (v > 2 * r)
        return v;
    if (v & 1)
        return r - ((v + 1) >> 1);
    return r + (v >> 1);
}

int32_t DecodeSubexp(BitReader& r, int32_t num_syms) {
    int i = 0, mk = 0;
    const int k = 3;
    while (true) {
        const int b2 = i ? k + i - 1 : k;
        const int a = 1 << b2;
        if (num_syms <= mk + 3 * a)
            return int32_t(r.NonSymmetric(uint32_t(num_syms - mk))) + mk;
        if (r.Flag()) {
            i++;
            mk += a;
        } else {
            return int32_t(r.Bits(b2)) + mk;
        }
    }
}

int32_t DecodeUnsignedSubexpWithRef(BitReader& r, int32_t mx, int32_t ref) {
    const int32_t v = DecodeSubexp(r, mx);
    if ((ref << 1) <= mx)
        return InverseRecenter(ref, v);
    return mx - 1 - InverseRecenter(mx - 1 - ref, v);
}

int32_t DecodeSignedSubexpWithRef(BitReader& r, int32_t low, int32_t high, int32_t ref) {
    return DecodeUnsignedSubexpWithRef(r, high - low, ref - low) + low;
}

// What the decoder's session depends on: a sequence header that changes any of it needs a new session.
bool SameDecodeConfig(const SequenceHeader& a, const SequenceHeader& b) {
    return a.seq_profile == b.seq_profile && a.max_frame_width_minus_1 == b.max_frame_width_minus_1 &&
           a.max_frame_height_minus_1 == b.max_frame_height_minus_1 && a.bit_depth == b.bit_depth &&
           a.frame_id_numbers_present == b.frame_id_numbers_present &&
           a.delta_frame_id_length_minus_2 == b.delta_frame_id_length_minus_2 &&
           a.additional_frame_id_length_minus_1 == b.additional_frame_id_length_minus_1 &&
           a.use_128x128_superblock == b.use_128x128_superblock && a.enable_filter_intra == b.enable_filter_intra &&
           a.enable_intra_edge_filter == b.enable_intra_edge_filter &&
           a.enable_interintra_compound == b.enable_interintra_compound &&
           a.enable_masked_compound == b.enable_masked_compound && a.enable_warped_motion == b.enable_warped_motion &&
           a.enable_dual_filter == b.enable_dual_filter && a.enable_order_hint == b.enable_order_hint &&
           a.enable_jnt_comp == b.enable_jnt_comp && a.enable_ref_frame_mvs == b.enable_ref_frame_mvs &&
           a.seq_force_screen_content_tools == b.seq_force_screen_content_tools &&
           a.seq_force_integer_mv == b.seq_force_integer_mv && a.order_hint_bits == b.order_hint_bits &&
           a.enable_superres == b.enable_superres && a.enable_cdef == b.enable_cdef &&
           a.enable_restoration == b.enable_restoration && a.color_range == b.color_range &&
           a.separate_uv_delta_q == b.separate_uv_delta_q && a.film_grain_params_present == b.film_grain_params_present;
}

}  // namespace

bool Parser::ParseSequenceHeader(const uint8_t* data, size_t size, std::string* why) {
    BitReader r(data, size);
    SequenceHeader s;
    s.seq_profile = uint8_t(r.Bits(3));
    s.still_picture = r.Flag();
    s.reduced_still_picture_header = r.Flag();
    if (s.reduced_still_picture_header) {
        s.seq_level_idx[0] = uint8_t(r.Bits(5));
    } else {
        s.timing_info_present = r.Flag();
        if (s.timing_info_present) {
            s.num_units_in_display_tick = r.Bits(32);
            s.time_scale = r.Bits(32);
            s.equal_picture_interval = r.Flag();
            if (s.equal_picture_interval)
                s.num_ticks_per_picture_minus_1 = Uvlc(r);
            s.decoder_model_info_present = r.Flag();
            if (s.decoder_model_info_present) {
                s.buffer_delay_length_minus_1 = uint8_t(r.Bits(5));
                r.Bits(32);  // num_units_in_decoding_tick
                s.buffer_removal_time_length_minus_1 = uint8_t(r.Bits(5));
                s.frame_presentation_time_length_minus_1 = uint8_t(r.Bits(5));
            }
        }
        const bool initial_display_delay_present = r.Flag();
        s.operating_points_cnt_minus_1 = uint8_t(r.Bits(5));
        for (int i = 0; i <= s.operating_points_cnt_minus_1; i++) {
            s.operating_point_idc[i] = uint16_t(r.Bits(12));
            s.seq_level_idx[i] = uint8_t(r.Bits(5));
            s.seq_tier[i] = s.seq_level_idx[i] > 7 ? uint8_t(r.Bits(1)) : 0;
            if (s.decoder_model_info_present) {
                s.decoder_model_present_for_this_op[i] = r.Flag();
                if (s.decoder_model_present_for_this_op[i]) {
                    const int n = s.buffer_delay_length_minus_1 + 1;
                    r.Bits(n);  // decoder_buffer_delay
                    r.Bits(n);  // encoder_buffer_delay
                    r.Bit();    // low_delay_mode_flag
                }
            }
            if (initial_display_delay_present && r.Flag())
                r.Bits(4);
        }
    }
    s.frame_width_bits_minus_1 = uint8_t(r.Bits(4));
    s.frame_height_bits_minus_1 = uint8_t(r.Bits(4));
    s.max_frame_width_minus_1 = r.Bits(s.frame_width_bits_minus_1 + 1);
    s.max_frame_height_minus_1 = r.Bits(s.frame_height_bits_minus_1 + 1);
    if (!s.reduced_still_picture_header)
        s.frame_id_numbers_present = r.Flag();
    if (s.frame_id_numbers_present) {
        s.delta_frame_id_length_minus_2 = uint8_t(r.Bits(4));
        s.additional_frame_id_length_minus_1 = uint8_t(r.Bits(3));
    }
    s.use_128x128_superblock = r.Flag();
    s.enable_filter_intra = r.Flag();
    s.enable_intra_edge_filter = r.Flag();
    if (!s.reduced_still_picture_header) {
        s.enable_interintra_compound = r.Flag();
        s.enable_masked_compound = r.Flag();
        s.enable_warped_motion = r.Flag();
        s.enable_dual_filter = r.Flag();
        s.enable_order_hint = r.Flag();
        if (s.enable_order_hint) {
            s.enable_jnt_comp = r.Flag();
            s.enable_ref_frame_mvs = r.Flag();
        }
        const bool choose_screen_content_tools = r.Flag();
        s.seq_force_screen_content_tools = choose_screen_content_tools ? 2 : uint8_t(r.Bits(1));
        if (s.seq_force_screen_content_tools > 0) {
            const bool choose_integer_mv = r.Flag();
            s.seq_force_integer_mv = choose_integer_mv ? 2 : uint8_t(r.Bits(1));
        } else {
            s.seq_force_integer_mv = 2;
        }
        s.order_hint_bits = s.enable_order_hint ? uint8_t(r.Bits(3) + 1) : 0;
    }
    s.enable_superres = r.Flag();
    s.enable_cdef = r.Flag();
    s.enable_restoration = r.Flag();
    // color_config
    const bool high_bitdepth = r.Flag();
    if (s.seq_profile == 2 && high_bitdepth)
        s.bit_depth = r.Flag() ? 12 : 10;
    else
        s.bit_depth = high_bitdepth ? 10 : 8;
    s.mono_chrome = s.seq_profile == 1 ? false : r.Flag();
    s.color_description_present = r.Flag();
    if (s.color_description_present) {
        s.color_primaries = uint8_t(r.Bits(8));
        s.transfer_characteristics = uint8_t(r.Bits(8));
        s.matrix_coefficients = uint8_t(r.Bits(8));
    }
    if (s.mono_chrome) {
        s.color_range = r.Flag();
        s.subsampling_x = s.subsampling_y = 1;
    } else if (s.color_primaries == 1 && s.transfer_characteristics == 13 && s.matrix_coefficients == 0) {
        s.color_range = true;
        s.subsampling_x = s.subsampling_y = 0;
    } else {
        s.color_range = r.Flag();
        if (s.seq_profile == 0) {
            s.subsampling_x = s.subsampling_y = 1;
        } else if (s.seq_profile == 1) {
            s.subsampling_x = s.subsampling_y = 0;
        } else if (s.bit_depth == 12) {
            s.subsampling_x = uint8_t(r.Bits(1));
            s.subsampling_y = s.subsampling_x ? uint8_t(r.Bits(1)) : 0;
        } else {
            s.subsampling_x = 1;
            s.subsampling_y = 0;
        }
        if (s.subsampling_x && s.subsampling_y)
            s.chroma_sample_position = uint8_t(r.Bits(2));
    }
    if (!s.mono_chrome)
        s.separate_uv_delta_q = r.Flag();
    s.film_grain_params_present = r.Flag();
    if (r.overrun()) {
        *why = "damaged AV1 sequence header";
        return false;
    }
    if (s.seq_profile != 0) {
        *why = "AV1 profile " + std::to_string(s.seq_profile) + " (only Main, 4:2:0, is supported)";
        return false;
    }
    if (s.mono_chrome || s.subsampling_x != 1 || s.subsampling_y != 1) {
        *why = "AV1 monochrome or non-4:2:0 video is not supported";
        return false;
    }
    if (s.bit_depth != 8 && s.bit_depth != 10) {
        *why = "AV1 bit depth " + std::to_string(s.bit_depth) + " is not supported";
        return false;
    }
    if (s.film_grain_params_present) {
        *why = "AV1 film grain is not applied (the sequence enables film_grain_params)";
        return false;
    }
    if (s.enable_superres) {
        *why = "AV1 super-resolution is not supported";
        return false;
    }
    if (s.reduced_still_picture_header || s.still_picture) {
        *why = "AV1 still pictures are not supported";
        return false;
    }
    // Operating point 0 only; spatial layers would need output of the highest layer.
    if (((s.operating_point_idc[0] >> 8) & 0xf) > 1) {
        *why = "AV1 spatial scalability is not supported";
        return false;
    }
    if (have_sequence_ && decode_config_locked_ && !SameDecodeConfig(seq_, s)) {
        *why = "the AV1 sequence header changes inside the stream";
        return false;
    }
    seq_ = s;
    have_sequence_ = true;
    return true;
}

void Parser::LockSequence() { decode_config_locked_ = have_sequence_; }

bool Parser::Configure(const std::vector<uint8_t>& av1c, std::string* why) {
    have_sequence_ = false;
    Reset();
    if (av1c.size() <= 4)
        return true;  // the sequence header comes with the first key frame
    size_t pos = 4;
    while (pos < av1c.size()) {
        const uint8_t header = av1c[pos];
        const int type = (header >> 3) & 15;
        const bool ext = (header >> 2) & 1, has_size = (header >> 1) & 1;
        size_t head = 1 + (ext ? 1 : 0);
        if (!has_size || pos + head >= av1c.size())
            return true;
        uint64_t obu_size;
        size_t leb;
        if (!Leb128(av1c.data() + pos + head, av1c.size() - pos - head, &obu_size, &leb) ||
            pos + head + leb + obu_size > av1c.size())
            return true;
        if (type == kObuSequenceHeader)
            return ParseSequenceHeader(av1c.data() + pos + head + leb, size_t(obu_size), why);
        pos += head + leb + size_t(obu_size);
    }
    return true;
}

void Parser::Reset() {
    for (Slot& s : slots_)
        s = Slot{};
    std::memset(order_hints_, 0, sizeof(order_hints_));
    in_frame_ = false;
    current_ = Picture{};
}

int Parser::RelativeDist(int a, int b) const {
    if (!seq_.enable_order_hint)
        return 0;
    int diff = a - b;
    const int m = 1 << (seq_.order_hint_bits - 1);
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

// 7.8 set_frame_refs
bool Parser::SetFrameRefs(FrameHeader* h, int last_idx, int gold_idx) {
    int ref_frame_idx[kRefsPerFrame];
    for (int& v : ref_frame_idx)
        v = -1;
    ref_frame_idx[0] = last_idx;
    ref_frame_idx[3] = gold_idx;  // GOLDEN_FRAME - LAST_FRAME
    bool used[kNumRefFrames] = {};
    used[last_idx] = used[gold_idx] = true;
    const int cur_frame_hint = 1 << (seq_.order_hint_bits - 1);
    int shifted[kNumRefFrames];
    for (int i = 0; i < kNumRefFrames; i++)
        shifted[i] = cur_frame_hint + RelativeDist(slots_[i].order_hint, h->order_hint);
    auto find_latest_backward = [&]() {
        int ref = -1, latest = 0;
        for (int i = 0; i < kNumRefFrames; i++)
            if (!used[i] && shifted[i] >= cur_frame_hint && (ref < 0 || shifted[i] >= latest)) {
                ref = i;
                latest = shifted[i];
            }
        return ref;
    };
    auto find_earliest_backward = [&]() {
        int ref = -1, earliest = 0;
        for (int i = 0; i < kNumRefFrames; i++)
            if (!used[i] && shifted[i] >= cur_frame_hint && (ref < 0 || shifted[i] < earliest)) {
                ref = i;
                earliest = shifted[i];
            }
        return ref;
    };
    auto find_latest_forward = [&]() {
        int ref = -1, latest = 0;
        for (int i = 0; i < kNumRefFrames; i++)
            if (!used[i] && shifted[i] < cur_frame_hint && (ref < 0 || shifted[i] >= latest)) {
                ref = i;
                latest = shifted[i];
            }
        return ref;
    };
    int ref = find_latest_backward();
    if (ref >= 0) {
        ref_frame_idx[6] = ref;  // ALTREF
        used[ref] = true;
    }
    ref = find_earliest_backward();
    if (ref >= 0) {
        ref_frame_idx[4] = ref;  // BWDREF
        used[ref] = true;
    }
    ref = find_earliest_backward();
    if (ref >= 0) {
        ref_frame_idx[5] = ref;  // ALTREF2
        used[ref] = true;
    }
    static const int kRefFrameList[5] = {2, 3, 5, 6, 7};  // LAST2, LAST3, BWDREF, ALTREF2, ALTREF
    for (int i = 0; i < kRefsPerFrame - 2; i++) {
        const int ref_frame = kRefFrameList[i];
        if (ref_frame_idx[ref_frame - 1] < 0) {
            ref = find_latest_forward();
            if (ref >= 0) {
                ref_frame_idx[ref_frame - 1] = ref;
                used[ref] = true;
            }
        }
    }
    ref = -1;
    int earliest = 0;
    for (int i = 0; i < kNumRefFrames; i++)
        if (ref < 0 || shifted[i] < earliest) {
            ref = i;
            earliest = shifted[i];
        }
    for (int i = 0; i < kRefsPerFrame; i++) {
        if (ref_frame_idx[i] < 0)
            ref_frame_idx[i] = ref;
        h->ref_frame_idx[i] = uint8_t(ref_frame_idx[i]);
    }
    return true;
}

bool Parser::ParseFrameHeader(BitReader& r, FrameHeader* hp, int temporal_id, int spatial_id, std::string* why) {
    FrameHeader& h = *hp;
    h = FrameHeader{};
    const SequenceHeader& s = seq_;
    const int id_len = s.frame_id_numbers_present ? s.additional_frame_id_length_minus_1 + s.delta_frame_id_length_minus_2 + 3 : 0;
    const uint8_t all_frames = 0xff;

    h.show_existing_frame = r.Flag();
    if (h.show_existing_frame) {
        h.frame_to_show_map_idx = uint8_t(r.Bits(3));
        if (s.decoder_model_info_present && !s.equal_picture_interval)
            r.Bits(s.frame_presentation_time_length_minus_1 + 1);
        if (s.frame_id_numbers_present)
            r.Bits(id_len);
        const Slot& slot = slots_[h.frame_to_show_map_idx];
        if (!slot.valid) {
            *why = "AV1 show_existing_frame of an empty reference slot";
            return false;
        }
        h.frame_type = slot.frame_type;
        h.refresh_frame_flags = h.frame_type == kKeyFrame ? all_frames : 0;
        h.show_frame = true;
        return !r.overrun();
    }
    h.frame_type = uint8_t(r.Bits(2));
    const bool frame_is_intra = h.frame_type == kIntraOnlyFrame || h.frame_type == kKeyFrame;
    h.show_frame = r.Flag();
    if (h.show_frame && s.decoder_model_info_present && !s.equal_picture_interval)
        r.Bits(s.frame_presentation_time_length_minus_1 + 1);
    if (h.show_frame)
        h.showable_frame = h.frame_type != kKeyFrame;
    else
        h.showable_frame = r.Flag();
    if (h.frame_type == kSwitchFrame || (h.frame_type == kKeyFrame && h.show_frame))
        h.error_resilient_mode = true;
    else
        h.error_resilient_mode = r.Flag();
    if (h.frame_type == kKeyFrame && h.show_frame) {
        for (Slot& slot : slots_) {
            slot.valid = false;
            slot.order_hint = 0;
        }
        std::memset(order_hints_, 0, sizeof(order_hints_));
    }
    h.disable_cdf_update = r.Flag();
    h.allow_screen_content_tools = s.seq_force_screen_content_tools == 2 ? r.Flag() : s.seq_force_screen_content_tools != 0;
    if (h.allow_screen_content_tools)
        h.force_integer_mv = s.seq_force_integer_mv == 2 ? r.Flag() : s.seq_force_integer_mv != 0;
    if (s.frame_id_numbers_present) {
        h.current_frame_id = r.Bits(id_len);
        const uint32_t diff_len = s.delta_frame_id_length_minus_2 + 2u;
        for (Slot& slot : slots_) {
            if (h.current_frame_id > (1u << diff_len)) {
                if (slot.frame_id > h.current_frame_id || slot.frame_id < h.current_frame_id - (1u << diff_len))
                    slot.valid = false;
            } else if (slot.frame_id > h.current_frame_id &&
                       slot.frame_id < (1u << id_len) + h.current_frame_id - (1u << diff_len)) {
                slot.valid = false;
            }
        }
    }
    if (h.frame_type == kSwitchFrame)
        h.frame_size_override_flag = true;
    else
        h.frame_size_override_flag = r.Flag();
    h.order_hint = uint8_t(r.Bits(s.order_hint_bits));
    h.primary_ref_frame = (frame_is_intra || h.error_resilient_mode) ? kPrimaryRefNone : uint8_t(r.Bits(3));
    if (s.decoder_model_info_present) {
        h.buffer_removal_time_present = r.Flag();
        if (h.buffer_removal_time_present) {
            for (int op = 0; op <= s.operating_points_cnt_minus_1; op++) {
                if (!s.decoder_model_present_for_this_op[op])
                    continue;
                const uint32_t idc = s.operating_point_idc[op];
                const bool in_temporal = (idc >> temporal_id) & 1;
                const bool in_spatial = (idc >> (spatial_id + 8)) & 1;
                if (idc == 0 || (in_temporal && in_spatial))
                    r.Bits(s.buffer_removal_time_length_minus_1 + 1);
            }
        }
    }
    if (h.frame_type == kSwitchFrame || (h.frame_type == kKeyFrame && h.show_frame))
        h.refresh_frame_flags = all_frames;
    else
        h.refresh_frame_flags = uint8_t(r.Bits(8));
    if ((!frame_is_intra || h.refresh_frame_flags != all_frames) && h.error_resilient_mode && s.enable_order_hint) {
        for (int i = 0; i < kNumRefFrames; i++) {
            const uint8_t ref_order_hint = uint8_t(r.Bits(s.order_hint_bits));
            if (ref_order_hint != slots_[i].order_hint || !slots_[i].valid) {
                *why = "AV1 error resilient frame with a missing reference (a grey reference frame is not implemented)";
                return false;
            }
        }
    }

    const uint32_t sb_shift = s.use_128x128_superblock ? 5 : 4;
    uint32_t mi_cols = 0, mi_rows = 0;
    auto frame_size = [&]() {
        if (h.frame_size_override_flag) {
            h.frame_width = r.Bits(s.frame_width_bits_minus_1 + 1) + 1;
            h.frame_height = r.Bits(s.frame_height_bits_minus_1 + 1) + 1;
        } else {
            h.frame_width = s.max_frame_width_minus_1 + 1;
            h.frame_height = s.max_frame_height_minus_1 + 1;
        }
        // superres_params (refused at the sequence header when enabled)
        h.use_superres = false;
        h.upscaled_width = h.frame_width;
    };
    auto compute_image_size = [&]() {
        mi_cols = 2 * ((h.frame_width + 7) >> 3);
        mi_rows = 2 * ((h.frame_height + 7) >> 3);
    };
    auto render_size = [&]() {
        h.render_and_frame_size_different = r.Flag();
        if (h.render_and_frame_size_different) {
            h.render_width = r.Bits(16) + 1;
            h.render_height = r.Bits(16) + 1;
        } else {
            h.render_width = h.upscaled_width;
            h.render_height = h.frame_height;
        }
    };

    if (h.frame_type == kKeyFrame || h.frame_type == kIntraOnlyFrame) {
        frame_size();
        compute_image_size();
        render_size();
        if (h.allow_screen_content_tools && h.upscaled_width == h.frame_width)
            h.allow_intrabc = r.Flag();
    } else {
        if (!s.enable_order_hint) {
            h.frame_refs_short_signaling = false;
        } else {
            h.frame_refs_short_signaling = r.Flag();
            if (h.frame_refs_short_signaling) {
                const int last_idx = int(r.Bits(3));
                const int gold_idx = int(r.Bits(3));
                if (!SetFrameRefs(&h, last_idx, gold_idx)) {
                    *why = "AV1 set_frame_refs failed";
                    return false;
                }
            }
        }
        for (int i = 0; i < kRefsPerFrame; i++) {
            if (!h.frame_refs_short_signaling)
                h.ref_frame_idx[i] = uint8_t(r.Bits(3));
            if (s.frame_id_numbers_present)
                r.Bits(s.delta_frame_id_length_minus_2 + 2);
            if (!slots_[h.ref_frame_idx[i]].valid) {
                *why = "AV1 frame references an empty slot (decoding started at a frame that is not a start point)";
                return false;
            }
        }
        if (h.frame_size_override_flag && !h.error_resilient_mode) {
            bool found = false;
            for (int i = 0; i < kRefsPerFrame && !found; i++) {
                found = r.Flag();
                if (found) {
                    const Slot& slot = slots_[h.ref_frame_idx[i]];
                    h.upscaled_width = slot.upscaled_width;
                    h.frame_width = h.upscaled_width;
                    h.frame_height = slot.frame_height;
                    h.render_width = slot.render_width;
                    h.render_height = slot.render_height;
                }
            }
            if (!found) {
                frame_size();
                render_size();
            }
            compute_image_size();
        } else {
            frame_size();
            compute_image_size();
            render_size();
        }
        h.allow_high_precision_mv = h.force_integer_mv ? false : r.Flag();
        h.is_filter_switchable = r.Flag();
        h.interpolation_filter = h.is_filter_switchable ? 4 : uint8_t(r.Bits(2));
        h.is_motion_mode_switchable = r.Flag();
        h.use_ref_frame_mvs = (h.error_resilient_mode || !s.enable_ref_frame_mvs) ? false : r.Flag();
        for (int i = 0; i < kRefsPerFrame; i++)
            order_hints_[1 + i] = slots_[h.ref_frame_idx[i]].order_hint;
    }
    if (h.frame_width != s.width() || h.frame_height != s.height()) {
        *why = "AV1 frame size " + std::to_string(h.frame_width) + "x" + std::to_string(h.frame_height) +
               " differs from the sequence's " + std::to_string(s.width()) + "x" + std::to_string(s.height()) +
               " (reference scaling is not supported)";
        return false;
    }
    h.disable_frame_end_update_cdf = h.disable_cdf_update ? true : r.Flag();

    // Inherited state: setup_past_independence or load_previous.
    int32_t prev_gm[8][6];
    for (int ref = 0; ref < 8; ref++)
        for (int i = 0; i < 6; i++)
            prev_gm[ref][i] = (i % 3 == 2) ? (1 << 16) : 0;
    if (h.primary_ref_frame == kPrimaryRefNone) {
        // loop_filter_ref_deltas and the feature data keep their defaults (zero feature data)
    } else {
        const Slot& prev = slots_[h.ref_frame_idx[h.primary_ref_frame]];
        std::memcpy(h.loop_filter_ref_deltas, prev.lf_ref_deltas, sizeof(h.loop_filter_ref_deltas));
        std::memcpy(h.loop_filter_mode_deltas, prev.lf_mode_deltas, sizeof(h.loop_filter_mode_deltas));
        std::memcpy(h.feature_enabled, prev.feature_enabled, sizeof(h.feature_enabled));
        std::memcpy(h.feature_data, prev.feature_data, sizeof(h.feature_data));
        std::memcpy(prev_gm, prev.gm_params, sizeof(prev_gm));
    }

    // tile_info
    {
        const uint32_t sb_cols = s.use_128x128_superblock ? ((mi_cols + 31) >> 5) : ((mi_cols + 15) >> 4);
        const uint32_t sb_rows = s.use_128x128_superblock ? ((mi_rows + 31) >> 5) : ((mi_rows + 15) >> 4);
        const uint32_t sb_size = sb_shift + 2;
        const uint32_t max_tile_width_sb = 4096u >> sb_size;
        uint32_t max_tile_area_sb = (4096u * 2304u) >> (2 * sb_size);
        const uint32_t min_log2_tile_cols = TileLog2(max_tile_width_sb, sb_cols);
        const uint32_t max_log2_tile_cols = TileLog2(1, std::min(sb_cols, 64u));
        const uint32_t max_log2_tile_rows = TileLog2(1, std::min(sb_rows, 64u));
        const uint32_t min_log2_tiles = std::max(min_log2_tile_cols, TileLog2(max_tile_area_sb, sb_rows * sb_cols));
        h.uniform_tile_spacing = r.Flag();
        if (h.uniform_tile_spacing) {
            h.tile_cols_log2 = min_log2_tile_cols;
            while (h.tile_cols_log2 < max_log2_tile_cols) {
                if (r.Flag())
                    h.tile_cols_log2++;
                else
                    break;
            }
            const uint32_t tile_width_sb = (sb_cols + (1u << h.tile_cols_log2) - 1) >> h.tile_cols_log2;
            uint32_t i = 0;
            for (uint32_t start = 0; start < sb_cols; start += tile_width_sb) {
                h.mi_col_starts[i] = uint16_t(start << sb_shift);
                h.width_in_sbs_minus_1[i] = uint16_t(std::min(tile_width_sb, sb_cols - start) - 1);
                i++;
            }
            h.mi_col_starts[i] = uint16_t(mi_cols);
            h.tile_cols = i;
            const uint32_t min_log2_tile_rows = min_log2_tiles > h.tile_cols_log2 ? min_log2_tiles - h.tile_cols_log2 : 0;
            h.tile_rows_log2 = min_log2_tile_rows;
            while (h.tile_rows_log2 < max_log2_tile_rows) {
                if (r.Flag())
                    h.tile_rows_log2++;
                else
                    break;
            }
            const uint32_t tile_height_sb = (sb_rows + (1u << h.tile_rows_log2) - 1) >> h.tile_rows_log2;
            i = 0;
            for (uint32_t start = 0; start < sb_rows; start += tile_height_sb) {
                h.mi_row_starts[i] = uint16_t(start << sb_shift);
                h.height_in_sbs_minus_1[i] = uint16_t(std::min(tile_height_sb, sb_rows - start) - 1);
                i++;
            }
            h.mi_row_starts[i] = uint16_t(mi_rows);
            h.tile_rows = i;
        } else {
            uint32_t widest_tile_sb = 0, start_sb = 0, i = 0;
            for (; start_sb < sb_cols && i < 64; i++) {
                h.mi_col_starts[i] = uint16_t(start_sb << sb_shift);
                const uint32_t max_width = std::min(sb_cols - start_sb, max_tile_width_sb);
                const uint32_t size_sb = r.NonSymmetric(max_width) + 1;
                h.width_in_sbs_minus_1[i] = uint16_t(size_sb - 1);
                widest_tile_sb = std::max(size_sb, widest_tile_sb);
                start_sb += size_sb;
            }
            h.mi_col_starts[i] = uint16_t(mi_cols);
            h.tile_cols = i;
            h.tile_cols_log2 = TileLog2(1, h.tile_cols);
            max_tile_area_sb = min_log2_tiles > 0 ? (sb_rows * sb_cols) >> (min_log2_tiles + 1) : sb_rows * sb_cols;
            const uint32_t max_tile_height_sb = std::max(max_tile_area_sb / std::max(widest_tile_sb, 1u), 1u);
            start_sb = 0;
            for (i = 0; start_sb < sb_rows && i < 64; i++) {
                h.mi_row_starts[i] = uint16_t(start_sb << sb_shift);
                const uint32_t max_height = std::min(sb_rows - start_sb, max_tile_height_sb);
                const uint32_t size_sb = r.NonSymmetric(max_height) + 1;
                h.height_in_sbs_minus_1[i] = uint16_t(size_sb - 1);
                start_sb += size_sb;
            }
            h.mi_row_starts[i] = uint16_t(mi_rows);
            h.tile_rows = i;
            h.tile_rows_log2 = TileLog2(1, h.tile_rows);
        }
        if (h.tile_cols_log2 > 0 || h.tile_rows_log2 > 0) {
            h.context_update_tile_id = r.Bits(int(h.tile_rows_log2 + h.tile_cols_log2));
            h.tile_size_bytes_minus_1 = r.Bits(2);
        }
    }

    // quantization_params
    h.base_q_idx = uint8_t(r.Bits(8));
    auto read_delta_q = [&]() -> int8_t { return r.Flag() ? int8_t(r.Signed(7)) : int8_t(0); };
    h.delta_q_y_dc = read_delta_q();
    h.diff_uv_delta = s.separate_uv_delta_q ? r.Flag() : false;
    h.delta_q_u_dc = read_delta_q();
    h.delta_q_u_ac = read_delta_q();
    if (h.diff_uv_delta) {
        h.delta_q_v_dc = read_delta_q();
        h.delta_q_v_ac = read_delta_q();
    } else {
        h.delta_q_v_dc = h.delta_q_u_dc;
        h.delta_q_v_ac = h.delta_q_u_ac;
    }
    h.using_qmatrix = r.Flag();
    if (h.using_qmatrix) {
        h.qm_y = uint8_t(r.Bits(4));
        h.qm_u = uint8_t(r.Bits(4));
        h.qm_v = s.separate_uv_delta_q ? uint8_t(r.Bits(4)) : h.qm_u;
    }

    // segmentation_params
    {
        static const int kBits[8] = {8, 6, 6, 6, 6, 3, 0, 0};
        static const int kSigned[8] = {1, 1, 1, 1, 1, 0, 0, 0};
        static const int kMax[8] = {255, 63, 63, 63, 63, 7, 0, 0};
        h.segmentation_enabled = r.Flag();
        if (h.segmentation_enabled) {
            if (h.primary_ref_frame == kPrimaryRefNone) {
                h.segmentation_update_map = true;
                h.segmentation_temporal_update = false;
                h.segmentation_update_data = true;
            } else {
                h.segmentation_update_map = r.Flag();
                if (h.segmentation_update_map)
                    h.segmentation_temporal_update = r.Flag();
                h.segmentation_update_data = r.Flag();
            }
            if (h.segmentation_update_data) {
                for (int i = 0; i < 8; i++)
                    for (int j = 0; j < 8; j++) {
                        int value = 0;
                        const bool enabled = r.Flag();
                        h.feature_enabled[i][j] = enabled;
                        if (enabled) {
                            if (kSigned[j])
                                value = std::clamp(r.Signed(1 + kBits[j]), -kMax[j], kMax[j]);
                            else
                                value = std::clamp(int(r.Bits(kBits[j])), 0, kMax[j]);
                        }
                        h.feature_data[i][j] = int16_t(value);
                    }
            }
        } else {
            std::memset(h.feature_enabled, 0, sizeof(h.feature_enabled));
            std::memset(h.feature_data, 0, sizeof(h.feature_data));
        }
    }
    // delta_q_params, delta_lf_params
    if (h.base_q_idx > 0)
        h.delta_q_present = r.Flag();
    if (h.delta_q_present)
        h.delta_q_res = uint8_t(r.Bits(2));
    if (h.delta_q_present) {
        if (!h.allow_intrabc)
            h.delta_lf_present = r.Flag();
        if (h.delta_lf_present) {
            h.delta_lf_res = uint8_t(r.Bits(2));
            h.delta_lf_multi = r.Flag();
        }
    }
    // CodedLossless
    h.coded_lossless = true;
    for (int segment = 0; segment < 8; segment++) {
        int qindex = h.base_q_idx;
        if (h.segmentation_enabled && h.feature_enabled[segment][0])
            qindex = std::clamp(h.base_q_idx + int(h.feature_data[segment][0]), 0, 255);
        const bool lossless = qindex == 0 && h.delta_q_y_dc == 0 && h.delta_q_u_ac == 0 && h.delta_q_u_dc == 0 &&
                              h.delta_q_v_ac == 0 && h.delta_q_v_dc == 0;
        if (!lossless)
            h.coded_lossless = false;
    }
    const bool all_lossless = h.coded_lossless && h.frame_width == h.upscaled_width;

    // loop_filter_params
    if (h.coded_lossless || h.allow_intrabc) {
        h.loop_filter_level[0] = h.loop_filter_level[1] = 0;
        static const int8_t kDefaultRef[8] = {1, 0, 0, 0, -1, 0, -1, -1};
        std::memcpy(h.loop_filter_ref_deltas, kDefaultRef, sizeof(kDefaultRef));
        h.loop_filter_mode_deltas[0] = h.loop_filter_mode_deltas[1] = 0;
    } else {
        h.loop_filter_level[0] = uint8_t(r.Bits(6));
        h.loop_filter_level[1] = uint8_t(r.Bits(6));
        if (h.loop_filter_level[0] || h.loop_filter_level[1]) {
            h.loop_filter_level[2] = uint8_t(r.Bits(6));
            h.loop_filter_level[3] = uint8_t(r.Bits(6));
        }
        h.loop_filter_sharpness = uint8_t(r.Bits(3));
        h.loop_filter_delta_enabled = r.Flag();
        if (h.loop_filter_delta_enabled) {
            h.loop_filter_delta_update = r.Flag();
            if (h.loop_filter_delta_update) {
                for (int i = 0; i < 8; i++)
                    if (r.Flag()) {
                        h.loop_filter_ref_deltas[i] = int8_t(r.Signed(7));
                        h.update_ref_delta |= uint8_t(1u << i);
                    }
                for (int i = 0; i < 2; i++)
                    if (r.Flag()) {
                        h.loop_filter_mode_deltas[i] = int8_t(r.Signed(7));
                        h.update_mode_delta |= uint8_t(1u << i);
                    }
            }
        }
    }
    // cdef_params
    if (h.coded_lossless || h.allow_intrabc || !s.enable_cdef) {
        h.cdef_bits = 0;
        h.cdef_damping_minus_3 = 0;
    } else {
        h.cdef_damping_minus_3 = uint8_t(r.Bits(2));
        h.cdef_bits = uint8_t(r.Bits(2));
        for (int i = 0; i < (1 << h.cdef_bits); i++) {
            h.cdef_y_pri[i] = uint8_t(r.Bits(4));
            h.cdef_y_sec[i] = uint8_t(r.Bits(2));
            h.cdef_uv_pri[i] = uint8_t(r.Bits(4));
            h.cdef_uv_sec[i] = uint8_t(r.Bits(2));
        }
    }
    // lr_params
    if (!(all_lossless || h.allow_intrabc || !s.enable_restoration)) {
        static const uint8_t kRemap[4] = {0, 3, 1, 2};
        for (int i = 0; i < 3; i++) {
            h.frame_restoration_type[i] = kRemap[r.Bits(2)];
            if (h.frame_restoration_type[i] != 0) {
                h.uses_lr = true;
                if (i > 0)
                    h.uses_chroma_lr = true;
            }
        }
        if (h.uses_lr) {
            if (s.use_128x128_superblock) {
                h.lr_unit_shift = uint8_t(r.Bits(1) + 1);
            } else {
                h.lr_unit_shift = uint8_t(r.Bits(1));
                if (h.lr_unit_shift)
                    h.lr_unit_shift = uint8_t(h.lr_unit_shift + r.Bits(1));
            }
            h.lr_uv_shift = (s.subsampling_x && s.subsampling_y && h.uses_chroma_lr) ? uint8_t(r.Bits(1)) : 0;
        }
    }
    // read_tx_mode
    if (h.coded_lossless)
        h.tx_mode = 0;
    else
        h.tx_mode = r.Flag() ? 2 : 1;
    // frame_reference_mode
    h.reference_select = frame_is_intra ? false : r.Flag();
    // skip_mode_params
    {
        bool skip_mode_allowed = false;
        if (!(frame_is_intra || !h.reference_select || !s.enable_order_hint)) {
            int forward_idx = -1, backward_idx = -1, forward_hint = 0, backward_hint = 0;
            for (int i = 0; i < kRefsPerFrame; i++) {
                const int ref_hint = slots_[h.ref_frame_idx[i]].order_hint;
                if (RelativeDist(ref_hint, h.order_hint) < 0) {
                    if (forward_idx < 0 || RelativeDist(ref_hint, forward_hint) > 0) {
                        forward_idx = i;
                        forward_hint = ref_hint;
                    }
                } else if (RelativeDist(ref_hint, h.order_hint) > 0) {
                    if (backward_idx < 0 || RelativeDist(ref_hint, backward_hint) < 0) {
                        backward_idx = i;
                        backward_hint = ref_hint;
                    }
                }
            }
            if (forward_idx < 0) {
                skip_mode_allowed = false;
            } else if (backward_idx >= 0) {
                skip_mode_allowed = true;
                h.skip_mode_frame[0] = uint8_t(1 + std::min(forward_idx, backward_idx));
                h.skip_mode_frame[1] = uint8_t(1 + std::max(forward_idx, backward_idx));
            } else {
                int second_forward_idx = -1, second_forward_hint = 0;
                for (int i = 0; i < kRefsPerFrame; i++) {
                    const int ref_hint = slots_[h.ref_frame_idx[i]].order_hint;
                    if (RelativeDist(ref_hint, forward_hint) < 0) {
                        if (second_forward_idx < 0 || RelativeDist(ref_hint, second_forward_hint) > 0) {
                            second_forward_idx = i;
                            second_forward_hint = ref_hint;
                        }
                    }
                }
                if (second_forward_idx >= 0) {
                    skip_mode_allowed = true;
                    h.skip_mode_frame[0] = uint8_t(1 + std::min(forward_idx, second_forward_idx));
                    h.skip_mode_frame[1] = uint8_t(1 + std::max(forward_idx, second_forward_idx));
                }
            }
        }
        h.skip_mode_present = skip_mode_allowed ? r.Flag() : false;
    }
    if (frame_is_intra || h.error_resilient_mode || !s.enable_warped_motion)
        h.allow_warped_motion = false;
    else
        h.allow_warped_motion = r.Flag();
    h.reduced_tx_set = r.Flag();

    // global_motion_params
    for (int ref = 0; ref < 8; ref++)
        for (int i = 0; i < 6; i++)
            h.gm_params[ref][i] = (i % 3 == 2) ? (1 << 16) : 0;
    if (!frame_is_intra) {
        for (int ref = 1; ref <= 7; ref++) {
            int type = 0;  // IDENTITY
            if (r.Flag()) {                  // is_global
                if (r.Flag())                // is_rot_zoom
                    type = 2;                // ROTZOOM
                else
                    type = r.Flag() ? 1 : 3;  // TRANSLATION : AFFINE
            }
            h.gm_type[ref] = uint8_t(type);
            auto read_param = [&](int idx) {
                int abs_bits = 12, prec_bits = 15;
                if (idx < 2) {
                    if (type == 1) {
                        abs_bits = 9 - (h.allow_high_precision_mv ? 0 : 1);
                        prec_bits = 3 - (h.allow_high_precision_mv ? 0 : 1);
                    } else {
                        abs_bits = 12;
                        prec_bits = 6;
                    }
                }
                const int prec_diff = 16 - prec_bits;
                const int round = (idx % 3) == 2 ? (1 << 16) : 0;
                const int sub = (idx % 3) == 2 ? (1 << prec_bits) : 0;
                const int mx = 1 << abs_bits;
                const int rr = (prev_gm[ref][idx] >> prec_diff) - sub;
                h.gm_params[ref][idx] = (DecodeSignedSubexpWithRef(r, -mx, mx + 1, rr) << prec_diff) + round;
            };
            if (type >= 2) {
                read_param(2);
                read_param(3);
                if (type == 3) {
                    read_param(4);
                    read_param(5);
                } else {
                    h.gm_params[ref][4] = -h.gm_params[ref][3];
                    h.gm_params[ref][5] = h.gm_params[ref][2];
                }
            }
            if (type >= 1) {
                read_param(0);
                read_param(1);
            }
        }
    }
    // film_grain_params: absent (the sequence header refuses it)
    if (r.overrun()) {
        *why = "damaged AV1 frame header";
        return false;
    }
    std::memcpy(h.order_hints, order_hints_, sizeof(h.order_hints));
    return true;
}

void Parser::UpdateReferences(const FrameHeader& h) {
    for (int i = 0; i < kNumRefFrames; i++) {
        if (!((h.refresh_frame_flags >> i) & 1))
            continue;
        Slot& slot = slots_[i];
        slot.valid = true;
        slot.frame_id = h.current_frame_id;
        slot.upscaled_width = h.upscaled_width;
        slot.frame_width = h.frame_width;
        slot.frame_height = h.frame_height;
        slot.render_width = h.render_width;
        slot.render_height = h.render_height;
        slot.frame_type = h.frame_type;
        slot.order_hint = h.order_hint;
        std::memcpy(slot.saved_order_hints, order_hints_, sizeof(slot.saved_order_hints));
        std::memcpy(slot.gm_params, h.gm_params, sizeof(slot.gm_params));
        std::memcpy(slot.lf_ref_deltas, h.loop_filter_ref_deltas, sizeof(slot.lf_ref_deltas));
        std::memcpy(slot.lf_mode_deltas, h.loop_filter_mode_deltas, sizeof(slot.lf_mode_deltas));
        std::memcpy(slot.feature_enabled, h.feature_enabled, sizeof(slot.feature_enabled));
        std::memcpy(slot.feature_data, h.feature_data, sizeof(slot.feature_data));
    }
}

bool Parser::ReadTileGroup(const uint8_t* data, size_t size, size_t base_offset, bool in_frame_obu, std::string* why) {
    (void)in_frame_obu;
    FrameHeader& h = current_.h;
    const uint32_t num_tiles = h.tile_cols * h.tile_rows;
    BitReader r(data, size);
    uint32_t tg_start = 0, tg_end = num_tiles - 1;
    if (num_tiles > 1 && r.Flag()) {
        const int tile_bits = int(h.tile_cols_log2 + h.tile_rows_log2);
        tg_start = r.Bits(tile_bits);
        tg_end = r.Bits(tile_bits);
    }
    r.ByteAlign();
    if (r.overrun() || tg_start != next_tile_ || tg_end < tg_start || tg_end >= num_tiles) {
        *why = "damaged AV1 tile group";
        return false;
    }
    size_t pos = r.byte_position();
    const uint32_t tile_size_bytes = h.tile_size_bytes_minus_1 + 1;
    for (uint32_t tile = tg_start; tile <= tg_end; tile++) {
        TileRange range;
        range.row = uint16_t(tile / h.tile_cols);
        range.column = uint16_t(tile % h.tile_cols);
        if (tile == tg_end) {
            if (pos > size) {
                *why = "damaged AV1 tile group";
                return false;
            }
            range.size = uint32_t(size - pos);
        } else {
            if (pos + tile_size_bytes > size) {
                *why = "damaged AV1 tile size";
                return false;
            }
            uint32_t minus1 = 0;
            for (uint32_t b = 0; b < tile_size_bytes; b++)
                minus1 |= uint32_t(data[pos + b]) << (8 * b);
            pos += tile_size_bytes;
            range.size = minus1 + 1;
            if (pos + range.size > size) {
                *why = "damaged AV1 tile size";
                return false;
            }
        }
        range.offset = uint32_t(base_offset + pos);
        pos += range.size;
        current_.tiles.push_back(range);
    }
    next_tile_ = tg_end + 1;
    return true;
}

bool Parser::ParseTemporalUnit(const uint8_t* data, size_t size, std::vector<Picture>* pictures, std::string* why) {
    pictures->clear();
    size_t pos = 0;
    while (pos < size) {
        const uint8_t header = data[pos];
        const int type = (header >> 3) & 15;
        const bool ext = (header >> 2) & 1, has_size = (header >> 1) & 1;
        int temporal_id = 0, spatial_id = 0;
        size_t head = 1;
        if (ext) {
            if (pos + 1 >= size) {
                *why = "truncated AV1 OBU";
                return false;
            }
            temporal_id = data[pos + 1] >> 5;
            spatial_id = (data[pos + 1] >> 3) & 3;
            head = 2;
        }
        uint64_t obu_size = 0;
        size_t leb = 0;
        if (has_size) {
            if (!Leb128(data + pos + head, size - pos - head, &obu_size, &leb)) {
                *why = "damaged AV1 OBU size";
                return false;
            }
        } else {
            obu_size = size - pos - head;
        }
        if (pos + head + leb + obu_size > size) {
            *why = "truncated AV1 OBU";
            return false;
        }
        const size_t payload_offset = pos + head + leb;
        const uint8_t* payload = data + payload_offset;
        const size_t payload_size = size_t(obu_size);
        pos = payload_offset + payload_size;

        if (type != kObuSequenceHeader && type != kObuTemporalDelimiter && ext && have_sequence_ &&
            seq_.operating_point_idc[0] != 0) {
            const uint32_t idc = seq_.operating_point_idc[0];
            if (!((idc >> temporal_id) & 1) || !((idc >> (spatial_id + 8)) & 1))
                continue;  // not in operating point 0
        }
        switch (type) {
            case kObuSequenceHeader:
                if (!ParseSequenceHeader(payload, payload_size, why))
                    return false;
                break;
            case kObuTemporalDelimiter:
                break;
            case kObuFrameHeader:
            case kObuRedundantFrameHeader:
            case kObuFrame: {
                if (!have_sequence_) {
                    *why = "AV1 frame before the sequence header";
                    return false;
                }
                if (in_frame_) {
                    if (type == kObuFrame) {
                        *why = "AV1 frame OBU inside an unfinished frame";
                        return false;
                    }
                    break;  // a copy of the frame header
                }
                BitReader r(payload, payload_size);
                Picture picture;
                for (int i = 0; i < kNumRefFrames; i++) {
                    picture.refs[i].valid = slots_[i].valid;
                    picture.refs[i].frame_type = slots_[i].frame_type;
                    picture.refs[i].order_hint = slots_[i].order_hint;
                    picture.refs[i].frame_id = slots_[i].frame_id;
                    std::memcpy(picture.refs[i].saved_order_hints, slots_[i].saved_order_hints, 8);
                }
                if (!ParseFrameHeader(r, &picture.h, temporal_id, spatial_id, why))
                    return false;
                if (picture.h.show_existing_frame) {
                    if (type == kObuFrame) {
                        *why = "AV1 frame OBU with show_existing_frame";
                        return false;
                    }
                    picture.shown = true;
                    if (picture.h.frame_type == kKeyFrame) {
                        // 7.21 frame loading, then the reference update with every slot refreshed.
                        const Slot shown = slots_[picture.h.frame_to_show_map_idx];
                        std::memcpy(order_hints_, shown.saved_order_hints, sizeof(order_hints_));
                        for (Slot& slot : slots_)
                            slot = shown;
                    }
                    pictures->push_back(std::move(picture));
                    break;
                }
                current_ = std::move(picture);
                current_.shown = current_.h.show_frame;
                current_.tiles.clear();
                in_frame_ = true;
                next_tile_ = 0;
                if (type == kObuFrame) {
                    r.ByteAlign();
                    const size_t used = r.byte_position();
                    if (r.overrun() || used > payload_size) {
                        *why = "damaged AV1 frame OBU";
                        return false;
                    }
                    if (!ReadTileGroup(payload + used, payload_size - used, payload_offset + used, true, why))
                        return false;
                }
                break;
            }
            case kObuTileGroup:
                if (!in_frame_) {
                    *why = "AV1 tile group without a frame header";
                    return false;
                }
                if (!ReadTileGroup(payload, payload_size, payload_offset, false, why))
                    return false;
                break;
            case kObuTileList:
                *why = "AV1 large scale tile is not supported";
                return false;
            default:
                break;
        }
        if (in_frame_ && next_tile_ == current_.h.tile_cols * current_.h.tile_rows) {
            UpdateReferences(current_.h);
            pictures->push_back(std::move(current_));
            current_ = Picture{};
            in_frame_ = false;
        }
    }
    if (in_frame_) {
        *why = "AV1 temporal unit ends inside a frame";
        return false;
    }
    return true;
}

bool StartsWithKeyFrame(const uint8_t* data, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        const uint8_t header = data[pos];
        const int type = (header >> 3) & 15;
        const bool ext = (header >> 2) & 1, has_size = (header >> 1) & 1;
        size_t head = ext ? 2 : 1;
        if (!has_size || pos + head >= size)
            return false;
        uint64_t obu_size;
        size_t leb;
        if (!Leb128(data + pos + head, size - pos - head, &obu_size, &leb) || pos + head + leb >= size)
            return false;
        const uint8_t* payload = data + pos + head + leb;
        if (type == kObuFrameHeader || type == kObuFrame) {
            // Only the first bits matter; the OBU may extend beyond the bytes given.
            if (obu_size == 0)
                return false;
            BitReader r(payload, std::min<size_t>(size_t(obu_size), size - (pos + head + leb)));
            if (r.Flag())
                return false;  // show_existing_frame
            const uint32_t frame_type = r.Bits(2);
            const bool show_frame = r.Flag();
            return frame_type == kKeyFrame && show_frame;
        }
        if (pos + head + leb + obu_size > size)
            return false;
        pos += head + leb + size_t(obu_size);
    }
    return false;
}

}  // namespace renderer::clips::video::av1
