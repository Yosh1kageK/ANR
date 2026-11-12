load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "imgui",
    srcs = glob(["*.cpp"]),
    hdrs = glob(["*.h"]),
    includes = ["."],
    visibility = ["//visibility:public"],
)

# The SDL3 platform backend (the renderer's input; renderer/app.cpp).
cc_library(
    name = "backends_sdl3",
    srcs = ["backends/imgui_impl_sdl3.cpp"],
    hdrs = ["backends/imgui_impl_sdl3.h"],
    includes = ["backends"],
    visibility = ["//visibility:public"],
    deps = [
        ":imgui",
        "@sdl3",
    ],
)
