# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Run one fixed agent backtest scenario."""

from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import date
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterable, Sequence

from ..scripts import isolated_agent
from . import checkpoint_accounting, directory_snapshot


CommandRunner = Callable[..., subprocess.CompletedProcess[bytes]]

OUTPUT_DIR = "output"
DEFAULT_OUTPUT = "output/main.md"

CRITIC_ITERATE_RULE = PurePosixPath("critic-iterate.md")
TOOL_FILES = {
    "codex-reviewer.py": PurePosixPath("critic-iterate/codex-reviewer.py"),
    "reformat-md": PurePosixPath("scripts/reformat-md"),
    "session_current_model_id.py": PurePosixPath(
        "critic-iterate/session_current_model_id.py"
    ),
}
CHECKPOINT_TOOL_FILES = {
    "backtest-checkpoint": PurePosixPath("tests/checkpoint.py"),
}
CHECKPOINT_RUNTIME_FILES = (PurePosixPath("tests/directory_snapshot.py"),)
AGENT_RUNTIME_FILES = (PurePosixPath("scripts/isolated_agent.py"),)
RESERVED_INPUT_NAMES = {"AGENTS.md", "AGENTS.override.md"}
RESERVED_INPUT_ROOTS = {".codex", ".git", ".llms"}


class RunnerError(ValueError):
    pass


@dataclass(frozen=True)
class Mapping:
    source: PurePosixPath
    destination: PurePosixPath


@dataclass(frozen=True)
class Manifest:
    prompt: PurePosixPath
    inputs: tuple[Mapping, ...]
    rules: tuple[PurePosixPath, ...]
    no_rules_prompt: PurePosixPath | None = None
    packages_under_test: tuple[str, ...] = ()
    outputs: tuple[PurePosixPath, ...] = (PurePosixPath(DEFAULT_OUTPUT),)


@dataclass(frozen=True)
class Run:
    root: Path
    agent_workspace: isolated_agent.Workspace
    prompt: Path
    install_rules: bool
    critic_iterate_rounds: int | None
    checkpoint: bool
    outputs: tuple[PurePosixPath, ...] = (PurePosixPath(DEFAULT_OUTPUT),)

    @property
    def workdir(self) -> Path:
        return self.agent_workspace.task

    @property
    def agent_home(self) -> Path:
        return self.agent_workspace.home


@dataclass(frozen=True)
class Checkout:
    root: Path
    command: str


def _relative_path(
    value: Any, field: str, *, allow_parent: bool = False
) -> PurePosixPath:
    assert type(value) is str
    assert value
    path = PurePosixPath(value)
    if not path.parts or path.is_absolute():
        raise RunnerError(f"{field} must be a relative path: {value}")
    if not allow_parent and ".." in path.parts:
        raise RunnerError(f"{field} must be a relative path without '..': {value}")
    return path


def _is_development_doc(path: PurePosixPath) -> bool:
    return path.name in {
        "README.md",
        "TESTING.md",
        "TUTORIAL.md",
        "INSTALL.md",
        "CONTRIB.md",
    } or path.name.endswith(".contrib.md")


def _validate_input_destination(path: PurePosixPath) -> None:
    if path.parts[0] == "rules":
        raise RunnerError("input destinations may not use the reserved rules/")
    if path.name in RESERVED_INPUT_NAMES:
        raise RunnerError(f"input destination would be loaded as hidden policy: {path}")
    if path.parts[0] in RESERVED_INPUT_ROOTS:
        raise RunnerError(f"input destination uses a reserved policy root: {path}")


