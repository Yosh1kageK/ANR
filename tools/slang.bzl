"""Slang to SPIR-V (Vulkan) or DXIL (NRI's Metal backend converts DXIL to Metal) at build time: one compute (or
graphics) entry point per output, with its reflection JSON, by the pinned slangc of the build host (@slang,
@slang_macos, @slang_windows), at -O3 (slangc's default is -O1). What ships carries no names or debug data: slangc's
-obfuscate, spirv_strip on SPIR-V, and DXC's -Qstrip_debug and -Qstrip_reflect on DXIL (tools/package_check.py).

DXIL takes two steps: slangc writes HLSL and the reflection, then DXC (//third_party:dxcompiler) compiles the HLSL
with the arguments slangc's own DXIL target passes it (slang-dxc-compiler.cpp and slang-code-gen.cpp: -HV 2021,
-O3 for slangc's -O3, -disable-payload-qualifiers, -no-warnings, and -enable-16bit-types from shader model 6.2). Slang's macOS releases are built without their DXC path (SLANG_ENABLE_DXIL_SUPPORT is 0 on
Apple), so slangc cannot run DXC there itself.

slang_metal emits native Metal compute from Slang, adapts buffer arguments to NRI's descriptor-table ABI, and
builds a metallib with Xcode at -O3. It does not pass native matrix code through DXC or Metal Shader Converter.

slang_dxil_d3d12 compiles the same two steps on Linux with Microsoft's DXC release for D3D12 (signed by its
validator, libdxil.so); slang_dxil_linalg takes Shader Model 6.10 wave-matrix kernels through the SM 6.10 preview
DXC, whose HLSL headers (dx/linalg.h) the generated HLSL includes."""

# DXC's shader model prefix per Slang stage.
_DXC_STAGES = {"vertex": "vs", "fragment": "ps", "pixel": "ps", "compute": "cs", "geometry": "gs", "hull": "hs",
               "domain": "ds", "mesh": "ms", "amplification": "as"}

_EXTENSIONS = {"spirv": "spv", "dxil": "dxil"}

def _slang_shaders_impl(ctx):
    outputs = []
    includes = depset([f.dirname for f in ctx.files.srcs + ctx.files.deps]).to_list()
    target = ctx.attr.target
    slangc = ctx.executable._slangc
    tools = ctx.files._slang_files
    dxc = None
    if target == "dxil":
        dxc = [f for f in ctx.files.dxc if f.basename == "dxc"]
        if not dxc:
            fail("DXIL needs dxc = \"//third_party:dxcompiler\" (slang_dxil sets it)")
        dxc = dxc[0]
    for entry in ctx.attr.entries:
        name, _, stage = entry.partition(":")
        stage = stage or "compute"
        code = ctx.actions.declare_file("%s/%s.%s.%s" % (ctx.attr.name, ctx.file.src.basename, name, _EXTENSIONS[target]))
        reflection = ctx.actions.declare_file("%s/%s.%s.json" % (ctx.attr.name, ctx.file.src.basename, name))
        # SPIR-V: slangc's module goes through spirv_strip (no names or debug data in what ships); packages leave
        # the _unstripped directory out (tools/package_check.py fails on names).
        if dxc:
            slang_out = ctx.actions.declare_file("%s/%s.%s.hlsl" % (ctx.attr.name, ctx.file.src.basename, name))
        else:
            slang_out = ctx.actions.declare_file("_unstripped/%s/%s.%s.spv" % (ctx.attr.name, ctx.file.src.basename,
                                                                                 name))
        args = ctx.actions.args()
        args.add(ctx.file.src.path)
        # -obfuscate: hashed identifiers and no source information in the module (the reflection JSON keeps the
        # names the hosts bind by; entry points keep theirs).
        args.add_all(["-entry", name, "-stage", stage, "-target", "hlsl" if dxc else target, "-O3", "-obfuscate"])
        args.add_all(["-profile", ctx.attr.profile if target == "spirv" else ctx.attr.dxil_profile])
        args.add_all(["-o", slang_out.path, "-reflection-json", reflection.path])
        for include in includes:
            args.add_all(["-I", include])
        args.add_all(ctx.attr.copts if target == "spirv" else ctx.attr.dxil_copts)
        ctx.actions.run(
            executable = slangc,
            arguments = [args],
            inputs = ctx.files.srcs + ctx.files.deps + [ctx.file.src],
            tools = tools,
            outputs = [slang_out, reflection],
            mnemonic = "SlangCompile",
            progress_message = "slangc %s %s (%s)" % (ctx.file.src.short_path, name, "hlsl" if dxc else target),
        )
        if not dxc:
            ctx.actions.run(
                executable = ctx.executable._spirv_strip,
                arguments = [slang_out.path, code.path],
                inputs = [slang_out],
                outputs = [code],
                mnemonic = "SpirvStrip",
                progress_message = "spirv_strip %s %s" % (ctx.file.src.short_path, name),
            )
        if dxc:
            if stage not in _DXC_STAGES:
                fail("no DXC profile for stage %s" % stage)
            profile = "%s_%s" % (_DXC_STAGES[stage], ctx.attr.dxil_profile.removeprefix("sm_"))
            version = ctx.attr.dxil_profile.removeprefix("sm_").split("_")
            dxc_args = ctx.actions.args()
            # -Qstrip_debug and -Qstrip_reflect: no debug or reflection parts in what ships (the hosts bind by the
            # reflection JSON).
            dxc_args.add_all(["-T", profile, "-E", name, "-HV", "2021", "-O3", "-disable-payload-qualifiers",
                              "-no-warnings", "-Qstrip_debug", "-Qstrip_reflect"])
            if (int(version[0]), int(version[1])) >= (6, 2):
                dxc_args.add("-enable-16bit-types")
            # A DXC release's HLSL headers (dx/linalg.h) and validator (lib/libdxil.so, which signs the DXIL).
            env = {}
            for f in ctx.files.dxc:
                if f.path.endswith("include/hlsl/dx/linalg.h"):
                    dxc_args.add_all(["-I", f.dirname.removesuffix("/dx")])
                if f.basename == "libdxil.so":
                    env["LD_LIBRARY_PATH"] = f.dirname
            dxc_args.add_all(ctx.attr.dxc_copts)
            dxc_args.add_all([slang_out.path, "-Fo", code.path])
            ctx.actions.run(
                executable = dxc,
                arguments = [dxc_args],
                inputs = [slang_out],
                tools = ctx.files.dxc,
                env = env,
                outputs = [code],
                mnemonic = "DxcCompile",
                progress_message = "dxc %s %s (dxil)" % (ctx.file.src.short_path, name),
            )
        outputs += [code, reflection]
    return [DefaultInfo(files = depset(outputs), runfiles = ctx.runfiles(files = outputs))]

