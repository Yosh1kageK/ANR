// H.265 (ITU-T H.265, 7.3 syntax, 8.3.1 picture order count, 8.3.2 reference picture sets): Main and Main 10, 4:2:0,
// one layer, without explicit scaling list data (NRI's session parameters take it, but it is not parsed here). The
// slice segment header is read up to the reference picture set and long-term picture syntax; the decoder gets each
// picture's reference pictures as DPB slots with their POC, the slice offsets and the two header facts a driver needs
// to skip the slice header's reference picture set (its size in bits and the size of the set it predicts from).

#include <algorithm>
#include <cstring>
#include <map>

#include "renderer/clips/video/bitreader.h"
#include "renderer/clips/video/codec_backend.h"

namespace renderer::clips::video {

namespace {

enum NalType : int {
    kTrailN = 0,
    kRadlN = 6,
    kRadlR = 7,
    kRaslN = 8,
    kRaslR = 9,
    kBlaWLp = 16,
    kIdrWRadl = 19,
    kIdrNLp = 20,
    kCra = 21,
    kReservedIrap23 = 23,
    kVps = 32,
    kSps = 33,
    kPps = 34,
};

struct ProfileTierLevel {
    uint8_t profile_space = 0;
    bool tier = false;
    uint8_t profile_idc = 0;
    bool progressive = false, interlaced = false, non_packed = false, frame_only = false;
    uint8_t level_idc = 0;
};

void ParseProfileTierLevel(BitReader& r, int max_sub_layers_minus1, ProfileTierLevel* ptl) {
    ptl->profile_space = uint8_t(r.Bits(2));
    ptl->tier = r.Flag();
    ptl->profile_idc = uint8_t(r.Bits(5));
    r.Skip(32);  // general_profile_compatibility_flag[32]
    ptl->progressive = r.Flag();
    ptl->interlaced = r.Flag();
    ptl->non_packed = r.Flag();
    ptl->frame_only = r.Flag();
    r.Skip(44);
    ptl->level_idc = uint8_t(r.Bits(8));
    bool profile_present[8] = {}, level_present[8] = {};
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        profile_present[i] = r.Flag();
        level_present[i] = r.Flag();
    }
    if (max_sub_layers_minus1 > 0)
        for (int i = max_sub_layers_minus1; i < 8; i++)
            r.Skip(2);
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        if (profile_present[i])
            r.Skip(88);
        if (level_present[i])
            r.Skip(8);
    }
}

struct Vps {
    uint8_t id = 0, max_sub_layers_minus1 = 0;
    bool temporal_id_nesting = false, sub_layer_ordering_info_present = false;
    bool timing_info_present = false, poc_proportional = false;
    uint32_t num_units_in_tick = 0, time_scale = 0, num_ticks_poc_diff_one_minus1 = 0;
    ProfileTierLevel ptl;
    uint8_t max_dec_pic_buffering_minus1[8] = {}, max_num_reorder_pics[8] = {};
    uint32_t max_latency_increase_plus1[8] = {};
    std::vector<uint8_t> bytes;
};

// A short-term reference picture set as derived (7.4.8): the POC deltas and which pictures the picture uses.
struct StRps {
    int num_negative = 0, num_positive = 0;
    int delta_poc_s0[16] = {}, delta_poc_s1[16] = {};
    bool used_s0[16] = {}, used_s1[16] = {};
    int num_delta_pocs() const { return num_negative + num_positive; }
};

struct Sps {
    uint8_t vps_id = 0, max_sub_layers_minus1 = 0, id = 0, chroma_format_idc = 1;
    bool temporal_id_nesting = false, separate_colour_plane = false;
    uint32_t width = 0, height = 0;
    bool conformance_window = false;
    uint32_t conf_left = 0, conf_right = 0, conf_top = 0, conf_bottom = 0;
    uint8_t bit_depth_luma = 8, bit_depth_chroma = 8, log2_max_poc_lsb = 4;
    bool sub_layer_ordering_info_present = false;
    uint8_t max_dec_pic_buffering_minus1[8] = {}, max_num_reorder_pics[8] = {};
    uint32_t max_latency_increase_plus1[8] = {};
    uint8_t log2_min_cb_minus3 = 0, log2_diff_max_min_cb = 0, log2_min_tb_minus2 = 0, log2_diff_max_min_tb = 0;
    uint8_t max_th_depth_inter = 0, max_th_depth_intra = 0;
    bool scaling_list_enabled = false, scaling_list_data_present = false;
    bool amp = false, sao = false, pcm = false, pcm_loop_filter_disabled = false;
    uint8_t pcm_bit_depth_luma_minus1 = 0, pcm_bit_depth_chroma_minus1 = 0, log2_min_pcm_cb_minus3 = 0, log2_diff_max_min_pcm_cb = 0;
    std::vector<StRps> st_rps;
    bool long_term_ref_pics_present = false;
    uint8_t num_long_term_ref_pics_sps = 0;
    uint16_t lt_ref_pic_poc_lsb_sps[32] = {};
    bool used_by_curr_pic_lt_sps[32] = {};
    bool temporal_mvp = false, strong_intra_smoothing = false, vui_present = false;
    ProfileTierLevel ptl;
    bool has_color = false, full_range = false;
    uint8_t color_primaries = 2, transfer = 2, matrix = 2;
    std::vector<uint8_t> bytes;
    uint32_t ctb_log2() const { return log2_min_cb_minus3 + 3 + log2_diff_max_min_cb; }
    uint32_t pic_size_in_ctbs() const {
        const uint32_t ctb = 1u << ctb_log2();
        return ((width + ctb - 1) / ctb) * ((height + ctb - 1) / ctb);
    }
};

struct Pps {
    uint8_t id = 0, sps_id = 0;
    bool dependent_slice_segments_enabled = false, output_flag_present = false;
    uint8_t num_extra_slice_header_bits = 0;
    bool sign_data_hiding = false, cabac_init_present = false;
    uint8_t num_ref_idx_l0_default_minus1 = 0, num_ref_idx_l1_default_minus1 = 0;
    int8_t init_qp_minus26 = 0;
    bool constrained_intra_pred = false, transform_skip = false, cu_qp_delta_enabled = false;
    uint8_t diff_cu_qp_delta_depth = 0;
    int8_t cb_qp_offset = 0, cr_qp_offset = 0;
    bool slice_chroma_qp_offsets_present = false, weighted_pred = false, weighted_bipred = false, transquant_bypass = false;
    bool tiles_enabled = false, entropy_coding_sync = false;
    uint8_t num_tile_columns_minus1 = 0, num_tile_rows_minus1 = 0;
    bool uniform_spacing = true, loop_filter_across_tiles = true;
    uint16_t column_width_minus1[19] = {}, row_height_minus1[21] = {};
    bool loop_filter_across_slices = false, deblocking_filter_control_present = false, deblocking_filter_override_enabled = false;
    bool deblocking_filter_disabled = false;
    int8_t beta_offset_div2 = 0, tc_offset_div2 = 0;
    bool scaling_list_data_present = false, lists_modification_present = false, slice_segment_header_extension_present = false;
    uint8_t log2_parallel_merge_level_minus2 = 0;
    std::vector<uint8_t> bytes;
};

