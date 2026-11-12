// The packing of the video pass's constants: layout offsets the shader reads, regions, filters and what exists.
#include "renderer/clips/video_constants.h"

#include <cstddef>

#include "renderer/clips/check.h"

using namespace renderer::clips;

int main() {
    CHECK_EQ((int)offsetof(VideoConstants, region), 0);
    CHECK_EQ((int)offsetof(VideoConstants, video), 96);
    CHECK_EQ((int)offsetof(VideoConstants, header), 192);
    CHECK_EQ((int)offsetof(VideoConstants, target), 208);
    CHECK_EQ((int)offsetof(RegionData, map), 16);
    CHECK_EQ((int)offsetof(RegionData, mode), 32);
    CHECK_EQ((int)offsetof(VideoData, b), 16);
    CHECK_EQ((int)offsetof(VideoData, size), 32);

    const CompareLayout layout = compare_layout(Layout::side_by_side, {0, 0, 1920, 1080}, 0.5);
    const Regions regions = compare_regions(layout, 3840, 2160, renderer::clips::Camera{0.5, 0.5, 1.0});
    VideoSourceParams source[2];
    source[0].width = source[1].width = 3840;
    source[0].height = source[1].height = 2160;
    source[0].exists = true;
    source[1].exists = false;
    source[1].yuv = MakeYuvParams(YuvMatrix::BT601, YuvRange::FULL, 8);
    PassTarget target;
    target.encoding = 1;
    target.code_encoding = 0;
    target.paper_white = 250.0f;
    target.hw_srgb = true;
    target.gap_gray = 0.1f;
    const VideoConstants c = MakeVideoConstants(regions, source, Filter::AUTO, target);
    CHECK_EQ(c.header[0], 2);
    CHECK_EQ(c.header[2], 1);
    CHECK_EQ(c.header[3], 0);
    CHECK_NEAR(c.target[0], 250.0, 1e-9);
    CHECK_NEAR(c.target[1], 1.0, 1e-9);
    CHECK_NEAR(c.target[2], 0.1, 1e-6);
    CHECK_EQ(c.region[0].mode[0], 0);
    CHECK_EQ(c.region[1].mode[0], 1);
    CHECK_EQ(c.region[0].mode[2], 1);  // only A exists
    CHECK_EQ(c.region[0].mode[1], (int)Filter::BOX);  // 3840 into 944 pixels: below a scale of 1
    CHECK_NEAR(c.region[0].map[2], regions.region[0].scale, 1e-6);
    CHECK_EQ(c.video[0].size[0], 3840u);
    CHECK_NEAR(c.video[1].a[0], 0.299, 1e-6);   // B's matrix
    CHECK_NEAR(c.video[1].a[3], 1.0, 1e-9);     // full range: no scaling of luma
    CHECK_NEAR(c.video[0].a[2], 16.0 / 255.0, 1e-6);
    // zoomed to 1:1 or more: nearest
    const Regions zoomed = compare_regions(layout, 3840, 2160, renderer::clips::Camera{0.5, 0.5, 8.0});
    const VideoConstants z = MakeVideoConstants(zoomed, source, Filter::AUTO, target);
    CHECK_EQ(z.region[0].mode[1], (int)Filter::NEAREST);
    // a forced filter
    const VideoConstants f = MakeVideoConstants(zoomed, source, Filter::BICUBIC, target);
    CHECK_EQ(f.region[0].mode[1], (int)Filter::BICUBIC);
    return check_result("video_constants");
}
