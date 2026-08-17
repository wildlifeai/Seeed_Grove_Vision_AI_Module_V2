# Development reports

Working records of how the firmware got here: review threads, proposals, bench evidence,
and the discussion between developers. Same convention as
`documentation/development reports/` in ww-mobile-app, ww-website and ww-backend.

Two rules keep this simple:

1. **Docs are the record; GitHub issues are the tracker.** Anything still *open* when a
   discussion pauses — a bug found, a decision not yet made, a follow-up — is filed as a
   GitHub issue (they land on the
   [project board](https://github.com/orgs/wildlifeai/projects/3) automatically). A
   document is never the only place an open item lives.
2. **Every thread README starts with Status / Outcome / Open items.** The Outcome section
   is the short summary a future developer reads instead of the thread — what was agreed
   and why. Open items are issue links, nothing else.

Starting a thread: make a dated folder `YYYY-MM_short-topic/`, add a README with the
three headers, drop the working files beside it. Append as it evolves — these are the
audit trail, don't rewrite them.

Closing a thread (the only ritual — checklist also sits in each README):

- [ ] Outcome written (short; the "why" for future developers)
- [ ] every remaining open item filed as an issue and linked under Open items
- [ ] affected topic docs updated (`_Documentation/*.md`, `ww500_md/doc/*.md`)

## Threads

| Thread | Status |
|---|---|
| [2026-07_pr141-review-cgp](2026-07_pr141-review-cgp/README.md) | Review complete — outcomes being agreed (call pending) |
