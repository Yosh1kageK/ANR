load("@rules_cc//cc:defs.bzl", "cc_library")

# metal-cpp: C++ headers for Metal, QuartzCore and MetalFX (NRI's Metal backend compiles their implementation).
cc_library(
    name = "metal_cpp",
    hdrs = glob(["**/*.hpp"]),
    includes = ["."],
    linkopts = [
        "-framework Foundation",
        "-framework Metal",
        "-framework MetalFX",
        "-framework QuartzCore",
        "-framework CoreGraphics",
    ],
    visibility = ["//visibility:public"],
)