// 7.3.7 st_ref_pic_set: the derived set. sps_sets holds the sets before this one (index < idx); in a slice header idx
// is num_short_term_ref_pic_sets and delta_idx_minus1 is coded.
bool ParseStRps(BitReader& r, int idx, int num_sets, const std::vector<StRps>& sps_sets, StRps* out, int* ref_rps_num_delta_pocs,
                std::string* why) {
    *out = StRps{};
    bool inter = false;
    if (idx != 0)
        inter = r.Flag();
    if (inter) {
        uint32_t delta_idx_minus1 = 0;
        if (idx == num_sets)
            delta_idx_minus1 = r.Ue();
        const bool delta_rps_sign = r.Flag();
        const uint32_t abs_delta_rps_minus1 = r.Ue();
        const int ref_idx = idx - int(delta_idx_minus1 + 1);
        if (ref_idx < 0 || ref_idx >= int(sps_sets.size())) {
            *why = "damaged H.265 reference picture set";
            return false;
        }
        const StRps& ref = sps_sets[size_t(ref_idx)];
        if (ref_rps_num_delta_pocs)
            *ref_rps_num_delta_pocs = ref.num_delta_pocs();
        const int delta_rps = (1 - 2 * int(delta_rps_sign)) * int(abs_delta_rps_minus1 + 1);
        bool used_by_curr[33] = {}, use_delta[33] = {};
        for (int j = 0; j <= ref.num_delta_pocs(); j++) {
            used_by_curr[j] = r.Flag();
            use_delta[j] = true;
            if (!used_by_curr[j])
                use_delta[j] = r.Flag();
        }
        int i = 0;
        for (int j = ref.num_positive - 1; j >= 0; j--) {
            const int d = ref.delta_poc_s1[j] + delta_rps;
            if (d < 0 && use_delta[ref.num_negative + j] && i < 16) {
                out->delta_poc_s0[i] = d;
                out->used_s0[i++] = used_by_curr[ref.num_negative + j];
            }
        }
        if (delta_rps < 0 && use_delta[ref.num_delta_pocs()] && i < 16) {
            out->delta_poc_s0[i] = delta_rps;
            out->used_s0[i++] = used_by_curr[ref.num_delta_pocs()];
        }
        for (int j = 0; j < ref.num_negative; j++) {
            const int d = ref.delta_poc_s0[j] + delta_rps;
            if (d < 0 && use_delta[j] && i < 16) {
                out->delta_poc_s0[i] = d;
                out->used_s0[i++] = used_by_curr[j];
            }
        }
        out->num_negative = i;
        i = 0;
        for (int j = ref.num_negative - 1; j >= 0; j--) {
            const int d = ref.delta_poc_s0[j] + delta_rps;
            if (d > 0 && use_delta[j] && i < 16) {
                out->delta_poc_s1[i] = d;
                out->used_s1[i++] = used_by_curr[j];
            }
        }
        if (delta_rps > 0 && use_delta[ref.num_delta_pocs()] && i < 16) {
            out->delta_poc_s1[i] = delta_rps;
            out->used_s1[i++] = used_by_curr[ref.num_delta_pocs()];
        }
        for (int j = 0; j < ref.num_positive; j++) {
            const int d = ref.delta_poc_s1[j] + delta_rps;
            if (d > 0 && use_delta[ref.num_negative + j] && i < 16) {
                out->delta_poc_s1[i] = d;
                out->used_s1[i++] = used_by_curr[ref.num_negative + j];
            }
        }
        out->num_positive = i;
    } else {
        const uint32_t num_negative = r.Ue();
        const uint32_t num_positive = r.Ue();
        if (num_negative > 16 || num_positive > 16) {
            *why = "damaged H.265 reference picture set";
            return false;
        }
        int poc = 0;
        for (uint32_t i = 0; i < num_negative; i++) {
            poc -= int(r.Ue() + 1);
            out->delta_poc_s0[i] = poc;
            out->used_s0[i] = r.Flag();
        }
        poc = 0;
        for (uint32_t i = 0; i < num_positive; i++) {
            poc += int(r.Ue() + 1);
            out->delta_poc_s1[i] = poc;
            out->used_s1[i] = r.Flag();
        }
        out->num_negative = int(num_negative);
        out->num_positive = int(num_positive);
    }
    return !r.overrun();
}

bool ParseVps(const uint8_t* nal, size_t size, Vps* vps, std::string* why) {
    if (size < 6) {
        *why = "damaged H.265 VPS";
        return false;
    }
    const std::vector<uint8_t> rbsp = Unescape(nal + 2, size - 2);
    BitReader r(rbsp.data(), rbsp.size());
    vps->bytes.assign(nal, nal + size);
    vps->id = uint8_t(r.Bits(4));
    r.Bits(2);
    r.Bits(6);  // vps_max_layers_minus1
    vps->max_sub_layers_minus1 = uint8_t(r.Bits(3));
    vps->temporal_id_nesting = r.Flag();
    r.Bits(16);
    ParseProfileTierLevel(r, vps->max_sub_layers_minus1, &vps->ptl);
    vps->sub_layer_ordering_info_present = r.Flag();
    for (int i = vps->sub_layer_ordering_info_present ? 0 : vps->max_sub_layers_minus1; i <= vps->max_sub_layers_minus1; i++) {
        vps->max_dec_pic_buffering_minus1[i] = uint8_t(r.Ue());
        vps->max_num_reorder_pics[i] = uint8_t(r.Ue());
        vps->max_latency_increase_plus1[i] = r.Ue();
    }
    if (!vps->sub_layer_ordering_info_present)
        for (int i = 0; i < vps->max_sub_layers_minus1; i++) {
            vps->max_dec_pic_buffering_minus1[i] = vps->max_dec_pic_buffering_minus1[vps->max_sub_layers_minus1];
            vps->max_num_reorder_pics[i] = vps->max_num_reorder_pics[vps->max_sub_layers_minus1];
            vps->max_latency_increase_plus1[i] = vps->max_latency_increase_plus1[vps->max_sub_layers_minus1];
        }
    const uint32_t max_layer_id = r.Bits(6);
    const uint32_t num_layer_sets_minus1 = r.Ue();
    for (uint32_t i = 1; i <= num_layer_sets_minus1 && i < 1024; i++)
        for (uint32_t j = 0; j <= max_layer_id; j++)
            r.Bit();
    vps->timing_info_present = r.Flag();
    if (vps->timing_info_present) {
        vps->num_units_in_tick = r.Bits(32);
        vps->time_scale = r.Bits(32);
        vps->poc_proportional = r.Flag();
        if (vps->poc_proportional)
            vps->num_ticks_poc_diff_one_minus1 = r.Ue();
    }
    if (r.overrun()) {
        *why = "damaged H.265 VPS";
        return false;
    }
    return true;
}

