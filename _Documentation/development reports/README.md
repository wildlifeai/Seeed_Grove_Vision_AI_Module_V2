# Development reports

Working records of the firmware development threads: reviews, proposals, bench evidence,
and development discussions.

We follow three rules:

1. **Docs are the record; GitHub issues are the tracker.** Outstanding tasks should be
   filed and tracked as GitHub issues. A document is never the only place a task lives.
2. **Every thread README has at least Status, Outcome and Open items.** The Status can be
   open or closed. The Outcome summarises what was agreed and why. Open items are links to
   GitHub issues.
3. **This folder records how the work happened, not how the code works.** Threads are the
   audit trail (what we tried, what we found, what we decided and why). How the firmware
   behaves belongs in up-to-date docs and guidelines. Nobody should have to read a thread
   to find out how something behaves now.

**Starting a thread:** make a folder named `YYYY-MM-DD_short-description/`, dated the day
the work began, add a README with the three headers, drop the working files beside it.
Append as it evolves, don't rewrite them.

**When creating new documents:**
To make it easier to follow, add these lines towards the top of each markdown file:
1. Filename (the name of the markdown file)
2. Author (e.g. a person or AI)
3. Date (preferrably day as well as month and year)

Example:
```
# Responses to the PR #142 and #140 reviews

#### File: review_responses_pr142_pr140.md
#### Author: Claude (Opus 5), reviewed by Victor Anton
#### August 2026
```


**To close a thread ensure:**

- [ ] **Outcome is written.** The outcome should summarise for future developers what the
      result of this thread was.
- [ ] **Linked documentation.** Every conversation and relevant file is linked and easy to
      find.
- [ ] **Open items.** Every follow up or remaining work is captured and linked as a GitHub
      issue.
- [ ] **Durable docs updated.** Anything a future developer needs to know about how the
      code works now lives in `_Documentation/` or `ww500_md/doc/`, not only here.
