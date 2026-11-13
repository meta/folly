"""Provides helper functions for the folly library
[folly]
    have_libgflags_override = {True|[False]}
"""

load("@fbsource//tools/build_defs:cell_defs.bzl", "get_fbsource_cell")
load(
    "@fbsource//tools/build_defs:default_platform_defs.bzl",
    "ANDROID",
    "APPLE",
    "APPLETVOS",
    "CXX",
    "FBCODE",
    "IOS",
    "MACOSX",
    "WINDOWS",
)
load("@fbsource//tools/build_defs:fb_xplat_cxx_binary.bzl", "fb_xplat_cxx_binary")
load("@fbsource//tools/build_defs:fb_xplat_cxx_library.bzl", "fb_xplat_cxx_library")
load("@fbsource//tools/build_defs:fb_xplat_cxx_test.bzl", "fb_xplat_cxx_test")
load("@fbsource//tools/build_defs:sanitizers.bzl", "sanitizers")
load("@fbsource//tools/build_defs:selects.bzl", "selects")
load("@fbsource//tools/build_defs/dirsync:dirsync_redirect.bzl", "dirsync_redirect")
load("@fbsource//tools/target_determinator/macros:ci.bzl", "ci")
load("@fbsource//tools/target_determinator/macros:fbcode_ci_helpers.bzl", "fbcode_ci")
load("@prelude//utils:buckconfig.bzl", "read_bool")

def should_enable_gflags():
    return read_bool("folly", "have_libgflags_override", False)

def is_folly_mobile_flag():
    return native.read_config("cpp_flags", "preprocessing", "") == "DFOLLY_MOBILE"

def cpp_flags():
    # In the fbcode/server configuration (project/folly:mobile[disabled]),
    # xplat/folly must behave like fbcode//folly, which exports no folly feature
    # overrides and relies on the autoconf folly-config.h. Forcing the portable
    # (mobile) feature set there flips #if branches in the shared, dirsynced
    # headers -- e.g. disabling F14 vector intrinsics and changing jemalloc/type
    # layouts -- which breaks fbcode targets built against xplat/folly. So drop
    # these overrides when mobile is disabled and let autoconf match fbcode.
    flags = selects.with_or({
        "DEFAULT": [
            "-DFOLLY_HAVE_LIBJEMALLOC=0",
            "-DFOLLY_HAVE_PREADV=0",
            "-DFOLLY_HAVE_PWRITEV=0",
            "-DFOLLY_HAVE_TFO=0",
        ],
        # @fb-only[end= ]: ("ovr_config//os:linux", "ovr_config//os:macos"): select({
            # @fb-only[end= ]: "DEFAULT": [],
            # @fb-only[end= ]: "ovr_config//project/folly:mobile[enabled]": [
                # @fb-only[end= ]: "-DFOLLY_HAVE_LIBJEMALLOC=0",
                # @fb-only[end= ]: "-DFOLLY_HAVE_PREADV=0",
                # @fb-only[end= ]: "-DFOLLY_HAVE_PWRITEV=0",
                # @fb-only[end= ]: "-DFOLLY_HAVE_TFO=0",
            # @fb-only[end= ]: ],
        # @fb-only[end= ]: }),
    })

    if is_folly_mobile_flag():
        flags += select({
            "DEFAULT": [],
            "ovr_config//os:linux": ["-DFOLLY_MOBILE=1"],
        })

    else:
        flags += select({
            "DEFAULT": selects.with_or({
                "DEFAULT": ["-DFOLLY_MOBILE=1"],
                # @fb-only[end= ]: ("ovr_config//os:linux", "ovr_config//os:macos"): select({
                    # @fb-only[end= ]: "DEFAULT": [],
                    # @fb-only[end= ]: "ovr_config//project/folly:mobile[enabled]": ["-DFOLLY_MOBILE=1"],
                # @fb-only[end= ]: }),
                "ovr_config//os:windows": [],
            }),
            "ovr_config//build_mode:arvr_mode[enabled]": select({
                "DEFAULT": ["-DFOLLY_MOBILE=1"],
                "ovr_config//os:linux": [],
                "ovr_config//os:macos": [],
            }),
        })

    return flags

