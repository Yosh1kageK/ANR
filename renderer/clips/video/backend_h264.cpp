// H.264 (ITU-T H.264, 7.3 syntax, 8.2.1 picture order count, 8.2.5 reference picture marking): progressive 8-bit 4:2:0
// streams of the profiles NRI's High profile decode session covers, without scaling matrices (NRI's parameter set
// descriptions have no fields for them), POC type 1, interlacing, slice groups or redundant slices. The container's
// length-prefixed NAL units become an Annex-B byte stream of the picture's slices; the parameter sets go to the session.

#include <algorithm>
#include <cstring>
#include <map>

#include "renderer/clips/video/bitreader.h"
#include "renderer/clips/video/codec_backend.h"

namespace renderer::clips::video {

namespace {

struct Sps {
    uint8_t profile_idc = 0, constraint_flags = 0, level_idc = 0, id = 0;
    uint8_t chroma_format_idc = 1;
    bool separate_colour_plane = false;
    uint8_t bit_depth_luma = 8, bit_depth_chroma = 8;
    bool qpprime_y_zero_transform_bypass = false;
    uint8_t log2_max_frame_num = 4;
    uint8_t poc_type = 0;
    uint8_t log2_max_poc_lsb = 4;
    bool delta_pic_order_always_zero = false;
    int32_t offset_for_non_ref_pic = 0, offset_for_top_to_bottom_field = 0;
    uint8_t max_num_ref_frames = 0;
    bool gaps_in_frame_num_allowed = false;
    uint32_t width_mbs = 0, height_map_units = 0;
    bool frame_mbs_only = true, mb_adaptive_frame_field = false, direct_8x8_inference = false;
    bool frame_cropping = false;
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    bool has_color = false, full_range = false;
    uint8_t color_primaries = 2, transfer = 2, matrix = 2;
    std::vector<uint8_t> bytes;  // the NAL unit, to compare duplicates
};

struct Pps {
    uint8_t id = 0, sps_id = 0;
    bool entropy_coding_mode = false, bottom_field_pic_order_in_frame_present = false;
    uint8_t num_ref_idx_l0_default_minus1 = 0, num_ref_idx_l1_default_minus1 = 0;
    bool weighted_pred = false;
    uint8_t weighted_bipred_idc = 0;
    int8_t pic_init_qp_minus26 = 0, pic_init_qs_minus26 = 0, chroma_qp_index_offset = 0, second_chroma_qp_index_offset = 0;
    bool deblocking_filter_control_present = false, constrained_intra_pred = false, redundant_pic_cnt_present = false;
    bool transform_8x8_mode = false;
    std::vector<uint8_t> bytes;
};

bool ParseSps(const uint8_t* nal, size_t size, Sps* sps, std::string* why) {
    if (size < 4) {
        *why = "damaged H.264 SPS";
        return false;
    }
    const std::vector<uint8_t> rbsp = Unescape(nal + 1, size - 1);
    BitReader r(rbsp.data(), rbsp.size());
    sps->bytes.assign(nal, nal + size);
    sps->profile_idc = uint8_t(r.Bits(8));
    sps->constraint_flags = uint8_t(r.Bits(8));
    sps->level_idc = uint8_t(r.Bits(8));
    sps->id = uint8_t(r.Ue());
    const int p = sps->profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 ||
        p == 139 || p == 134 || p == 135) {
        sps->chroma_format_idc = uint8_t(r.Ue());
        if (sps->chroma_format_idc == 3)
            sps->separate_colour_plane = r.Flag();
        sps->bit_depth_luma = uint8_t(r.Ue() + 8);
        sps->bit_depth_chroma = uint8_t(r.Ue() + 8);
        sps->qpprime_y_zero_transform_bypass = r.Flag();
        if (r.Flag()) {
            *why = "H.264 scaling matrices are not supported";
            return false;
        }
    }
    sps->log2_max_frame_num = uint8_t(r.Ue() + 4);
    sps->poc_type = uint8_t(r.Ue());
    if (sps->poc_type == 0) {
        sps->log2_max_poc_lsb = uint8_t(r.Ue() + 4);
    } else if (sps->poc_type == 1) {
        *why = "H.264 picture order count type 1 is not supported";
        return false;
    }
    sps->max_num_ref_frames = uint8_t(r.Ue());
    sps->gaps_in_frame_num_allowed = r.Flag();
    sps->width_mbs = r.Ue() + 1;
    sps->height_map_units = r.Ue() + 1;
    sps->frame_mbs_only = r.Flag();
    if (!sps->frame_mbs_only)
        sps->mb_adaptive_frame_field = r.Flag();
    sps->direct_8x8_inference = r.Flag();
    sps->frame_cropping = r.Flag();
    if (sps->frame_cropping) {
        sps->crop_left = r.Ue();
        sps->crop_right = r.Ue();
        sps->crop_top = r.Ue();
        sps->crop_bottom = r.Ue();
    }
    if (r.Flag()) {  // vui_parameters_present_flag: only the colour description is read
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
    if (r.overrun() && !sps->has_color) {
        // A truncated VUI is not fatal; the fields before it were read.
    }
    return true;
}

bool ParsePps(const uint8_t* nal, size_t size, Pps* pps, std::string* why) {
    if (size < 3) {
        *why = "damaged H.264 PPS";
        return false;
    }
    const std::vector<uint8_t> rbsp = Unescape(nal + 1, size - 1);
    BitReader r(rbsp.data(), rbsp.size());
    pps->bytes.assign(nal, nal + size);
    pps->id = uint8_t(r.Ue());
    pps->sps_id = uint8_t(r.Ue());
    pps->entropy_coding_mode = r.Flag();
    pps->bottom_field_pic_order_in_frame_present = r.Flag();
    if (r.Ue() > 0) {
        *why = "H.264 slice groups (FMO) are not supported";
        return false;
    }
    pps->num_ref_idx_l0_default_minus1 = uint8_t(r.Ue());
    pps->num_ref_idx_l1_default_minus1 = uint8_t(r.Ue());
    pps->weighted_pred = r.Flag();
    pps->weighted_bipred_idc = uint8_t(r.Bits(2));
    pps->pic_init_qp_minus26 = int8_t(r.Se());
    pps->pic_init_qs_minus26 = int8_t(r.Se());
    pps->chroma_qp_index_offset = int8_t(r.Se());
    pps->deblocking_filter_control_present = r.Flag();
    pps->constrained_intra_pred = r.Flag();
    pps->redundant_pic_cnt_present = r.Flag();
    pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    if (r.MoreRbspData()) {
        pps->transform_8x8_mode = r.Flag();
        if (r.Flag()) {
            *why = "H.264 scaling matrices are not supported";
            return false;
        }
        pps->second_chroma_qp_index_offset = int8_t(r.Se());
    }
    if (pps->redundant_pic_cnt_present) {
        *why = "H.264 redundant slices are not supported";
        return false;
    }
    return !r.overrun();
}

// A frame in the DPB, marked as used for reference.
struct Frame {
    int slot = -1;
    uint16_t frame_num = 0;
    int frame_num_wrap = 0;
    int long_term_frame_idx = -1;  // >= 0: long-term
    int32_t poc[2] = {0, 0};
};

struct NalRef {
    uint8_t type = 0, ref_idc = 0;
    const uint8_t* data = nullptr;  // the NAL unit, header byte first
    size_t size = 0;
};

class H264Backend final : public CodecBackend {
public:
    bool Init(const Mp4& mp4, std::string* why) override {
        const Mp4Track& t = mp4.track();
        nal_length_size_ = t.nal_length_size;
        // avcC: version, profile, compatibility, level, lengthSizeMinusOne, then the parameter sets.
        const std::vector<uint8_t>& c = t.config;
        if (c.size() < 7) {
            *why = "damaged avcC";
            return false;
        }
        size_t pos = 5;
        const int sps_count = c[pos++] & 31;
        for (int i = 0; i < sps_count; i++) {
            if (pos + 2 > c.size())
                break;
            const size_t n = (size_t(c[pos]) << 8) | c[pos + 1];
            pos += 2;
            if (pos + n > c.size())
                break;
            if (!AddSps(c.data() + pos, n, why))
                return false;
            pos += n;
        }
        const int pps_count = pos < c.size() ? c[pos++] : 0;
        for (int i = 0; i < pps_count; i++) {
            if (pos + 2 > c.size())
                break;
            const size_t n = (size_t(c[pos]) << 8) | c[pos + 1];
            pos += 2;
            if (pos + n > c.size())
                break;
            if (!AddPps(c.data() + pos, n, why))
                return false;
            pos += n;
        }
        // Scan the samples: in-band parameter sets, and the IDR pictures (start points).
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
                if (n == 0 || offset + n > end) {
                    *why = "damaged H.264 NAL unit length";
                    return false;
                }
                uint8_t header = 0;
                mp4.ReadRange(offset, &header, 1);
                const int type = header & 31;
                if (type == 5)
                    idr = true;
                if (type == 2 || type == 3 || type == 4) {
                    *why = "H.264 data partitioning is not supported";
                    return false;
                }
                if (type == 7 || type == 8) {
                    head.resize(size_t(n));
                    mp4.ReadRange(offset, head.data(), size_t(n));
                    if (!(type == 7 ? AddSps(head.data(), head.size(), why) : AddPps(head.data(), head.size(), why)))
                        return false;
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
        if (sps_.empty() || pps_.empty()) {
            *why = "no H.264 parameter sets";
            return false;
        }
        // One resolution and one DPB size for the session.
        const Sps& first = sps_.begin()->second;
        uint32_t max_refs = 0;
        for (const auto& [id, sps] : sps_) {
            if (sps.width_mbs != first.width_mbs || sps.height_map_units != first.height_map_units ||
                sps.crop_left != first.crop_left || sps.crop_right != first.crop_right || sps.crop_top != first.crop_top ||
                sps.crop_bottom != first.crop_bottom) {
                *why = "H.264 sequences of different sizes in one stream are not supported";
                return false;
            }
            max_refs = std::max<uint32_t>(max_refs, sps.max_num_ref_frames);
        }
        format_.coded_width = first.width_mbs * 16;
        format_.coded_height = first.height_map_units * 16;
        format_.crop_x = first.crop_left * 2;
        format_.crop_y = first.crop_top * 2;
        format_.width = format_.coded_width - (first.crop_left + first.crop_right) * 2;
        format_.height = format_.coded_height - (first.crop_top + first.crop_bottom) * 2;
        format_.bit_depth = 8;
        format_.dpb_slots = std::max<uint32_t>(max_refs, 1) + 1;
        format_.has_color = first.has_color;
        format_.color_primaries = first.color_primaries;
        format_.transfer = first.transfer;
        format_.matrix = first.matrix;
        format_.full_range = first.full_range;
        Reset();
        return true;
    }

    nri::VideoCodec nri_codec() const override { return nri::VideoCodec::H264; }

    void Reset() override {
        refs_.clear();
        prev_ref_frame_num_ = 0;
        prev_frame_num_ = 0;
        prev_frame_num_offset_ = 0;
        prev_poc_msb_ = prev_poc_lsb_ = 0;
        max_long_term_frame_idx_ = -1;
    }

    void FillSessionParameters(nri::VideoSessionParametersDesc* desc) override {
        nri_sps_.clear();
        nri_pps_.clear();
        for (const auto& [id, s] : sps_) {
            nri::VideoH264SequenceParameterSetDesc d = {};
            using B = nri::VideoH264SequenceParameterSetBits;
            B flags = B::NONE;
            const bool cs[6] = {bool(s.constraint_flags & 0x80), bool(s.constraint_flags & 0x40), bool(s.constraint_flags & 0x20),
                                bool(s.constraint_flags & 0x10), bool(s.constraint_flags & 0x08), bool(s.constraint_flags & 0x04)};
            const B cs_bits[6] = {B::CONSTRAINT_SET0, B::CONSTRAINT_SET1, B::CONSTRAINT_SET2,
                                  B::CONSTRAINT_SET3, B::CONSTRAINT_SET4, B::CONSTRAINT_SET5};
            for (int i = 0; i < 6; i++)
                if (cs[i])
                    flags |= cs_bits[i];
            if (s.direct_8x8_inference)
                flags |= B::DIRECT_8X8_INFERENCE;
            if (s.mb_adaptive_frame_field)
                flags |= B::MB_ADAPTIVE_FRAME_FIELD;
            if (s.frame_mbs_only)
                flags |= B::FRAME_MBS_ONLY;
            if (s.delta_pic_order_always_zero)
                flags |= B::DELTA_PIC_ORDER_ALWAYS_ZERO;
            if (s.separate_colour_plane)
                flags |= B::SEPARATE_COLOUR_PLANE;
            if (s.gaps_in_frame_num_allowed)
                flags |= B::GAPS_IN_FRAME_NUM_ALLOWED;
            if (s.qpprime_y_zero_transform_bypass)
                flags |= B::QPPRIME_Y_ZERO_TRANSFORM_BYPASS;
            if (s.frame_cropping)
                flags |= B::FRAME_CROPPING;
            d.flags = flags;
            d.profileIdc = 100;  // NRI's decode session is High profile; Baseline and Main streams decode as High
            d.levelIdc = s.level_idc;
            d.chromaFormatIdc = s.chroma_format_idc;
            d.sequenceParameterSetId = s.id;
            d.bitDepthLumaMinus8 = uint8_t(s.bit_depth_luma - 8);
            d.bitDepthChromaMinus8 = uint8_t(s.bit_depth_chroma - 8);
            d.log2MaxFrameNumMinus4 = uint8_t(s.log2_max_frame_num - 4);
            d.pictureOrderCountType = s.poc_type;
            d.offsetForNonReferencePicture = s.offset_for_non_ref_pic;
            d.offsetForTopToBottomField = s.offset_for_top_to_bottom_field;
            d.log2MaxPictureOrderCountLsbMinus4 = uint8_t(s.log2_max_poc_lsb - 4);
            d.referenceFrameNum = s.max_num_ref_frames;
            d.pictureWidthInMbsMinus1 = uint16_t(s.width_mbs - 1);
            d.pictureHeightInMapUnitsMinus1 = uint16_t(s.height_map_units - 1);
            d.frameCropLeftOffset = uint16_t(s.crop_left);
            d.frameCropRightOffset = uint16_t(s.crop_right);
            d.frameCropTopOffset = uint16_t(s.crop_top);
            d.frameCropBottomOffset = uint16_t(s.crop_bottom);
            nri_sps_.push_back(d);
        }
        for (const auto& [id, p] : pps_) {
            nri::VideoH264PictureParameterSetDesc d = {};
            using B = nri::VideoH264PictureParameterSetBits;
            B flags = B::NONE;
            if (p.transform_8x8_mode)
                flags |= B::TRANSFORM_8X8_MODE;
            if (p.redundant_pic_cnt_present)
                flags |= B::REDUNDANT_PIC_CNT_PRESENT;
            if (p.constrained_intra_pred)
                flags |= B::CONSTRAINED_INTRA_PRED;
            if (p.deblocking_filter_control_present)
                flags |= B::DEBLOCKING_FILTER_CONTROL_PRESENT;
            if (p.weighted_pred)
                flags |= B::WEIGHTED_PRED;
            if (p.bottom_field_pic_order_in_frame_present)
                flags |= B::BOTTOM_FIELD_PIC_ORDER_IN_FRAME;
            if (p.entropy_coding_mode)
                flags |= B::ENTROPY_CODING_MODE;
            d.flags = flags;
            d.sequenceParameterSetId = p.sps_id;
            d.pictureParameterSetId = p.id;
            d.refIndexL0DefaultActiveMinus1 = p.num_ref_idx_l0_default_minus1;
            d.refIndexL1DefaultActiveMinus1 = p.num_ref_idx_l1_default_minus1;
            d.weightedBipredIdc = p.weighted_bipred_idc;
            d.pictureInitQpMinus26 = p.pic_init_qp_minus26;
            d.pictureInitQsMinus26 = p.pic_init_qs_minus26;
            d.chromaQpIndexOffset = p.chroma_qp_index_offset;
            d.secondChromaQpIndexOffset = p.second_chroma_qp_index_offset;
            nri_pps_.push_back(d);
        }
        nri_parameters_ = {};
        nri_parameters_.sequenceParameterSets = nri_sps_.data();
        nri_parameters_.sequenceParameterSetNum = uint32_t(nri_sps_.size());
        nri_parameters_.pictureParameterSets = nri_pps_.data();
        nri_parameters_.pictureParameterSetNum = uint32_t(nri_pps_.size());
        *desc = {};
        desc->h264Parameters = &nri_parameters_;
    }

    bool ParseSample(uint32_t sample, const std::vector<uint8_t>& bytes, std::vector<PictureJob>* jobs,
                     std::string* why) override {
        (void)sample;
        jobs->clear();
        // NAL units of the access unit.
        std::vector<NalRef> slices;
        size_t pos = 0;
        while (pos + nal_length_size_ <= bytes.size()) {
            uint64_t n = 0;
            for (uint32_t b = 0; b < nal_length_size_; b++)
                n = (n << 8) | bytes[pos + b];
            pos += nal_length_size_;
            if (n == 0 || pos + n > bytes.size()) {
                *why = "damaged H.264 NAL unit";
                return false;
            }
            NalRef nal;
            nal.ref_idc = (bytes[pos] >> 5) & 3;
            nal.type = bytes[pos] & 31;
            nal.data = bytes.data() + pos;
            nal.size = size_t(n);
            pos += size_t(n);
            if (nal.type == 1 || nal.type == 5)
                slices.push_back(nal);
            else if (nal.type == 7 || nal.type == 8) {
                // In-band parameter sets were collected when the stream was scanned; one that differs is an error.
                std::string ignored;
                if (nal.type == 7) {
                    Sps s;
                    if (ParseSps(nal.data, nal.size, &s, &ignored) && sps_.count(s.id) && sps_[s.id].bytes != s.bytes) {
                        *why = "an H.264 SPS changes inside the stream";
                        return false;
                    }
                }
            }
        }
        if (slices.empty()) {
            *why = "no H.264 slice in the sample";
            return false;
        }

        // The first slice's header gives the picture's parameters.
        const NalRef& first = slices[0];
        const std::vector<uint8_t> rbsp = Unescape(first.data + 1, std::min<size_t>(first.size - 1, 2048));
        BitReader r(rbsp.data(), rbsp.size());
        r.Ue();  // first_mb_in_slice
        const uint32_t slice_type_raw = r.Ue();
        const uint32_t slice_type = slice_type_raw % 5;
        const uint32_t pps_id = r.Ue();
        auto pps_it = pps_.find(uint8_t(pps_id));
        if (pps_it == pps_.end()) {
            *why = "an H.264 slice names a missing PPS";
            return false;
        }
        const Pps& pps = pps_it->second;
        auto sps_it = sps_.find(pps.sps_id);
        if (sps_it == sps_.end()) {
            *why = "an H.264 PPS names a missing SPS";
            return false;
        }
        const Sps& sps = sps_it->second;
        const bool idr = first.type == 5;
        if (sps.separate_colour_plane)
            r.Bits(2);
        const uint32_t max_frame_num = 1u << sps.log2_max_frame_num;
        const uint32_t frame_num = r.Bits(sps.log2_max_frame_num);
        uint32_t idr_pic_id = 0;
        if (idr)
            idr_pic_id = r.Ue();
        uint32_t poc_lsb = 0;
        int32_t delta_poc_bottom = 0;
        if (sps.poc_type == 0) {
            poc_lsb = r.Bits(sps.log2_max_poc_lsb);
            if (pps.bottom_field_pic_order_in_frame_present)
                delta_poc_bottom = r.Se();
        }
        uint32_t num_ref_idx_l0 = pps.num_ref_idx_l0_default_minus1 + 1u, num_ref_idx_l1 = pps.num_ref_idx_l1_default_minus1 + 1u;
        if (slice_type == 1)
            r.Bit();  // direct_spatial_mv_pred_flag
        if (slice_type == 0 || slice_type == 3 || slice_type == 1) {
            if (r.Flag()) {
                num_ref_idx_l0 = r.Ue() + 1;
                if (slice_type == 1)
                    num_ref_idx_l1 = r.Ue() + 1;
            }
        }
        // ref_pic_list_modification
        auto skip_modification = [&] {
            if (r.Flag()) {
                uint32_t idc;
                do {
                    idc = r.Ue();
                    if (idc == 0 || idc == 1 || idc == 2)
                        r.Ue();
                } while (idc != 3 && !r.overrun());
            }
        };
        if (slice_type != 2 && slice_type != 4) {
            skip_modification();
            if (slice_type == 1)
                skip_modification();
        }
        if ((pps.weighted_pred && (slice_type == 0 || slice_type == 3)) || (pps.weighted_bipred_idc == 1 && slice_type == 1)) {
            r.Ue();  // luma_log2_weight_denom
            r.Ue();  // chroma_log2_weight_denom (ChromaArrayType != 0 for 4:2:0)
            auto weights = [&](uint32_t count) {
                for (uint32_t i = 0; i < count && !r.overrun(); i++) {
                    if (r.Flag()) {
                        r.Se();
                        r.Se();
                    }
                    if (r.Flag())
                        for (int j = 0; j < 2; j++) {
                            r.Se();
                            r.Se();
                        }
                }
            };
            weights(num_ref_idx_l0);
            if (slice_type == 1)
                weights(num_ref_idx_l1);
        }
        bool long_term_reference_flag = false, adaptive = false;
        struct Mmco {
            uint32_t op, a, b;
        };
        std::vector<Mmco> mmcos;
        if (first.ref_idc != 0) {
            if (idr) {
                r.Bit();  // no_output_of_prior_pics_flag
                long_term_reference_flag = r.Flag();
            } else {
                adaptive = r.Flag();
                if (adaptive) {
                    while (!r.overrun()) {
                        Mmco m = {r.Ue(), 0, 0};
                        if (m.op == 0)
                            break;
                        if (m.op == 1 || m.op == 3)
                            m.a = r.Ue();  // difference_of_pic_nums_minus1
                        if (m.op == 2)
                            m.a = r.Ue();  // long_term_pic_num
                        if (m.op == 3 || m.op == 6)
                            m.b = r.Ue();  // long_term_frame_idx
                        if (m.op == 4)
                            m.a = r.Ue();  // max_long_term_frame_idx_plus1
                        mmcos.push_back(m);
                        if (mmcos.size() > 66)
                            break;
                    }
                }
            }
        }
        if (r.overrun()) {
            *why = "damaged H.264 slice header";
            return false;
        }
        bool all_intra = true;
        for (const NalRef& s : slices) {
            const std::vector<uint8_t> sr = Unescape(s.data + 1, std::min<size_t>(s.size - 1, 32));
            BitReader br(sr.data(), sr.size());
            br.Ue();
            const uint32_t t = br.Ue() % 5;
            if (t != 2 && t != 4)
                all_intra = false;
        }

        // frame_num continuity (no gaps).
        if (idr) {
            // IDR: frame_num is 0
        } else if (frame_num != prev_ref_frame_num_ && frame_num != (prev_ref_frame_num_ + 1) % max_frame_num) {
            if (!sps.gaps_in_frame_num_allowed) {
                *why = "H.264 frame_num gap (decoding started at a frame that is not an IDR, or the stream lost frames)";
                return false;
            }
            *why = "H.264 frame_num gaps (non-existing frames) are not supported";
            return false;
        }

        // 8.2.1 picture order count
        int32_t top = 0, bottom = 0;
        if (sps.poc_type == 0) {
            const int32_t max_lsb = 1 << sps.log2_max_poc_lsb;
            int32_t prev_msb = prev_poc_msb_, prev_lsb = prev_poc_lsb_;
            if (idr)
                prev_msb = prev_lsb = 0;
            int32_t msb;
            if (int32_t(poc_lsb) < prev_lsb && (prev_lsb - int32_t(poc_lsb)) >= max_lsb / 2)
                msb = prev_msb + max_lsb;
            else if (int32_t(poc_lsb) > prev_lsb && (int32_t(poc_lsb) - prev_lsb) > max_lsb / 2)
                msb = prev_msb - max_lsb;
            else
                msb = prev_msb;
            top = msb + int32_t(poc_lsb);
            bottom = top + delta_poc_bottom;
            poc_msb_ = msb;
        } else {
            int32_t frame_num_offset;
            if (idr)
                frame_num_offset = 0;
            else if (prev_frame_num_ > frame_num)
                frame_num_offset = prev_frame_num_offset_ + int32_t(max_frame_num);
            else
                frame_num_offset = prev_frame_num_offset_;
            int32_t temp;
            if (idr)
                temp = 0;
            else if (first.ref_idc == 0)
                temp = 2 * (frame_num_offset + int32_t(frame_num)) - 1;
            else
                temp = 2 * (frame_num_offset + int32_t(frame_num));
            top = bottom = temp;
            frame_num_offset_ = frame_num_offset;
        }

        // The pictures kept for reference, as the decode command binds them.
        if (idr)
            refs_.clear();
        for (Frame& f : refs_)
            f.frame_num_wrap = f.long_term_frame_idx >= 0 ? 0 : (f.frame_num > frame_num ? int(f.frame_num) - int(max_frame_num) : int(f.frame_num));

        PictureJob job;
        H264Job h264;
        h264.sps_id = sps.id;
        h264.pps_id = pps.id;
        h264.frame_num = uint16_t(frame_num);
        h264.idr_pic_id = uint16_t(idr_pic_id);
        h264.poc[0] = top;
        h264.poc[1] = bottom;
        h264.idr = idr;
        h264.intra = all_intra;
        h264.reference = first.ref_idc != 0;
        for (const Frame& f : refs_) {
            H264Ref ref;
            ref.slot = f.slot;
            ref.frame_num = uint16_t(f.long_term_frame_idx >= 0 ? f.long_term_frame_idx : f.frame_num);
            ref.poc[0] = f.poc[0];
            ref.poc[1] = f.poc[1];
            ref.long_term = f.long_term_frame_idx >= 0;
            h264.refs.push_back(ref);
            job.bound_slots.push_back(f.slot);
        }
        // A free slot: one no reference frame uses.
        int slot = -1;
        for (uint32_t i = 0; i < format_.dpb_slots && slot < 0; i++) {
            bool used = false;
            for (const Frame& f : refs_)
                used = used || f.slot == int(i);
            if (!used)
                slot = int(i);
        }
        if (slot < 0) {
            *why = "H.264 reference frames exceed the DPB";
            return false;
        }
        job.setup_slot = slot;
        job.shown = true;
        job.show_slot = slot;
        // The bitstream: each slice as Annex-B.
        for (const NalRef& s : slices) {
            h264.slice_offsets.push_back(uint32_t(job.bitstream.size()));
            const uint8_t start_code[4] = {0, 0, 0, 1};
            job.bitstream.insert(job.bitstream.end(), start_code, start_code + 4);
            job.bitstream.insert(job.bitstream.end(), s.data, s.data + s.size);
        }
        job.codec = std::move(h264);

        // 8.2.5 marking, after the decode.
        if (first.ref_idc != 0) {
            Frame cur;
            cur.slot = slot;
            cur.frame_num = uint16_t(frame_num);
            cur.poc[0] = top;
            cur.poc[1] = bottom;
            bool mmco5 = false;
            if (idr) {
                refs_.clear();
                if (long_term_reference_flag) {
                    cur.long_term_frame_idx = 0;
                    max_long_term_frame_idx_ = 0;
                } else {
                    max_long_term_frame_idx_ = -1;
                }
            } else if (!adaptive) {
                // Sliding window.
                if (int(refs_.size()) >= std::max<int>(sps.max_num_ref_frames, 1)) {
                    int victim = -1;
                    for (size_t i = 0; i < refs_.size(); i++)
                        if (refs_[i].long_term_frame_idx < 0 && (victim < 0 || refs_[i].frame_num_wrap < refs_[victim].frame_num_wrap))
                            victim = int(i);
                    if (victim >= 0)
                        refs_.erase(refs_.begin() + victim);
                }
            } else {
                for (const Mmco& m : mmcos) {
                    auto find_short = [&](int pic_num) {
                        for (size_t i = 0; i < refs_.size(); i++)
                            if (refs_[i].long_term_frame_idx < 0 && refs_[i].frame_num_wrap == pic_num)
                                return int(i);
                        return -1;
                    };
                    auto find_long_idx = [&](int idx) {
                        for (size_t i = 0; i < refs_.size(); i++)
                            if (refs_[i].long_term_frame_idx == idx)
                                return int(i);
                        return -1;
                    };
                    const int curr_pic_num = int(frame_num);
                    if (m.op == 1) {
                        const int i = find_short(curr_pic_num - int(m.a + 1));
                        if (i >= 0)
                            refs_.erase(refs_.begin() + i);
                    } else if (m.op == 2) {
                        const int i = find_long_idx(int(m.a));
                        if (i >= 0)
                            refs_.erase(refs_.begin() + i);
                    } else if (m.op == 3) {
                        const int i = find_short(curr_pic_num - int(m.a + 1));
                        const int existing = find_long_idx(int(m.b));
                        if (i >= 0) {
                            if (existing >= 0 && existing != i) {
                                refs_.erase(refs_.begin() + existing);
                                const int j = find_short(curr_pic_num - int(m.a + 1));
                                if (j >= 0)
                                    refs_[j].long_term_frame_idx = int(m.b);
                            } else {
                                refs_[i].long_term_frame_idx = int(m.b);
                            }
                        }
                    } else if (m.op == 4) {
                        max_long_term_frame_idx_ = int(m.a) - 1;
                        for (size_t i = refs_.size(); i-- > 0;)
                            if (refs_[i].long_term_frame_idx > max_long_term_frame_idx_)
                                refs_.erase(refs_.begin() + i);
                    } else if (m.op == 5) {
                        refs_.clear();
                        max_long_term_frame_idx_ = -1;
                        mmco5 = true;
                    } else if (m.op == 6) {
                        const int existing = find_long_idx(int(m.b));
                        if (existing >= 0)
                            refs_.erase(refs_.begin() + existing);
                        cur.long_term_frame_idx = int(m.b);
                    }
                }
            }
            if (mmco5) {
                // The picture is treated as having frame_num 0 and a POC rebased to its own.
                const int32_t temp = std::min(top, bottom);
                cur.poc[0] = top - temp;
                cur.poc[1] = bottom - temp;
                cur.frame_num = 0;
            }
            refs_.push_back(cur);
            prev_ref_frame_num_ = mmco5 ? 0 : frame_num;
            if (sps.poc_type == 0) {
                if (mmco5) {
                    prev_poc_msb_ = 0;
                    prev_poc_lsb_ = cur.poc[0];
                } else {
                    prev_poc_msb_ = poc_msb_;
                    prev_poc_lsb_ = int32_t(poc_lsb);
                }
            }
        }
        prev_frame_num_ = frame_num;
        prev_frame_num_offset_ = sps.poc_type == 2 ? frame_num_offset_ : 0;
        jobs->push_back(std::move(job));
        return true;
    }

private:
    bool AddSps(const uint8_t* nal, size_t size, std::string* why) {
        Sps s;
        if (!ParseSps(nal, size, &s, why))
            return false;
        auto it = sps_.find(s.id);
        if (it != sps_.end()) {
            if (it->second.bytes == s.bytes)
                return true;
            *why = "an H.264 SPS id is used with different parameters in one stream";
            return false;
        }
        if (s.profile_idc != 66 && s.profile_idc != 77 && s.profile_idc != 88 && s.profile_idc != 100) {
            if (s.bit_depth_luma != 8 || s.bit_depth_chroma != 8) {
                *why = "H.264 above 8 bits is not supported (NRI's decode sessions are High profile)";
                return false;
            }
            if (s.chroma_format_idc != 1) {
                *why = "H.264 other than 4:2:0 is not supported";
                return false;
            }
            *why = "H.264 profile " + std::to_string(s.profile_idc) + " is not supported (Baseline, Main, Extended and High are)";
            return false;
        }
        if (s.chroma_format_idc != 1) {
            *why = "H.264 other than 4:2:0 is not supported";
            return false;
        }
        if (!s.frame_mbs_only) {
            *why = "interlaced H.264 is not supported";
            return false;
        }
        sps_[s.id] = std::move(s);
        return true;
    }
    bool AddPps(const uint8_t* nal, size_t size, std::string* why) {
        Pps p;
        if (!ParsePps(nal, size, &p, why))
            return false;
        auto it = pps_.find(p.id);
        if (it != pps_.end()) {
            if (it->second.bytes == p.bytes)
                return true;
            *why = "an H.264 PPS id is used with different parameters in one stream";
            return false;
        }
        pps_[p.id] = std::move(p);
        return true;
    }

    uint32_t nal_length_size_ = 4;
    std::map<uint8_t, Sps> sps_;
    std::map<uint8_t, Pps> pps_;
    std::vector<Frame> refs_;
    uint32_t prev_ref_frame_num_ = 0, prev_frame_num_ = 0;
    int32_t prev_frame_num_offset_ = 0, frame_num_offset_ = 0;
    int32_t prev_poc_msb_ = 0, prev_poc_lsb_ = 0, poc_msb_ = 0;
    int max_long_term_frame_idx_ = -1;
    std::vector<nri::VideoH264SequenceParameterSetDesc> nri_sps_;
    std::vector<nri::VideoH264PictureParameterSetDesc> nri_pps_;
    nri::VideoH264SessionParametersDesc nri_parameters_ = {};
};

}  // namespace

std::unique_ptr<CodecBackend> MakeH264Backend() { return std::make_unique<H264Backend>(); }

}  // namespace renderer::clips::video
