# PR #141 review: dual-camera switching, light sensor, LED flash

**Status:** closed, 18 August 2026. Merged to `dev` as `1936ce37`.

Charles Palmer's review of `feat/camera-features-combined` (PR #141), with point-by-point
responses and hardware validation. The largest of the three reviews: about 4,900 lines
across 9 topics. The canonical discussion is on the PR; these files carry the material
that outgrew PR comments and used to live in email attachments.

## Outcome

**Decisions taken**

- **op26 (automatic camera switching) now defaults to 0, off.** The simplest configuration
  should be the default: a camera with one image installed would otherwise wake every op24
  minutes, run the ~1.9 s AE sampling burst, find it cannot switch, and sleep again.
  Deployments that want day/night switching turn it on from the app. See #165.
- **op22 (MD flash brightness) stays at 50%**, raised from 5, which was too dim for night
  motion detection in the field. 0 means dim, not off; op21 = 0 is the off switch.

**Hardware-validated**

- Both items the PR listed as "pending on hardware", the op26 box/unbox auto-switch cycle
  and the MODE_SLEEP sensor-wake light check, pass on a bench WW500 C02. Evidence in
  [`bench_validation_evidence.md`](bench_validation_evidence.md), raw captures in
  [`logs/`](logs/).
- CGP's watchdog-to-cold-boot claim confirmed with raw PMU registers. His capture-retry
  observation reproduced exactly: cold-boot IMX708 captures fail all 3 instant retries,
  DPD-wake captures are reliable.
- The battery estimate first given in this thread was roughly 2x too high and was corrected
  after CGP challenged it. Wake to decision is ~2.6 s plus a 1 s inactivity timeout, so
  about 6.4 min extra awake per day, not 8 to 16. The larger figures were an artefact of
  the console driver holding the device awake for 60 s.

**Slot-label design rationale**, asked during review

Each image labels its own slot on **every** boot, because flash-time labelling can lie
(X-Modem wipes the selector sector; writes can be interrupted) and "first boot" cannot be
inferred from cold/warm (an interrupted OTA makes a plain DPD wake a new image's first
boot). `firmware` deliberately clears the target label to *unknown*; auto-switch refuses
unlabelled slots; manual `switchslot` is the bootstrap. Cost is 3 short SPI reads per boot.
Do not gate the label call on cold boot.

## Open items

- **Light sensor**, the cluster Charles raised and the agreed next focus: #182 (extract into
  one module with a document), #181 (instrument and validate the AE sampling window), #183
  (expose the decision in EXIF and to the app), #186 (design question, assigned to CGP: is
  MD illumination meant to depend on op13?). Blocked on #143/#144: #158, #154, #184.
- **Other:** #151 (cold-boot gate on `cameraSwitch_labelBootSlot()`), #152 (preserve the RTC
  across deliberate reboots), #168 (`adjustInactivityPeriod()`), #163 (doc and comment
  corrections), #165 (op26/op24 defaults).

Full set on the [project board](https://github.com/orgs/wildlifeai/projects/3), labelled
`review-finding`.

## Files

| File | What it is |
|---|---|
| [CGP's review notes](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/CGP_Code_Review_July26.md) | Charles's notes, all 9 topics, committed by CGP in `ww500_md/doc/` |
| [`review_responses.md`](review_responses.md) | Point-by-point responses with file:line evidence: bench results, dispute verdicts, camreg/vcm audit, battery impact, triage of CGP's changes |
| [`bench_validation_evidence.md`](bench_validation_evidence.md) | Serial-log evidence: slot-labelling ladder, op26 cycle, staged-exposure validation |
| [`logs/`](logs/) | Raw timestamped serial captures behind the evidence doc |
| [`reviewer_worktree_setup.md`](reviewer_worktree_setup.md) | The worktree and Meld recipe written for this review. Superseded by [`Reviewing_External_PRs_with_Worktrees.md`](../../Reviewing_External_PRs_with_Worktrees.md) and [`Reviewing_Stacked_PRs.md`](../../Reviewing_Stacked_PRs.md), which are canonical. Retained as an artefact of this thread; consolidation is #178 |

## Related threads

- [PR #142 review](../2026-08-08_pr142-ble-fast-transfer-review/README.md)
- [PR #140 review](../2026-08-10_pr140-rp3-image-quality-review/README.md)