bool ParseSps(const uint8_t* nal, size_t size, Sps* sps, std::string* why) {
    if (size < 8) {
        *why = "damaged H.265 SPS";
        return false;
    }
    const std::vector<uint8_t> rbsp = Unescape(nal + 2, size - 2);
    BitReader r(rbsp.data(), rbsp.size());
    sps->bytes.assign(nal, nal + size);
    sps->vps_id = uint8_t(r.Bits(4));
    sps->max_sub_layers_minus1 = uint8_t(r.Bits(3));
    sps->temporal_id_nesting = r.Flag();
    ParseProfileTierLevel(r, sps->max_sub_layers_minus1, &sps->ptl);
    sps->id = uint8_t(r.Ue());
    sps->chroma_format_idc = uint8_t(r.Ue());
    if (sps->chroma_format_idc == 3)
        sps->separate_colour_plane = r.Flag();
    sps->width = r.Ue();
    sps->height = r.Ue();
    sps->conformance_window = r.Flag();
    if (sps->conformance_window) {
        sps->conf_left = r.Ue();
        sps->conf_right = r.Ue();
        sps->conf_top = r.Ue();
        sps->conf_bottom = r.Ue();
    }
    sps->bit_depth_luma = uint8_t(r.Ue() + 8);
    sps->bit_depth_chroma = uint8_t(r.Ue() + 8);
    sps->log2_max_poc_lsb = uint8_t(r.Ue() + 4);
    sps->sub_layer_ordering_info_present = r.Flag();
    for (int i = sps->sub_layer_ordering_info_present ? 0 : sps->max_sub_layers_minus1; i <= sps->max_sub_layers_minus1; i++) {
        sps->max_dec_pic_buffering_minus1[i] = uint8_t(r.Ue());
        sps->max_num_reorder_pics[i] = uint8_t(r.Ue());
        sps->max_latency_increase_plus1[i] = r.Ue();
    }
    if (!sps->sub_layer_ordering_info_present)
        for (int i = 0; i < sps->max_sub_layers_minus1; i++) {
            sps->max_dec_pic_buffering_minus1[i] = sps->max_dec_pic_buffering_minus1[sps->max_sub_layers_minus1];
            sps->max_num_reorder_pics[i] = sps->max_num_reorder_pics[sps->max_sub_layers_minus1];
            sps->max_latency_increase_plus1[i] = sps->max_latency_increase_plus1[sps->max_sub_layers_minus1];
        }
    sps->log2_min_cb_minus3 = uint8_t(r.Ue());
    sps->log2_diff_max_min_cb = uint8_t(r.Ue());
    sps->log2_min_tb_minus2 = uint8_t(r.Ue());
    sps->log2_diff_max_min_tb = uint8_t(r.Ue());
    sps->max_th_depth_inter = uint8_t(r.Ue());
    sps->max_th_depth_intra = uint8_t(r.Ue());
    sps->scaling_list_enabled = r.Flag();
    if (sps->scaling_list_enabled) {
        sps->scaling_list_data_present = r.Flag();
        if (sps->scaling_list_data_present) {
            *why = "H.265 scaling list data is not supported";
            return false;
        }
    }
    sps->amp = r.Flag();
    sps->sao = r.Flag();
    sps->pcm = r.Flag();
    if (sps->pcm) {
        sps->pcm_bit_depth_luma_minus1 = uint8_t(r.Bits(4));
        sps->pcm_bit_depth_chroma_minus1 = uint8_t(r.Bits(4));
        sps->log2_min_pcm_cb_minus3 = uint8_t(r.Ue());
        sps->log2_diff_max_min_pcm_cb = uint8_t(r.Ue());
        sps->pcm_loop_filter_disabled = r.Flag();
    }
    const uint32_t num_st_rps = r.Ue();
    if (num_st_rps > 64) {
        *why = "damaged H.265 SPS";
        return false;
    }
    for (uint32_t i = 0; i < num_st_rps; i++) {
        StRps set;
        if (!ParseStRps(r, int(i), int(num_st_rps), sps->st_rps, &set, nullptr, why))
            return false;
        sps->st_rps.push_back(set);
    }
    sps->long_term_ref_pics_present = r.Flag();
    if (sps->long_term_ref_pics_present) {
        sps->num_long_term_ref_pics_sps = uint8_t(r.Ue());
        if (sps->num_long_term_ref_pics_sps > 32) {
            *why = "damaged H.265 SPS";
            return false;
        }
        for (int i = 0; i < sps->num_long_term_ref_pics_sps; i++) {
            sps->lt_ref_pic_poc_lsb_sps[i] = uint16_t(r.Bits(sps->log2_max_poc_lsb));
            sps->used_by_curr_pic_lt_sps[i] = r.Flag();
        }
    }
    sps->temporal_mvp = r.Flag();
    sps->strong_intra_smoothing = r.Flag();
    sps->vui_present = r.Flag();
    if (sps->vui_present) {  // the colour description only
        if (r.Flag()) {  // aspect_ratio_info_present_flag
            if (r.Bits(8) == 255) {
                r.Bits(16);
                r.Bits(16);
            }
        }
        if (r.Flag())  // overscan_info_present_flag
            r.Bit();
        if (r.Flag()) {  // video_signal_type_present_flag
            r.Bits(3);
            sps->full_range = r.Flag();
            if (r.Flag()) {
                sps->has_color = true;
                sps->color_primaries = uint8_t(r.Bits(8));
                sps->transfer = uint8_t(r.Bits(8));
                sps->matrix = uint8_t(r.Bits(8));
            }
        }
    }
    return true;
}

