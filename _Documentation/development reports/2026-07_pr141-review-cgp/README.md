# PR #141, #142 and #140 review, July to August 2026

**Status:** CLOSED, 18 August 2026. All three PRs reviewed, agreed and merged to `dev`.
Remaining work is tracked as GitHub issues, listed below.

Review of the three stacked camera PRs, with point-by-point responses and hardware
validation. Started on `feat/camera-features-combined` (PR #141) and grew to cover
PR #142 (BLE fast transfer) and PR #140 (RP3 live preview, AE, AWB). The canonical PR
discussion is on GitHub; these files carry the material that outgrew PR comments and used
to live in email attachments.

## Outcome

### Merged

| PR | What | Merge commit |
|---|---|---|
| #141 | Dual-camera slot switching, light sensor, LED flash | `1936ce37` |
| #142 | BLE fast file transfer | `59ced6fd` |
| #140 | RP3 live preview, highlight-metered AE, AWB, colour pipeline | `8cbb6a2b` |

Merged in the same window, outside this review: #146 (iOS session inactivity), #147 (CI
manual dispatch), #150 (agent guide). `dev` tip after all of it: `02279d76`.

Both camera variants were built from that tip and uploaded to the dev firmware database,
so the mobile app can install a matched pair.

### Decisions taken

- **op26 (automatic camera switching) now defaults to 0, off.** The simplest configuration
  should be the default: a camera with one image installed would otherwise wake every op24
  minutes, run the ~1.9 s AE sampling burst, find it cannot switch, and sleep again.
  Deployments that want day/night switching turn it on from the app. See #165.
- **op22 (MD flash brightness) stays at 50%**, raised from 5, which was too dim for night
  motion detection in the field. 0 means dim, not off; op21 = 0 is the off switch.
- **On-device image correction is retained** rather than moved server-side.
- **EXIF NN output (logits vs percentages) was NOT settled.** Deferred to #189, which
  consolidates it with Charles's August input, the Camtrap DP check and the finding that
  the quantization parameters never leave `cvapp.cpp`, so server-side conversion is not
  currently possible from what the firmware ships.

### Hardware-validated

- Both items PR #141 listed as "pending on hardware", the op26 box/unbox auto-switch cycle
  and the MODE_SLEEP sensor-wake light check, pass on a bench WW500 C02
  (`bench_validation_evidence.md`).
- CGP's watchdog-to-cold-boot claim confirmed with raw PMU registers. His capture-retry
  observation reproduced exactly: cold-boot IMX708 captures fail all 3 instant retries,
  DPD-wake captures are reliable.
- The battery estimate first given in this thread was roughly 2x too high and was corrected
  after CGP challenged it: wake to decision is ~2.6 s plus a 1 s inactivity timeout, so
  about 6.4 min extra awake per day, not 8 to 16. The larger figures were an artefact of
  the console driver holding the device awake for 60 s.

### Slot-label design rationale

Asked during review. Each image labels its own slot on **every** boot because flash-time
labelling can lie (X-Modem wipes the selector sector; writes can be interrupted) and "first
boot" cannot be inferred from cold/warm (an interrupted OTA makes a plain DPD wake a new
image's first boot). `firmware` deliberately clears the target label to *unknown*;
auto-switch refuses unlabelled slots; manual `switchslot` is the bootstrap. Cost is 3 short
SPI reads per boot. Do not gate the label call on cold boot.

### Defects the merge itself surfaced

Merging exposed four problems that the review had not, all in build and CI rather than
firmware behaviour:

- **#188** the `D:/hxbuild` MAX_PATH workaround was unguarded, so on Linux it created a
  directory named `D:` and broke every CI build. Fixed. The image-generation binaries were
  also stored non-executable, which broke CI a second time once image generation became
  part of the default build. Fixed.
- **#190** `device_image` clears `R*.IMG` and `H*.IMG` before writing, so building the
  second camera variant deletes the first variant's image. Open.
- **#156** the PR gate built one variant on toolchain 14.2 while everything else uses
  14.3.rel1, which is why both of the above reached `dev`. Fixed in #191.
- **#155** the `firmware` console command truncated names over 13 characters, reporting a
  file the user never typed. Fixed in #191, reproduced and re-verified on hardware.

### This folder's workflow

Agreed August 2026: docs are the record, issues are the tracker. See
[`../README.md`](../README.md).

## Open items

40 issues carry the review's findings, all labelled `review-finding` and on the
[project board](https://github.com/orgs/wildlifeai/projects/3).

**Decisions still open:** #165, #167, #175, #189.

**Conflict-free, safe to do now:** #152, #155, #156, #157, #162, #163, #164, #171, #172,
#174, #176, #177, #178, #179, #181, #182, #183, #186, #188, #190.

**Blocked until PRs #143 and #144 land** (they touch the same files): #151, #153, #154,
#158, #159, #160, #161, #168, #170, #173, #180, #184, #185, #187.

**Light sensor** (also tagged `light-sensor`, the cluster Charles raised): #154, #158,
#181, #182, #183, #184, #186.

**Closed:** #166 (superseded by #189), #169 (build outputs restored and verified).

## Files

| File | What it is |
|---|---|
| [CGP's review notes](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/CGP_Code_Review_July26.md) | Charles's review notes, all 9 topics, committed by CGP in `ww500_md/doc/` |
| `review_responses.md` | Point-by-point responses to the #141 review with file:line evidence: bench results, dispute verdicts, camreg/vcm audit, battery impact, triage of CGP's changes |
| `review_responses_pr142_pr140.md` | The same for PRs #142 and #140 |
| [dual_image_build_and_flash.md](../../dual_image_build_and_flash.md) | Standalone topic doc distilled from this thread: two-variant build, image, flash, verify chain plus bench checklist (lives in `_Documentation/`, not here) |
| `reviewer_worktree_setup.md` | Reviewer workflow: git worktrees and Meld, `review/cgp-*` branches, frozen-stack rule |
| `bench_validation_evidence.md` | Serial-log evidence: slot-labelling ladder, op26 cycle, staged-exposure validation |
| `logs/console_session*.log` | Raw timestamped serial captures behind the evidence doc |

## Closing checklist

- [x] Outcome written
- [x] every remaining open item filed as an issue and linked above
- [x] affected topic docs updated

Topic docs updated from this thread: `Operational_Parameters.md` and
`MANIFEST/config_file.md` (op22, op26), `dual_image_build_and_flash.md` and
`firmware_update_and_recovery.md` (#191), `doc/light_sensor.md` (CGP).

## What came next

Post-merge validation of the combined result on hardware and through the mobile app is a
separate thread. The review branches (`review/cgp-140`, `review/cgp-141`, `review/cgp-142`)
were merged and deleted; their content is in `dev`.