def load_manifest(path: Path) -> Manifest:
    raw = json.loads(path.read_text())
    assert type(raw) is dict
    manifest_keys = {"prompt", "inputs", "rules"}
    manifest_keys.update(set(raw) & {"no_rules_prompt", "outputs"})
    if "packages_under_test" in raw:
        manifest_keys.add("packages_under_test")
    assert set(raw) == manifest_keys
    prompt = _relative_path(raw["prompt"], "prompt")
    no_rules_prompt = (
        _relative_path(raw["no_rules_prompt"], "no_rules_prompt")
        if "no_rules_prompt" in raw
        else None
    )
    raw_inputs = raw["inputs"]
    raw_rules = raw["rules"]
    assert type(raw_inputs) is list
    assert type(raw_rules) is list

    inputs = []
    for index, raw_input in enumerate(raw_inputs):
        assert type(raw_input) is dict
        assert set(raw_input) == {"source", "destination"}
        inputs.append(
            Mapping(
                _relative_path(
                    raw_input["source"],
                    f"inputs[{index}].source",
                    allow_parent=True,
                ),
                _relative_path(
                    raw_input["destination"], f"inputs[{index}].destination"
                ),
            )
        )
        _validate_input_destination(inputs[-1].destination)

    rules = tuple(
        _relative_path(rule, f"rules[{index}]") for index, rule in enumerate(raw_rules)
    )
    for rule in rules:
        if _is_development_doc(rule):
            raise RunnerError(f"rules may not include development material: {rule}")
    packages = raw.get("packages_under_test", [])
    assert type(packages) is list
    for package in packages:
        package_path = _relative_path(package, "packages_under_test")
        assert len(package_path.parts) == 1 and package_path.suffix == ""
    raw_outputs = raw.get("outputs", [DEFAULT_OUTPUT])
    assert type(raw_outputs) is list
    outputs = tuple(
        _relative_path(output, f"outputs[{index}]")
        for index, output in enumerate(raw_outputs)
    )
    destinations: set[PurePosixPath] = set()
    for output in outputs:
        if len(output.parts) < 2 or output.parts[0] != OUTPUT_DIR:
            raise RunnerError(f"required output must be below output/: {output}")
        _claim(destinations, output)
    return Manifest(
        prompt, tuple(inputs), rules, no_rules_prompt, tuple(packages), outputs
    )


def _prompt_for_run(manifest: Manifest, install_rules: bool) -> PurePosixPath:
    if install_rules:
        return manifest.prompt
    if manifest.no_rules_prompt is None:
        raise RunnerError("--no-rules requires no_rules_prompt in scenario.json")
    return manifest.no_rules_prompt


def _checkpoint_instruction() -> str:
    return (
        "Immediately after writing the initial draft, run exactly "
        "`backtest-checkpoint 0`; print and follow its stdout.\n\n"
    )


def _rule_loading_instruction(rules_root: Path) -> str:
    return (
        f"For this run, `{{FA}}` means `{rules_root}`. Read every rule listed "
        "in `{FA}/rules-inventory.md`, in order. Follow those rules "
        "for conditional loads; do not look for ambient rule files.\n\n"
    )


def _validate_critic_iterate_rounds(
    manifest: Manifest,
    install_rules: bool,
    critic_iterate_rounds: int | None,
) -> None:
    if critic_iterate_rounds is None:
        return
    if critic_iterate_rounds < 0:
        raise RunnerError("critic-iterate rounds must be nonnegative")
    if not install_rules:
        raise RunnerError("critic-iterate rounds cannot be set with --no-rules")
    if CRITIC_ITERATE_RULE not in manifest.rules:
        raise RunnerError(
            "critic-iterate rounds require critic-iterate.md in the scenario rules"
        )


def _uses_checkpoints(manifest: Manifest, install_rules: bool) -> bool:
    """Enable phased checkpoints for scenarios that author a new draft."""
    return (
        install_rules
        and CRITIC_ITERATE_RULE in manifest.rules
        # For now, review-only scenarios compare their staged draft with the output.
        and not any(
            mapping.destination.parts[0] == OUTPUT_DIR for mapping in manifest.inputs
        )
    )


def _resolve_below(root: Path, relative: PurePosixPath, field: str) -> Path:
    root = root.resolve()
    path = (root / relative).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise RunnerError(f"{field} resolves outside {root}: {relative}") from error
    return path


def _normalize_cpp_snapshot(path: PurePosixPath) -> PurePosixPath:
    if path.name.endswith((".h.txt", ".cpp.txt")):
        return path.with_name(path.name[:-4])
    return path


