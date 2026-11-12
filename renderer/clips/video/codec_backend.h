// The codec side of the GPU decoder, apart from NRI's session and queues: what a stream needs (coded size, DPB slots),
// and, sample by sample, the pictures to decode in the form NRI's decode command takes them, with the reference
// pictures as DPB slot numbers. Each backend owns its codec's parameter sets and its reference picture management
// (AV1's eight reference slots, H.264's marking process, H.265's reference picture sets).

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <NRI.h>
#include <Extensions/NRIVideo.h>
#include "renderer/clips/video/av1.h"
#include "renderer/clips/video/mp4.h"

namespace renderer::clips::video {

// What the session and the DPB pictures are created from.
struct StreamFormat {
    uint32_t coded_width = 0, coded_height = 0;      // the decoded picture's size (macroblock or coding block aligned)
    uint32_t crop_x = 0, crop_y = 0;                 // the displayed rectangle inside it
    uint32_t width = 0, height = 0;                  // its size
    uint32_t bit_depth = 8;
    uint32_t dpb_slots = 0;                          // pictures kept for reference, with one for the picture being decoded
    bool has_color = false;
    uint8_t color_primaries = 2, transfer = 2, matrix = 2;
    bool full_range = false;
};

struct Av1Job {
    av1::Picture pic;
    std::array<int8_t, av1::kNumRefFrames> slot_of_ref{};  // DPB slot of each reference map entry (-1: empty)
    std::vector<uint32_t> tile_offsets, tile_sizes;         // in the picture's bitstream
};

struct H264Ref {
    int slot = 0;
    uint16_t frame_num = 0;  // FrameNum, or LongTermFrameIdx for a long-term reference
    int32_t poc[2] = {0, 0};
    bool long_term = false;
};
struct H264Job {
    uint8_t sps_id = 0, pps_id = 0;
    uint16_t frame_num = 0, idr_pic_id = 0;
    int32_t poc[2] = {0, 0};
    bool idr = false, intra = false, reference = false;
    std::vector<uint32_t> slice_offsets;  // of the 4-byte start codes in the picture's bitstream
    std::vector<H264Ref> refs;
};

struct H265Ref {
    int slot = 0;
    int32_t poc = 0;
    bool long_term = false;
};
struct H265Job {
    uint8_t vps_id = 0, sps_id = 0, pps_id = 0;
    int32_t poc = 0;
    bool irap = false, idr = false, reference = false, short_term_ref_pic_set_sps = false;
    uint8_t num_delta_pocs_of_ref_rps_idx = 0;
    uint16_t num_bits_for_st_ref_pic_set_in_slice = 0;
    std::vector<uint32_t> slice_offsets;  // of the 4-byte start codes in the picture's bitstream
    std::vector<H265Ref> refs;            // the references the picture uses (RefPicSetStCurrBefore/After, LtCurr)
};

// One picture to decode (or, for AV1's show_existing_frame, to show again).
struct PictureJob {
    bool decode = true;
    int setup_slot = -1;              // DPB slot the picture is written to
    std::vector<int> bound_slots;     // DPB slots bound as references (distinct)
    bool shown = false;               // the picture is displayed once decoded
    int show_slot = -1;               // DPB slot of the displayed picture
    std::vector<uint8_t> bitstream;   // the bytes the decode command reads
    std::variant<std::monostate, Av1Job, H264Job, H265Job> codec;
};

class CodecBackend {
public:
    virtual ~CodecBackend() = default;
    // Reads the codec configuration and scans the samples; false with a reason when the stream is refused.
    virtual bool Init(const Mp4& mp4, std::string* why) = 0;
    const StreamFormat& format() const { return format_; }
    // Samples at which decoding can start with every reference empty, ascending (the first sample at least).
    const std::vector<uint32_t>& start_samples() const { return start_samples_; }
    // Forget all reference pictures (decoding starts at a start sample).
    virtual void Reset() = 0;
    virtual bool ParseSample(uint32_t sample, const std::vector<uint8_t>& bytes, std::vector<PictureJob>* jobs,
                             std::string* why) = 0;
    virtual nri::VideoCodec nri_codec() const = 0;
    // The parameter sets for the session parameters object; the pointers stay valid as long as the backend.
    virtual void FillSessionParameters(nri::VideoSessionParametersDesc* desc) = 0;

protected:
    StreamFormat format_;
    std::vector<uint32_t> start_samples_;
};

std::unique_ptr<CodecBackend> MakeAv1Backend();
std::unique_ptr<CodecBackend> MakeH264Backend();
std::unique_ptr<CodecBackend> MakeH265Backend();

}  // namespace renderer::clips::video
