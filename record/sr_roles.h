// SR's and RR's texture roles (FORMAT.md "SR", "RR") and the NGX parameters they come from: what the SR capture DLL
// records and what nr-replay hands an NGX generator.
#pragma once

namespace record {

struct SrRole {
    const char* role;
    const char* parameter;
    bool required;  // no recording without it
    bool rr;        // RR's guide buffers only
};

inline constexpr SrRole SR_ROLES[] = {
    {"color", "Color", true, false},
    {"depth", "Depth", true, false},
    {"motion", "MotionVectors", true, false},
    {"exposure", "ExposureTexture", false, false},
    {"reactive", "FSR.reactive", false, false},
    {"transparency", "FSR.transparencyAndComposition", false, false},
    {"transparency_mask", "TransparencyMask", false, false},
    {"bias", "DLSS.Input.Bias.Current.Color.Mask", false, false},
    {"diffuse_albedo", "DLSS.Input.DiffuseAlbedo", false, true},
    {"specular_albedo", "DLSS.Input.SpecularAlbedo", false, true},
    {"normals", "GBuffer.Normals", false, true},
    {"roughness", "GBuffer.Roughness", false, true},
    {"specular_hit_distance", "DLSSD.SpecularHitDistance", false, true},
    {"specular_motion", "GBuffer.SpecularMvec", false, true},
    {"transparency_layer", "DLSS.TransparencyLayer", false, true},
    {"DLSSD.DiffuseHitDistance", "DLSSD.DiffuseHitDistance", false, true},
    {"DLSSD.DiffuseRayDirection", "DLSSD.DiffuseRayDirection", false, true},
    {"DLSSD.SpecularRayDirection", "DLSSD.SpecularRayDirection", false, true},
    {"DLSSD.DiffuseRayDirectionHitDistance", "DLSSD.DiffuseRayDirectionHitDistance", false, true},
    {"DLSSD.SpecularRayDirectionHitDistance", "DLSSD.SpecularRayDirectionHitDistance", false, true},
    {"DLSSD.ReflectedAlbedo", "DLSSD.ReflectedAlbedo", false, true},
    {"DLSS.TransparencyLayerOpacity", "DLSS.TransparencyLayerOpacity", false, true},
    {"DLSS.TransparencyLayerMvecs", "DLSS.TransparencyLayerMvecs", false, true},
    {"DLSS.DisocclusionMask", "DLSS.DisocclusionMask", false, true},
    {"DLSSD.ResponsivityMask", "DLSSD.ResponsivityMask", false, true},
    {"DLSSD.Alpha", "DLSSD.Alpha", false, true},
    {"GBuffer.Emissive", "GBuffer.Emissive", false, true},
    {"DepthHighRes", "DepthHighRes", false, true},
    {"MotionVectorsReflection", "MotionVectorsReflection", false, true},
    {"output", "Output", false, false},  // the source's own result, recorded after its call
};

}  // namespace record