def _mapped_files(
    scenario: Path, mapping: Mapping
) -> Iterable[tuple[PurePosixPath, bytes]]:
    source = (scenario / mapping.source).resolve()
    if source.is_file():
        yield _normalize_cpp_snapshot(mapping.destination), source.read_bytes()
        return
    if not source.is_dir():
        raise RunnerError(f"input does not exist: {mapping.source}")
    for path in sorted(source.rglob("*")):
        if path.is_symlink():
            raise RunnerError(f"input trees may not contain symlinks: {path}")
        if not path.is_file():
            continue
        relative = _normalize_cpp_snapshot(
            PurePosixPath(path.relative_to(source).as_posix())
        )
        yield mapping.destination / relative, path.read_bytes()


def _find_checkout(path: Path) -> Checkout:
    start = path if path.is_dir() else path.parent
    for directory in (start, *start.parents):
        if (directory / ".hg").exists():
            return Checkout(directory, "sl")
        if (directory / ".git").exists():
            return Checkout(directory, "git")
    raise RunnerError(f"generation files are not in a source checkout: {path}")


def _run_source_control(
    checkout: Checkout,
    arguments: Sequence[str],
    command_runner: CommandRunner,
) -> str:
    result = command_runner(
        [checkout.command, *arguments],
        cwd=checkout.root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        error = result.stderr.decode(errors="replace").strip()
        raise RunnerError(
            f"{checkout.command} {' '.join(arguments)} failed"
            + (f": {error}" if error else "")
        )
    return result.stdout.decode(errors="replace").strip()


def _package_files(
    rules_root: Path, packages: tuple[str, ...]
) -> tuple[PurePosixPath, ...]:
    if not packages:
        return ()
    rules_root = rules_root.resolve()
    checkout = _find_checkout(rules_root)
    relative_root = rules_root.relative_to(checkout.root)
    # Include both the committed and working tracked sets: removed files and
    # new tracked additions must still reach the clean-source check.
    commands = (
        (["files", "-r", ".", "-0"], ["files", "-0"])
        if checkout.command == "sl"
        else (["ls-tree", "-r", "--name-only", "-z", "HEAD"], ["ls-files", "-z"])
    )
    tracked = set()
    for command in commands:
        tracked.update(
            _run_source_control(
                checkout, [*command, "--", str(relative_root)], subprocess.run
            ).split("\0")
        )
    files = []
    found = set()
    for name in sorted(tracked - {""}):
        path = PurePosixPath(name).relative_to(relative_root.as_posix())
        if (
            path.suffix != ".md"
            or _is_development_doc(path)
            or {"tests", "backtest"}.intersection(path.parts)
        ):
            continue
        package = (
            path.parts[0] if len(path.parts) > 1 else path.stem.removesuffix(".loader")
        )
        if package not in packages:
            continue
        if any(
            (rules_root / ancestor).is_symlink() for ancestor in (path, *path.parents)
        ):
            raise RunnerError(f"rule packages may not contain symlinks: {path}")
        files.append(path)
        found.add(package)
    missing = set(packages) - found
    if missing:
        raise RunnerError(
            "no tracked operational Markdown for packages: "
            + ", ".join(sorted(missing))
        )
    return tuple(files)


def _generation_sources(
    scenario: Path,
    manifest: Manifest,
    rules_root: Path,
    runner_path: Path,
    install_rules: bool,
    package_files: tuple[PurePosixPath, ...],
) -> tuple[Path, ...]:
    sources = [
        runner_path.resolve(),
        (runner_path.parent / "directory_snapshot.py").resolve(),
        (scenario / "scenario.json").resolve(),
        _resolve_below(scenario, _prompt_for_run(manifest, install_rules), "prompt"),
    ]
    sources.extend((scenario / mapping.source).resolve() for mapping in manifest.inputs)
    if install_rules and not set(manifest.rules).issubset(package_files):
        raise RunnerError("startup rules must be tracked files in packages_under_test")
    tool_files = TOOL_FILES.values() if install_rules else ()
    if _uses_checkpoints(manifest, install_rules):
        tool_files = (
            *tool_files,
            *CHECKPOINT_TOOL_FILES.values(),
            *CHECKPOINT_RUNTIME_FILES,
        )
        sources.append((runner_path.parent / "checkpoint_accounting.py").resolve())
    sources.extend(
        _resolve_below(rules_root, path, "rule")
        for path in (
            *AGENT_RUNTIME_FILES,
            *(package_files if install_rules else ()),
            *tool_files,
        )
    )
    return tuple(dict.fromkeys(sources))


def generation_revision(
    scenario: Path,
    manifest: Manifest,
    rules_root: Path,
    *,
    install_rules: bool = True,
    runner_path: Path | None = None,
    command_runner: CommandRunner = subprocess.run,
    package_files: tuple[PurePosixPath, ...] | None = None,
) -> str:
    if runner_path is None:
        runner_path = Path(__file__)
    if package_files is None:
        package_files = (
            _package_files(rules_root, manifest.packages_under_test)
            if install_rules
            else ()
        )
    sources = _generation_sources(
        scenario,
        manifest,
        rules_root,
        runner_path,
        install_rules,
        package_files,
    )
    checkout = _find_checkout(sources[0])
    relative_sources = []
    for source in sources:
        source_checkout = _find_checkout(source)
        if source_checkout != checkout:
            raise RunnerError("generation files must come from one source checkout")
        relative_sources.append(str(source.relative_to(checkout.root)))

    if checkout.command == "sl":
        status_arguments = [
            "status",
            "-mardui",
            "--root-relative",
            "--",
            *relative_sources,
        ]
        revision_arguments = ["log", "-r", ".", "-T", "{node}\\n"]
    else:
        status_arguments = [
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
            "--ignored=matching",
            "--",
            *relative_sources,
        ]
        revision_arguments = ["rev-parse", "HEAD"]

    status = _run_source_control(checkout, status_arguments, command_runner)
    if status:
        raise RunnerError(
            "generation files contain local changes or untracked/ignored files; "
            "refusing to run.\nCommit or amend source files, remove ignored "
            "artifacts, or ask the user to authorize a commit containing only "
            "the listed source files, then retry:\n" + status
        )
    revision = _run_source_control(checkout, revision_arguments, command_runner)
    if not revision:
        raise RunnerError("source control returned an empty generation revision")
    return revision


def _claim(destinations: set[PurePosixPath], destination: PurePosixPath) -> None:
    if destination in destinations:
        raise RunnerError(f"two files map to {destination}")
    if any(parent in destinations for parent in destination.parents) or any(
        destination in existing.parents for existing in destinations
    ):
        raise RunnerError(f"file/directory destination collision at {destination}")
    destinations.add(destination)


def _rules_inventory(rules: tuple[PurePosixPath, ...]) -> bytes:
    listed_rules = "\n".join(
        f"{index}. `{rule.as_posix()}`" for index, rule in enumerate(rules, 1)
    )
    if not listed_rules:
        listed_rules = "No rule files were selected."
    return (
        "# Rules inventory\n\n"
        "Read these files in order, resolving each path relative to this file:\n\n"
        f"{listed_rules}\n\n"
        "Other files in the declared packages are staged too. "
        "Read them only when directed.\n"
    ).encode()


def stage(
    scenario: Path,
    manifest: Manifest,
    rules_root: Path,
    workdir: Path,
    *,
    install_rules: bool = True,
    runtime_files: Sequence[PurePosixPath] = (),
    package_files: tuple[PurePosixPath, ...] | None = None,
) -> None:
    rules_root = rules_root.resolve()
    if not rules_root.is_dir():
        raise RunnerError(f"rules root is not a directory: {rules_root}")
    plan: list[tuple[PurePosixPath, bytes]] = []
    destinations: set[PurePosixPath] = set()
    for mapping in manifest.inputs:
        for destination, contents in _mapped_files(scenario, mapping):
            _validate_input_destination(destination)
            _claim(destinations, destination)
            plan.append((destination, contents))

    if install_rules:
        if package_files is None:
            package_files = _package_files(rules_root, manifest.packages_under_test)
        if not set(manifest.rules).issubset(package_files):
            raise RunnerError(
                "startup rules must be tracked files in packages_under_test"
            )
        inventory = PurePosixPath("rules/rules-inventory.md")
        _claim(destinations, inventory)
        plan.append((inventory, _rules_inventory(manifest.rules)))
        # Normal runs reject local deletions in generation_revision() before staging.
        for rule in dict.fromkeys((*package_files, *runtime_files)):
            destination = PurePosixPath("rules") / rule
            _claim(destinations, destination)
            source = _resolve_below(rules_root, rule, "rule")
            if not source.is_file():
                raise RunnerError(f"rule file does not exist: {rule}")
            plan.append((destination, source.read_bytes()))

    for destination, contents in plan:
        path = workdir / destination
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        if destination.parts[0] == OUTPUT_DIR:
            path.chmod(0o644)
        else:
            path.chmod(path.stat().st_mode & 0o555)


def prepare(
    scenario: Path,
    rules_root: Path,
    run_root: Path,
    model: str,
    effort: str,
    generation_revision: str,
    *,
    install_rules: bool = True,
    critic_iterate_rounds: int | None = None,
    package_files: tuple[PurePosixPath, ...] | None = None,
) -> Run:
    scenario = scenario.resolve()
    if not scenario.is_dir():
        raise RunnerError(f"scenario is not a directory: {scenario}")
    rules_root = rules_root.resolve()
    run_root = run_root.resolve()
    if run_root.is_relative_to(scenario):
        raise RunnerError("run root may not be inside the scenario")
    if run_root.is_relative_to(rules_root):
        raise RunnerError("run root may not be inside the rules tree")
    manifest_path = scenario / "scenario.json"
    manifest = load_manifest(manifest_path)
    _validate_critic_iterate_rounds(manifest, install_rules, critic_iterate_rounds)
    checkpoint_run = _uses_checkpoints(manifest, install_rules)
    prompt_source = _prompt_for_run(manifest, install_rules)
    prompt = _resolve_below(scenario, prompt_source, "prompt")
    if not prompt.is_file():
        raise RunnerError(f"prompt does not exist: {prompt_source}")

    run_root.mkdir(parents=True, exist_ok=True)
    root = Path(
        tempfile.mkdtemp(prefix=f"{date.today():%Y%m%d}-{scenario.name}-", dir=run_root)
    )
    try:
        agent_workspace = isolated_agent.CODEX.prepare(root, initialize_engine=False)
        run = Run(
            root,
            agent_workspace,
            root / "author-prompt.md",
            install_rules,
            critic_iterate_rounds,
            checkpoint_run,
            manifest.outputs,
        )
        prompt_prefix = (
            _rule_loading_instruction(run.workdir / "rules") if install_rules else ""
        )
        if critic_iterate_rounds is not None:
            prompt_prefix += f"c-i-{critic_iterate_rounds}\n\n"
        if checkpoint_run:
            prompt_prefix += _checkpoint_instruction()
        run.prompt.write_text(prompt_prefix + prompt.read_text())
        run.prompt.chmod(0o444)
        shutil.copyfile(manifest_path, root / "scenario.json")
        runtime_files = (*TOOL_FILES.values(), *AGENT_RUNTIME_FILES)
        if checkpoint_run:
            runtime_files += (
                *CHECKPOINT_TOOL_FILES.values(),
                *CHECKPOINT_RUNTIME_FILES,
            )
        stage(
            scenario,
            manifest,
            rules_root,
            run.workdir,
            install_rules=install_rules,
            runtime_files=runtime_files,
            package_files=package_files,
        )
        (run.workdir / OUTPUT_DIR).mkdir(exist_ok=True)
        metadata: dict[str, object] = {
            "model": model,
            "reasoning_effort": effort,
            "generation_revision": generation_revision,
            "rules": (
                [rule.as_posix() for rule in manifest.rules] if install_rules else []
            ),
            "scenario": str(scenario),
            "status": "prepared",
        }
        if not install_rules:
            metadata["no_rules"] = True
        if manifest.outputs != (PurePosixPath(DEFAULT_OUTPUT),):
            metadata["outputs"] = [output.as_posix() for output in manifest.outputs]
        if critic_iterate_rounds is not None:
            metadata["critic_iterate_rounds"] = critic_iterate_rounds
        if checkpoint_run:
            metadata["checkpoint_key"] = secrets.token_hex(16)
        (root / "run.json").write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n"
        )
    except (OSError, RunnerError, isolated_agent.IsolationError):
        shutil.rmtree(root, ignore_errors=True)
        raise
    return run


