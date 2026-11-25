# Writing style

Resolve package paths relative to this file's directory.

When writing or reviewing `CONTRIB.md` or `*.contrib.md`, follow
`{FA}/write/contributor-notes.md`.

When writing or reviewing a commit or diff Summary, follow
`{FA}/write/commit-summary/author.md`, with or without critic-iterate.

## Critic-iterate

To author prose under critic-iterate, follow `{FA}/write/prose/c-i-review.md`,
then `{FA}/write/prose/c-i-author.md`.

To prepare a prose fresh review, follow the generic base
`{FA}/critic-iterate/fresh-review.md`, then the prose extension
`{FA}/write/prose/c-i-fresh-review.md`. For a commit or diff message, then
follow `{FA}/write/commit-summary/c-i-fresh-review.md`.

## General maxims (apply to all prose)

- **Set the reader before the outline.** Before drafting durable prose,
  establish:
  - its primary reader;
  - what that reader already knows; and
  - what that reader should know or be able to do afterward.

  Inspectable material is not starting knowledge. State the framing the reader
  needs; rely on that material only for detail and evidence.

  Derive these from where the artifact will live and why it exists. If two
  plausible audiences would require materially different content, ask which one
  is intended before drafting. Name a secondary audience only when it has a
  distinct required task.

- **Optimize for the needed understanding, then length.** Give the target
  audience exactly the facts and relationships it needs for the artifact's
  purpose — no more and no less. First minimize the effort needed to understand
  them correctly. If two versions do that equally well, choose the shorter one.

- **Rework, don't append.** When fixing prose, default to rewriting the line
  rather than adding to it. Append-style patches ("See X for Y", "Note: Z") are
  almost always barnacles.

- **Do not edit for motion.** In review mode, change text only to improve
  clarity or accuracy, or to make it materially shorter without losing needed
  content or increasing reader effort. Replace an abstract process label when
  the target audience would have to unpack it.

- **Pick the right shape.** Choose the form that makes the text easiest to
  follow.

  - Use numbered steps for instructions whose order matters.

  - Prefer bullets for parallel facts, choices or reasons, rather than packing
    them into paragraphs.

  - Keep a connected explanation in prose when splitting it would make it harder
    to follow.

  - Use labels only when they help the reader find, compare or interpret
    information.

- **Lead with what the reader needs first.** For prose about code, start with
  the problem or purpose. A fact that needs no background explanation may come
  first when it helps interpret the rest. Explain enough of what happens or is
  proposed to make the point clear; leave routine mechanics in the code. If one
  sentence is enough, stop.

## Author disposition

The author must adopt a persona free from cognitive biases like rationalization
& sunk-cost. Channel these traits as you revise:

- **Reader-first.** Judge needed context from the reader's stated background.
  Keep the setting and causal steps needed to understand the point or do the
  task.
- **You are not your draft.** Existing wording gets no preference. Rewrite or
  delete it when that better serves the reader.
- **Rationalization-hostile.** "Load-bearing," "critical," and "archaeologist
  needs it" must name the concrete failure caused by cutting.
- **Subtractive.** After selecting the necessary facts, remove excess wording.
  Among versions that are equally easy to understand, the shorter one wins.

## Substance

**Establish the reader's understanding before cutting.** Assume the intended
reader's normal background, not knowledge of this artifact or its discussion.
Explain the starting situation and connections needed to understand the point or
use the text. For actions or behavior, say who does what and why before relying
on names or shorthand. Use sources to get the facts right; a definition alone
may leave a needed connection unexplained.

Check that the reader can explain the starting situation and how the relevant
parts relate from the text and their expected knowledge. Add only the missing
facts or connections needed for that understanding. Then cut facts and words the
reader's task does not need. Being true, related, or non-obvious is not enough
to earn space. Reread the whole passage after cuts; a shorter version that is
harder to follow is worse.

- Be direct: each word earns its place, but not telegraphic. Prose should read
  naturally.
- Avoid wordiness: cut filler, hedging, and restatement (don't say the same
  thing twice in different words — merge sentences that make the same point).
- State simple points simply. Add how or why only when the reader needs it.
- Prefer familiar words and concrete verbs without losing precision. Keep needed
  technical terms; explain them when the reader may not know them. Replace vague
  labels with who or what acts, what happens, or how the parts relate.
- Prefer a concrete example when it makes a needed point clearer than abstract
  prose.
- Use identifiers to point readers to relevant code, not in place of a plain
  explanation.
- When shortening rules/guidelines, preserve the decision function: enough
  detail to apply the rule, not just know it exists.

## Sentences and paragraphs

Prefer short sentences and paragraphs. Keep a connected explanation together
when a split would make it harder to follow. Start a new paragraph when moving
to a different point.

