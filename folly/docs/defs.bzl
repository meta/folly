load("@bazel_skylib//lib:paths.bzl", "paths")
load("@bazel_skylib//lib:shell.bzl", "shell")
load("@fbcode_macros//build_defs:custom_rule.bzl", "custom_rule")

#
# Helper functions to emit fbconfig rules
#

# Helper function to copy a file to the output directory
def copy(path, name = None, **_kwargs):
    custom_rule(
        name = name or path,
        srcs = [path],
        build_args = shell.quote(path),
        build_script_dep = "fbsource//xplat/folly/docs/facebook:copy.py",
        output_gen_files = [path],
        has_content_based_path = False,
    )

# Helper function to define a custom_rule() that will emit the HTML output.
def html(src, name = None, support = None, style = "style.css", copy_support = True, **_kwargs):
    html_name = name or (paths.split_extension(src)[0] + ".html")

    if support == None:
        support = []

    custom_rule(
        name = html_name,
        srcs = [src, style] + support,
        build_args = "--style %s %s" % (shell.quote(style), shell.quote(src)) + " --pandoc-path $(exe fbsource//third-party/stackage-lts:pandoc)",
        build_script_dep = "fbsource//xplat/folly/docs/facebook:build_html.py",
        output_gen_files = [paths.split_extension(src)[0] + ".html"],
        has_content_based_path = False,
    )

    if copy_support:
        for path in support:
            copy(path)
