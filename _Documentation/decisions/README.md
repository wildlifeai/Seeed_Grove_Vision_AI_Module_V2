# Decision records

Short, numbered records of design decisions: **what we decided, why, and what it costs** —
so a developer can learn what the code stands for without reading the discussions that
produced it. The raw back-and-forth lives in
[`../development reports/`](../development%20reports/README.md); each record links to its thread.

Rules of the game:

- One decision per file: `NNNN-short-title.md`, numbered in order of creation.
- **Keep it under a page.** If it needs more, the detail belongs in a topic doc or the
  discussion thread — link to it.
- `Status:` is one of **Proposed** (written to frame a pending decision), **Accepted**,
  **Superseded by NNNN**. Update the status line rather than deleting records — a
  superseded record explains why the code *used to* be that way.
- When a record is Accepted, also update the affected topic doc
  (`_Documentation/*.md`, `ww500_md/doc/*.md`) — the decision record says *why*,
  the topic doc says *what*.

Template:

```markdown
# NNNN. Title (imperative or noun phrase)

Status: Proposed | Accepted (date) | Superseded by NNNN
Thread: link into ../development reports/

## Context
Two or three sentences: the problem and the forces at play.

## Decision
What we chose. Plain statements.

## Consequences
What this buys, what it costs, what to watch out for.
```

## Index

| # | Decision | Status |
|---|---|---|
| [0001](0001-slot-labels-self-heal-at-first-boot.md) | Slot camera-labels self-heal at first boot | Accepted |
| [0002](0002-development-reports-and-decision-records.md) | Development reports + decision records workflow | Accepted |
| [0003](0003-op26-op24-defaults.md) | Defaults for automatic camera switching (op26/op24) | Proposed |
| [0004](0004-exif-nn-output-contract.md) | EXIF NN output contract (logits vs percentages) | Proposed |