- **Split dense sentences.** If a sentence stacks 4+ noun phrases or chained
  possessives, cut it if unneeded; otherwise restructure it. For a sentence over
  20 words, read it aloud; cut or restructure it if you stumble.
- One claim per sentence by default — the goal is fast comprehension. Glue
  (em-dash, semicolon) when the second clause depends on the first to make sense
  ("keep X — stripping breaks Y"). Split independent claims: "X — and Y" → "X.
  Y." Definitions glue freely ("the Foo — what makes the move safe and
  revertible").
- With a shared subject, put a comma before "and" when a long first predicate
  makes the boundary easy to miss; omit it between a pair of short predicates or
  adjectives, and retain the Oxford comma in lists.
- Labels, headings, and bullet lead-ins are fine when they create useful
  structure. In running prose, do not let a label replace a precise
  plain-language claim with a vague category; state the actual relationship,
  trade, gate, cause, or decision.
- Backticks for code identifiers (`FrontendReadService`, `metadata`).
- Em-dashes ( — ) with spaces.

## Document

- **Make the opening survive the body.** In a substantial explanatory document,
  the opening should establish its problem or purpose and enough scope to
  organize what follows. When the document explains why something happens, state
  the relationships needed to follow the cause and why it matters. Do not assume
  that a reader who knows the surrounding systems also knows this work's terms
  or details. If unfamiliar language carries one of those relationships, say who
  does what to what and what changes before relying on it. Later detail may
  refine that understanding, but should not force the reader to replace it.
  During cold re-read, compare the opening with the full document.
- Headings: short and direct. Cut filler like "Discussion of" or "Notes on" —
  "Discussion of potato pros & cons" → "Potato trade-offs". Noun ("Pastes") or
  verb ("Prefer X"), both fine.
- Dedup cross-references — keep the informative version (a listing entry with
  description beats a bare "see X").
- Wrap text to 80 chars with `{FA}/scripts/reformat-md FILE...`; it edits files
  in place.
- **Proposals.** Apply `design-vetting.md` before a substantive design or fix
  proposal. State where the problem occurs and what outcome the proposal must
  produce.
- **Investigations.** Include:
  - how the evidence was produced;
  - where to inspect it or how to reproduce the work;
  - what was observed; and
  - only conclusions the observations support, with the reasoning that connects
    them.
- Keep godbolt links — critical for human readers even if useless to agents.

## Code comments

For code comments, load `code.md` and apply its "Code comments" section.

## Explain intent of code changes

Code-change prose exists to **reduce the audience's work**. Its primary audience
depends on the genre:

| Genre                  | Audience                                                       |
| ---------------------- | -------------------------------------------------------------- |
| Commit / diff messages | Reviewer first; future maintainer second                       |
| Code comments          | Future code reader (next to touch this code)                   |
| Design proposals       | Design reviewers first; future implementers after the decision |
| Code-review comments   | The author of the diff being reviewed                          |

**Commit messages are read before the code.** The reviewer first forms a mental
model from the message, then reads the code through that framing. The message
must make sense before the diff is opened and include the facts and
relationships needed to review the change. Once that model is clear, leave the
remaining implementation detail to the diff. A fact's presence in the diff
neither earns nor disqualifies it; keep it here only when the reviewer needs it
before reading the code.

**Prepare the reader to review.** For commit / diff messages and design docs,
make the purpose clear early. Include the approach when the reader needs it to
assess the work. Keep needed framing in the text; a brief reference to a prior
`D<num>` can supply detail or evidence.

## Titles

Titles must make sense without the body. For a change, name the concrete object
and the relationship or condition changed. For other documents, name the
concrete topic or task. Keep the words that identify the subject and distinguish
it from nearby work. Prefer plain language; keep technical names when they help
readers identify the subject.

Example: Prefer `check job migration state vs that of its reservation` over
`validate paired migration states`.

**Title-only test.** A title term whose meaning only resolves after reading the
body is suspect — "paired" in the bad title above fails this test (paired how,
with what?). Swap context-dependent jargon for plainer language.

## Commit Summary — brief or essay

Match the Summary shape to the change:

In either mode, state framing in the prose; rely on external links only for
detail or evidence.

**Simple explanation → brief.** Use one or two sentences when they carry the
complete mental model the audience needs. A large diff of self-explanatory test
cases may need only the reason they were added.

**Use the shortest form that carries the needed model.** Essay mode is justified
by several independent reader concerns or by one causal chain that cannot be
understood correctly in one or two sentences. Diff size does not decide message
length: a one-line race fix may need how the bug was detected, what caused it,
and why the fix works.

Example (D104870443, ~70 words):

