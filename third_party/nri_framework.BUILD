load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "detex",
    srcs = glob(["External/Detex/*.c"]),
    hdrs = glob(["External/Detex/*.h"]),
    copts = select({
        "@platforms//os:windows": ["/w"],
        "//conditions:default": ["-w"],
    }),
    includes = ["External"],
)

# NRIFramework without its GLFW window (SampleBase.cpp; the renderer's is renderer/app.cpp on SDL3): the camera,
# timers, the glTF scene loader and the NRI helpers. Its header still names GLFW's types, so GLFW's headers come
# along.
cc_library(
    name = "nri_framework",
    srcs = [
        "Source/Camera.cpp",
        "Source/DebugAllocator.cpp",
        "Source/Timer.cpp",
        "Source/Utils.cpp",
    ],
    hdrs = glob(["Include/*.h"]),
    copts = select({
        "@platforms//os:windows": ["/W0"],
        "//conditions:default": [
            "-Wno-missing-field-initializers",
            "-Wno-strict-aliasing",
            "-Wno-unused-variable",
        ],
    }),
    local_defines = ["USE_MONOTONIC_TIMER"] + select({  # Timer: CLOCK_MONOTONIC (the realtime clock jumps when set)
        "@platforms//os:windows": [
            "WIN32_LEAN_AND_MEAN",
            "NOMINMAX",
            "_CRT_SECURE_NO_WARNINGS",
        ],
        "//conditions:default": [],
    }),
    includes = [
        "External",
        "Include",
        "Source",
    ],
    linkopts = select({
        "@platforms//os:linux": ["-ldl", "-lpthread"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":detex",
        "@cgltf",
        "@glfw//:headers",
        "@imgui",
        "@mathlib",
        "@nri",
    ],
)