slang_shaders = rule(
    implementation = _slang_shaders_impl,
    doc = "Compiles `entries` (\"name\" for compute, or \"name:stage\") of `src` to <name>/<src>.<entry>.spv (or " +
          ".dxil) and .json.",
    attrs = {
        "src": attr.label(allow_single_file = [".slang"], mandatory = True),
        "entries": attr.string_list(mandatory = True),
        "srcs": attr.label_list(allow_files = [".slang"], doc = "Modules that src imports."),
        "deps": attr.label_list(allow_files = True, doc = "Other included files."),
        "target": attr.string(default = "spirv", values = ["spirv", "dxil"]),
        "profile": attr.string(default = "spirv_1_6"),
        "dxil_profile": attr.string(default = "sm_6_6"),
        "copts": attr.string_list(doc = "Options for SPIR-V."),
        "dxil_copts": attr.string_list(doc = "Options for DXIL."),
        "dxc_copts": attr.string_list(doc = "Options for DXC, after slangc's HLSL (DXIL only)."),
        "_slangc": attr.label(default = "//tools:slangc", executable = True, cfg = "exec", allow_single_file = True),
        "_slang_files": attr.label(default = "//tools:slang_files", cfg = "exec"),
        "_spirv_strip": attr.label(default = "//tools:spirv_strip", executable = True, cfg = "exec"),
        "dxc": attr.label(cfg = "exec", doc = "DXC (its dxc executable and library), for DXIL."),
    },
)

def slang_spirv(**kwargs):
    slang_shaders(target = "spirv", **kwargs)

def slang_dxil(**kwargs):
    """DXIL for NRI's Metal backend: macOS only (DXC builds from source there)."""
    slang_shaders(
        target = "dxil",
        dxc = "//third_party:dxcompiler",
        target_compatible_with = ["@platforms//os:macos"],
        **kwargs
    )

def slang_dxil_d3d12(**kwargs):
    """DXIL for D3D12, built on Linux by Microsoft's DXC release (the version slang_dxil builds from source)."""
    slang_shaders(
        target = "dxil",
        dxc = "@dxc_linux//:files",
        exec_compatible_with = ["@platforms//os:linux"],
        **kwargs
    )

