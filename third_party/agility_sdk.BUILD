load("@rules_cc//cc:defs.bzl", "cc_library")

# The D3D12 Agility SDK NuGet package (MODULE.bazel): its headers, ahead of the Windows SDK's d3d12.h, and the
# redistributable runtime that a Windows bundle copies next to its executable.
cc_library(
    name = "headers",
    hdrs = glob(["build/native/include/**"]),
    includes = ["build/native/include"],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "redist",
    srcs = [
        "build/native/bin/x64/D3D12Core.dll",
        "build/native/bin/x64/d3d12SDKLayers.dll",
    ],
    visibility = ["//visibility:public"],
)
