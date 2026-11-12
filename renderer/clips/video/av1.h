// AV1 (AV1 bitstream specification, 5.5 sequence header and 5.9 uncompressed header) for GPU decode: the sequence
// header, the frame headers with the decoder state they depend on (the eight reference slots, the loop filter and
// segmentation values a frame inherits, global motion), and the location of every tile in a temporal unit. Film grain,
// super-resolution, frame sizes that change within a stream, scalability and monochrome are refused with a reason.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "renderer/clips/video/bitreader.h"

namespace renderer::clips::video::av1 {

constexpr int kRefsPerFrame = 7;
constexpr int kNumRefFrames = 8;
constexpr int kKeyFrame = 0, kInterFrame = 1, kIntraOnlyFrame = 2, kSwitchFrame = 3;
constexpr uint8_t kPrimaryRefNone = 7;

struct SequenceHeader {
    uint8_t seq_profile = 0;
    bool still_picture = false, reduced_still_picture_header = false;
    bool timing_info_present = false, decoder_model_info_present = false, equal_picture_interval = false;
    uint32_t num_units_in_display_tick = 0, time_scale = 0, num_ticks_per_picture_minus_1 = 0;
    uint8_t buffer_delay_length_minus_1 = 0, buffer_removal_time_length_minus_1 = 0, frame_presentation_time_length_minus_1 = 0;
    uint8_t operating_points_cnt_minus_1 = 0;
    uint16_t operating_point_idc[32] = {};
    uint8_t seq_level_idx[32] = {}, seq_tier[32] = {};
    bool decoder_model_present_for_this_op[32] = {};
    uint8_t frame_width_bits_minus_1 = 0, frame_height_bits_minus_1 = 0;
    uint32_t max_frame_width_minus_1 = 0, max_frame_height_minus_1 = 0;
    bool frame_id_numbers_present = false;
    uint8_t delta_frame_id_length_minus_2 = 0, additional_frame_id_length_minus_1 = 0;
    bool use_128x128_superblock = false, enable_filter_intra = false, enable_intra_edge_filter = false;
    bool enable_interintra_compound = false, enable_masked_compound = false, enable_warped_motion = false;
    bool enable_dual_filter = false, enable_order_hint = false, enable_jnt_comp = false, enable_ref_frame_mvs = false;
    uint8_t seq_force_screen_content_tools = 2;  // 2: select per frame
    uint8_t seq_force_integer_mv = 2;
    uint8_t order_hint_bits = 0;
    bool enable_superres = false, enable_cdef = false, enable_restoration = false;
    // color_config
    uint8_t bit_depth = 8;
    bool mono_chrome = false, color_description_present = false;
    uint8_t color_primaries = 2, transfer_characteristics = 2, matrix_coefficients = 2;
    bool color_range = false;  // true: full range
    uint8_t subsampling_x = 1, subsampling_y = 1, chroma_sample_position = 0;
    bool separate_uv_delta_q = false;
    bool film_grain_params_present = false;
    uint32_t width() const { return max_frame_width_minus_1 + 1; }
    uint32_t height() const { return max_frame_height_minus_1 + 1; }
};

struct TileRange {
    uint32_t offset = 0;  // of the tile's data in the temporal unit (after its size field)
    uint32_t size = 0;
    uint16_t row = 0, column = 0;
};

struct FrameHeader {
    bool show_existing_frame = false;
    uint8_t frame_to_show_map_idx = 0;
    uint8_t frame_type = kKeyFrame;
    bool show_frame = true, showable_frame = false, error_resilient_mode = false, disable_cdf_update = false;
    bool allow_screen_content_tools = false, force_integer_mv = false, frame_size_override_flag = false;
    uint32_t current_frame_id = 0;
    uint8_t order_hint = 0;
    uint8_t primary_ref_frame = kPrimaryRefNone;
    bool buffer_removal_time_present = false;
    uint8_t refresh_frame_flags = 0;
    bool frame_refs_short_signaling = false;
    uint8_t ref_frame_idx[kRefsPerFrame] = {};
    uint32_t frame_width = 0, frame_height = 0, upscaled_width = 0, render_width = 0, render_height = 0;
    bool use_superres = false;
    uint8_t coded_denom = 0;
    bool render_and_frame_size_different = false;
    bool allow_intrabc = false, allow_high_precision_mv = false, is_filter_switchable = false;
    uint8_t interpolation_filter = 0;  // 4: switchable
    bool is_motion_mode_switchable = false, use_ref_frame_mvs = false, disable_frame_end_update_cdf = false;
    // tile_info
    bool uniform_tile_spacing = true;
    uint32_t tile_cols = 1, tile_rows = 1, tile_cols_log2 = 0, tile_rows_log2 = 0;
    uint32_t context_update_tile_id = 0, tile_size_bytes_minus_1 = 3;
    uint16_t mi_col_starts[65] = {}, mi_row_starts[65] = {};
    uint16_t width_in_sbs_minus_1[64] = {}, height_in_sbs_minus_1[64] = {};
    // quantization_params
    uint8_t base_q_idx = 0;
    int8_t delta_q_y_dc = 0, delta_q_u_dc = 0, delta_q_u_ac = 0, delta_q_v_dc = 0, delta_q_v_ac = 0;
    bool using_qmatrix = false, diff_uv_delta = false;
    uint8_t qm_y = 0, qm_u = 0, qm_v = 0;
    // segmentation_params (the values the frame uses, inherited ones included)
    bool segmentation_enabled = false, segmentation_update_map = false, segmentation_temporal_update = false,
         segmentation_update_data = false;
    uint8_t feature_enabled[8][8] = {};
    int16_t feature_data[8][8] = {};
    bool delta_q_present = false;
    uint8_t delta_q_res = 0;
    bool delta_lf_present = false, delta_lf_multi = false;
    uint8_t delta_lf_res = 0;
    // loop_filter_params (inherited deltas included)
    uint8_t loop_filter_level[4] = {};
    uint8_t loop_filter_sharpness = 0;
    bool loop_filter_delta_enabled = false, loop_filter_delta_update = false;
    uint8_t update_ref_delta = 0, update_mode_delta = 0;  // bit masks of the deltas this header codes
    int8_t loop_filter_ref_deltas[8] = {1, 0, 0, 0, -1, 0, -1, -1};
    int8_t loop_filter_mode_deltas[2] = {};
    // cdef_params (as coded: a secondary strength of 3 means 4)
    uint8_t cdef_damping_minus_3 = 0, cdef_bits = 0;
    uint8_t cdef_y_pri[8] = {}, cdef_y_sec[8] = {}, cdef_uv_pri[8] = {}, cdef_uv_sec[8] = {};
    // lr_params
    uint8_t frame_restoration_type[3] = {};  // 0 none, 1 Wiener, 2 SGR, 3 switchable
    uint8_t lr_unit_shift = 0, lr_uv_shift = 0;
    bool uses_lr = false, uses_chroma_lr = false;
    uint8_t tx_mode = 0;  // 0 only 4x4, 1 largest, 2 select
    bool reference_select = false, skip_mode_present = false, allow_warped_motion = false, reduced_tx_set = false;
    uint8_t skip_mode_frame[2] = {};  // reference names, 1 (LAST) to 7 (ALTREF)
    uint8_t gm_type[8] = {};
    int32_t gm_params[8][6] = {};
    bool coded_lossless = false;
    uint8_t order_hints[8] = {};  // OrderHints[]: by reference name, 0 unused
};

struct RefInfo {
    bool valid = false;
    uint8_t frame_type = kKeyFrame;
    uint8_t order_hint = 0;
    uint32_t frame_id = 0;
    uint8_t saved_order_hints[8] = {};
};

// What the decoder gets for one frame: its header, where its tiles are, and the reference map when it starts.
struct Picture {
    FrameHeader h;
    std::vector<TileRange> tiles;
    std::array<RefInfo, kNumRefFrames> refs;
    bool shown = false;  // this picture is a displayed frame: show_frame, or a show_existing_frame
};

class Parser {
public:
    // The configOBUs of an av1C record (sequence header); false with a reason when it holds a refused sequence.
    bool Configure(const std::vector<uint8_t>& av1c, std::string* why);
    // At a start point: every reference is invalid.
    void Reset();
    // The pictures of one temporal unit in decode order; at most one of them is shown.
    bool ParseTemporalUnit(const uint8_t* data, size_t size, std::vector<Picture>* pictures, std::string* why);
    bool has_sequence() const { return have_sequence_; }
    const SequenceHeader& sequence() const { return seq_; }
    // From now on a sequence header that differs in what the decoder is configured with is an error. The av1C record
    // and the first key frame can carry different headers (SVT-AV1 does); the first key frame's is the one to lock.
    void LockSequence();

private:
    struct Slot {
        bool valid = false;
        uint32_t frame_id = 0;
        uint32_t upscaled_width = 0, frame_width = 0, frame_height = 0, render_width = 0, render_height = 0;
        uint8_t frame_type = kKeyFrame;
        uint8_t order_hint = 0;
        uint8_t saved_order_hints[8] = {};
        int32_t gm_params[8][6] = {};
        int8_t lf_ref_deltas[8] = {1, 0, 0, 0, -1, 0, -1, -1};
        int8_t lf_mode_deltas[2] = {};
        uint8_t feature_enabled[8][8] = {};
        int16_t feature_data[8][8] = {};
    };

    bool ParseSequenceHeader(const uint8_t* data, size_t size, std::string* why);
    bool ParseFrameHeader(BitReader& r, FrameHeader* h, int temporal_id, int spatial_id, std::string* why);
    void UpdateReferences(const FrameHeader& h);
    int RelativeDist(int a, int b) const;
    bool SetFrameRefs(FrameHeader* h, int last_idx, int gold_idx);
    bool ReadTileGroup(const uint8_t* data, size_t size, size_t base_offset, bool in_frame_obu, std::string* why);

    SequenceHeader seq_;
    bool have_sequence_ = false;
    bool decode_config_locked_ = false;
    std::array<Slot, kNumRefFrames> slots_;
    uint8_t order_hints_[8] = {};  // OrderHints[], kept across frames as the specification does
    // The frame being parsed (between its header and its last tile group).
    bool in_frame_ = false;
    Picture current_;
    uint32_t next_tile_ = 0;
};

// True if the temporal unit starts with a key frame that is shown (a point to start decoding at), judged from the
// first frame header only. False when it cannot tell.
bool StartsWithKeyFrame(const uint8_t* data, size_t size);

}  // namespace renderer::clips::video::av1