def slang_dxil_linalg(**kwargs):
    """Shader Model 6.10 DXIL for D3D12 (wave matrices): the SM 6.10 preview DXC, on Linux. The
    preview's DXIL carries the preview hash, which D3D12 runs only with experimental shader models in developer
    mode."""
    slang_shaders(
        target = "dxil",
        dxil_profile = "sm_6_10",
        dxc = "@dxc_preview_linux//:files",
        # Slang 2026.21's HLSL names the E4M3 matrix component F8_E4M3; the preview's dx/linalg.h has F8_E4M3FN.
        dxc_copts = ["-D", "F8_E4M3=F8_E4M3FN"],
        exec_compatible_with = ["@platforms//os:linux"],
        **kwargs
    )

def _slang_metal_impl(ctx):
    outputs = []
    for name in ctx.attr.entries:
        base = "%s/%s.%s" % (ctx.attr.output_dir or ctx.attr.name, ctx.file.src.basename, name)
        raw = ctx.actions.declare_file(base + ".slang.metal")
        raw_reflection = ctx.actions.declare_file(base + ".slang.json")
        metal = ctx.actions.declare_file(base + ".metal")
        reflection = ctx.actions.declare_file(base + ".json")
        air = ctx.actions.declare_file(base + ".air")
        library = ctx.actions.declare_file(base + ".metallib")
        args = ctx.actions.args()
        # -obfuscate: hashed identifiers in the Metal source and so in the metallib (metal_bindings.py renames the
        # entry); the reflection keeps the names the host binds by.
        # -ignore-capabilities: slangc checks every entry of the imported modules against Metal, and the DXIL-only
        # entries there use wave intrinsics that Slang's Metal target lacks (fg_lite_trust.slang sil_cells:
        # WavePrefixCountBits). Xcode's compile below still rejects anything the emitted entry cannot run.
        args.add_all([ctx.file.src.path, "-entry", name, "-stage", "compute", "-target", "metal", "-profile", "sm_6_6",
                      "-O3", "-obfuscate", "-ignore-capabilities", "-o", raw.path, "-reflection-json",
                      raw_reflection.path])
        for include in depset([f.dirname for f in ctx.files.srcs + [ctx.file.src]]).to_list():
            args.add_all(["-I", include])
        args.add_all(ctx.attr.copts)
        ctx.actions.run(
            executable = ctx.executable._slangc,
            arguments = [args],
            inputs = ctx.files.srcs + [ctx.file.src],
            tools = ctx.files._slang_files,
            outputs = [raw, raw_reflection],
            mnemonic = "SlangMetal",
        )
        ctx.actions.run_shell(
            inputs = [raw, raw_reflection, ctx.file._adapter],
            outputs = [metal, reflection],
            command = "python3 -I \"$1\" --source \"$2\" --reflection \"$3\" --out \"$4\" --out-reflection \"$5\"",
            arguments = [ctx.file._adapter.path, raw.path, raw_reflection.path, metal.path, reflection.path],
            mnemonic = "NriMetalBindings",
        )
        ctx.actions.run_shell(
            inputs = [metal],
            outputs = [air, library],
            command = "xcrun -sdk macosx metal -std=metal4.0 -mmacosx-version-min=26.0 -O3 -c \"$1\" -o \"$2\" && " +
                      "xcrun -sdk macosx metallib \"$2\" -o \"$3\"",
            arguments = [metal.path, air.path, library.path],
            execution_requirements = {"no-sandbox": "1"},  # Xcode and the Metal toolchain, as for NRI's shaders
            mnemonic = "MetalCompile",
        )
        outputs += [library, reflection]
    return [DefaultInfo(files = depset(outputs), runfiles = ctx.runfiles(files = outputs))]

_slang_metal = rule(
    implementation = _slang_metal_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".slang"], mandatory = True),
        "srcs": attr.label_list(allow_files = [".slang"]),
        "entries": attr.string_list(mandatory = True),
        "copts": attr.string_list(),
        "output_dir": attr.string(doc = "Module directory shared with DXIL entries (distinct entry names)."),
        "_slangc": attr.label(default = "//tools:slangc", executable = True, cfg = "exec", allow_single_file = True),
        "_slang_files": attr.label(default = "//tools:slang_files", cfg = "exec"),
        "_adapter": attr.label(default = "//tools:metal_bindings.py", allow_single_file = True),
    },
)

def slang_metal(**kwargs):
    """Slang compute to a native metallib with NRI's descriptor-table ABI; no Metal Shader Converter."""
    _slang_metal(target_compatible_with = ["@platforms//os:macos"], **kwargs)
