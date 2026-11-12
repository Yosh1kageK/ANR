// ffprobe's JSON for the sample recordings and the formats around them.
#include "renderer/clips/probe.h"

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

// ffprobe -print_format json -show_streams -show_format on cyberpunk-apex-gen-only.mp4 (trimmed): AV1, 8-bit, BT.709,
// 60 fps, 5490 frames, and an audio stream.
const char* SAMPLE = R"({
 "streams": [
  {"index": 0, "codec_name": "av1", "codec_type": "video", "width": 3840, "height": 2160, "pix_fmt": "yuv420p",
   "color_range": "tv", "color_space": "bt709", "color_transfer": "bt709", "r_frame_rate": "60/1",
   "avg_frame_rate": "60/1", "duration": "91.500000", "nb_frames": "5490", "disposition": {"attached_pic": 0}},
  {"index": 1, "codec_name": "aac", "codec_type": "audio", "sample_rate": "48000", "channels": 2}
 ],
 "format": {"duration": "91.521000", "nb_streams": 2}
})";

void sample_recording() {
    ProbeResult p;
    std::string error;
    CHECK(ParseProbe(SAMPLE, p, error));
    CHECK_EQ(p.info.width, 3840u);
    CHECK_EQ(p.info.height, 2160u);
    CHECK_NEAR(p.info.fps, 60.0, 1e-12);
    CHECK_EQ(p.info.frames, 5490);
    CHECK(p.info.codec == "av1");
    CHECK_EQ(p.info.bit_depth, 8u);
    CHECK(p.info.matrix == YuvMatrix::BT709);
    CHECK(p.info.range == YuvRange::LIMITED);
    CHECK(p.has_audio);
    CHECK(!p.variable_frame_rate);
    CHECK_NEAR(p.duration, 91.5, 1e-9);
    CHECK(p.pix_fmt == "yuv420p");
}

void other_formats() {
    ProbeResult p;
    std::string error;
    // NTSC rate, no frame count (counted from the duration), 10-bit, BT.2020, full range
    const char* hdr = R"({"streams": [{"codec_type": "video", "codec_name": "hevc", "width": 1920, "height": 1080,
      "pix_fmt": "yuv420p10le", "color_range": "pc", "color_space": "bt2020nc", "r_frame_rate": "60000/1001",
      "avg_frame_rate": "60000/1001", "duration": "10.0"}], "format": {"duration": "10.01"}})";
    CHECK(ParseProbe(hdr, p, error));
    CHECK_NEAR(p.info.fps, 59.94005994, 1e-6);
    CHECK_EQ(p.info.frames, 599);
    CHECK_EQ(p.info.bit_depth, 10u);
    CHECK(p.info.matrix == YuvMatrix::BT2020);
    CHECK(p.info.range == YuvRange::FULL);
    CHECK(!p.has_audio);
    // unspecified color: HD is 709, SD is 601; yuvj is full range
    const char* sd = R"({"streams": [{"codec_type": "video", "codec_name": "h264", "width": 640, "height": 480,
      "pix_fmt": "yuvj420p", "r_frame_rate": "30/1", "avg_frame_rate": "30/1", "nb_frames": "300"}],
      "format": {"duration": "10"}})";
    CHECK(ParseProbe(sd, p, error));
    CHECK(p.info.matrix == YuvMatrix::BT601);
    CHECK(p.info.range == YuvRange::FULL);
    CHECK_EQ(p.info.frames, 300);
    CHECK_NEAR(p.duration, 10.0, 1e-9);
    const char* hd = R"({"streams": [{"codec_type": "video", "width": 1920, "height": 1080, "pix_fmt": "yuv420p",
      "r_frame_rate": "30/1", "avg_frame_rate": "30/1", "nb_frames": "30"}], "format": {"duration": "1"}})";
    CHECK(ParseProbe(hd, p, error));
    CHECK(p.info.matrix == YuvMatrix::BT709);
    CHECK(p.info.range == YuvRange::LIMITED);
    // a variable frame rate recording is flagged
    const char* vfr = R"({"streams": [{"codec_type": "video", "width": 1280, "height": 720, "pix_fmt": "yuv420p",
      "r_frame_rate": "60/1", "avg_frame_rate": "5400/100", "nb_frames": "100"}], "format": {"duration": "2"}})";
    CHECK(ParseProbe(vfr, p, error));
    CHECK(p.variable_frame_rate);
}

void failures() {
    ProbeResult p;
    std::string error;
    CHECK(!ParseProbe("not json", p, error));
    CHECK(!ParseProbe(R"({"streams": [{"codec_type": "audio"}]})", p, error));
    CHECK(error.find("no video") != std::string::npos);
    CHECK(!ParseProbe(R"({"streams": [{"codec_type": "video", "width": 10, "height": 10}]})", p, error));
    // cover art is not the video
    CHECK(!ParseProbe(R"({"streams": [{"codec_type": "video", "width": 10, "height": 10, "r_frame_rate": "25/1",
      "disposition": {"attached_pic": 1}, "nb_frames": "1"}]})", p, error));
    CHECK_NEAR(ParseRate("30000/1001"), 29.97002997, 1e-6);
    CHECK_NEAR(ParseRate("0/0"), 0.0, 1e-12);
    CHECK_NEAR(ParseRate("25"), 25.0, 1e-12);
}

}  // namespace

int main() {
    sample_recording();
    other_formats();
    failures();
    return check_result("probe");
}
