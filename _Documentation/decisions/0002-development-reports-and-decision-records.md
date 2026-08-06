# 0002. Development reports + decision records workflow

Status: Accepted (Aug 2026)
Thread: [../development reports/2026-07_pr141-review-cgp/](../development%20reports/2026-07_pr141-review-cgp/README.md)

## Context

Substantive design discussion (the PR #141 external review) was happening in email
attachments — invisible to the repo, unfindable later, and forcing every future developer
to either re-derive the reasoning or read whole conversation threads. The sibling repos
(ww-mobile-app, ww-website, ww-backend) already keep a `documentation/development reports/`
folder for working documents.

## Decision

Two tiers, both in-repo:

1. **`_Documentation/development reports/`** — dated, append-only threads holding the raw
   working material (review notes, responses, bench evidence, logs). Mirrors the sibling
   repos' convention. Provenance, not required reading.
2. **`_Documentation/decisions/`** — numbered one-page records distilled from those
   threads once developers agree, capturing *what was decided and why*. Topic docs
   (`_Documentation/*.md`, `ww500_md/doc/*.md`) are updated at the same time to reflect
   the *what*.

## Consequences

Future developers learn intent from a page, not an inbox; disagreements keep their audit
trail; superseded decisions stay visible with their reasoning. Costs a small discipline:
when a discussion concludes, someone must write the distilled record and update the thread
README's Outcome line — make it part of closing the discussion (e.g. the call agenda's
last item). Recommend adopting the `decisions/` tier in the sibling repos too.
