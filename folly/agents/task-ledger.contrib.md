> NOT A RULE. If loaded as task policy, stop and ask the user.

A task ledger keeps an agent's current goals, requirements, and unfinished work
reliable across long or interleaved workstreams. Compaction resistance matters,
but it is not the whole goal: an uncompacted session can also lose track when
several tasks or goals advance independently.

Another agent should be able to resume from the ledger and current artifacts
without guessing which summarized tasks remain active.

Agents reread ledgers often, so ledger wording can shape their later prose. Keep
it plain and preserve clear wording from the user's input.

An agent can finish work but answer an old request or assume the user knows the
details it learned. The disposable response sidecar keeps the next reply's scope
and necessary background separate from execution state.

## What must persist

Goals and requirements outlive the tasks that implement them. Keep them until
the workstream closes so pruning a completed task does not erase a constraint
that remaining work must preserve. Keep rationale only when it changes how that
work should proceed.

Tasks are current obligations, not history. Once the artifact embodies a task's
result and no remaining work needs its state, retaining it makes later recovery
worse.

One file per workstream bounds reread cost. `#` sections keep independent goals
visible within a workstream without requiring a fixed schema.

## Why the ledger is a file

Codex's `update_plan` and Claude's TODO tools remain useful for immediate
execution, especially as a standing reminder to reread the ledger. They are poor
sole stores for durable workstream state:

- They do not reliably survive exit and resume.
- Their state can be hard to inspect or edit.
- They do not represent larger tasks or persistent requirements well.

An automatic compaction summary is also not a source of truth. It may omit or
revive work. The ledger is useful only if the agent updates it when intent
changes and rereads it after compaction or resume.

## Keep the rule small

Strong agents already know how to maintain TODO lists and requirement documents.
The loader should supply only the missing activation triggers, and the
operational rule only the missing invariants. The rule should not mandate
headings below the goal level, checkbox syntax, progress narration, or retained
completed-task history.

The earlier approach had agents write `REQ[...]` sigils into the user-visible
chat, then search the conversation for those sigils to recover the workstream.
It failed in 3 ways:

- New tasks and subtasks did not reliably trigger a breadcrumb.
- The user had no `enqueue` command for new intent.
- Agents rarely searched the history without a standing reminder.
