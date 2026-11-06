# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import os
import select
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from shlex import quote as shellquote

from .envfuncs import Env
from .getdeps_platform import is_windows


class RunCommandError(Exception):
    pass


def make_memory_limit_preexec_fn(
    job_weight_mib: int,
) -> Callable[[], None] | None:
    """Create a preexec_fn that sets a per-process virtual memory limit.

    When getdeps spawns build commands (cmake -> ninja -> N compiler processes),
    the parallelism is computed from available RAM divided by job_weight_mib.
    However, there is no enforcement of that budget: if a compiler or linker
    process exceeds its expected memory usage, the system can run out of RAM
    and the Linux OOM killer may terminate arbitrary processes — including the
    user's shell or terminal.

    This function returns a callable suitable for subprocess.Popen's preexec_fn
    parameter. It runs in each child process after fork() but before exec(),
    setting RLIMIT_AS (virtual address space limit) so that a runaway process
    gets a failed allocation (std::bad_alloc / ENOMEM) instead of triggering
    the OOM killer. The limit is inherited by all descendant processes (ninja,
    compiler invocations, etc.).

    The per-process limit is set to job_weight_mib * 10. The 10x multiplier
    accounts for the fact that RLIMIT_AS caps virtual address space, which is
    typically 2-4x larger than resident (physical) memory for C++ compilers
    due to memory-mapped files, shared libraries, and address space reservations
    that don't consume physical RAM. The multiplier is intentionally generous:
    the goal is a safety net that catches genuine runaways before the OOM killer
    fires, not a tight per-job budget.

    Only applies on Linux, where the OOM killer is the problem. Returns None
    on other platforms, or when GETDEPS_NO_MEMORY_LIMIT is set: AddressSanitizer
    binaries reserve terabytes of address space for shadow memory at startup,
    so any that run during the build (test discovery, code generators) fail
    under any practical limit.
    """
    if sys.platform != "linux" or os.environ.get("GETDEPS_NO_MEMORY_LIMIT"):
        return None

    # Each job is budgeted job_weight_mib of physical RAM. Virtual address
    # space is typically 2-4x RSS. Use 10x as a generous safety net: tight
    # enough to stop a runaway process before the OOM killer fires, but loose
    # enough to avoid false positives from normal virtual memory overhead.
    limit_bytes: int = job_weight_mib * 10 * 1024 * 1024

    def _set_memory_limit() -> None:
        import resource

        resource.setrlimit(resource.RLIMIT_AS, (limit_bytes, limit_bytes))

    return _set_memory_limit


def _print_env_diff(env: Env, log_fn: Callable[[str], None]) -> None:
    current_keys = set(os.environ.keys())
    wanted_env = set(env.keys())

    unset_keys = current_keys.difference(wanted_env)
    for k in sorted(unset_keys):
        log_fn("+ unset %s\n" % k)

    added_keys = wanted_env.difference(current_keys)
    for k in wanted_env.intersection(current_keys):
        if os.environ[k] != env[k]:
            added_keys.add(k)

    for k in sorted(added_keys):
        if ("PATH" in k) and (os.pathsep in env[k]):
            log_fn("+ %s=\\\n" % k)
            for elem in env[k].split(os.pathsep):
                log_fn("+      %s%s\\\n" % (shellquote(elem), os.pathsep))
        else:
            log_fn("+ %s=%s \\\n" % (k, shellquote(env[k])))


def check_cmd(
    cmd: list[str],
    env: Env | None = None,
    cwd: str | Path | None = None,
    allow_fail: bool = False,
    log_file: str | Path | None = None,
) -> None:
    """Run the command and abort on failure"""
    rc = run_cmd(cmd, env=env, cwd=cwd, allow_fail=allow_fail, log_file=log_file)
    if rc != 0:
        raise RuntimeError(f"Failure exit code {rc} for command {cmd}")


def run_cmd(
    cmd: list[str],
    env: Env | None = None,
    cwd: str | Path | None = None,
    allow_fail: bool = False,
    log_file: str | Path | None = None,
    preexec_fn: Callable[[], None] | None = None,
) -> int:
    def log_to_stdout(msg: str) -> None:
        sys.stdout.buffer.write(msg.encode(errors="surrogateescape"))

    if log_file is not None:
        with open(log_file, "a", encoding="utf-8", errors="surrogateescape") as log:

            def log_function(msg: str) -> None:
                log.write(msg)
                log_to_stdout(msg)

            return _run_cmd(
                cmd,
                env=env,
                cwd=cwd,
                allow_fail=allow_fail,
                log_fn=log_function,
                preexec_fn=preexec_fn,
            )
    else:
        return _run_cmd(
            cmd,
            env=env,
            cwd=cwd,
            allow_fail=allow_fail,
            log_fn=log_to_stdout,
            preexec_fn=preexec_fn,
        )


