// nr-record recordings (record/FORMAT.md) in the Captures tab: a recording folder read as a CaptureInfo through
// //record:reader, and its real frames' textures decoded into the tab's images. Frame generation recordings only.
// Thread safe: the image store's and the thumbnails' workers read at once.
//
//   CaptureInfo c;
//   if (IsRecording(folder) && LoadRecording(folder, c, error)) ...
//   ReadSource(source, out_w, out_h, image, error);  // any source of a pair (sources.h), recorded or not

#pragma once

#include <string>
#include <vector>

#include "renderer/clips/captures/capture_info.h"
#include "renderer/clips/captures/capture_reader.h"
#include "renderer/clips/captures/sources.h"

namespace renderer::clips::captures {

inline constexpr const char* RECORDING_FORMAT = "nr-record";

// The folder holds a recording (record.json).
bool IsRecording(const std::string& folder);

// A recording as a capture: its real frames in recording order, their textures by role, each frame's calls (generated
// frame index i of count at t = i / (count + 1)), the gaps where frames were dropped.
bool LoadRecording(const std::string& folder, CaptureInfo& capture, std::string& error);

// Real frame `frame`'s texture `role`: colour as an Image (float16, UNORM codes divided by their steps, float formats as
// they are, the game's DXGI format), depth (float32) and motion (two floats a texel) as recorded. A recording's last
// decoded frames stay in memory, so the sources of a pair decode once.
bool ReadRecordedColor(const std::string& folder, int frame, const std::string& role, Image& image, std::string& error);
bool ReadRecordedDepth(const std::string& folder, int frame, const std::string& role, std::vector<float>& depth,
                       uint32_t& width, uint32_t& height, std::string& error);
bool ReadRecordedMotion(const std::string& folder, int frame, const std::string& role, std::vector<float>& motion_xy,
                        uint32_t& width, uint32_t& height, std::string& error);

// The picture of a source of a pair, whatever holds it: a capture folder's texture file, a recorded texture, or a
// generator's raw frame; depth and motion as false colour at out_w x out_h.
bool ReadSource(const Source& source, uint32_t out_w, uint32_t out_h, Image& image, std::string& error);

// A colour texture of a capture's real frame (the thumbnails).
bool ReadFrameColor(const CaptureInfo& capture, int frame, const std::string& name, Image& image, std::string& error);

}  // namespace renderer::clips::captures
