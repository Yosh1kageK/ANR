# slangc and its libraries (the compiler loads libslang-compiler and the glslang module next to it): bin/ and lib/
# on Linux and macOS, bin/ on Windows.
exports_files(glob(
    ["bin/slangc", "bin/slangc.exe"],
    allow_empty = True,
))

filegroup(
    name = "files",
    srcs = glob(["bin/**", "lib/**"]),
    visibility = ["//visibility:public"],
)
