// AV1: reference slot management on top of the av1 parser. The reference map has eight entries, each naming a DPB
// picture; a picture stays in the DPB while an entry names it. Nine DPB slots cover eight distinct references and the
// picture being decoded.

#include <algorithm>
#include <cstring>

#include "renderer/clips/video/codec_backend.h"

namespace renderer::clips::video {

namespace {

constexpr int kSlots = av1::kNumRefFrames + 1;

class Av1Backend final : public CodecBackend {
public:
    bool Init(const Mp4& mp4, std::string* why) override {
        const Mp4Track& t = mp4.track();
        if (!parser_.Configure(t.config, why))
            return false;
        // The sequence header: the first sample's, which can differ from the av1C record's (SVT-AV1 writes a record
        // with other tool flags); the decoder session is configured with the one the frames were coded with.
        std::vector<uint8_t> bytes;
        if (!mp4.ReadSample(0, &bytes)) {
            *why = "cannot read the first sample";
            return false;
        }
        std::vector<av1::Picture> pictures;
        if (!parser_.ParseTemporalUnit(bytes.data(), bytes.size(), &pictures, why))
            return false;
        if (!parser_.has_sequence()) {
            *why = "no AV1 sequence header";
            return false;
        }
        parser_.LockSequence();
        const av1::SequenceHeader& s = parser_.sequence();
        format_.coded_width = format_.width = s.width();
        format_.coded_height = format_.height = s.height();
        format_.bit_depth = s.bit_depth;
        format_.dpb_slots = kSlots;
        format_.has_color = s.color_description_present;
        format_.color_primaries = s.color_primaries;
        format_.transfer = s.transfer_characteristics;
        format_.matrix = s.matrix_coefficients;
        format_.full_range = s.color_range;
        // Start points: sync samples that begin with a shown key frame.
        for (uint32_t i = 0; i < t.samples.size(); i++) {
            if (!t.samples[i].sync)
                continue;
            uint8_t head[160];
            const size_t n = std::min<size_t>(sizeof(head), t.samples[i].size);
            if (mp4.ReadRange(t.samples[i].offset, head, n) && av1::StartsWithKeyFrame(head, n))
                start_samples_.push_back(i);
        }
        if (start_samples_.empty() || start_samples_.front() != 0) {
            *why = "the stream does not start with a key frame";
            return false;
        }
        Reset();
        sequence_ = s;
        return true;
    }

    void Reset() override {
        parser_.Reset();
        for (int8_t& s : refmap_)
            s = -1;
        for (int& c : refcount_)
            c = 0;
    }

    nri::VideoCodec nri_codec() const override { return nri::VideoCodec::AV1; }

    void FillSessionParameters(nri::VideoSessionParametersDesc* desc) override {
        const av1::SequenceHeader& s = sequence_;
        nri::VideoAV1SequenceDesc& d = av1_parameters_.sequence;
        d = {};
        using Bits = nri::VideoAV1SequenceBits;
        Bits flags = Bits::NONE;
        auto set = [&](bool cond, Bits bit) {
            if (cond)
                flags |= bit;
        };
        set(s.still_picture, Bits::STILL_PICTURE);
        set(s.reduced_still_picture_header, Bits::REDUCED_STILL_PICTURE_HEADER);
        set(s.use_128x128_superblock, Bits::USE_128X128_SUPERBLOCK);
        set(s.enable_filter_intra, Bits::ENABLE_FILTER_INTRA);
        set(s.enable_intra_edge_filter, Bits::ENABLE_INTRA_EDGE_FILTER);
        set(s.enable_interintra_compound, Bits::ENABLE_INTERINTRA_COMPOUND);
        set(s.enable_masked_compound, Bits::ENABLE_MASKED_COMPOUND);
        set(s.enable_warped_motion, Bits::ENABLE_WARPED_MOTION);
        set(s.enable_dual_filter, Bits::ENABLE_DUAL_FILTER);
        set(s.enable_order_hint, Bits::ENABLE_ORDER_HINT);
        set(s.enable_jnt_comp, Bits::ENABLE_JNT_COMP);
        set(s.enable_ref_frame_mvs, Bits::ENABLE_REF_FRAME_MVS);
        set(s.frame_id_numbers_present, Bits::FRAME_ID_NUMBERS_PRESENT);
        set(s.enable_superres, Bits::ENABLE_SUPERRES);
        set(s.enable_cdef, Bits::ENABLE_CDEF);
        set(s.enable_restoration, Bits::ENABLE_RESTORATION);
        set(s.film_grain_params_present, Bits::FILM_GRAIN_PARAMS_PRESENT);
        set(s.timing_info_present, Bits::TIMING_INFO_PRESENT);
        set(s.mono_chrome, Bits::MONO_CHROME);
        set(s.color_range, Bits::COLOR_RANGE);
        set(s.separate_uv_delta_q, Bits::SEPARATE_UV_DELTA_Q);
        set(s.color_description_present, Bits::COLOR_DESCRIPTION_PRESENT);
        d.flags = flags;
        d.seqProfile = s.seq_profile;
        d.bitDepth = s.bit_depth;
        d.subsamplingX = s.subsampling_x;
        d.subsamplingY = s.subsampling_y;
        d.maxFrameWidthMinus1 = uint16_t(s.max_frame_width_minus_1);
        d.maxFrameHeightMinus1 = uint16_t(s.max_frame_height_minus_1);
        d.frameWidthBitsMinus1 = s.frame_width_bits_minus_1;
        d.frameHeightBitsMinus1 = s.frame_height_bits_minus_1;
        d.deltaFrameIdLengthMinus2 = s.delta_frame_id_length_minus_2;
        d.additionalFrameIdLengthMinus1 = s.additional_frame_id_length_minus_1;
        d.orderHintBitsMinus1 = s.order_hint_bits ? uint8_t(s.order_hint_bits - 1) : 0;
        d.seqForceIntegerMv = s.seq_force_integer_mv;
        d.seqForceScreenContentTools = s.seq_force_screen_content_tools;
        d.level = s.seq_level_idx[0];
        d.colorPrimaries = s.color_primaries;
        d.transferCharacteristics = s.transfer_characteristics;
        d.matrixCoefficients = s.matrix_coefficients;
        d.chromaSamplePosition = s.chroma_sample_position;
        d.numUnitsInDisplayTick = s.num_units_in_display_tick;
        d.timeScale = s.time_scale;
        d.numTicksPerPictureMinus1 = s.num_ticks_per_picture_minus_1;
        *desc = {};
        desc->av1Parameters = &av1_parameters_;
    }