def _run_cmd(
    cmd: list[str],
    env: Env | None,
    cwd: str | Path | None,
    allow_fail: bool,
    log_fn: Callable[[str], None],
    preexec_fn: Callable[[], None] | None = None,
) -> int:
    log_fn("---\n")
    try:
        cmd_str = " \\\n+      ".join(shellquote(arg) for arg in cmd)
    except TypeError:
        # eg: one of the elements is None
        raise RunCommandError("problem quoting cmd: %r" % cmd)

    if env:
        assert isinstance(env, Env)
        _print_env_diff(env, log_fn)

        # Convert from our Env type to a regular dict.
        # This is needed because python3 looks up b'PATH' and 'PATH'
        # and emits an error if both are present.  In our Env type
        # we'll return the same value for both requests, but we don't
        # have duplicate potentially conflicting values which is the
        # spirit of the check.
        env_dict: dict[str, str] | None = dict(env.items())
    else:
        env_dict = None

    if cwd:
        # Normalize to str: the prefix surgery below is string manipulation,
        # and subprocess receives the normalized value.
        cwd_str = os.fspath(cwd)
        log_fn("+ cd %s && \\\n" % shellquote(cwd_str))
        # Our long path escape sequence may confuse cmd.exe, so if the cwd
        # is short enough, strip that off.
        if is_windows() and (len(cwd_str) < 250) and cwd_str.startswith("\\\\?\\"):
            cwd_str = cwd_str[4:]
    else:
        cwd_str = None

    log_fn("+ %s\n" % cmd_str)

    isinteractive = os.isatty(sys.stdout.fileno())
    if isinteractive:
        stdout = None
        sys.stdout.buffer.flush()
    else:
        stdout = subprocess.PIPE

    try:
        p = subprocess.Popen(
            cmd,
            env=env_dict,
            cwd=cwd_str,
            stdout=stdout,
            stderr=subprocess.STDOUT,
            preexec_fn=preexec_fn,
        )
    except (TypeError, ValueError, OSError) as exc:
        log_fn("error running `%s`: %s" % (cmd_str, exc))
        raise RunCommandError(
            "%s while running `%s` with env=%r\nos.environ=%r"
            % (str(exc), cmd_str, env_dict, os.environ)
        )

    if not isinteractive:
        _pipe_output(p, log_fn)

    p.wait()
    if p.returncode != 0 and not allow_fail:
        raise subprocess.CalledProcessError(p.returncode, cmd)

    return p.returncode


if hasattr(select, "poll"):

    def _pipe_output(p: subprocess.Popen[bytes], log_fn: Callable[[str], None]) -> None:
        """Read output from p.stdout and call log_fn() with each chunk of data as it
        becomes available."""
        # Perform non-blocking reads
        import fcntl

        assert p.stdout is not None
        fcntl.fcntl(p.stdout.fileno(), fcntl.F_SETFL, os.O_NONBLOCK)
        poll = select.poll()
        poll.register(p.stdout.fileno(), select.POLLIN)

        buffer_size = 4096
        while True:
            poll.poll()
            data = p.stdout.read(buffer_size)
            if not data:
                break
            # log_fn() accepts arguments as str (binary in Python 2, unicode in
            # Python 3).  In Python 3 the subprocess output will be plain bytes,
            # and need to be decoded.
            if not isinstance(data, str):
                data = data.decode("utf-8", errors="surrogateescape")
            log_fn(data)

else:

    def _pipe_output(p: subprocess.Popen[bytes], log_fn: Callable[[str], None]) -> None:
        """Read output from p.stdout and call log_fn() with each chunk of data as it
        becomes available."""
        # Perform blocking reads.  Use a smaller buffer size to avoid blocking
        # for very long when data is available.
        assert p.stdout is not None
        buffer_size = 64
        while True:
            data = p.stdout.read(buffer_size)
            if not data:
                break
            # log_fn() accepts arguments as str (binary in Python 2, unicode in
            # Python 3).  In Python 3 the subprocess output will be plain bytes,
            # and need to be decoded.
            if not isinstance(data, str):
                data = data.decode("utf-8", errors="surrogateescape")
            log_fn(data)