def _resolve_executable(value: str | Path, name: str) -> Path:
    resolved = shutil.which(os.fspath(value))
    if resolved is None:
        raise RunnerError(f"{name} is not executable: {value}")
    return Path(resolved).resolve()


def _install_tools(run: Run) -> tuple[Path, dict[str, Path]]:
    tool_bin = run.root / "bin"
    tool_bin.mkdir()
    lines = []
    selected_tools = TOOL_FILES
    if run.checkpoint:
        selected_tools = {**selected_tools, **CHECKPOINT_TOOL_FILES}
    tools = {}
    for name, relative in selected_tools.items():
        executable = _resolve_below(run.workdir / "rules", relative, f"{name} tool")
        executable.chmod(0o555)
        tools[name] = _resolve_executable(executable, name)
    for name, executable in tools.items():
        shim = tool_bin / name
        if executable.name == name:
            shim.symlink_to(executable)
            executable_paths = [str(shim), str(executable)]
        else:
            shutil.copyfile(executable, shim)
            shim.chmod(0o755)
            executable_paths = [str(shim)]
        lines.extend(
            [
                f"host_executable(name={json.dumps(name)}, "
                f"paths={json.dumps(executable_paths)})",
                f'prefix_rule(pattern=[{json.dumps(name)}], decision="allow", '
                'justification="Tool required by the staged rules.")',
            ]
        )
    if run.checkpoint:
        for relative in CHECKPOINT_RUNTIME_FILES:
            shutil.copyfile(run.workdir / "rules" / relative, tool_bin / relative.name)
            (tool_bin / relative.name).chmod(0o444)
        # The installed reviewer wrapper writes its attempts here for accounting.
        (run.root / "reviews").mkdir(mode=0o700)
    policy = run.agent_home / "rules" / "default.rules"
    policy.parent.mkdir(parents=True)
    policy.write_text("\n".join(lines) + "\n")
    return tool_bin, tools


