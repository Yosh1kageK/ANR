load("@rules_cc//cc:defs.bzl", "cc_library")

# D3D12 Memory Allocator for NRI's D3D12 backend, which includes D3D12MemAlloc.cpp into its own translation unit.
cc_library(
    name = "d3d12ma",
    hdrs = ["include/D3D12MemAlloc.h"],
    includes = [
        "include",
        "src",
    ],
    textual_hdrs = ["src/D3D12MemAlloc.cpp"],
    visibility = ["//visibility:public"],
)
