# PR #141 review — Charles Palmer (CGP), July–August 2026

External review of `feat/camera-features-combined` (PR #141: dual-camera slot switching,
camera tuning CLI, AE light sensor, RP3 white balance, EXIF, stability fixes, op-param
persistence, update/recovery guide, CI matrix), with responses and hardware validation.
The canonical PR discussion is on GitHub; these files carry the material that grew too
large for PR comments and used to live in email attachments.

## Outcome (updated as decisions land)

- **Hardware-validated:** both "pending on hardware" items in the PR's verification —
  the op26 box/unbox auto-switch cycle and the MODE_SLEEP sensor-wake light check —
  now pass on a bench WW500 C02 (see `bench_validation_evidence.md`).
- **Confirmed for the fix batch:** camera-register file back to `/MANIFEST`; EXIF Model
  `#ifdef` ordering; RTC preserved across watchdog reboots; AE-burst last-frame guard;
  `camRegFileName` const fix; `firmware` filename-length check; PR-gate CI both-variant
  matrix; assorted comment/doc corrections. Open decisions (op26/op24 defaults, EXIF NN
  contract, JPEGENC benchmark, AE persistence across DPD) are tracked in
  [`../../decisions/`](../../decisions/README.md).

## Files

| File | What it is |
|---|---|
| `CGP_Code_Review_July26.md` | Charles's review notes, all 9 topics (his working doc, authored by CGP) |
| `review_response_and_next_prs.md` | Point-by-point responses with file:line evidence; bench results §0; triage of CGP's changes; focus lists for reviewing #142 and #140 |
| `call_preread.md` | Pre-read for the review call: cold-boot gate analysis, `autoSwitchCheck()` walkthrough, camreg/vcm verdicts, battery/DPD impact, the RP3 image-quality "position" |
| `build_flash_reviewer_guide.md` | Two-variant build → image → XMODEM/`firmware` flash → verify walkthrough (Parts A–G, incl. bench checklist and findings list) |
| `reviewer_worktree_setup.md` | Reviewer workflow: git worktrees + Meld, `review/cgp-*` branches, frozen-stack rule |
| `bench_validation_evidence.md` | Serial-log evidence: Part E slot-labelling ladder, op26 cycle (both directions), staged-exposure validation addendum |
| `logs/console_session*.log` | Raw timestamped serial captures behind the evidence doc |
