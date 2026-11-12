#include "record/xess_upscaler.h"

#include <cmath>

#include <xess.h>
#include <xess_d3d12.h>

namespace record {

namespace {

using CreateFn = xess_result_t (*)(ID3D12Device*, xess_context_handle_t*);
using InitFn = xess_result_t (*)(xess_context_handle_t, const xess_d3d12_init_params_t*);
using ExecuteFn = xess_result_t (*)(xess_context_handle_t, ID3D12GraphicsCommandList*,
                                    const xess_d3d12_execute_params_t*);
using DestroyFn = xess_result_t (*)(xess_context_handle_t);
using VelocityFn = xess_result_t (*)(xess_context_handle_t, float, float);
using OptimalFn = xess_result_t (*)(xess_context_handle_t, const xess_2d_t*, xess_quality_settings_t, xess_2d_t*,
                                    xess_2d_t*, xess_2d_t*);
using VersionFn = xess_result_t (*)(xess_version_t*);

}  // namespace

XessUpscaler::~XessUpscaler() {
    Release();
}

bool XessUpscaler::Load(const std::wstring& dll, std::string& error) {
    module_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module_) {
        error = "cannot load libxess.dll (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto get = [&](const char* name) { return (void*)GetProcAddress(module_, name); };
    create_ = get("xessD3D12CreateContext");
    init_ = get("xessD3D12Init");
    execute_ = get("xessD3D12Execute");
    destroy_ = get("xessDestroyContext");
    velocity_ = get("xessSetVelocityScale");
    optimal_ = get("xessGetOptimalInputResolution");
    version_ = get("xessGetVersion");
    if (!create_ || !init_ || !execute_ || !destroy_ || !velocity_ || !optimal_) {
        error = "the DLL lacks XeSS's D3D12 exports (xessD3D12CreateContext, xessD3D12Init, xessD3D12Execute)";
        return false;
    }
    return true;
}

bool XessUpscaler::Create(ID3D12Device* device, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                          uint32_t output_height, const SrCall& c, bool exposure_texture, bool responsive_mask,
                          std::string& error) {
    xess_context_handle_t context = nullptr;
    xess_result_t r = ((CreateFn)create_)(device, &context);
    if (r != XESS_RESULT_SUCCESS) {
        error = "xessD3D12CreateContext: " + std::to_string((int)r);
        return false;
    }
    context_ = context;
    // The quality setting whose input range holds the render size, the closest optimum first.
    const xess_2d_t out = {output_width, output_height};
    xess_quality_settings_t best = XESS_QUALITY_SETTING_BALANCED;
    double best_distance = 1e30;
    for (int q = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE; q <= XESS_QUALITY_SETTING_AA; q++) {
        xess_2d_t optimal = {}, low = {}, high = {};
        if (((OptimalFn)optimal_)(context, &out, (xess_quality_settings_t)q, &optimal, &low, &high) !=
            XESS_RESULT_SUCCESS)
            continue;
        if (render_width < low.x || render_width > high.x || render_height < low.y || render_height > high.y)
            continue;
        const double d = std::fabs((double)optimal.x - render_width);
        if (d < best_distance)
            best_distance = d, best = (xess_quality_settings_t)q;
    }
    quality_ = (int)best;
    xess_d3d12_init_params_t init = {};
    init.outputResolution = out;
    init.qualitySetting = best;
    init.initFlags = (c.motion_low_res ? 0u : (uint32_t)XESS_INIT_FLAG_HIGH_RES_MV) |
                     (c.depth_inverted ? (uint32_t)XESS_INIT_FLAG_INVERTED_DEPTH : 0u) |
                     (c.hdr ? 0u : (uint32_t)XESS_INIT_FLAG_LDR_INPUT_COLOR) |
                     (c.motion_jittered ? (uint32_t)XESS_INIT_FLAG_JITTERED_MV : 0u) |
                     (c.auto_exposure ? (uint32_t)XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : 0u) |
                     (exposure_texture ? (uint32_t)XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE : 0u) |
                     (responsive_mask ? (uint32_t)XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK : 0u);
    init.creationNodeMask = init.visibleNodeMask = 1;
    r = ((InitFn)init_)(context, &init);
    if (r != XESS_RESULT_SUCCESS) {
        error = "xessD3D12Init: " + std::to_string((int)r);
        return false;
    }
    return true;
}

bool XessUpscaler::Dispatch(ID3D12GraphicsCommandList* list, const UpscaleTextures& t, const SrCall& c,
                            std::string& error) {
    if (!context_) {
        error = "no XeSS context";
        return false;
    }
    auto context = (xess_context_handle_t)context_;
    // NGX's jitter is XeSS's; the stored motion times MV.Scale is in pixels, as XeSS's velocity scale takes it.
    ((VelocityFn)velocity_)(context, c.motion_scale[0], c.motion_scale[1]);
    xess_d3d12_execute_params_t e = {};
    e.pColorTexture = t.color;
    e.pVelocityTexture = t.motion;
    e.pDepthTexture = t.depth;
    e.pExposureScaleTexture = t.exposure;
    e.pResponsivePixelMaskTexture = t.reactive;
    e.pOutputTexture = t.output;
    e.jitterOffsetX = c.jitter[0];
    e.jitterOffsetY = c.jitter[1];
    e.exposureScale = c.exposure_scale > 0 ? c.exposure_scale : 1.0f;
    e.resetHistory = c.reset;
    e.inputWidth = c.render_size[0];
    e.inputHeight = c.render_size[1];
    e.inputColorBase = {c.color_base[0], c.color_base[1]};
    e.inputMotionVectorBase = {c.motion_base[0], c.motion_base[1]};
    e.inputDepthBase = {c.depth_base[0], c.depth_base[1]};
    const xess_result_t r = ((ExecuteFn)execute_)(context, list, &e);
    if (r != XESS_RESULT_SUCCESS) {
        error = "xessD3D12Execute: " + std::to_string((int)r);
        return false;
    }
    return true;
}

void XessUpscaler::Release() {
    if (context_ && destroy_)
        ((DestroyFn)destroy_)((xess_context_handle_t)context_);
    context_ = nullptr;
}

std::string XessUpscaler::version() const {
    xess_version_t v = {};
    if (!version_ || ((VersionFn)version_)(&v) != XESS_RESULT_SUCCESS)
        return "";
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

}  // namespace record
