load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# The Khronos Vulkan loader, built from source as libvulkan.so.1 for the runfiles (device/vulkan.cpp loads
# it by path; NRI gets the same path). Xlib, Xlib-Xrandr and Wayland WSI (the viewer's swap chains); no XCB
# surfaces. GPU drivers (ICDs) and layers still come from the system's JSON manifests, as with any loader.

LOADER_DEFINES = [
    "_GNU_SOURCE",
    "LOADER_ENABLE_LINUX_SORT",
    "HAVE_SECURE_GETENV",
    "HAVE_REALPATH",
    "HAVE_CET_H",
    "SYSCONFDIR=\\\"/etc\\\"",
    "FALLBACK_CONFIG_DIRS=\\\"/etc/xdg\\\"",
    "FALLBACK_DATA_DIRS=\\\"/usr/local/share:/usr/share\\\"",
    "VK_USE_PLATFORM_XLIB_KHR",
    "VK_USE_PLATFORM_XLIB_XRANDR_EXT",
    "VK_USE_PLATFORM_WAYLAND_KHR",
    "VK_ENABLE_BETA_EXTENSIONS",
]

cc_library(
    name = "loader_headers",
    hdrs = glob([
        "loader/*.h",
        "loader/generated/*.h",
    ]),
    includes = [
        "loader",
        "loader/generated",
    ],
    textual_hdrs = ["loader/generated/vk_loader_extensions.c"],  # loader.c includes it
    deps = [
        "@@//third_party:x11_headers",
        "@vulkan_headers",
        "@wayland//:client_headers",
    ],
)

# Struct offsets for the assembly trampolines of functions the loader does not know (gen_defines.asm)
cc_binary(
    name = "asm_offset",
    srcs = ["loader/asm_offset.c"],
    copts = ["-w"],
    local_defines = LOADER_DEFINES,
    deps = [":loader_headers"],
)

genrule(
    name = "gen_defines",
    outs = ["gen_defines.asm"],
    cmd = "$(execpath :asm_offset) GAS && mv gen_defines.asm $@",
    tools = [":asm_offset"],
)

cc_library(
    name = "gen_defines_header",
    includes = ["."],
    textual_hdrs = [":gen_defines"],
)

cc_binary(
    name = "libvulkan.so.1",
    srcs = [
        "loader/allocation.c",
        "loader/cJSON.c",
        "loader/debug_utils.c",
        "loader/dev_ext_trampoline.c",
        "loader/extension_manual.c",
        "loader/generated/vk_loader_core_trampolines.c",
        "loader/gpa_helper.c",
        "loader/loader.c",
        "loader/loader_environment.c",
        "loader/loader_json.c",
        "loader/loader_linux.c",
        "loader/log.c",
        "loader/phys_dev_ext.c",
        "loader/settings.c",
        "loader/terminator.c",
        "loader/trampoline.c",
        "loader/unknown_ext_chain_gas_x86.S",
        "loader/unknown_function_handling.c",
        "loader/vk_loader_platform.c",
        "loader/wsi.c",
    ],
    copts = [
        "-w",
        "-fvisibility=hidden",
    ],
    linkopts = [
        "-Wl,-soname,libvulkan.so.1",
        "-Wl,--exclude-libs,ALL",  # only the loader's own vk* functions are exported
        "-ldl",
        "-lpthread",
    ],
    linkshared = True,
    local_defines = LOADER_DEFINES + ["UNKNOWN_FUNCTIONS_SUPPORTED"],
    visibility = ["//visibility:public"],
    deps = [
        ":gen_defines_header",
        ":loader_headers",
    ],
)
