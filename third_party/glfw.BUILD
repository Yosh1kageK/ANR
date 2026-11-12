load("@rules_cc//cc:defs.bzl", "cc_library")

# GLFW's headers alone: NRIFramework's header declares GLFW types (the renderer's window is SDL3's).
cc_library(
    name = "headers",
    hdrs = glob(["include/GLFW/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
