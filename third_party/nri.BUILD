load("@rules_cc//cc:defs.bzl", "cc_library")

# NRI per OS, statically linked, with validation, the NONE backend and the ImGui extension:
# - Linux: the Vulkan backend with Xlib and Wayland swap chains, and the NGX upscalers (DLSS SR and RR).
# - Windows: the Vulkan backend with Win32 swap chains, the NGX upscalers, and the D3D12 backend with the Agility
#   SDK preview's headers (@agility_sdk), without NVAPI and AMD AGS (the D3D12 hosts).
# - --//third_party:nri_ngx=false leaves NGX (NVIDIA's code) out on Linux and Windows, and
#   --//third_party:nri_d3d12=false the D3D12 backend and the Agility SDK out on Windows (both: DLLs that run on a
#   game's own device).
# - macOS: the Metal 4 backend (macOS 26+) through metal-cpp; DXIL shaders where Apple's Metal Shader Converter is
#   installed (@metal_shader_converter defines NRI_ENABLE_METAL_SHADER_CONVERTER).
# Each backend compiles from one implementation file that includes the rest; as in NRI's CMake, only the Vulkan
# backend sees the platform defines (X11's Window would clash with nri::Window elsewhere).

COMMON_DEFINES = [
    "NRI_STATIC_LIBRARY=1",
    "NRI_ENABLE_NONE_SUPPORT=1",
    "NRI_ENABLE_VALIDATION_SUPPORT=1",
    "NRI_ENABLE_DEBUG_NAMES_AND_ANNOTATIONS=1",
    "NRI_ENABLE_IMGUI_EXTENSION=1",
    "NRI_STREAMER_THREAD_SAFE=1",
]

NRI_COPTS = select({
    "@platforms//os:windows": ["/W0"],
    "//conditions:default": [
        "-Wno-missing-field-initializers",
        "-Wno-unused-parameter",
        "-Wno-strict-aliasing",
        "-Wno-nullability-completeness",
        "-fvisibility=hidden",
    ],
})

cc_library(
    name = "headers",
    hdrs = glob(["Include/**"]) + glob([
        "Source/**/*.h",
        "Source/**/*.hpp",
    ]),
    defines = COMMON_DEFINES + select({
        "@platforms//os:linux": [
            "NRI_ENABLE_VK_SUPPORT=1",
            "NRI_ENABLE_XLIB_SUPPORT=1",
            "NRI_ENABLE_WAYLAND_SUPPORT=1",
        ],
        "@platforms//os:windows": [
            "NRI_ENABLE_VK_SUPPORT=1",
            "WIN32_LEAN_AND_MEAN",
            "NOMINMAX",
            "_CRT_SECURE_NO_WARNINGS",
        ],
        "@platforms//os:macos": ["NRI_ENABLE_METAL_SUPPORT=1"],
    }) + select({
        "@@//third_party:nri_d3d12_windows": [
            "NRI_ENABLE_D3D12_SUPPORT=1",
            "NRI_ENABLE_AGILITY_SDK_SUPPORT=1",
            "NRI_AGILITY_SDK_VERSION_MAJOR=721",  # @agility_sdk's D3D12SDKVersion (NRI's CMake sets this define)
        ],
        "//conditions:default": [],
    }) + select({
        "@@//third_party:nri_ngx_linux": ["NRI_ENABLE_NGX_SDK=1"],
        "@@//third_party:nri_ngx_windows": ["NRI_ENABLE_NGX_SDK=1"],
        "//conditions:default": [],
    }),
    includes = [
        "Include",
        "Source",
        "Source/Shared",
    ],
    # The ImGui extension includes its shader sources; NRI.hlsl comes in through the shared headers.
    textual_hdrs = glob(["Shaders/**"]),
    deps = ["@metal_shader_converter"] + select({
        "@platforms//os:linux": [
            "@@//third_party:x11_headers",
            "@wayland//:client_headers",
            "@vma",
            "@vulkan_headers",
        ],
        "@platforms//os:windows": [
            "@vma",
            "@vulkan_headers",
        ],
        "@platforms//os:macos": [
            ":metal_shaders",
            "@metal_cpp",
        ],
    }) + select({
        "@@//third_party:nri_d3d12_windows": [
            "@agility_sdk//:headers",
            "@d3d12ma",
        ],
        "//conditions:default": [],
    }) + select({
        "@@//third_party:nri_ngx_linux": ["@dlss//:ngx"],
        "@@//third_party:nri_ngx_windows": ["@dlss//:ngx"],
        "//conditions:default": [],
    }),
)

