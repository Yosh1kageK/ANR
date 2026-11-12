// Prints what the demuxer and the codec parsers read from a file: the track, the sample table in display order, and
// the pictures of each AV1 temporal unit (or the H.264 and H.265 pictures). A development and test aid:
//   bazel run //renderer/clips/video:parse_dump -- FILE [MAX_SAMPLES]

#include <cstdio>
#include <cstdlib>
#include <string>

#include "renderer/clips/video/av1.h"
#include "renderer/clips/video/mp4.h"

using namespace renderer::clips::video;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: parse_dump FILE [MAX_SAMPLES]\n");
        return 2;
    }
    std::string why;
    auto mp4 = Mp4::Open(argv[1], &why);
    if (!mp4) {
        std::printf("unsupported: %s\n", why.c_str());
        return 1;
    }
    const Mp4Track& t = mp4->track();
    const size_t max_samples = argc > 2 ? size_t(std::atoll(argv[2])) : t.samples.size();
    std::printf("%s %ux%u timescale %u fps %.4f frames %zu samples %zu config %zu bytes\n", CodecName(t.codec), t.width,
                t.height, t.timescale, t.fps, t.display_to_sample.size(), t.samples.size(), t.config.size());
    av1::Parser av1_parser;
    if (t.codec == Codec::AV1 && !av1_parser.Configure(t.config, &why)) {
        std::printf("unsupported: %s\n", why.c_str());
        return 1;
    }
    std::vector<uint8_t> bytes;
    for (size_t s = 0; s < t.samples.size() && s < max_samples; s++) {
        const Sample& sample = t.samples[s];
        std::printf("sample %zu: display %lld pts %lld size %u sync %d\n", s, (long long)t.sample_to_display[s],
                    (long long)sample.pts, sample.size, sample.sync ? 1 : 0);
        if (t.codec != Codec::AV1)
            continue;
        mp4->ReadSample(uint32_t(s), &bytes);
        if (sample.sync)
            av1_parser.Reset();
        std::vector<av1::Picture> pictures;
        if (!av1_parser.ParseTemporalUnit(bytes.data(), bytes.size(), &pictures, &why)) {
            std::printf("  parse error: %s\n", why.c_str());
            return 1;
        }
        for (const av1::Picture& p : pictures) {
            const av1::FrameHeader& h = p.h;
            if (h.show_existing_frame) {
                std::printf("  show_existing_frame slot %u type %u\n", h.frame_to_show_map_idx, h.frame_type);
                continue;
            }
            std::printf("  frame type %u show %d showable %d order_hint %u primary %u refresh 0x%02x q %u tiles %zu "
                        "(%ux%u) refs",
                        h.frame_type, h.show_frame, h.showable_frame, h.order_hint, h.primary_ref_frame,
                        h.refresh_frame_flags, h.base_q_idx, p.tiles.size(), h.tile_cols, h.tile_rows);
            for (int i = 0; i < 7; i++)
                std::printf(" %u", h.ref_frame_idx[i]);
            std::printf(" lf %u %u %u %u cdef_bits %u lr %u %u %u skip_mode %d ref_select %d tx %u\n",
                        h.loop_filter_level[0], h.loop_filter_level[1], h.loop_filter_level[2], h.loop_filter_level[3],
                        h.cdef_bits, h.frame_restoration_type[0], h.frame_restoration_type[1],
                        h.frame_restoration_type[2], h.skip_mode_present, h.reference_select, h.tx_mode);
        }
    }
    return 0;
}
