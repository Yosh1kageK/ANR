load("@rules_cc//cc:defs.bzl", "cc_library")

# The headers of an X.Org release (no library: GLFW loads the X11 client libraries at run time).
cc_library(
    name = "headers",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
