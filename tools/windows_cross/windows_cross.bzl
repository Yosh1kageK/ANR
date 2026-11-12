"""Windows x64 cross compilation on Linux: clang-cl, lld-link and llvm-lib of the pinned LLVM (toolchains_llvm's
distribution, MODULE.bazel) with the MSVC C runtime and the Windows SDK, which xwin downloads from Microsoft and lays
out for clang-cl's /winsysroot. The repository it makes (@windows_cross) holds the SDK, wrapper scripts, a clang-cl
cc_toolchain and the target platform; README.md has the use."""

_BUILD = """\
load("@rules_cc//cc/private/toolchain:windows_cc_toolchain_config.bzl", "cc_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

# The target: Windows on x86-64, built by clang-cl.
platform(
    name = "x64_windows",
    constraint_values = [
        "@platforms//cpu:x86_64",
        "@platforms//os:windows",
        "@rules_cc//cc/private/toolchain:clang-cl",
    ],
)

filegroup(
    name = "winsysroot",
    srcs = glob(["winsysroot/**"]),
)

filegroup(
    name = "files",
    srcs = [
        "bin/clang-cl",
        "bin/lld-link",
        "bin/llvm-lib",
        ":winsysroot",
        "@llvm_toolchain_llvm//:bin",
        "@llvm_toolchain_llvm//:include",
    ],
)

cc_toolchain_config(
    name = "config",
    cpu = "x64_windows",
    compiler = "clang-cl",
    host_system_name = "x86_64-unknown-linux-gnu",
    target_system_name = "x86_64-pc-windows-msvc",
    target_libc = "msvcrt",
    abi_version = "clang",
    abi_libc_version = "msvcrt",
    toolchain_identifier = "windows_cross_clang_cl_x64",
    msvc_env_tmp = "/tmp",
    msvc_env_path = "/usr/bin:/bin",
    msvc_env_include = "%{sysroot}",  # an existing directory; /winsysroot sets the include paths
    msvc_env_lib = "%{sysroot}",
    msvc_cl_path = "bin/clang-cl",
    msvc_ml_path = "bin/clang-cl",
    msvc_link_path = "bin/lld-link",
    msvc_lib_path = "bin/llvm-lib",
    cxx_builtin_include_directories = ["%{llvm_root}/lib/clang", "%{root}/winsysroot"],
    tool_paths = {
        "ar": "bin/llvm-lib",
        "ml": "bin/clang-cl",
        "cpp": "bin/clang-cl",
        "gcc": "bin/clang-cl",
        "gcov": "/bin/false",
        "ld": "bin/lld-link",
        "nm": "/bin/false",
        "objcopy": "/bin/false",
        "objdump": "/bin/false",
        "strip": "/bin/false",
    },
    archiver_flags = ["/MACHINE:X64"],
    default_link_flags = ["/MACHINE:X64", "/winsysroot:%{sysroot}"],
    dbg_mode_debug_flag = "/DEBUG:FULL",
    fastbuild_mode_debug_flag = "/DEBUG:MINI",
    supports_parse_showincludes = False,
    # Joined: clang-cl loses a separate /winsysroot argument in a response file.
    all_compile_flags = ["/winsysroot%{sysroot}", "-Wno-unused-command-line-argument"],
    conly_flags = [],
    cxx_flags = [],
    link_flags = [],
    win32_winnt_flag = "/D_WIN32_WINNT=0x0A00",
)

cc_toolchain(
    name = "cc",
    all_files = ":files",
    ar_files = ":files",
    as_files = ":files",
    compiler_files = ":files",
    dwp_files = ":files",
    linker_files = ":files",
    objcopy_files = ":files",
    strip_files = ":files",
    supports_param_files = 1,
    toolchain_config = ":config",
    toolchain_identifier = "windows_cross_clang_cl_x64",
)

toolchain(
    name = "cc-toolchain",
    exec_compatible_with = [
        "@platforms//cpu:x86_64",
        "@platforms//os:linux",
    ],
    target_compatible_with = [
        "@platforms//cpu:x86_64",
        "@platforms//os:windows",
    ],
    toolchain = ":cc",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
"""

_WRAPPER = """\
#!/bin/sh
# Runs the pinned LLVM's {tool} (Bazel runs actions from the execution root).
exec "{llvm}/bin/{tool}" "$@"
"""

def _windows_cross_impl(rctx):
    rctx.download_and_extract(
        url = rctx.attr.xwin_url,
        sha256 = rctx.attr.xwin_sha256,
        stripPrefix = rctx.attr.xwin_strip_prefix,
        output = "xwin",
    )
    cache = rctx.path("xwin-cache")
    command = [
        rctx.path("xwin/xwin"),
        # The user accepted Microsoft's license terms for this download (README.md).
        "--accept-license",
        "--log-level",
        "warn",
        "--cache-dir",
        str(cache),
        "--manifest-version",
        rctx.attr.manifest_version,
        "--crt-version",
        rctx.attr.crt_version,
        "--sdk-version",
        rctx.attr.sdk_version,
        "--arch",
        "x86_64",
        "--variant",
        "desktop",
        "splat",
        "--use-winsysroot-style",
        "--preserve-ms-arch-notation",
        "--output",
        str(rctx.path("winsysroot")),
    ]
    rctx.report_progress("xwin: MSVC CRT %s and Windows SDK %s" % (rctx.attr.crt_version, rctx.attr.sdk_version))
    result = rctx.execute(command, timeout = 3600)
    if result.return_code:
        fail("xwin failed (%d):\n%s\n%s" % (result.return_code, result.stdout, result.stderr))
    rctx.delete(cache)
    rctx.delete("xwin")

    # Paths as actions see them, from the execution root; the absolute LLVM root for clang's own headers, which the
    # compiler reports at their resolved location.
    llvm_root = rctx.path(rctx.attr.clang).dirname.dirname
    llvm = "external/" + llvm_root.basename
    root = "external/" + rctx.path(".").basename
    for tool in ("clang-cl", "lld-link", "llvm-lib"):
        rctx.file("bin/" + tool, _WRAPPER.format(llvm = llvm, tool = tool), executable = True)
    rctx.file("BUILD.bazel", _BUILD.replace("%{sysroot}", root + "/winsysroot").replace(
        "%{llvm_root}",
        str(llvm_root),
    ).replace("%{root}", str(rctx.path("."))))

windows_cross = repository_rule(
    implementation = _windows_cross_impl,
    attrs = {
        "clang": attr.label(mandatory = True, doc = "the pinned LLVM's bin/clang"),
        "crt_version": attr.string(mandatory = True),
        "manifest_version": attr.string(mandatory = True),
        "sdk_version": attr.string(mandatory = True),
        "xwin_sha256": attr.string(mandatory = True),
        "xwin_strip_prefix": attr.string(mandatory = True),
        "xwin_url": attr.string(mandatory = True),
    },
)
