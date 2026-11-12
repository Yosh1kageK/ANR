load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "mathlib",
    hdrs = ["ml.h"] + glob(["Guts/**"]),
    textual_hdrs = ["ml.hlsli"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = select({
        "@platforms//cpu:aarch64": ["@sse2neon"],
        "//conditions:default": [],
    }),
)
