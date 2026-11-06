# Easy builds for Facebook projects

This directory contains tools designed to simplify continuous-integration
(and other builds) of Facebook open source projects.  In particular, this helps
manage builds for cross-project dependencies.

The main entry point is the `getdeps.py` script.  This script has several
subcommands, but the most notable is the `build` command.  This will download
and build all dependencies for a project, and then build the project itself.

`getdeps.py` requires Python 3.6 or newer and enforces this at startup.

## Deployment

This directory is copied literally into a number of different Facebook open
source repositories.  Any change made to code in this directory will be
automatically be replicated by our open source tooling into all GitHub hosted
repositories that use `fbcode_builder`.  Typically this directory is copied
into the open source repositories as `build/fbcode_builder/`, or as
`tools/fbcode_builder/` for a project whose manifest sets
`shipit_fbcode_builder_layout = tools`.


## Vendoring dependencies for offline builds

Distributions typically require builds to run without network access and to
ship third-party sources alongside the project.  `getdeps.py vendor
--output-dir DIR project` copies the source tree of every dependency that is
not satisfied by system packages to `DIR/<project>` and records what it
vendored in `DIR/getdeps-vendor.txt`.  A later `getdeps.py --vendor-dir DIR
build project` then takes those trees instead of fetching, and fails if a
dependency is missing from `DIR` rather than downloading it.  Pass the same
`--allow-system-packages` and `--no-tests` options to both commands so they
agree on the dependency set.

# Project Configuration Files

The `manifests` subdirectory contains configuration files for many different
projects, describing how to build each project.  These files also list
dependencies between projects, enabling `getdeps.py` to build all dependencies
for a project before building the project itself.


# Manifest features

A manifest may declare named *features* that toggle parts of its build. Each
feature appears as a `feature_<name>` variable that conditional sections such
as `[dependencies.feature_<name>=on]` or `[cmake.defines.feature_<name>=off]`
can gate on. Consumers of a project pick which features they want by attaching
a feature spec to the dep entry.

A project that supports features looks like:

```ini
[features]
default = rpc
rpc =

[cmake.defines.feature_rpc=off]
THRIFT_RPC=OFF

[dependencies.feature_rpc=on]
fizz
wangle
```

`default` lists features enabled when no consumer overrides; the other entries
declare valid feature names. There is no CLI flag for selecting features —
they are configured per dependency edge inside manifests.

A consumer's dep entry uses comma-separated tokens after `=`:

| Spec                            | Effect                                                |
| ------------------------------- | ----------------------------------------------------- |
| `fbthrift`                      | request the dep's default features                    |
| `fbthrift = extra`              | defaults plus `extra`                                 |
| `fbthrift = !default`           | this edge contributes no features                     |
| `fbthrift = !rpc`               | prohibit `rpc` (this edge and globally for the build) |
| `fbthrift = !default, extra`    | only `extra`, none of the dep's defaults              |

Resolution composes feature requests by union across all edges in the dep
graph, then checks prohibitions against the union; a conflict (one consumer
requests a feature another prohibits) is a hard error. Each project produces a
single build per invocation, so its resolved feature set is global to that
invocation. The resolved set is folded into the project's cache hash, so a
serialization-only build of `fbthrift` caches separately from a full build.



Since this directory is copied into many Facebook open source repositories,
it is also used to help share some CMake utility files across projects.  The
`CMake/` subdirectory contains a number of `.cmake` files that are shared by
the CMake-based build systems across several different projects.


# Older Build Scripts

This directory also still contains a handful of older build scripts that
pre-date the current `getdeps.py` build system.  Most of the other `.py` files
in this top directory, apart from `getdeps.py` itself, are from this older
build system.  This older system is only used by a few remaining projects, and
new projects should generally use the newer `getdeps.py` script, by adding a
new configuration file in the `manifests/` subdirectory.