# macOS: NRI's own Metal shaders, compiled by Xcode's Metal compiler (xcrun metal; Xcode 26 with its Metal
# toolchain) and embedded as C arrays, as NRI's CMake does.
[genrule(
    name = name + "_metallib",
    srcs = ["Shaders/%s.metal" % name],
    outs = ["metallib/%s.metallib.h" % name],
    cmd = " && ".join([
        "xcrun -sdk macosx metal -std=metal4.0 -mmacosx-version-min=26.0 -c $< -o $(@D)/{name}.air",
        "xcrun -sdk macosx metallib $(@D)/{name}.air -o $(@D)/{name}.metallib",
        "$(execpath @@//tools:embed) $(@D)/{name}.metallib g_{name}_metallib $@",
    ]).format(name = name),
    tags = ["no-sandbox"],  # xcrun finds Xcode and the Metal toolchain outside the sandbox
    target_compatible_with = ["@platforms//os:macos"],
    tools = ["@@//tools:embed"],
) for name in [
    "Imgui",
    "InternalMetal",
]]

cc_library(
    name = "metal_shaders",
    hdrs = [
        "metallib/Imgui.metallib.h",
        "metallib/InternalMetal.metallib.h",
    ],
    includes = ["metallib"],
    target_compatible_with = ["@platforms//os:macos"],
)

[cc_library(
    name = name,
    srcs = [src],
    copts = NRI_COPTS,
    local_defines = defines,
    deps = [":headers"],
) for name, src, defines in [
    # On macOS, Shared.cpp compiles MetalFX's metal-cpp implementation and ImplMetal.cpp the rest, as with NRI's
    # CMake, which defines NRI_METAL_CPP_PRIVATE_IMPLEMENTATION for every NRI target.
    ("shared", "Source/Shared/Shared.cpp", select({
        "@platforms//os:macos": ["NRI_METAL_CPP_PRIVATE_IMPLEMENTATION=1"],
        "//conditions:default": [],
    })),
    ("none", "Source/NONE/ImplNONE.cpp", []),
    ("validation", "Source/Validation/ImplVal.cpp", []),
]]

cc_library(
    name = "vk",
    srcs = ["Source/VK/ImplVK.cpp"],
    copts = NRI_COPTS,
    local_defines = select({
        "@platforms//os:linux": ["VK_USE_PLATFORM_XLIB_KHR", "VK_USE_PLATFORM_WAYLAND_KHR"],
        "@platforms//os:windows": ["VK_USE_PLATFORM_WIN32_KHR"],
        "//conditions:default": [],
    }),
    target_compatible_with = select({
        "@platforms//os:macos": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    deps = [":headers"],
)

cc_library(
    name = "d3d12",
    srcs = ["Source/D3D12/ImplD3D12.cpp"],
    copts = NRI_COPTS,
    linkopts = [
        "d3d12.lib",
        "dxgi.lib",
        "dxguid.lib",
    ],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [":headers"],
)

cc_library(
    name = "metal",
    srcs = ["Source/Metal/ImplMetal.cpp"],
    copts = NRI_COPTS,
    local_defines = ["NRI_METAL_CPP_PRIVATE_IMPLEMENTATION=1"],
    target_compatible_with = ["@platforms//os:macos"],
    deps = [":headers"],
)

cc_library(
    name = "nri",
    srcs = ["Source/Creation/Creation.cpp"],
    copts = NRI_COPTS,
    linkopts = select({
        "@platforms//os:linux": ["-ldl", "-lpthread"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":headers",
        ":none",
        ":shared",
        ":validation",
    ] + select({
        "@platforms//os:macos": [":metal"],
        "//conditions:default": [":vk"],
    }) + select({
        "@@//third_party:nri_d3d12_windows": [":d3d12"],
        "//conditions:default": [],
    }),
)
