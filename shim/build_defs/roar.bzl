load("@fbsource//tools/build_defs:buckconfig.bzl", "read_bool")

def roar_no_jit():
    use_roar_jit = read_bool("fbcode", "use_roar_jit", required = False)
    if use_roar_jit:
        return ["-fforce-no-jit"]
    return []
