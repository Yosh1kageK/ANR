# A DXC release for Linux (MODULE.bazel: @dxc_linux, @dxc_preview_linux): bin/dxc with lib/libdxcompiler.so and the
# validator lib/libdxil.so, and the HLSL headers (dx/linalg.h) that Slang's wave-matrix HLSL includes.
filegroup(
    name = "files",
    srcs = glob(["bin/**", "lib/**", "include/**"]),
    visibility = ["//visibility:public"],
)

exports_files(["bin/dxc"])