bool ParsePps(const uint8_t* nal, size_t size, const std::map<uint8_t, Sps>& sps_map, Pps* pps, std::string* why) {
    if (size < 4) {
        *why = "damaged H.265 PPS";
        return false;
    }
    const std::vector<uint8_t> rbsp = Unescape(nal + 2, size - 2);
    BitReader r(rbsp.data(), rbsp.size());
    pps->bytes.assign(nal, nal + size);
    pps->id = uint8_t(r.Ue());
    pps->sps_id = uint8_t(r.Ue());
    pps->dependent_slice_segments_enabled = r.Flag();
    pps->output_flag_present = r.Flag();
    pps->num_extra_slice_header_bits = uint8_t(r.Bits(3));
    pps->sign_data_hiding = r.Flag();
    pps->cabac_init_present = r.Flag();
    pps->num_ref_idx_l0_default_minus1 = uint8_t(r.Ue());
    pps->num_ref_idx_l1_default_minus1 = uint8_t(r.Ue());
    pps->init_qp_minus26 = int8_t(r.Se());
    pps->constrained_intra_pred = r.Flag();
    pps->transform_skip = r.Flag();
    pps->cu_qp_delta_enabled = r.Flag();
    if (pps->cu_qp_delta_enabled)
        pps->diff_cu_qp_delta_depth = uint8_t(r.Ue());
    pps->cb_qp_offset = int8_t(r.Se());
    pps->cr_qp_offset = int8_t(r.Se());
    pps->slice_chroma_qp_offsets_present = r.Flag();
    pps->weighted_pred = r.Flag();
    pps->weighted_bipred = r.Flag();
    pps->transquant_bypass = r.Flag();
    pps->tiles_enabled = r.Flag();
    pps->entropy_coding_sync = r.Flag();
    if (pps->tiles_enabled) {
        pps->num_tile_columns_minus1 = uint8_t(r.Ue());
        pps->num_tile_rows_minus1 = uint8_t(r.Ue());
        if (pps->num_tile_columns_minus1 > 19 || pps->num_tile_rows_minus1 > 21) {
            *why = "H.265 with more than 20 tile columns or 22 tile rows is not supported";
            return false;
        }
        pps->uniform_spacing = r.Flag();
        if (!pps->uniform_spacing) {
            for (int i = 0; i < pps->num_tile_columns_minus1; i++)
                pps->column_width_minus1[i] = uint16_t(r.Ue());
            for (int i = 0; i < pps->num_tile_rows_minus1; i++)
                pps->row_height_minus1[i] = uint16_t(r.Ue());
        }
        pps->loop_filter_across_tiles = r.Flag();
    }
    pps->loop_filter_across_slices = r.Flag();
    pps->deblocking_filter_control_present = r.Flag();
    if (pps->deblocking_filter_control_present) {
        pps->deblocking_filter_override_enabled = r.Flag();
        pps->deblocking_filter_disabled = r.Flag();
        if (!pps->deblocking_filter_disabled) {
            pps->beta_offset_div2 = int8_t(r.Se());
            pps->tc_offset_div2 = int8_t(r.Se());
        }
    }
    pps->scaling_list_data_present = r.Flag();
    if (pps->scaling_list_data_present) {
        *why = "H.265 scaling list data is not supported";
        return false;
    }
    pps->lists_modification_present = r.Flag();
    pps->log2_parallel_merge_level_minus2 = uint8_t(r.Ue());
    pps->slice_segment_header_extension_present = r.Flag();
    if (r.overrun()) {
        *why = "damaged H.265 PPS";
        return false;
    }
    // Uniform tile spacing: the derived widths (6.5.1), which the decoder's parameter set wants too.
    if (pps->tiles_enabled && pps->uniform_spacing) {
        auto sps_it = sps_map.find(pps->sps_id);
        if (sps_it != sps_map.end()) {
            const Sps& sps = sps_it->second;
            const uint32_t ctb = 1u << sps.ctb_log2();
            const uint32_t width_ctbs = (sps.width + ctb - 1) / ctb, height_ctbs = (sps.height + ctb - 1) / ctb;
            const uint32_t cols = pps->num_tile_columns_minus1 + 1u, rows = pps->num_tile_rows_minus1 + 1u;
            for (uint32_t i = 0; i + 1 < cols; i++)
                pps->column_width_minus1[i] = uint16_t(((i + 1) * width_ctbs) / cols - (i * width_ctbs) / cols - 1);
            for (uint32_t i = 0; i + 1 < rows; i++)
                pps->row_height_minus1[i] = uint16_t(((i + 1) * height_ctbs) / rows - (i * height_ctbs) / rows - 1);
        }
    }
    return true;
}

struct DpbPicture {
    int slot = -1;
    int32_t poc = 0;
    bool long_term = false;
};

class H265Backend final : public CodecBackend {
public:
    bool Init(const Mp4& mp4, std::string* why) override {
        const Mp4Track& t = mp4.track();
        nal_length_size_ = t.nal_length_size;
        // hvcC: 22 bytes of record, then arrays of parameter set NAL units.
        const std::vector<uint8_t>& c = t.config;
        if (c.size() < 23) {
            *why = "damaged hvcC";
            return false;
        }
        size_t pos = 22;
        const int arrays = c[pos++];
        for (int a = 0; a < arrays && pos + 3 <= c.size(); a++) {
            pos++;  // array_completeness, NAL_unit_type
            const size_t count = (size_t(c[pos]) << 8) | c[pos + 1];
            pos += 2;
            for (size_t i = 0; i < count && pos + 2 <= c.size(); i++) {
                const size_t n = (size_t(c[pos]) << 8) | c[pos + 1];
                pos += 2;
                if (pos + n > c.size())
                    break;
                if (!AddParameterSet(c.data() + pos, n, why))
                    return false;
                pos += n;
            }
        }
        // Scan the samples: in-band parameter sets, and the IDR pictures.
        std::vector<uint8_t> head;
        for (uint32_t i = 0; i < t.samples.size(); i++) {
            const Sample& s = t.samples[i];
            uint64_t offset = s.offset;
            const uint64_t end = s.offset + s.size;
            bool idr = false;
            while (offset + nal_length_size_ < end) {
                uint8_t len_bytes[4] = {};
                if (!mp4.ReadRange(offset, len_bytes, nal_length_size_)) {
                    *why = "cannot read a sample";
                    return false;
                }
                uint64_t n = 0;
                for (uint32_t b = 0; b < nal_length_size_; b++)
                    n = (n << 8) | len_bytes[b];
                offset += nal_length_size_;
                if (n < 2 || offset + n > end) {
                    *why = "damaged H.265 NAL unit length";
                    return false;
                }
                uint8_t header[2] = {};
                mp4.ReadRange(offset, header, 2);
                const int type = (header[0] >> 1) & 63;
                const int layer = ((header[0] & 1) << 5) | (header[1] >> 3);
                if (layer == 0) {
                    if (type == kIdrWRadl || type == kIdrNLp)
                        idr = true;
                    if (type == kVps || type == kSps || type == kPps) {
                        head.resize(size_t(n));
                        mp4.ReadRange(offset, head.data(), size_t(n));
                        if (!AddParameterSet(head.data(), head.size(), why))
                            return false;
                    }
                }
                offset += n;
            }
            if (idr)
                start_samples_.push_back(i);
        }
        if (start_samples_.empty() || start_samples_.front() != 0) {
            *why = "the stream does not start with an IDR picture";
            return false;
        }
        if (vps_.empty() || sps_.empty() || pps_.empty()) {
            *why = "no H.265 parameter sets";
            return false;
        }
        // Re-derive PPS values that depend on the SPS (tile widths) now that all SPS are known.
        for (auto& [id, pps] : pps_) {
            Pps p;
            std::string ignored;
            if (ParsePps(pps.bytes.data(), pps.bytes.size(), sps_, &p, &ignored))
                pps = std::move(p);
        }
        const Sps& first = sps_.begin()->second;
        uint32_t dpb = 0;
        for (const auto& [id, sps] : sps_) {
            if (sps.width != first.width || sps.height != first.height || sps.conf_left != first.conf_left ||
                sps.conf_right != first.conf_right || sps.conf_top != first.conf_top || sps.conf_bottom != first.conf_bottom ||
                sps.bit_depth_luma != first.bit_depth_luma) {
                *why = "H.265 sequences of different sizes or bit depths in one stream are not supported";
                return false;
            }
            dpb = std::max<uint32_t>(dpb, sps.max_dec_pic_buffering_minus1[sps.max_sub_layers_minus1] + 1u);
        }
        format_.coded_width = first.width;
        format_.coded_height = first.height;
        format_.crop_x = first.conf_left * 2;
        format_.crop_y = first.conf_top * 2;
        format_.width = first.width - (first.conf_left + first.conf_right) * 2;
        format_.height = first.height - (first.conf_top + first.conf_bottom) * 2;
        format_.bit_depth = first.bit_depth_luma;
        format_.dpb_slots = std::max<uint32_t>(dpb, 1) + 1;
        format_.has_color = first.has_color;
        format_.color_primaries = first.color_primaries;
        format_.transfer = first.transfer;
        format_.matrix = first.matrix;
        format_.full_range = first.full_range;
        Reset();
        return true;
    }

    nri::VideoCodec nri_codec() const override { return nri::VideoCodec::H265; }

    void Reset() override {
        dpb_.clear();
        prev_tid0_lsb_ = prev_tid0_msb_ = 0;
        had_picture_ = false;
    }

