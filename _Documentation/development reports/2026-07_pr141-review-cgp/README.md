# PR #141 review — July–August 2026

**Status:** review complete; outcomes being agreed (call pending)

Review of `feat/camera-features-combined` (PR #141), with point-by-point
responses and hardware validation. The canonical PR discussion is on GitHub; these files
carry the material that outgrew PR comments and used to live in email attachments.

## Outcome (updated as agreements land)

- **Hardware-validated:** both items PR #141 listed as "pending on hardware" — the op26
  box/unbox auto-switch cycle and the MODE_SLEEP sensor-wake light check — pass on a
  bench WW500 C02 (`bench_validation_evidence.md`). CGP's watchdog-→-cold-boot claim
  confirmed with raw PMU registers; his capture-retry observation reproduced exactly
  (cold-boot IMX708 captures fail all 3 instant retries; DPD-wake captures reliable).
- **Slot-label design rationale** (asked during review): each image labels its own slot
  on **every** boot because flash-time labelling can lie (XMODEM wipes the selector
  sector; writes can be interrupted) and "first boot" cannot be inferred from cold/warm
  (an interrupted OTA makes a plain DPD wake a new image's first boot). `firmware`
  deliberately clears the target label to *unknown*; auto-switch refuses unlabelled
  slots; manual `switchslot` is the bootstrap. Cost: 3 short SPI reads per boot. Do not
  gate the label call on cold boot.
- **This folder's workflow** (agreed Aug 2026): docs are the record, issues are the
  tracker — see [`../README.md`](../README.md).

## Open items

All remaining findings and pending decisions are tracked as GitHub issues on the
[project board](https://github.com/orgs/wildlifeai/projects/3) — see the issues labelled
from this review (drafted from this thread's findings).

## Files

| File | What it is |
|---|---|
| [CGP's review notes](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/CGP_Code_Review_July26.md) | Charles's review notes, all 9 topics — committed by CGP in `ww500_md/doc/` (commit 55ccb85a on this branch) |
| `review_responses.md` | Point-by-point responses with file:line evidence: bench results, dispute verdicts, camreg/vcm audit, battery impact, triage of CGP's changes, next steps |
| [dual_image_build_and_flash.md](../../dual_image_build_and_flash.md) | Standalone topic doc distilled from this thread: two-variant build → image → flash → verify chain + bench checklist (lives in `_Documentation/`, not here) |
| `reviewer_worktree_setup.md` | Reviewer workflow: git worktrees + Meld, `review/cgp-*` branches, frozen-stack rule |
| `bench_validation_evidence.md` | Serial-log evidence: slot-labelling ladder, op26 cycle, staged-exposure validation |
| `logs/console_session*.log` | Raw timestamped serial captures behind the evidence doc |

## Closing checklist

- [ ] Outcome written
- [ ] every remaining open item filed as an issue and linked above
- [ ] affected topic docs updated
