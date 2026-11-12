#!/bin/bash
# Regenerates the test clips next to this script (about 5 to 9 KB each, 20 frames at 24 fps unless noted; ffmpeg with
# libsvtav1, libaom-av1, libx264 and libx265). They cover what the decoder has to get right:
#   av1_svt_128.mp4          SVT-AV1: hidden frames and show_existing_frame, intrabc key frames, reference MVs
#   av1_aom_128.mp4          libaom: shown frames only, no hidden ones
#   av1_svt_128_10bit.mp4    10-bit AV1 (P010 output)
#   h264_bf_160.mp4          x264 High with B-frames (pyramid), weighted prediction, POC reordering, non-reference B pictures
#   h265_144.mp4             x265 Main with B-frames, closed GOPs
#   h265_144_10bit.mp4       x265 Main 10 (10 frames, one GOP)
#   h264_crop_160x150.mp4    H.264 with a cropped frame (coded 160x160, 12 frames)
#   h265_crop_144x130.mp4    H.265 with a conformance window (coded 144x136, 12 frames)
#   av1_odd_131x137.mp4      AV1 with odd frame sizes (12 frames)
#   av1_tiles_640x360.mp4    AV1 with 128x128 superblocks and several tile columns and rows (12 frames)
#   av1_aom_seg_160.mp4      AV1 with segmentation and delta q (24 frames)
#   av1_aom_tools_160.mp4    AV1 with reference MVs, global and warped motion, OBMC, restoration, compound modes (24 frames)
#   h264_slices_160.mp4      H.264 with B-frames and four slices per picture (24 frames)
#   h265_refs5_160.mp4       H.265 with B-pyramid, five reference pictures (24 frames)
# and what it has to refuse, with a reason:
#   av1_svt_128_grain.mp4    AV1 with film grain (10 frames)
#   h264_interlaced_160.mp4  interlaced H.264 (10 frames)
# The sizes are the smallest NVIDIA's decoders take (AV1 128x128, H.265 144x144, H.264 160x160).
set -e
cd "$(dirname "$0")"
frames="-frames:v 20 -an"
pattern() { echo "testsrc2=size=$1x$1:rate=24"; }
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(pattern 128)" $frames -c:v libsvtav1 -g 10 -preset 8 -crf 50 -pix_fmt yuv420p av1_svt_128.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(pattern 128)" $frames -c:v libaom-av1 -cpu-used 8 -lag-in-frames 8 -g 10 -crf 50 -b:v 0 -pix_fmt yuv420p av1_aom_128.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(pattern 128)" $frames -c:v libsvtav1 -g 10 -preset 8 -crf 50 -pix_fmt yuv420p10le av1_svt_128_10bit.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(pattern 160)" $frames -c:v libx264 -preset medium -profile:v high -bf 3 -g 10 -crf 38 -pix_fmt yuv420p h264_bf_160.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(pattern 144)" $frames -c:v libx265 -preset medium -crf 38 -x265-params "keyint=10:open-gop=0:bframes=3:log-level=error" -pix_fmt yuv420p h265_144.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(pattern 144)" -frames:v 10 -an -c:v libx265 -preset medium -crf 38 -x265-params "keyint=10:log-level=error" -pix_fmt yuv420p10le h265_144_10bit.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(pattern 128)" -frames:v 10 -an -c:v libsvtav1 -g 10 -preset 8 -crf 50 -svtav1-params film-grain=10 -pix_fmt yuv420p av1_svt_128_grain.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(pattern 160)" -frames:v 10 -an -c:v libx264 -preset medium -profile:v high -g 10 -crf 38 -flags +ildct+ilme -x264-params tff=1 -pix_fmt yuv420p h264_interlaced_160.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=320x240:rate=24" -frames:v 12 -an -vf scale=160:150 -c:v libx264 -preset medium -profile:v high -bf 2 -g 10 -crf 38 -pix_fmt yuv420p h264_crop_160x150.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=320x240:rate=24" -frames:v 12 -an -vf scale=144:130 -c:v libx265 -preset medium -crf 38 -x265-params "keyint=10:log-level=error:bframes=2" -pix_fmt yuv420p h265_crop_144x130.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=320x240:rate=24" -frames:v 12 -an -vf scale=131:137 -c:v libaom-av1 -cpu-used 8 -g 10 -crf 50 -b:v 0 -pix_fmt yuv420p av1_odd_131x137.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=640x360:rate=24" -frames:v 12 -an -c:v libaom-av1 -cpu-used 8 -g 10 -crf 50 -b:v 0 -aom-params "sb-size=128:tile-columns=1:tile-rows=1" -pix_fmt yuv420p av1_tiles_640x360.mp4
pattern160="testsrc2=size=160x160:rate=24"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$pattern160" -frames:v 24 -an -c:v libaom-av1 -cpu-used 8 -g 12 -crf 50 -b:v 0 -aom-params "aq-mode=1:deltaq-mode=1" -pix_fmt yuv420p av1_aom_seg_160.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$pattern160" -frames:v 24 -an -c:v libaom-av1 -cpu-used 4 -lag-in-frames 16 -g 12 -crf 45 -b:v 0 -aom-params "enable-ref-frame-mvs=1:enable-global-motion=1:enable-warped-motion=1:enable-obmc=1:reduced-tx-type-set=1:enable-restoration=1:enable-cdef=1:enable-dual-filter=1:enable-interintra-comp=1:enable-masked-comp=1:enable-smooth-interintra=1:enable-onesided-comp=1:enable-diff-wtd-comp=1:enable-dist-wtd-comp=1" -pix_fmt yuv420p av1_aom_tools_160.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$pattern160" -frames:v 24 -an -c:v libx264 -preset medium -profile:v high -bf 3 -g 12 -crf 38 -x264-params slices=4 -pix_fmt yuv420p h264_slices_160.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$pattern160" -frames:v 24 -an -c:v libx265 -preset slow -crf 38 -x265-params "keyint=12:log-level=error:open-gop=0:bframes=8:rc-lookahead=20:no-weightp=1:tskip=1:limit-refs=0:ref=5:scenecut=0:lft=1" -pix_fmt yuv420p h265_refs5_160.mp4
ls -l *.mp4
