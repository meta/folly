# Task ledgers

Keep a short Markdown ledger as the source of truth for the workstream's current
intent and unfinished work. The ledger is current state, not a history.

Use task-ledger rules when:

- The user requests ledger use (“enqueue <work>”, “add to ledger”).
- The turn uses tools, has multiple steps, or risks interruption or compaction.

Otherwise, skip them for a direct, tool-free answer.

## Maintain the ledger

- Keep 1 short, descriptively named file under `~/task-ledgers/` for each
  workstream this session owns, so it can be found after resume. Start it with
  `Owner UUID: <current-session UUID>`. If it contains multiple goals or
  substreams, organize them under separate `#` headings.
- Keep goals separate from tasks. Keep each task's current requirements with it.
  Make the current activity and every outcome awaiting delivery explicit. Record
  only the detail and rationale needed to resume or verify the remaining work.
- Update the ledger before continuing when the user changes intent or a tracked
  task changes state.
- Edit ledgers only to change tracked state, using narrow patches. Never format
  them.
- Adding tasks doesn't interrupt ongoing work.
- As in all prose, write plainly. Use simpler words and short sentences when
  possible. Avoid abstract labels and legalese. Keep clear user wording; clean
  up rough wording, but never complexify.

## Track the next final response

Create `~/task-ledgers/<your-ledger>.response.md`. Use it to track what the
final must answer or report and its requested form. During the turn:

- Revise it when a steer changes the final.
- Reread it after interruption, compaction, asynchronous completion, or a large
  context load, and immediately before the final.

## Recover and debrief

After compaction or resume, reconcile the current activity and undelivered
outcomes from the whole ledger. Use an automatic summary only to locate
evidence. If the task set is incomplete or unclear, repair it from the relevant
direct messages and artifacts; ask the user if ambiguity remains.

Immediately before the final response:

- Start from the user's question and stated background. Do not assume the reader
  has seen your investigation notes or newly written artifacts.
- Compare the planned reply's assumptions with that starting point. Which
  concepts or connections are available only to you? Explain the needed ones in
  the response sidecar using [write.md](write.md#substance). Prepare framing,
  not just a reader label or reporting checklist.
- Draft the final from the response sidecar. Make each answer understandable
  without rereading its question; a short, plain paraphrase may be enough.
- Prune tasks fully delivered in this final.
- Delete the sidecar.
- Send the final.

Keep goals and any requirements or context that still constrain the workstream.
Remove them at the user's request, when a durable artifact supersedes them, or
when the workstream closes.
