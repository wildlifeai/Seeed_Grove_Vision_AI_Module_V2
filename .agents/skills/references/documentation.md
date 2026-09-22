# Documentation and the commit-time check

Where each kind of document lives, the house style, and the check that keeps this agent layer
from rotting.

# 1. Development conversations and documentation

**The rules live in [`_Documentation/development reports/README.md`](../../../_Documentation/development%20reports/README.md).
Read it before starting or closing a thread.** In short: docs are the record, GitHub
issues are the tracker (project board: `https://github.com/orgs/wildlifeai/projects/3`,
auto-add is enabled for this repo); every thread README carries Status, Outcome and Open
items; and threads record *how the work happened*, not how the code works.

What that means for an agent, beyond reading the rules:

* **Never leave substantive material only in a chat transcript, email or PR comment.** An
  investigation, review exchange or design discussion belongs in a dated thread under
  `_Documentation/development reports/YYYY-MM-DD_short-description/`. This is the failure
  mode to watch for: the work is done, the finding is real, and it evaporates because it
  only ever existed in a conversation.
* **Two homes, and do not confuse them.** How the firmware behaves now goes in the durable
  docs (`_Documentation/*.md`, `ww500_md/doc/*.md`); how it got that way goes in the
  thread. When something is agreed, update both in the same change: the topic doc gets the
  "what", the thread's Outcome gets the "why".
* **Never edit a thread to keep it true.** Threads are an append-only audit trail. If
  behaviour changes, the durable doc changes; the thread stays as the record of what was
  believed and decided at the time.
* **Open items are GitHub issue links, nothing else.** A document must never be the only
  place an open item lives. File with the `review-finding` template. Before closing a
  thread, check its issues are actually still open work: an issue already fixed and merged
  reads as available work and wastes someone's afternoon.
* **Keep hardware evidence.** Bench and serial logs supporting a claim belong in the
  thread's `logs/` folder, referenced from the write-up.
* **Bench findings: one folder each, reproduced before filed, updated in place.** A finding
  gets `<Letter>_short_name/` under its thread with `explanation.md` in the issue template's
  four sections, the script that reproduces it and `logs/` with the filtered three-way log.
  Nothing is filed until it has been reproduced on demand and section 2 says how; a finding
  that cannot be reproduced is not an issue (one was dropped that way). The issue body is the
  explanation without its header, with evidence as permalinks to the commit. When more is
  learned, edit the explanation and the issue body together; never add a comment that a
  reader has to reconcile with the document. Worked example:
  `2026-09-03_capture_bench_findings/`.

---

## House style

- **No em dashes.** Use commas, or start a new sentence. This applies to every document and
  anything else that gets pasted somewhere: em dashes read as machine-written to the people who
  fund this work. Existing C comments still carry them; leave those alone unless you are
  already rewriting the file.
- **Say which firmware and which variant you tested.** `AI ver` names the build and `AI slots`
  names the active image. A claim without both is not evidence, and this repo's history has
  more than one theory the bench later disproved.

## The commit-time check

Before every commit, look at what the change means for the agent layer, meaning `AGENTS.md`,
the skill and its reference files, and decide whether anything needs to be added, edited or
deleted. It takes a few seconds and it is what keeps this layer from rotting.

Three questions, in order:

1. **Did I learn something that would have saved me time today?** A trap, a contract, a command
   that does not behave as its name suggests. That belongs in
   [hardware-traps.md](hardware-traps.md) or the reference it fits, with the date and what it
   cost.
2. **Did I make something here wrong?** A renamed target, a changed command, an op parameter
   that moved, a build flag that now means something else. Fix the line that is now false in
   the same commit. A confidently wrong skill is worse than a thin one.
3. **Is something here now redundant?** A trap whose cause was fixed, a workaround for a board
   nobody runs, a rule CI now enforces. Delete it, and say in the commit message what was
   removed and why. This layer grows by default; only deliberate pruning shrinks it.

If the answer to all three is no, commit and say nothing. The check is a habit, not a ritual to
document each time.