def _update_metadata(run: Run, **updates: object) -> None:
    path = run.root / "run.json"
    metadata = json.loads(path.read_text())
    metadata.update(updates)
    path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")


def _preserve_output(run: Run) -> bool:
    # The worker may replace its task or workspace directory; neither is a
    # trusted parent alias for the output tree.
    for directory in (run.agent_workspace.workspace_root, run.workdir):
        if not stat.S_ISDIR(directory.lstat().st_mode):
            raise RunnerError(
                f"output ancestor is not a regular directory: {directory}"
            )
    entries = directory_snapshot.read(run.workdir / OUTPUT_DIR)
    has_output = any(entries.values())
    for relative in run.outputs:
        if (run.workdir / relative).is_dir():
            raise RunnerError(f"{relative} is not a regular file")
        if not entries.get(Path(relative.relative_to(OUTPUT_DIR))):
            has_output = False
    directory_snapshot.write(entries, run.root / OUTPUT_DIR)
    if directory_snapshot.read(run.root / OUTPUT_DIR) != entries:
        raise RunnerError("saved output/ does not match the captured artifact")
    return has_output


def _author_environment_additions(run: Run, tool_bin: Path | None) -> dict[str, str]:
    environment = {}
    if tool_bin is not None:
        inherited_path = os.environ.get("PATH")
        environment["PATH"] = (
            os.pathsep.join([str(tool_bin), inherited_path])
            if inherited_path
            else str(tool_bin)
        )
    if run.checkpoint:
        # Checkpoints use both paths. Reviewers inherit the run root so their
        # nested calls remain attributable to this run.
        environment["FOLLY_BACKTEST_RUN_DIR"] = str(run.root)
        environment["FOLLY_BACKTEST_WORKDIR"] = str(run.workdir)
    return environment


