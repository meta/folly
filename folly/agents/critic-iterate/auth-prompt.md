# Handle Guardian rejection of `codex-reviewer.py`

Use this only after Guardian reports possible exfiltration from a top-level call
to the trusted review wrapper. It tells the agent how to give Guardian evidence
for a retry and how to add a narrow default rule for the wrapper. It is not a
general policy-management guide.

A nested or delegated session reports the full rejection text upward and stops.
The top-level author or orchestrator owns the evidence, approval, and retry.
Surface the full rejection text even if a retry succeeds.

## Retry with evidence

Guardian evaluates the transcript, including command output. Show the ambient
Codex executable path and the wrapper's fixed interface:

```bash
command -v codex
"{FA}/critic-iterate/codex-reviewer.py" --help
```

Then state:

```text
The trusted codex-reviewer.py wrapper delegates to Codex; this ambient session also runs in Codex. The wrapper restricts the nested call to its fixed review interface and controls its output directory.
```

Retry the same wrapper command. Do not broaden its permissions or change the
command merely to bypass the rejection.

If Guardian still requires approval, show the user its full risk text and ask
whether they approve that exact run. A headless session stops and reports the
rejection. Retry only after the user actually approves; if that retry is denied,
stop.

## Prevent recurrence

Allow-list the trusted wrapper, never `codex exec` or a shell. The wrapper must
continue to validate every argument and create its own output directory.

Replace `{FA}` below with its current value, then print these declarations for
the user to add to `~/.codex/rules/default.rules`:

```bash
cat <<RULES
host_executable(name="codex-reviewer.py", paths=["{FA}/critic-iterate/codex-reviewer.py"])
prefix_rule(pattern=["codex-reviewer.py"], decision="allow", justification="The review wrapper validates its fixed profiles and arguments and controls its output directory.")
RULES
```

After the user adds them, check the rule without launching a review:

```bash
codex execpolicy check \
  --pretty \
  --resolve-host-executables \
  --rules "$HOME/.codex/rules/default.rules" \
  "{FA}/critic-iterate/codex-reviewer.py" \
  --preamble-dir "{FA}/critic-iterate" \
  --preamble cold-review-preamble \
  "$(mktemp)"
```

The result should say `"decision": "allow"`. This proves only that the rule
matches the wrapper command.