FBANDROID_CPPFLAGS = [
    "-DFOLLY_HAVE_IFUNC=0",
    "-DFOLLY_HAVE_INT128_T=0",
    "-DSSLCONTEXT_NO_REFCOUNT",
    "-D__STDC_FORMAT_MACROS",
    "-D__STDC_LIMIT_MACROS",
    "-D__STDC_CONSTANT_MACROS",
]

CLANG_CXX_FLAGS = [
    "-frtti",
    "-fexceptions",
    "-Wall",
    "-Werror",
    "-Wno-unused-variable",
    "-Wno-sign-compare",
    "-Wno-global-constructors",
    "-Wno-missing-prototypes",
    "-Wno-nullability-completeness",
    "-Wno-undef",
    "-Wno-unreachable-code",
    "-Wno-deprecated-declarations",
]
CXXFLAGS = select({
    "DEFAULT": [],
    "ovr_config//compiler:clang": CLANG_CXX_FLAGS,
})

FBOBJC_CXXFLAGS = ["-Os"]

FBANDROID_CXXFLAGS = [
    "-ffunction-sections",
    "-Wno-uninitialized",
]

WINDOWS_MSVC_CXXFLAGS = [
    "/EHs",
]

WINDOWS_CLANG_CXX_FLAGS = [
    "-Wno-deprecated-declarations",
    "-Wno-microsoft-cast",
    "-Wno-missing-braces",
    "-Wno-unused-function",
    "-Wno-undef",
    "-DBOOST_HAS_THREADS",
]

DEFAULT_APPLE_SDKS = (IOS, APPLETVOS, MACOSX)
DEFAULT_PLATFORMS = (CXX, ANDROID, APPLE, FBCODE, WINDOWS)

def _compute_include_directories():
    base_path = native.package_name()
    if base_path == "xplat/folly":
        return [".."]
    folly_path = base_path[6:]
    return ["/".join(len(folly_path.split("/")) * [".."])]