def _finish_run(run: Run, returncode: int) -> int:
    """Keep worker failures primary when saving output also fails.

    The original workspace remains available for diagnosis.
    """
    if returncode:
        _update_metadata(run, status="failed", exit_code=returncode)
        try:
            _preserve_output(run)
        except (OSError, ValueError) as error:
            _update_metadata(run, output_preservation_error=str(error))
        return returncode
    try:
        has_output = _preserve_output(run)
    except (OSError, ValueError) as error:
        _update_metadata(
            run,
            status="invalid-output",
            exit_code=2,
            output_preservation_error=str(error),
        )
        return 2
    if not has_output:
        _update_metadata(run, status="missing-output", exit_code=2)
        return 2
    if run.checkpoint:
        try:
            records = checkpoint_accounting.collect(
                run.root,
                run.critic_iterate_rounds,
                agent_home=run.agent_home,
            )
        except Exception:
            # The author succeeded; preserve the accounting traceback instead of
            # reporting it as an author-process exit code.
            _update_metadata(run, status="invalid-checkpoints")
            raise
        _update_metadata(run, checkpoint_count=len(records))
    _update_metadata(run, status="complete", exit_code=0)
    return 0


def launch(
    run: Run,
    codex: str | Path,
    model: str,
    effort: str,
    command_runner: CommandRunner = subprocess.run,
) -> int:
    try:
        codex_path = _resolve_executable(codex, "codex")
        tool_bin = None
        executables = {"codex": codex_path}
        if run.install_rules:
            tool_bin, tools = _install_tools(run)
            executables.update(tools)
        _update_metadata(
            run,
            executables={
                name: str(executable) for name, executable in executables.items()
            },
        )
    except (OSError, RunnerError):
        _update_metadata(run, status="launch-error")
        raise

    try:
        isolated_agent.CODEX.initialize(
            run.agent_workspace, command_runner=command_runner
        )
        with (
            run.prompt.open("rb") as stdin,
            (run.root / "trace.jsonl").open("wb") as stdout,
            (run.root / "err.txt").open("wb") as stderr,
        ):
            result = isolated_agent.CODEX.run(
                run.agent_workspace,
                isolated_agent.Request(
                    model=model,
                    effort=effort,
                    working_directory="task",
                ),
                stdin=stdin,
                stdout=stdout,
                stderr=stderr,
                additions=_author_environment_additions(run, tool_bin),
                executable=codex_path,
                command_runner=command_runner,
            )
    except (OSError, RunnerError, isolated_agent.IsolationError):
        _update_metadata(run, status="launch-error")
        raise
    return _finish_run(run, result.returncode)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "scenario", type=Path, help="directory containing scenario.json"
    )
    parser.add_argument(
        "--run-root",
        default=Path(tempfile.gettempdir()) / "folly-agents-backtest-runs",
        type=Path,
        help="run root (default: system temporary directory)",
    )
    parser.add_argument("--model", required=True, help="author model")
    parser.add_argument(
        "--reasoning-effort",
        required=True,
        choices=("none", "low", "medium", "high", "xhigh", "max", "ultra"),
        help="author reasoning effort",
    )
    parser.add_argument(
        "--prepare-only",
        action="store_true",
        help="stage inputs without starting Codex",
    )
    parser.add_argument(
        "--no-rules",
        action="store_true",
        help="run with no_rules_prompt and inputs but no staged rules",
    )
    parser.add_argument(
        "--critic-iterate-rounds",
        metavar="K",
        type=int,
        help="set the external critic-iterate review budget",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        agents_root = Path(__file__).resolve().parent.parent
        scenario = args.scenario.resolve()
        manifest = load_manifest(scenario / "scenario.json")
        install_rules = not args.no_rules
        package_files = (
            _package_files(agents_root, manifest.packages_under_test)
            if install_rules
            else ()
        )
        _validate_critic_iterate_rounds(
            manifest, install_rules, args.critic_iterate_rounds
        )
        revision = generation_revision(
            scenario,
            manifest,
            agents_root,
            install_rules=install_rules,
            package_files=package_files,
        )
        run = prepare(
            scenario,
            agents_root,
            args.run_root,
            args.model,
            args.reasoning_effort,
            revision,
            install_rules=install_rules,
            critic_iterate_rounds=args.critic_iterate_rounds,
            package_files=package_files,
        )
        print(run.root)
        if args.prepare_only:
            return 0
        return launch(
            run,
            "codex",
            args.model,
            args.reasoning_effort,
        )
    except (
        RunnerError,
        isolated_agent.IsolationError,
        json.JSONDecodeError,
        OSError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