    bool ParseSample(uint32_t sample, const std::vector<uint8_t>& bytes, std::vector<PictureJob>* jobs,
                     std::string* why) override {
        (void)sample;
        jobs->clear();
        std::vector<av1::Picture> pictures;
        if (!parser_.ParseTemporalUnit(bytes.data(), bytes.size(), &pictures, why))
            return false;
        for (av1::Picture& p : pictures) {
            PictureJob job;
            if (p.h.show_existing_frame) {
                job.decode = false;
                job.shown = true;
                job.show_slot = refmap_[p.h.frame_to_show_map_idx];
                if (job.show_slot < 0) {
                    *why = "AV1 show_existing_frame of a picture that was never decoded";
                    return false;
                }
                if (p.h.frame_type == av1::kKeyFrame)
                    Refresh(0xff, job.show_slot);
                jobs->push_back(std::move(job));
                continue;
            }
            int slot = -1;
            for (int i = 0; i < kSlots; i++)
                if (refcount_[i] == 0) {
                    slot = i;
                    break;
                }
            if (slot < 0) {
                *why = "AV1 reference pictures exceed the DPB";
                return false;
            }
            Av1Job av1_job;
            for (int i = 0; i < av1::kNumRefFrames; i++) {
                av1_job.slot_of_ref[i] = refmap_[i];
                if (refmap_[i] >= 0 && std::find(job.bound_slots.begin(), job.bound_slots.end(), refmap_[i]) == job.bound_slots.end())
                    job.bound_slots.push_back(refmap_[i]);
            }
            // The tiles' bytes, one after the other.
            for (const av1::TileRange& tile : p.tiles) {
                if (size_t(tile.offset) + tile.size > bytes.size()) {
                    *why = "AV1 tile outside the sample";
                    return false;
                }
                av1_job.tile_offsets.push_back(uint32_t(job.bitstream.size()));
                av1_job.tile_sizes.push_back(tile.size);
                job.bitstream.insert(job.bitstream.end(), bytes.begin() + tile.offset, bytes.begin() + tile.offset + tile.size);
            }
            job.setup_slot = slot;
            job.shown = p.shown;
            job.show_slot = slot;
            const uint8_t refresh = p.h.refresh_frame_flags;
            av1_job.pic = std::move(p);
            job.codec = std::move(av1_job);
            Refresh(refresh, slot);
            jobs->push_back(std::move(job));
        }
        return true;
    }

private:
    void Refresh(uint8_t flags, int slot) {
        for (int i = 0; i < av1::kNumRefFrames; i++) {
            if (!((flags >> i) & 1))
                continue;
            if (refmap_[i] >= 0)
                refcount_[refmap_[i]]--;
            refmap_[i] = int8_t(slot);
            refcount_[slot]++;
        }
    }

    av1::Parser parser_;
    av1::SequenceHeader sequence_;
    int8_t refmap_[av1::kNumRefFrames] = {};
    int refcount_[kSlots] = {};
    nri::VideoAV1SessionParametersDesc av1_parameters_ = {};
};

}  // namespace

std::unique_ptr<CodecBackend> MakeAv1Backend() { return std::make_unique<Av1Backend>(); }

}  // namespace renderer::clips::video
