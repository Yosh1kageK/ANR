"""http_files: downloads single files into one repository directory, each checked by its SHA-256."""

def _http_files_impl(ctx):
    for name, (url, sha256) in ctx.attr.files.items():
        ctx.download(url = url, output = name, sha256 = sha256)
    ctx.file("BUILD.bazel", ctx.attr.build_file_content or
                            ('filegroup(name = "all_files", srcs = glob(["**"], exclude = ["BUILD.bazel"]), ' +
                             'visibility = ["//visibility:public"])\n'))

http_files = repository_rule(
    implementation = _http_files_impl,
    attrs = {
        "files": attr.string_list_dict(mandatory = True, doc = "file name: [url, sha256]"),
        "build_file_content": attr.string(doc = "the repository's BUILD file (default: an all_files filegroup)"),
    },
)
