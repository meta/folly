# Agent rules

## Only for humans & agents editing rules

`README.md`, `TESTING.md`, every `CONTRIB.md`, and `*.contrib.md` are
development material, not operational rules. Never load them as task policy. If
asked to do so outside rule development, stop and ask the user.

The **user rule file** (`AGENTS.md`, `CLAUDE.md`, or equivalent) starts rule
loading. During normal work, load only operational files it or another rule
names.

## How do I use this directory?

Rule packages are identified by `*.loader.md` files. Each loader lists brief
trigger conditions for its rules. Read on to add these to your rule file. Then,
read the [tutorial](TUTORIAL.md).

### Meta-internal installation

Tell your agent to "follow xplat/folly/agents/facebook/INSTALL.md".

### OSS installation

Symlink this dir to `~/folly_agents`, and add this to your user rule file:

```markdown
# `folly/agents` rules

`{FA}` means literally `$HOME/folly_agents`, do NOT resolve.

Immediately batch-load these from `{FA}/`:

- `rule-conflicts.loader.md`
- `task-ledger.loader.md`
- `critic-iterate.loader.md`
- `design-vetting.loader.md`
- `write.loader.md`
- `code.loader.md`

If a loader or rule is missing, stop and notify the user.
```

## Purpose

More Folly work is being done by coding agents, but contributors apply very
different levels of rigor. The extra productivity helps only if the work stays
high-quality and cheap for humans to review, understand, and maintain. These
rules aim to make that repeatable. Most apply beyond Folly.

Agent work spans design, planning, implementation, measurement, and explanation.
Even strong agents are better at executing a stated plan than at recovering
requirements and constraints that were never stated. They also tend to write for
the context they just consumed rather than the context their readers have.

The main rules target those gaps:

- `task-ledger.md` keeps current goals, requirements, and unfinished work
  recoverable across long or interleaved workstreams.
- `critic-iterate.md` improves quality by spending more model time and tokens on
  repeated drafting and independent review across design, code, and writing.
- `design-vetting.md` surfaces requirements, constraints, failure modes, and the
  evidence needed to choose between options before a plan hardens.
- `write.md` pushes explanations toward plain language and the reader's actual
  context.
- `code.md` and the testing rules push implementation toward correctness,
  simplicity, and reviewability.

## Find files

- Other `.md` files may contain rules or support material, but their names do
  not activate them or make them package roots. During a task, load one only
  when the user rule file or another operational rule names it.
- `<name>/` holds more focused files used by that package.
- `backtest/` contains fixed tasks for measuring how rule changes affect an
  artifact. It is development tooling and is never loaded as task policy.
- `tests/` contains shared rule-test tooling and unit tests.
  [TESTING.md](TESTING.md) explains how to run fixed tasks and compare their
  outputs.
- `CONTRIB.md` and `<name>.contrib.md` keep maintainer context for a subtree and
  a particular rule, respectively.

See [CONTRIB.md](CONTRIB.md) for package principles, loading, and maintenance.