> `Template:Warning` and `Template:Error` are wiki pages whose body bakes in
> light hex backgrounds, illegible in dark mode. The HSL inversion helper that
> already handles this for QUIP applies just as cleanly here.
>
> The "proper framework fix" would be to route through `XDSBanner`, but
> currently MediaWiki `{{Warning}}` / `{{Error}}` go through
> `InternWikiTransclusion::genRenderTransclusion`, not through any React
> component on this path.

**Substantial explanation → structured essay.** A new data flow, a privacy-class
change, a killswitch rollout, a design with rejected alternatives, or a
non-obvious failure chain may need sections. Use `#`/`##` to chunk distinct
concerns; keep each section short. Sections that typically earn their keep:

- Motivation / situation / problem.
- Mechanism — only when the choice is non-obvious.
- Alternatives considered and why they lost.
- Privacy / killswitch / rollback strategy.

**Additional fixes.** Mark small incidental fixes so reviewers do not mistake
them for part of the main change:

- One simple fix:
  ```text
  **Also:** Simple fix.
  ```
- A few simple fixes:
  ```text
  Incidental fixes:

  - Simple fix 1
  - Simple fix 2
  ```

Briefly explain non-obvious reasons; do not restate code. Use this sparingly. If
the fixes are many, complex, or completely unrelated -- split the diff.

## What evergreen context means

These are common kinds of useful context, not a checklist. Keep one only when
the artifact's audience and purpose require it.

- The situation that made the change necessary, named as reviewer-facing pain
  ("untenable to review", "can't ship without manual diff inspection"), not the
  mechanical symptom ("version bump", "JSON churn").
- The constraint that pinned the design (compat, privacy, perf, deadline).
- The alternative considered and rejected, with a one-line why.
- The invariant preserved.
- The kill-switch / rollback story for risky changes.
- For multi-diff stacks where the endpoint isn't obvious from the diff: a brief
  trajectory line ("Up-stack: every caller migrated, old API deleted"). Skip
  when the next diff is clearly implied.

Exclude is operationalized by the Cut test and Banned patterns in "## Iterate"
below — don't re-enumerate here.

## Iterate — inner loop, until convergence

Use the writing criteria below for every commit or diff message. When
critic-iterate runs, apply them in its author cycle and follow its convergence,
review and closure rules.

Under critic-iterate, repeat: explanation → shape → cut (including the test
plan) → plain language → cold re-read, until a full round makes no edit.

### Cut test — per sentence

Ask: **"What would a reader with only the stated background misunderstand or
find materially harder if I cut this sentence?"** A fact does not earn its place
merely by being true, related, or mildly helpful. If no required part is lost,
cut it.

Cut on (locality first, style second):

- **Restates the title or an earlier sentence.** (Includes wrap-ups, especially
  invariant restatements after a goal-led lead.)
- **Unneeded intermediate detail.** Keep causal steps needed to understand the
  problem or why the change works. Omit implementation details that do not help
  explain either.
- **Opaque-identifier enumeration.** Lists of hex hashes / auto-generated IDs
  the reader can't act on — name the SET or the COUNT instead. "Three res_ids
  (`5f6bcc932c826`, `6196aa3142bcf`, `67056151d29d2`)" → "the three res_ids".
  Keep only when a specific ID is itself the load-bearing reference for a named
  gotcha.
- **Structure is not motivation.** Phrases like "co-locating the two", "instead
  of repeating X", or "the goal is to group..." are bloat when they re-label the
  diff's mechanics as a why. If cutting the sentence would make the reviewer
  misunderstand the change or do something wrong, name the concrete consequence
  or invariant; otherwise cut it.