def folly_xplat_library(
    name,
    srcs = (),
    header_namespace = "",
    exported_headers = (),
    raw_headers = (),
    raw_headers_as_headers_mode = "enabled",
    deps = (),
    exported_deps = (),
    force_static = True,
    apple_sdks = None,
    platforms = None,
    enable_static_variant = False,
    labels = (),
    redirect = True,
    shared_under_folly_linkage = False,
    **kwargs,
):
    """Translate a simpler declaration into the more complete library target.

    When a redirect is configured via PACKAGE (using dirsync_redirect.write()),
    this macro creates:
    1. The actual implementation target with name "_{name}"
    2. An alias target with the original name that uses select() to choose
       between the local impl and the redirected target based on platform
       constraints.
    """

    # Check for redirect configuration
    redirect_config = dirsync_redirect.read()
    use_redirect = redirect and redirect_config != None and not name.startswith("_")
    impl_name = "_{}".format(name) if use_redirect else name

    # Set default platform settings. `()` means empty, whereas None
    # means default
    if apple_sdks == None:
        apple_sdks = DEFAULT_APPLE_SDKS
    if platforms == None:
        platforms = DEFAULT_PLATFORMS

    # We use gflags on fbcode platforms, which don't mix well when mixing static
    # and dynamic linking. Also disable for android host tests (robolectric on
    # macOS) — same rationale: force_static causes multiple JNI dylibs to each
    # statically link folly, producing duplicate gflag registrations that crash
    # on macOS (macOS lacks Linux's flat namespace symbol deduplication).
    force_static = select({
        "DEFAULT": selects.with_or({
            ("ovr_config//os/constraints:macos", "ovr_config//runtime/constraints:android-host-test", "ovr_config//runtime:fbcode"): False,
            "DEFAULT": force_static,
        }),
        "ovr_config//build_mode:arvr_mode[enabled]": force_static,
    })
    if shared_under_folly_linkage:
        force_static = select({
            "DEFAULT": force_static,
            # @fb-only[end= ]: "ovr_config//folly_linkage:folly_linkage[shared]": False,
        })
        if "preferred_linkage" in kwargs:
            kwargs["preferred_linkage"] = select({
                "DEFAULT": kwargs["preferred_linkage"],
                # @fb-only[end= ]: "ovr_config//folly_linkage:folly_linkage[shared]": "shared",
            })
        else:
            kwargs["cxx_preferred_linkage"] = select({
                "DEFAULT": None,
                # @fb-only[end= ]: "ovr_config//folly_linkage:folly_linkage[shared]": "shared",
            })
        kwargs["link_style"] = select({
            "DEFAULT": kwargs.get("link_style"),
            # @fb-only[end= ]: "ovr_config//folly_linkage:folly_linkage[shared]": "static",
        })

    # Preserve lib_name when redirecting to maintain SONAME
    if use_redirect:
        kwargs.setdefault("soname", dirsync_redirect.soname(name))

    fb_xplat_cxx_library(
        name = impl_name,
        srcs = srcs,
        header_namespace = header_namespace,
        exported_headers = exported_headers,
        raw_headers = raw_headers,
        raw_headers_as_headers_mode = raw_headers_as_headers_mode,
        public_include_directories = _compute_include_directories(),
        mangled_keys = [
            "name",
            "deps",
            "exported_deps",
            "provided_deps",
            "tests",
            "soname",
            "precompiled_header",
        ],
        deps = deps,
        exported_deps = exported_deps,
        force_static = force_static,
        apple_sdks = apple_sdks,
        platforms = platforms,
        enable_static_variant = enable_static_variant,
        labels = list(labels),
        compiler_flags = CXXFLAGS
        + kwargs.pop("compiler_flags", [])
        + select({
            "DEFAULT": [],
            "ovr_config//os:android": FBANDROID_CXXFLAGS,
            # TODO: Why appletvos, iphoneos, and macos are not marked as clang compilers?
            "ovr_config//os:appletvos": CLANG_CXX_FLAGS,
            "ovr_config//os:iphoneos": CLANG_CXX_FLAGS,
            "ovr_config//os:macos": CLANG_CXX_FLAGS + ["-fvisibility=default"],
            "ovr_config//os:watchos": CLANG_CXX_FLAGS,
        })
        + select({
            "DEFAULT": [],
            "ovr_config//os:windows-cl": WINDOWS_MSVC_CXXFLAGS,
            "ovr_config//os:windows-gcc-or-clang": WINDOWS_CLANG_CXX_FLAGS,
        })
        + [
            "-fexceptions",
            "-frtti",
        ],
        fbobjc_compiler_flags = kwargs.pop("fbobjc_compiler_flags", []) + FBOBJC_CXXFLAGS,
        windows_preferred_linkage = "static",
        visibility = kwargs.pop("visibility", ["PUBLIC"]),
        **kwargs,
    )

    # Create redirect alias if configured
    if use_redirect:
        dirsync_redirect.create_alias(
            name,
            redirect_config,
            platforms = platforms,
            apple_sdks = apple_sdks,
            enable_static_variant = enable_static_variant,
        )

def fbcode_ci_default_labels(*extra, python = False):
    """CI labels giving an xplat/folly target fbcode's default CI modes.

    fbcode//folly inherits `fbcode_ci.defaults()` from fbcode/PACKAGE, but
    xplat/folly does not. Returns `[]` in the fbcode cell, where the defaults
    already apply.

    Pass a target's own `ci.labels` arguments as `extra` rather than
    concatenating a separate `ci.labels(...)`: modifiers such as `ci.remove`
    must apply to the defaults too.

    Set `python` for python tests: fbcode's python macros switch their diff jobs
    to opt, but only in the fbcode cell.
    """
    if get_fbsource_cell() == "fbcode":
        return ci.labels(*extra) if extra else []
    defaults = [fbcode_ci.defaults()]
    if python:
        defaults.append(fbcode_ci.use_python_defaults())
    return ci.labels(*(defaults + list(extra)))

