def _vendored_hashlib_repository_impl(ctx):
    for src in ctx.attr.srcs:
        ctx.symlink(src, ctx.path(src).basename)
    ctx.symlink(ctx.attr.build_file, "BUILD.bazel")

_vendored_hashlib_repository = repository_rule(
    implementation = _vendored_hashlib_repository_impl,
    attrs = {
        "srcs": attr.label_list(mandatory = True, allow_files = True),
        "build_file": attr.label(mandatory = True, allow_single_file = True),
    },
)

def _vendored_hashlib_extension_impl(_ctx):
    _vendored_hashlib_repository(
        name = "vendored-hashlib",
        srcs = [
            Label("//:third_party/hashlib/hashlib.cc"),
            Label("//:third_party/hashlib/hashlib.h"),
        ],
        build_file = Label("//:dependency_support/hashlib.BUILD.bazel"),
    )

vendored_hashlib = module_extension(
    implementation = _vendored_hashlib_extension_impl,
)
