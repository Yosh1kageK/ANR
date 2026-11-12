load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# libwayland 1.26.0: wayland-scanner (built here, for protocol code) and the client headers. No library: SDL3 and the
# GPU driver use the system's libwayland-client, which SDL3 loads at run time, and a second copy in the process would
# not share its objects.

expand_template(
    name = "wayland_version_h",
    out = "wayland-version.h",
    substitutions = {
        "@WAYLAND_VERSION_MAJOR@": "1",
        "@WAYLAND_VERSION_MINOR@": "26",
        "@WAYLAND_VERSION_MICRO@": "0",
        "@WAYLAND_VERSION@": "1.26.0",
    },
    template = "src/wayland-version.h.in",
)

cc_binary(
    name = "wayland_scanner",
    srcs = [
        "src/scanner.c",
        "src/wayland-util.c",
        ":wayland_version_h",
    ] + glob(["src/*.h"]),
    copts = ["-w"],
    visibility = ["//visibility:public"],
    deps = ["@libexpat"],
)

genrule(
    name = "client_protocol_header",
    srcs = ["protocol/wayland.xml"],
    outs = ["wayland-client-protocol.h"],
    cmd = "$(execpath :wayland_scanner) -s client-header < $< > $@",
    tools = [":wayland_scanner"],
)

# wayland-client-core.h, wayland-util.h and the version.
cc_library(
    name = "client_core_headers",
    hdrs = [
        "src/wayland-client-core.h",
        "src/wayland-util.h",
        ":wayland_version_h",
    ],
    includes = [
        ".",
        "src",
    ],
    visibility = ["//visibility:public"],
)

# wayland-client.h with the core protocol: for VK_USE_PLATFORM_WAYLAND_KHR.
cc_library(
    name = "client_headers",
    hdrs = [
        "src/wayland-client.h",
        ":client_protocol_header",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":client_core_headers"],
)
