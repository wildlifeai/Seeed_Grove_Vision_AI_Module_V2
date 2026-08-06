# Development reports

Working records of how the firmware got to be the way it is: review threads, proposals,
bench evidence, and the back-and-forth between developers. Same convention as
`documentation/development reports/` in ww-mobile-app, ww-website and ww-backend.

**These files are provenance, not required reading.** Nobody should need to read a
conversation thread to understand the code. The flow is:

1. **Discuss here.** One folder (or file) per thread, date-prefixed:
   `YYYY-MM_short-topic/`. Add review notes, responses, measurements, logs as the
   thread evolves. Append or add files — don't rewrite history; these records are
   the audit trail of who argued what and why.
2. **Agree → distil.** When a decision is reached (on a call, in a PR thread, here),
   write the *summary* where future developers will actually look:
   - a short decision record in [`../decisions/`](../decisions/README.md) for
     design choices ("why is it this way?"), linking back to the thread here, and/or
   - an update to the relevant topic doc in `_Documentation/` or
     `.../ww500_md/doc/` for behaviour ("what does it do?").
3. **Close the loop.** Note the outcome at the top of the thread's README so a
   later reader sees immediately what was concluded without reading the thread.

## Threads

| Thread | Status |
|---|---|
| [2026-07_pr141-review-cgp](2026-07_pr141-review-cgp/README.md) | Review complete; decisions being distilled (call pending) |
