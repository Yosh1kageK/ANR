// ISO base media file (MP4, MOV) demuxer for one video track: sample table, display order and codec configuration.
// It reads the sample table (moov) and nothing from mdat until ReadSample. Supported: AV1 (av01 + av1C), H.264 (avc1
// or avc3 + avcC) and H.265 (hvc1 or hev1 + hvcC) with a constant frame rate, an edit list with one media segment, and
// 32 or 64 bit chunk offsets. Fragmented files (moof) are not supported.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace renderer::clips::video {

enum class Codec : uint8_t { AV1, H264, H265 };

inline const char* CodecName(Codec codec) {
    return codec == Codec::AV1 ? "av1" : codec == Codec::H264 ? "h264" : "hevc";
}

// A file that supports reads at an offset from several threads at once.
class FileReader {
public:
    FileReader() = default;
    ~FileReader();
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    bool Open(const std::string& path, std::string* why);
    bool ReadAt(uint64_t offset, void* data, size_t size) const;
    uint64_t size() const { return size_; }

private:
    intptr_t handle_ = -1;
    uint64_t size_ = 0;
};

struct Sample {
    uint64_t offset = 0;
    uint32_t size = 0;
    int64_t dts = 0;  // in media timescale units
    int64_t pts = 0;
    bool sync = false;  // the container's sync flag (stss); the codecs' parsers decide what a start point is
};

struct Mp4Track {
    Codec codec = Codec::AV1;
    uint32_t width = 0, height = 0;  // the sample entry's coded size
    uint32_t timescale = 0;
    std::vector<uint8_t> config;     // av1C, avcC or hvcC payload
    uint32_t nal_length_size = 4;    // H.264 and H.265: bytes of each NAL unit's length prefix
    bool has_colr = false;           // colr (nclx)
    uint8_t color_primaries = 2, transfer = 2, matrix = 2;
    bool full_range = false;
    std::vector<Sample> samples;     // decode order
    // Display order, after the edit list: display_to_sample[d] is the sample shown as frame d, sample_to_display[s]
    // the frame index of sample s (-1: not shown).
    std::vector<uint32_t> display_to_sample;
    std::vector<int64_t> sample_to_display;
    double fps = 0;
    int64_t first_pts = 0;           // of display frame 0, in timescale units
    int64_t pts_step = 0;            // between display frames
};

class Mp4 {
public:
    // Null with a reason when the file is not an MP4 of a supported video track.
    static std::unique_ptr<Mp4> Open(const std::string& path, std::string* why);
    const Mp4Track& track() const { return track_; }
    bool ReadSample(uint32_t index, std::vector<uint8_t>* out) const;
    bool ReadRange(uint64_t offset, void* data, size_t size) const { return file_.ReadAt(offset, data, size); }

private:
    FileReader file_;
    Mp4Track track_;
};

}  // namespace renderer::clips::video