- **Reflexive reassurance** — preempting an objection the reader didn't raise.
  Surface forms: "Fine because…", "no risk of…", "to keep this focused on…",
  "you don't need to worry that…". Test: if you cut the sentence, what does the
  reader misunderstand or do wrong? If nothing, cut it. A real warning names the
  concrete risk and how the change prevents it ("if X is called twice, Y now
  dedups instead of erroring") — keep, leading with the risk.

### Cut test — per section

Before defending individual sentences, test the section itself: **would deleting
this section remove context the target audience needs?** Sentence-level cuts
protect sections that shouldn't exist — every sentence looks defensible when
read alone.

Commit Summaries usually don't need these sections:

- **Roadmap / implementation status.** Track future work outside the commit.
- **File inventories and change counts.** Let the diff carry these.
- **"What survives" / mission-preserved.** Keep the invariant once, not as a
  section.

### Cold re-read and loop

Under critic-iterate, cold re-read the WHOLE message after each round of cuts.
If any paragraph scans as dense, jargon-heavy, or "I had to re-read that" —
restart the loop.

For every finished Summary, check it against
`{FA}/write/commit-summary/author.md`. If a context packet was supplied, confirm
its "Reader must know" facts remain clear in the message.

### Banned patterns

**Cut on sight.** These patterns are bloat by default. Keep one only when it
passes the sentence Cut test, and lead with the content that earns it.

- Empirical checks in Summary prose: "we verified that …", "tested that …", "no
  production caller actually depends on …". These are Test Plan bullets, not
  Summary prose.
- Change counts and mechanical inventories: "Source diff is N lines per file",
  "renamed parameter X to Y", "the 13k-line diff is mechanical".
- Jargon that hides the actor, action, or outcome: "enables analysis of",
  "supports future extensibility", "provides a robust foundation". State the
  concrete outcome instead.
- Sibling-diff summaries: don't restate what a predecessor or follow-on diff
  does, and don't add a `Predecessor` line merely because a diff dependency
  exists. Continuation markers ("Continues from D<num>", "Next batch", "Same
  pattern as D<num>") are bookkeeping unless they carry reader context. Default:
  omit; the ddep edge carries the relationship.
- **Scope-defense prose**: explaining why the diff didn't do more. Surface
  forms: "X stays for now", "deferred to D<num>", "intentionally not bundled",
  "minimal translation here, polish in follow-up". The diff scope is what it is;
  defending it reads as anxiety. Brief stack xrefs that ARE warranted are
  governed by the trajectory bullet (up-stack) and the sibling-diff-summaries
  exception above (down-stack).
- Wrap-ups (closing form): sentences whose content the reader just read.
  Invariant restatements after a goal-led lead — if the lead states "I want
  refactors that don't touch the JSON", don't close with "after this commit,
  refactors produce byte-identical JSON." The reader closes the loop.
- Wrap-ups (opening form): the first sentence after a header, or a bullet's lead
  label, must not restate what the header named. The header carries the
  navigational anchor; the restatement pays nothing.
- Show-then-tell: the prose already showed it; then a tag tells the reader what
  they just extracted. "This is the X failure mode", "the result is Y", "what
  just happened was Z", "in summary, this means W". The tag pays nothing.
  Failure mode of "show, don't tell" — when both fire in sequence, cut the tell.

**Judgment calls** (cut OR keep depending on context):

- **Verb-as-label openings** that restate the title while hiding a needed WHY:
  "Extracted X.", "Consolidates Y.", "Removes Z." For trivial mechanical or
  no-op changes with no hidden rationale, a direct action sentence is fine;
  don't invent context.
- **Author-coined terms** — a term the reader can't decode from what they've
  seen: coined for this artifact with no in-text definition, or a code
  identifier used as prose without the reader having opened the source. Define
  on first use, or rewrite around the concrete operation. Industry-standard
  terms ("idempotent", "race condition") are fine in body prose; for titles,
  apply the title-only test.

## Test plans — what you checked, briefly

A reviewer learns trust from the rigor of your verification, not from
exit-code 0.

Failure modes:

- **Test-plan theatre.** A verification that adds no coverage over a cheaper
  check above it (e.g., manual byte-identity check when CI catches it).
- **Over-explaining CI.** Ordinary build/test/lint/format is "CI"; don't expand.
  Name only non-obvious/custom checks: ASan mode, a manual repro,
  generated-output inspection, screenshots.
- **Missing the obvious test.** Before writing the Test Plan, ask: does this
  change obviously merit a unit / integration test / manual repro? If yes and
  you haven't done it, fix the gap BEFORE writing.
- **Rigor mismatch.** "Verified" claimed when you only spot-checked — fix one or
  the other.
- **`sl diff` restatements.** "No other files changed" — the diff carries it.
- **Local-only artifacts.** Reviewers cannot see paths under your home directory
  or scratch output dirs. Include the result inline, link a durable paste, or
  cut the reference.
- **Over-detailed bullets.** A 20-bullet test plan is iteration debt.

Verb choice carries rigor. Match what you actually did: "checked", "verified",
"stepped through" for rigorous; "eyeballed", "spot-checked", "skimmed" for quick
/ representative; "manually ran", "repro'd" for repro. Command-style ("diffed
X", "ran Y") leaves ambiguous whether you read the result. Vary the word — don't
lean on "eyeballed" as a tic.

Good — `CI` / `Docs-only` first for vanilla diffs, the rest for the non-obvious
cases:

- "CI" / "Docs-only."
- "Skimmed materialized JSON — only the 6 expected handles changed."
- "Added a unit test for the new branch; pre-existing tests still pass."
- Before/after screenshots, repro, "A-B-A-B to rule out luck".

## Note on genre

The examples use commit messages. For design proposals, rules and guidelines,
group posts seeking input, review comments, and other prose, apply the same
principles for choosing content and reducing reader effort. Adapt the content
and structure to the artifact's purpose and established audience. Include
context for future readers when the artifact is expected to remain useful to
them.
