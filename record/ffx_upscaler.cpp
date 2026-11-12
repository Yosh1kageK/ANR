#include "record/ffx_upscaler.h"

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

namespace record {

namespace {

std::string Code(ffxReturnCode_t r) {
    return "FidelityFX error " + std::to_string((unsigned)r);
}

FfxApiResource Resource(ID3D12Resource* r, uint32_t state) {
    return r ? ffxApiGetResourceDX12(r, state) : FfxApiResource{};
}

}  // namespace

FfxUpscaler::~FfxUpscaler() {
    Release();
}

bool FfxUpscaler::Load(const std::wstring& dll, std::string& error) {
    module_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module_) {
        error = "cannot load the FidelityFX DLL (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    create_ = (void*)GetProcAddress(module_, "ffxCreateContext");
    destroy_ = (void*)GetProcAddress(module_, "ffxDestroyContext");
    query_ = (void*)GetProcAddress(module_, "ffxQuery");
    dispatch_ = (void*)GetProcAddress(module_, "ffxDispatch");
    if (!create_ || !destroy_ || !query_ || !dispatch_) {
        error = "the DLL lacks the FidelityFX API's exports (ffxCreateContext, ffxDispatch, ...)";
        return false;
    }
    return true;
}

bool FfxUpscaler::Create(ID3D12Device* device, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                         uint32_t output_height, const SrCall& call, std::string& error) {
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    ffxCreateContextDescUpscale desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.maxRenderSize = {render_width, render_height};
    desc.maxUpscaleSize = {output_width, output_height};
    desc.flags = (call.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0u) |
                 (call.motion_low_res ? 0u : FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) |
                 (call.motion_jittered ? FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0u) |
                 (call.depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u) |
                 (call.auto_exposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0u);
    ffxContext context = nullptr;
    const ffxReturnCode_t r = ((PfnFfxCreateContext)create_)(&context, &desc.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        error = "ffxCreateContext: " + Code(r);
        return false;
    }
    context_ = context;
    ffxQueryGetProviderVersion version = {};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (((PfnFfxQuery)query_)(&context, &version.header) == FFX_API_RETURN_OK && version.versionName)
        version_ = version.versionName;
    return true;
}

bool FfxUpscaler::Dispatch(ID3D12GraphicsCommandList* list, const UpscaleTextures& t, const SrCall& call,
                           std::string& error) {
    if (!context_) {
        error = "no FidelityFX context";
        return false;
    }
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = list;
    d.color = Resource(t.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = Resource(t.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = Resource(t.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.exposure = Resource(t.exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.reactive = Resource(t.reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.transparencyAndComposition = Resource(t.transparency, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = Resource(t.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // NGX's and the FidelityFX API's jitter and motion conventions are the same (render pixels; the stored motion
    // times the scale is in pixels).
    d.jitterOffset = {call.jitter[0], call.jitter[1]};
    d.motionVectorScale = {call.motion_scale[0], call.motion_scale[1]};
    d.renderSize = {call.render_size[0], call.render_size[1]};
    d.upscaleSize = {call.output_size[0], call.output_size[1]};
    d.enableSharpening = call.sharpness > 0;
    d.sharpness = call.sharpness;
    d.frameTimeDelta = call.frame_time_ms > 0 ? call.frame_time_ms : 1000.0f / 60;
    d.preExposure = call.pre_exposure > 0 ? call.pre_exposure : 1;
    d.reset = call.reset;
    // FSR games give the camera; for others the planes and field of view of a typical game camera.
    d.cameraNear = call.near_plane > 0 ? call.near_plane : 0.1f;
    d.cameraFar = call.far_plane > 0 ? call.far_plane : 10000.0f;
    d.cameraFovAngleVertical = call.fov > 0 ? call.fov : 1.0472f;
    d.viewSpaceToMetersFactor = 1;
    ffxContext context = context_;
    const ffxReturnCode_t r = ((PfnFfxDispatch)dispatch_)(&context, &d.header);
    if (r != FFX_API_RETURN_OK) {
        error = "ffxDispatch: " + Code(r);
        return false;
    }
    return true;
}

void FfxUpscaler::Release() {
    if (context_ && destroy_) {
        ffxContext context = context_;
        ((PfnFfxDestroyContext)destroy_)(&context, nullptr);
    }
    context_ = nullptr;
}

}  // namespace record