    void FillSessionParameters(nri::VideoSessionParametersDesc* desc) override {
        const uint8_t profile_idc = format_.bit_depth > 8 ? 2 : 1;  // the session's fixed profile
        auto ptl_desc = [&](const ProfileTierLevel& p) {
            nri::VideoH265ProfileTierLevelDesc d = {};
            using B = nri::VideoH265ProfileTierLevelBits;
            B flags = B::NONE;
            if (p.tier)
                flags |= B::TIER;
            if (p.progressive)
                flags |= B::PROGRESSIVE_SOURCE;
            if (p.interlaced)
                flags |= B::INTERLACED_SOURCE;
            if (p.non_packed)
                flags |= B::NON_PACKED_CONSTRAINT;
            if (p.frame_only)
                flags |= B::FRAME_ONLY_CONSTRAINT;
            d.flags = flags;
            d.generalProfileIdc = profile_idc;
            d.generalLevelIdc = p.level_idc;
            return d;
        };
        nri_vps_.clear();
        nri_sps_.clear();
        nri_pps_.clear();
        nri_st_rps_.clear();
        nri_lt_.clear();
        for (const auto& [id, v] : vps_) {
            nri::VideoH265VideoParameterSetDesc d = {};
            using B = nri::VideoH265VideoParameterSetBits;
            B flags = B::NONE;
            if (v.temporal_id_nesting)
                flags |= B::TEMPORAL_ID_NESTING;
            if (v.sub_layer_ordering_info_present)
                flags |= B::SUB_LAYER_ORDERING_INFO_PRESENT;
            if (v.timing_info_present)
                flags |= B::TIMING_INFO_PRESENT;
            if (v.poc_proportional)
                flags |= B::POC_PROPORTIONAL_TO_TIMING;
            d.flags = flags;
            d.videoParameterSetId = v.id;
            d.maxSubLayersMinus1 = v.max_sub_layers_minus1;
            d.numUnitsInTick = v.num_units_in_tick;
            d.timeScale = v.time_scale;
            d.numTicksPocDiffOneMinus1 = v.num_ticks_poc_diff_one_minus1;
            d.profileTierLevel = ptl_desc(v.ptl);
            for (int i = 0; i < 8; i++) {
                d.decPicBufMgr.maxDecPicBufferingMinus1[i] = v.max_dec_pic_buffering_minus1[i];
                d.decPicBufMgr.maxNumReorderPics[i] = v.max_num_reorder_pics[i];
                d.decPicBufMgr.maxLatencyIncreasePlus1[i] = v.max_latency_increase_plus1[i];
            }
            nri_vps_.push_back(d);
        }
        // The short-term sets of all SPS are stored first so the pointers stay valid.
        size_t st_total = 0, lt_total = 0;
        for (const auto& [id, s] : sps_) {
            st_total += s.st_rps.size();
            lt_total += s.num_long_term_ref_pics_sps ? 1 : 0;
        }
        nri_st_rps_.reserve(st_total);
        nri_lt_.reserve(lt_total);
        for (const auto& [id, s] : sps_) {
            nri::VideoH265SequenceParameterSetDesc d = {};
            using B = nri::VideoH265SequenceParameterSetBits;
            B flags = B::NONE;
            if (s.temporal_id_nesting)
                flags |= B::TEMPORAL_ID_NESTING;
            if (s.separate_colour_plane)
                flags |= B::SEPARATE_COLOUR_PLANE;
            if (s.conformance_window)
                flags |= B::CONFORMANCE_WINDOW;
            if (s.sub_layer_ordering_info_present)
                flags |= B::SUB_LAYER_ORDERING_INFO_PRESENT;
            if (s.scaling_list_enabled)
                flags |= B::SCALING_LIST_ENABLED;
            if (s.amp)
                flags |= B::AMP_ENABLED;
            if (s.sao)
                flags |= B::SAMPLE_ADAPTIVE_OFFSET_ENABLED;
            if (s.pcm)
                flags |= B::PCM_ENABLED;
            if (s.pcm_loop_filter_disabled)
                flags |= B::PCM_LOOP_FILTER_DISABLED;
            if (s.long_term_ref_pics_present)
                flags |= B::LONG_TERM_REF_PICS_PRESENT;
            if (s.temporal_mvp)
                flags |= B::TEMPORAL_MVP_ENABLED;
            if (s.strong_intra_smoothing)
                flags |= B::STRONG_INTRA_SMOOTHING_ENABLED;
            if (s.vui_present)
                flags |= B::VUI_PARAMETERS_PRESENT;
            d.flags = flags;
            d.videoParameterSetId = s.vps_id;
            d.maxSubLayersMinus1 = s.max_sub_layers_minus1;
            d.sequenceParameterSetId = s.id;
            d.chromaFormatIdc = s.chroma_format_idc;
            d.pictureWidthInLumaSamples = s.width;
            d.pictureHeightInLumaSamples = s.height;
            d.bitDepthLumaMinus8 = uint8_t(s.bit_depth_luma - 8);
            d.bitDepthChromaMinus8 = uint8_t(s.bit_depth_chroma - 8);
            d.log2MaxPictureOrderCountLsbMinus4 = uint8_t(s.log2_max_poc_lsb - 4);
            d.log2MinLumaCodingBlockSizeMinus3 = s.log2_min_cb_minus3;
            d.log2DiffMaxMinLumaCodingBlockSize = s.log2_diff_max_min_cb;
            d.log2MinLumaTransformBlockSizeMinus2 = s.log2_min_tb_minus2;
            d.log2DiffMaxMinLumaTransformBlockSize = s.log2_diff_max_min_tb;
            d.maxTransformHierarchyDepthInter = s.max_th_depth_inter;
            d.maxTransformHierarchyDepthIntra = s.max_th_depth_intra;
            d.pcmSampleBitDepthLumaMinus1 = s.pcm_bit_depth_luma_minus1;
            d.pcmSampleBitDepthChromaMinus1 = s.pcm_bit_depth_chroma_minus1;
            d.log2MinPcmLumaCodingBlockSizeMinus3 = s.log2_min_pcm_cb_minus3;
            d.log2DiffMaxMinPcmLumaCodingBlockSize = s.log2_diff_max_min_pcm_cb;
            d.confWinLeftOffset = s.conf_left;
            d.confWinRightOffset = s.conf_right;
            d.confWinTopOffset = s.conf_top;
            d.confWinBottomOffset = s.conf_bottom;
            // The sets as explicit lists (the derived form), which any reading of the syntax fields accepts.
            const size_t first_set = nri_st_rps_.size();
            for (const StRps& set : s.st_rps) {
                nri::VideoH265ShortTermRefPicSetDesc rps = {};
                rps.numNegativePics = uint8_t(set.num_negative);
                rps.numPositivePics = uint8_t(set.num_positive);
                int prev = 0;
                for (int i = 0; i < set.num_negative; i++) {
                    rps.deltaPocS0Minus1[i] = uint16_t(prev - set.delta_poc_s0[i] - 1);
                    prev = set.delta_poc_s0[i];
                    if (set.used_s0[i])
                        rps.usedByCurrPicS0Flag |= uint16_t(1u << i);
                }
                prev = 0;
                for (int i = 0; i < set.num_positive; i++) {
                    rps.deltaPocS1Minus1[i] = uint16_t(set.delta_poc_s1[i] - prev - 1);
                    prev = set.delta_poc_s1[i];
                    if (set.used_s1[i])
                        rps.usedByCurrPicS1Flag |= uint16_t(1u << i);
                }
                nri_st_rps_.push_back(rps);
            }
            d.shortTermRefPicSets = s.st_rps.empty() ? nullptr : nri_st_rps_.data() + first_set;
            d.numShortTermRefPicSets = uint8_t(s.st_rps.size());
            d.numLongTermRefPicsSps = s.num_long_term_ref_pics_sps;
            if (s.num_long_term_ref_pics_sps) {
                nri::VideoH265LongTermRefPicsSpsDesc lt = {};
                for (int i = 0; i < s.num_long_term_ref_pics_sps; i++) {
                    lt.ltRefPicPocLsbSps[i] = s.lt_ref_pic_poc_lsb_sps[i];
                    if (s.used_by_curr_pic_lt_sps[i])
                        lt.usedByCurrPicLtSpsFlag |= 1u << i;
                }
                nri_lt_.push_back(lt);
                d.longTermRefPicsSps = &nri_lt_.back();
            }
            d.profileTierLevel = ptl_desc(s.ptl);
            for (int i = 0; i < 8; i++) {
                d.decPicBufMgr.maxDecPicBufferingMinus1[i] = s.max_dec_pic_buffering_minus1[i];
                d.decPicBufMgr.maxNumReorderPics[i] = s.max_num_reorder_pics[i];
                d.decPicBufMgr.maxLatencyIncreasePlus1[i] = s.max_latency_increase_plus1[i];
            }
            nri_sps_.push_back(d);
        }
        for (const auto& [id, p] : pps_) {
            nri::VideoH265PictureParameterSetDesc d = {};
            using B = nri::VideoH265PictureParameterSetBits;
            B flags = B::NONE;
            auto set = [&](bool cond, B bit) {
                if (cond)
                    flags |= bit;
            };
            set(p.dependent_slice_segments_enabled, B::DEPENDENT_SLICE_SEGMENTS_ENABLED);
            set(p.output_flag_present, B::OUTPUT_FLAG_PRESENT);
            set(p.sign_data_hiding, B::SIGN_DATA_HIDING_ENABLED);
            set(p.cabac_init_present, B::CABAC_INIT_PRESENT);
            set(p.constrained_intra_pred, B::CONSTRAINED_INTRA_PRED);
            set(p.transform_skip, B::TRANSFORM_SKIP_ENABLED);
            set(p.cu_qp_delta_enabled, B::CU_QP_DELTA_ENABLED);
            set(p.slice_chroma_qp_offsets_present, B::SLICE_CHROMA_QP_OFFSETS_PRESENT);
            set(p.weighted_pred, B::WEIGHTED_PRED);
            set(p.weighted_bipred, B::WEIGHTED_BIPRED);
            set(p.transquant_bypass, B::TRANSQUANT_BYPASS_ENABLED);
            set(p.tiles_enabled, B::TILES_ENABLED);
            set(p.entropy_coding_sync, B::ENTROPY_CODING_SYNC_ENABLED);
            set(p.uniform_spacing, B::UNIFORM_SPACING);
            set(p.loop_filter_across_tiles, B::LOOP_FILTER_ACROSS_TILES_ENABLED);
            set(p.loop_filter_across_slices, B::LOOP_FILTER_ACROSS_SLICES_ENABLED);
            set(p.deblocking_filter_control_present, B::DEBLOCKING_FILTER_CONTROL_PRESENT);
            set(p.deblocking_filter_override_enabled, B::DEBLOCKING_FILTER_OVERRIDE_ENABLED);
            set(p.deblocking_filter_disabled, B::DEBLOCKING_FILTER_DISABLED);
            set(p.scaling_list_data_present, B::SCALING_LIST_DATA_PRESENT);
            set(p.lists_modification_present, B::LISTS_MODIFICATION_PRESENT);
            set(p.slice_segment_header_extension_present, B::SLICE_SEGMENT_HEADER_EXTENSION_PRESENT);
            d.flags = flags;
            d.pictureParameterSetId = p.id;
            d.sequenceParameterSetId = p.sps_id;
            auto sps_it = sps_.find(p.sps_id);
            d.videoParameterSetId = sps_it != sps_.end() ? sps_it->second.vps_id : 0;
            d.numExtraSliceHeaderBits = p.num_extra_slice_header_bits;
            d.refIndexL0DefaultActiveMinus1 = p.num_ref_idx_l0_default_minus1;
            d.refIndexL1DefaultActiveMinus1 = p.num_ref_idx_l1_default_minus1;
            d.initQpMinus26 = p.init_qp_minus26;
            d.diffCuQpDeltaDepth = p.diff_cu_qp_delta_depth;
            d.cbQpOffset = p.cb_qp_offset;
            d.crQpOffset = p.cr_qp_offset;
            d.betaOffsetDiv2 = p.beta_offset_div2;
            d.tcOffsetDiv2 = p.tc_offset_div2;
            d.log2ParallelMergeLevelMinus2 = p.log2_parallel_merge_level_minus2;
            d.tileColumnNumMinus1 = p.num_tile_columns_minus1;
            d.tileRowNumMinus1 = p.num_tile_rows_minus1;
            std::memcpy(d.columnWidthMinus1, p.column_width_minus1, sizeof(d.columnWidthMinus1));
            std::memcpy(d.rowHeightMinus1, p.row_height_minus1, sizeof(d.rowHeightMinus1));
            nri_pps_.push_back(d);
        }
        nri_parameters_ = {};
        nri_parameters_.videoParameterSets = nri_vps_.data();
        nri_parameters_.videoParameterSetNum = uint32_t(nri_vps_.size());
        nri_parameters_.sequenceParameterSets = nri_sps_.data();
        nri_parameters_.sequenceParameterSetNum = uint32_t(nri_sps_.size());
        nri_parameters_.pictureParameterSets = nri_pps_.data();
        nri_parameters_.pictureParameterSetNum = uint32_t(nri_pps_.size());
        *desc = {};
        desc->h265Parameters = &nri_parameters_;
    }

