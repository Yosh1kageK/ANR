"""Apple's Metal Shader Converter where it is installed (macOS: /usr/local/include and /usr/local/lib, from
developer.apple.com): NRI's Metal backend then takes DXIL shaders. Elsewhere, and on a Mac without it, the library is
empty and NRI_ENABLE_METAL_SHADER_CONVERTER is 0."""

def _metal_shader_converter_impl(rctx):
    include = rctx.path("/usr/local/include/metal_irconverter")
    runtime = rctx.path("/usr/local/include/metal_irconverter_runtime")
    lib = rctx.path("/usr/local/lib/libmetalirconverter.dylib")
    load_rules = 'load("@rules_cc//cc:defs.bzl", "cc_library")\n\n'
    if rctx.os.name.startswith("mac") and include.exists and runtime.exists and lib.exists:
        rctx.symlink(include, "include/metal_irconverter")
        rctx.symlink(runtime, "include/metal_irconverter_runtime")
        rctx.symlink(lib, "lib/libmetalirconverter.dylib")
        rctx.file("BUILD.bazel", load_rules + """cc_library(
    name = "metal_shader_converter",
    srcs = ["lib/libmetalirconverter.dylib"],
    hdrs = glob(["include/**"]),
    defines = ["NRI_ENABLE_METAL_SHADER_CONVERTER=1"],
    includes = ["include"],
    visibility = ["//visibility:public"],
)
""")
    else:
        rctx.file("BUILD.bazel", load_rules + """cc_library(
    name = "metal_shader_converter",
    defines = ["NRI_ENABLE_METAL_SHADER_CONVERTER=0"],
    visibility = ["//visibility:public"],
)
""")

metal_shader_converter = repository_rule(
    implementation = _metal_shader_converter_impl,
    local = True,
)
