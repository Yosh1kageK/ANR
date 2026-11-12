# Windows x64 cross compilation on Linux

`--config=windows_cross` builds Windows x64 DLLs and executables on a Linux host with clang-cl, lld-link and llvm-lib from the pinned LLVM (23.1.5), the MSVC C runtime 14.51 and the Windows SDK 10.0.28000.

```bash
bazel build --config=windows_cross //record:recorder
ls bazel-windows-bin/record/
```

The first such build fetches `@windows_cross`: xwin 0.10.0 downloads the MSVC C runtime and the Windows SDK from Microsoft (about 100 MiB) and runs with `--accept-license`. **This accepts Microsoft's license terms for the Visual Studio Build Tools (MSVC CRT and Windows SDK) on your behalf. Use `--config=windows_cross` only if you accept them.** Builds without it never fetch these files.

- `windows_cross.bzl` is the repository rule. It runs `xwin splat --use-winsysroot-style --preserve-ms-arch-notation`, so `/winsysroot` (clang-cl) and `/winsysroot:` (lld-link) find the CRT and SDK, writes wrapper scripts that run the LLVM tools from the execution root, and defines a `cc_toolchain` on rules_cc's Windows toolchain configuration.
- Outputs go to `bazel-out/k8-opt-windows`, linked as `bazel-windows-bin`; `bazel-bin` stays the Linux build's. Programs that need Windows run on Windows or under Wine.
- `MODULE.bazel` pins the xwin release (SHA-256), the Visual Studio manifest, the CRT and the SDK.
