load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "vulkan_headers",
    hdrs = glob(["include/**"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "all_files",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