def folly_xplat_cxx_library(name, **kwargs):
    folly_xplat_library(name = name, **kwargs)

def folly_xplat_cxx_test(name, srcs = [], raw_headers = [], headers = [], deps = [], oncall = None, **kwargs):
    # resources and env are cherry picked because some of the other kwargs
    # have issues that need to be investigated.
    # e.g., Some args are duplicated. Some args cause TSAN errors.
    # TODO(T188948036): Fix xplat/folly:folly-futures-test and folly_xplat_cxx_test
    resources = kwargs.get("resources", [])
    env = kwargs.get("env", None)
    args = kwargs.get("args", None)
    labels = kwargs.get("labels", None)
    modifiers = kwargs.get("modifiers", None)
    constraint_overrides = kwargs.get("constraint_overrides", None)
    if constraint_overrides != None:
        # fb_xplat_cxx_test has no `constraint_overrides`; test targets are
        # top-level, so modifiers apply the same pin.
        modifiers = (modifiers or []) + constraint_overrides
    compiler_flags = kwargs.get("compiler_flags", None)
    preprocessor_flags = kwargs.get("preprocessor_flags", None)
    linker_flags = kwargs.get("linker_flags", None)
    link_style = kwargs.get("link_style", None)
    target_compatible_with = kwargs.get("target_compatible_with", None)

    extra_kwargs = {}
    if oncall != None:
        extra_kwargs["oncall"] = oncall
    if env != None:
        extra_kwargs["env"] = env
    if args != None:
        extra_kwargs["args"] = args
    if labels != None:
        extra_kwargs["labels"] = labels
    if modifiers != None:
        extra_kwargs["modifiers"] = modifiers
    if compiler_flags != None:
        extra_kwargs["compiler_flags"] = compiler_flags
    if preprocessor_flags != None:
        extra_kwargs["preprocessor_flags"] = preprocessor_flags
    extra_kwargs["linker_flags"] = (linker_flags if linker_flags != None else []) + select({
        "DEFAULT": [],
        "ovr_config//compiler:cl-windows": ["/INCLUDE:main"],
    })
    if link_style != None:
        extra_kwargs["link_style"] = link_style
    if target_compatible_with != None:
        extra_kwargs["target_compatible_with"] = target_compatible_with

    fb_xplat_cxx_test(
        name = name,
        srcs = srcs,
        raw_headers = raw_headers,
        headers = headers,
        resources = resources,
        include_directories = _compute_include_directories(),
        deps = deps
        + [
            "//xplat/folly/test/common:test_main",
        ],
        platforms = (CXX,),
        **extra_kwargs,
    )

def folly_xplat_cxx_binary(
    name, srcs = [], raw_headers = [], deps = [], oncall = None, dlopen_enabled = False, cxx_sanitizer_runtime_provider = sanitizers.RUNTIME_PROVIDER_TARGET, **kwargs
):
    if sanitizers.runtime_provider_includes_host(cxx_sanitizer_runtime_provider) and not dlopen_enabled:
        fail("cxx_sanitizer_runtime_provider = 'host' requires dlopen_enabled")

    extra_kwargs = {"oncall": oncall} if oncall != None else {}
    for key in ("compiler_flags", "labels", "linker_flags", "target_compatible_with"):
        if key in kwargs:
            extra_kwargs[key] = kwargs[key]
    clang_flags = kwargs.get("compiler_specific_flags", {}).get("clang")
    if clang_flags:
        extra_kwargs["compiler_flags"] = extra_kwargs.get("compiler_flags", []) + clang_flags

    if dlopen_enabled:
        extra_kwargs["link_style"] = kwargs.get("link_style")

    fb_xplat_cxx_binary(
        name = name,
        srcs = srcs,
        raw_headers = raw_headers,
        include_directories = _compute_include_directories(),
        deps = deps,
        platforms = (CXX,),
        cxx_sanitizer_runtime_provider = cxx_sanitizer_runtime_provider,
        **extra_kwargs,
    )
