load("@rules_cc//cc:defs.bzl", "cc_library")

# NGX: headers and the static library that loads the DLSS runtime libraries at run time (Linux and Windows; the
# Windows library is the static-CRT one, /MT, as .bazelrc links the CRT statically). There is no NGX on macOS.
cc_library(
    name = "ngx",
    srcs = select({
        "@platforms//os:linux": ["lib/Linux_x86_64/libnvsdk_ngx.a"],
        "@platforms//os:windows": ["lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"],
        "//conditions:default": [],
    }),
    hdrs = glob(["include/*.h"]),
    includes = ["include"],
    linkopts = select({
        "@platforms//os:linux": ["-ldl"],
        "@platforms//os:windows": ["advapi32.lib", "user32.lib"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

# The NGX headers alone, as interface declarations (code that implements or calls NGX's exports): no NVIDIA
# library.
cc_library(
    name = "headers",
    hdrs = glob(["include/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

# The DLSS (SR), DLSS-D (RR) and DLSS-G (FG) runtime libraries, release builds.
filegroup(
    name = "runtime",
    srcs = select({
        "@platforms//os:linux": glob(["lib/Linux_x86_64/rel/*.so*"]),
        "@platforms//os:windows": glob(["lib/Windows_x86_64/rel/*.dll"]),
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "all_files",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