    bool ParseSample(uint32_t sample, const std::vector<uint8_t>& bytes, std::vector<PictureJob>* jobs,
                     std::string* why) override {
        (void)sample;
        jobs->clear();
        struct Nal {
            int type, temporal_id;
            const uint8_t* data;
            size_t size;
        };
        std::vector<Nal> slices;
        size_t pos = 0;
        while (pos + nal_length_size_ <= bytes.size()) {
            uint64_t n = 0;
            for (uint32_t b = 0; b < nal_length_size_; b++)
                n = (n << 8) | bytes[pos + b];
            pos += nal_length_size_;
            if (n < 2 || pos + n > bytes.size()) {
                *why = "damaged H.265 NAL unit";
                return false;
            }
            const int type = (bytes[pos] >> 1) & 63;
            const int layer = ((bytes[pos] & 1) << 5) | (bytes[pos + 1] >> 3);
            const int temporal_id = (bytes[pos + 1] & 7) - 1;
            if (layer == 0 && (type <= kRaslR || (type >= kBlaWLp && type <= kCra)))
                slices.push_back({type, temporal_id, bytes.data() + pos, size_t(n)});
            pos += size_t(n);
        }
        if (slices.empty()) {
            *why = "no H.265 slice in the sample";
            return false;
        }
        const Nal& first = slices[0];
        const std::vector<uint8_t> rbsp = Unescape(first.data + 2, std::min<size_t>(first.size - 2, 1024));
        BitReader r(rbsp.data(), rbsp.size());
        const bool first_slice_segment_in_pic = r.Flag();
        if (!first_slice_segment_in_pic) {
            *why = "H.265 access unit does not start with a first slice segment";
            return false;
        }
        const bool irap = first.type >= kBlaWLp && first.type <= kReservedIrap23;
        const bool idr = first.type == kIdrWRadl || first.type == kIdrNLp;
        if (irap)
            r.Bit();  // no_output_of_prior_pics_flag
        const uint32_t pps_id = r.Ue();
        auto pps_it = pps_.find(uint8_t(pps_id));
        if (pps_it == pps_.end()) {
            *why = "an H.265 slice names a missing PPS";
            return false;
        }
        const Pps& pps = pps_it->second;
        auto sps_it = sps_.find(pps.sps_id);
        if (sps_it == sps_.end()) {
            *why = "an H.265 PPS names a missing SPS";
            return false;
        }
        const Sps& sps = sps_it->second;
        for (int i = 0; i < pps.num_extra_slice_header_bits; i++)
            r.Bit();
        r.Ue();  // slice_type
        if (pps.output_flag_present)
            r.Bit();
        if (sps.separate_colour_plane)
            r.Bits(2);
        const int32_t max_lsb = 1 << sps.log2_max_poc_lsb;
        int32_t poc = 0, poc_lsb = 0, poc_msb = 0;
        bool st_rps_sps_flag = false;
        uint16_t st_bits = 0;
        uint8_t ref_rps_num_delta_pocs = 0;
        StRps rps;
        struct LongTerm {
            int32_t poc;
            bool used, msb_present;
        };
        std::vector<LongTerm> long_terms;
        if (!idr) {
            poc_lsb = int32_t(r.Bits(sps.log2_max_poc_lsb));
            // 8.3.1
            int32_t msb;
            if (irap && first.type != kCra) {
                msb = 0;  // BLA
            } else if (irap && !had_picture_) {
                msb = 0;  // CRA first in the stream
            } else {
                if (poc_lsb < prev_tid0_lsb_ && (prev_tid0_lsb_ - poc_lsb) >= max_lsb / 2)
                    msb = prev_tid0_msb_ + max_lsb;
                else if (poc_lsb > prev_tid0_lsb_ && (poc_lsb - prev_tid0_lsb_) > max_lsb / 2)
                    msb = prev_tid0_msb_ - max_lsb;
                else
                    msb = prev_tid0_msb_;
            }
            poc_msb = msb;
            poc = msb + poc_lsb;
            st_rps_sps_flag = r.Flag();
            if (!st_rps_sps_flag) {
                const uint64_t before = r.position();
                int ref_deltas = 0;
                if (!ParseStRps(r, int(sps.st_rps.size()), int(sps.st_rps.size()), sps.st_rps, &rps, &ref_deltas, why))
                    return false;
                st_bits = uint16_t(r.position() - before);
                ref_rps_num_delta_pocs = uint8_t(ref_deltas);
            } else {
                int idx = 0;
                if (sps.st_rps.size() > 1) {
                    int bits = 0;
                    while ((1u << bits) < sps.st_rps.size())
                        bits++;
                    idx = int(r.Bits(bits));
                }
                if (idx >= int(sps.st_rps.size())) {
                    *why = "damaged H.265 slice header";
                    return false;
                }
                rps = sps.st_rps[size_t(idx)];
            }
            if (sps.long_term_ref_pics_present) {
                uint32_t num_long_term_sps = 0;
                if (sps.num_long_term_ref_pics_sps > 0)
                    num_long_term_sps = r.Ue();
                const uint32_t num_long_term_pics = r.Ue();
                if (num_long_term_sps + num_long_term_pics > 32) {
                    *why = "damaged H.265 slice header";
                    return false;
                }
                int32_t delta_msb_cycle = 0;
                for (uint32_t i = 0; i < num_long_term_sps + num_long_term_pics; i++) {
                    int32_t lsb;
                    bool used;
                    if (i < num_long_term_sps) {
                        uint32_t lt_idx = 0;
                        if (sps.num_long_term_ref_pics_sps > 1) {
                            int bits = 0;
                            while ((1u << bits) < sps.num_long_term_ref_pics_sps)
                                bits++;
                            lt_idx = r.Bits(bits);
                        }
                        lsb = sps.lt_ref_pic_poc_lsb_sps[lt_idx];
                        used = sps.used_by_curr_pic_lt_sps[lt_idx];
                    } else {
                        lsb = int32_t(r.Bits(sps.log2_max_poc_lsb));
                        used = r.Flag();
                    }
                    const bool msb_present = r.Flag();
                    uint32_t cycle = 0;
                    if (msb_present)
                        cycle = r.Ue();
                    if (i == 0 || i == num_long_term_sps)
                        delta_msb_cycle = int32_t(cycle);
                    else
                        delta_msb_cycle += int32_t(cycle);
                    int32_t lt_poc = lsb;
                    if (msb_present)
                        lt_poc += poc - delta_msb_cycle * max_lsb - (poc & (max_lsb - 1));
                    long_terms.push_back({lt_poc, used, msb_present});
                }
            }
        }
        if (r.overrun()) {
            *why = "damaged H.265 slice header";
            return false;
        }

        // 8.3.2: IRAP with NoRaslOutputFlag drops every reference picture.
        if (irap && (first.type != kCra || !had_picture_))
            dpb_.clear();
        std::vector<int32_t> st_before, st_after, st_foll, lt_curr_poc, lt_foll_poc;
        std::vector<bool> lt_curr_msb, lt_foll_msb;
        if (!idr) {
            for (int i = 0; i < rps.num_negative; i++) {
                const int32_t p = poc + rps.delta_poc_s0[i];
                (rps.used_s0[i] ? st_before : st_foll).push_back(p);
            }
            for (int i = 0; i < rps.num_positive; i++) {
                const int32_t p = poc + rps.delta_poc_s1[i];
                (rps.used_s1[i] ? st_after : st_foll).push_back(p);
            }
            for (const LongTerm& lt : long_terms) {
                if (lt.used) {
                    lt_curr_poc.push_back(lt.poc);
                    lt_curr_msb.push_back(lt.msb_present);
                } else {
                    lt_foll_poc.push_back(lt.poc);
                    lt_foll_msb.push_back(lt.msb_present);
                }
            }
        }
        auto find_long = [&](int32_t lt_poc, bool msb_present) -> DpbPicture* {
            for (DpbPicture& p : dpb_)
                if (msb_present ? p.poc == lt_poc : (p.poc & (max_lsb - 1)) == lt_poc)
                    return &p;
            return nullptr;
        };
        auto find_short = [&](int32_t st_poc) -> DpbPicture* {
            for (DpbPicture& p : dpb_)
                if (!p.long_term && p.poc == st_poc)
                    return &p;
            return nullptr;
        };
        H265Job h265;
        std::vector<DpbPicture> next;
        auto add = [&](const DpbPicture& p) {
            for (const DpbPicture& q : next)
                if (q.slot == p.slot)
                    return;
            next.push_back(p);
        };
        for (size_t i = 0; i < lt_curr_poc.size(); i++) {
            DpbPicture* p = find_long(lt_curr_poc[i], lt_curr_msb[i]);
            if (!p) {
                *why = "H.265 missing long-term reference picture";
                return false;
            }
            DpbPicture q = *p;
            q.long_term = true;
            add(q);
            h265.refs.push_back({q.slot, q.poc, true});
        }
        for (size_t i = 0; i < lt_foll_poc.size(); i++)
            if (DpbPicture* p = find_long(lt_foll_poc[i], lt_foll_msb[i])) {
                DpbPicture q = *p;
                q.long_term = true;
                add(q);
            }
        for (int32_t p_poc : st_before) {
            DpbPicture* p = find_short(p_poc);
            if (!p) {
                *why = "H.265 missing short-term reference picture (POC " + std::to_string(p_poc) + ")";
                return false;
            }
            add(*p);
            h265.refs.push_back({p->slot, p->poc, false});
        }
        for (int32_t p_poc : st_after) {
            DpbPicture* p = find_short(p_poc);
            if (!p) {
                *why = "H.265 missing short-term reference picture (POC " + std::to_string(p_poc) + ")";
                return false;
            }
            add(*p);
            h265.refs.push_back({p->slot, p->poc, false});
        }
        for (int32_t p_poc : st_foll)
            if (DpbPicture* p = find_short(p_poc))
                add(*p);
        dpb_ = next;

        PictureJob job;
        for (const H265Ref& ref : h265.refs)
            if (std::find(job.bound_slots.begin(), job.bound_slots.end(), ref.slot) == job.bound_slots.end())
                job.bound_slots.push_back(ref.slot);
        int slot = -1;
        for (uint32_t i = 0; i < format_.dpb_slots && slot < 0; i++) {
            bool used = false;
            for (const DpbPicture& p : dpb_)
                used = used || p.slot == int(i);
            if (!used)
                slot = int(i);
        }
        if (slot < 0) {
            *why = "H.265 reference pictures exceed the DPB";
            return false;
        }
        h265.vps_id = sps.vps_id;
        h265.sps_id = sps.id;
        h265.pps_id = pps.id;
        h265.poc = poc;
        h265.irap = irap;
        h265.idr = idr;
        h265.reference = true;
        h265.short_term_ref_pic_set_sps = st_rps_sps_flag;
        h265.num_delta_pocs_of_ref_rps_idx = ref_rps_num_delta_pocs;
        h265.num_bits_for_st_ref_pic_set_in_slice = st_bits;
        for (const Nal& s : slices) {
            h265.slice_offsets.push_back(uint32_t(job.bitstream.size()));
            const uint8_t start_code[4] = {0, 0, 0, 1};
            job.bitstream.insert(job.bitstream.end(), start_code, start_code + 4);
            job.bitstream.insert(job.bitstream.end(), s.data, s.data + s.size);
        }
        job.setup_slot = slot;
        job.shown = true;
        job.show_slot = slot;
        job.codec = std::move(h265);
        // The decoded picture joins the DPB as a short-term reference.
        dpb_.push_back({slot, poc, false});
        // 8.3.1: prevTid0Pic is a TemporalId 0 picture that is not RASL, RADL or sub-layer non-reference.
        const bool slnr = first.type <= 14 && (first.type % 2) == 0;
        if (first.temporal_id == 0 && first.type != kRadlN && first.type != kRadlR && first.type != kRaslN &&
            first.type != kRaslR && !slnr) {
            prev_tid0_lsb_ = poc_lsb;
            prev_tid0_msb_ = poc_msb;
        }
        had_picture_ = true;
        jobs->push_back(std::move(job));
        return true;
    }

private:
    bool AddParameterSet(const uint8_t* nal, size_t size, std::string* why) {
        if (size < 3)
            return true;
        const int type = (nal[0] >> 1) & 63;
        const int layer = ((nal[0] & 1) << 5) | (nal[1] >> 3);
        if (layer != 0)
            return true;
        if (type == kVps) {
            Vps v;
            if (!ParseVps(nal, size, &v, why))
                return false;
            auto it = vps_.find(v.id);
            if (it != vps_.end()) {
                if (it->second.bytes != v.bytes) {
                    *why = "an H.265 VPS id is used with different parameters in one stream";
                    return false;
                }
                return true;
            }
            vps_[v.id] = std::move(v);
        } else if (type == kSps) {
            Sps s;
            if (!ParseSps(nal, size, &s, why))
                return false;
            auto it = sps_.find(s.id);
            if (it != sps_.end()) {
                if (it->second.bytes != s.bytes) {
                    *why = "an H.265 SPS id is used with different parameters in one stream";
                    return false;
                }
                return true;
            }
            const uint8_t profile = s.ptl.profile_idc;
            if (profile != 1 && profile != 2 && profile != 3) {
                *why = "H.265 profile " + std::to_string(profile) + " is not supported (Main and Main 10 are)";
                return false;
            }
            if (s.chroma_format_idc != 1) {
                *why = "H.265 other than 4:2:0 is not supported";
                return false;
            }
            if ((s.bit_depth_luma != 8 && s.bit_depth_luma != 10) || s.bit_depth_luma != s.bit_depth_chroma) {
                *why = "H.265 bit depth " + std::to_string(s.bit_depth_luma) + " is not supported";
                return false;
            }
            if (!s.ptl.frame_only && s.ptl.interlaced) {
                *why = "interlaced H.265 is not supported";
                return false;
            }
            sps_[s.id] = std::move(s);
        } else if (type == kPps) {
            Pps p;
            if (!ParsePps(nal, size, sps_, &p, why))
                return false;
            auto it = pps_.find(p.id);
            if (it != pps_.end()) {
                if (it->second.bytes != p.bytes) {
                    *why = "an H.265 PPS id is used with different parameters in one stream";
                    return false;
                }
                return true;
            }
            pps_[p.id] = std::move(p);
        }
        return true;
    }

    uint32_t nal_length_size_ = 4;
    std::map<uint8_t, Vps> vps_;
    std::map<uint8_t, Sps> sps_;
    std::map<uint8_t, Pps> pps_;
    std::vector<DpbPicture> dpb_;
    int32_t prev_tid0_lsb_ = 0, prev_tid0_msb_ = 0;
    bool had_picture_ = false;
    std::vector<nri::VideoH265VideoParameterSetDesc> nri_vps_;
    std::vector<nri::VideoH265SequenceParameterSetDesc> nri_sps_;
    std::vector<nri::VideoH265PictureParameterSetDesc> nri_pps_;
    std::vector<nri::VideoH265ShortTermRefPicSetDesc> nri_st_rps_;
    std::vector<nri::VideoH265LongTermRefPicsSpsDesc> nri_lt_;
    nri::VideoH265SessionParametersDesc nri_parameters_ = {};
};

}  // namespace

std::unique_ptr<CodecBackend> MakeH265Backend() { return std::make_unique<H265Backend>(); }

}  // namespace renderer::clips::video
