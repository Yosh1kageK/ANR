// Reads a capture folder's texture files (float16x4 colours, float32 depth, float16x2 motion) and the raw frames the
// generators write (NRRAW1) into memory. Pure (files only), tested by captures_test.cpp on a synthetic
// capture. Colours stay float16 as stored: the viewer uploads them as they are (RGBA16F), so a source pixel is exactly what
// the capture holds, and a UNORM texture's code is round(value * steps).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "renderer/clips/captures/capture_info.h"

namespace renderer::clips::captures {

float HalfToFloat(uint16_t h);
uint16_t FloatToHalf(float f);

// UNORM steps per channel of a DXGI colour format (1023 for R10G10B10A2, 255 for the 8-bit formats), 0 for floats.
int CodeSteps(int dxgi);

struct Image {
    uint32_t width = 0, height = 0;
    int dxgi = 0;                  // the game's format (steps and meaning); 0 for false colour
    std::vector<uint16_t> rgba;    // float16, width * height * 4
};

// A colour texture file of a capture ("f5_hudless.bin"): float16x4, the size the capture says.
bool ReadColor(const std::string& path, const TextureInfo& info, Image& image, std::string& error);
// A raw frame a generator wrote (NRRAW1 header, then float16x4).
bool ReadRawFrame(const std::string& path, Image& image, std::string& error);
// Only the header of a raw frame.
bool ReadRawHeader(const std::string& path, uint32_t& width, uint32_t& height, int& dxgi);
// A frame in the raw format (for tests and tools).
bool WriteRawFrame(const std::string& path, const Image& image, std::string& error);

// Depth (float32) and motion (float16x2) as the capture holds them.
bool ReadDepth(const std::string& path, const TextureInfo& info, std::vector<float>& depth, std::string& error);
bool ReadMotion(const std::string& path, const TextureInfo& info, std::vector<float>& motion_xy, std::string& error);

// False colour of depth and motion at out_w x out_h (nearest from the texture's grid), as sRGB codes in an Image (dxgi 0):
// depth in magma between its 1st and 99th percentile, motion as hue (direction) and brightness (length over the 99th percentile).
Image DepthImage(const std::vector<float>& depth, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h);
Image MotionImage(const std::vector<float>& motion_xy, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h);

}  // namespace renderer::clips::captures
