# PR #142 review: BLE fast file transfer

**Status:** closed, 18 August 2026. Merged to `dev` as `59ced6fd`.

Charles Palmer's review of `feat/ble-fast-transfer` (PR #142). Roughly a tenth the size of
the #141 review. Reviewed alongside PR #140, so the response document covers both.

## Outcome

Merged with the review changes applied. Three conflicts when bringing `dev` forward, all
resolved by taking both sides: CGP's rewritten comments on `cameraSwitch_labelBootSlot()`
plus this branch's loud-failure-and-retry-once label write, and the op22 default of 50 with
CGP's clarification that 0 means dim rather than off.

**The half-shipped protocol, found later on hardware**

This is the part worth knowing. The AI-processor side of the sliding-window transfer merged
here, but the matching nRF side sat unmerged in [ww-hardware#26] (which contained
[ww-hardware#27]). A device on current `dev` firmware with released BLE firmware therefore
could not receive files at all: the transfer accepted `FILE_START`, wrote one packet, then
stalled with zero packets acknowledged until the app timed out.

`if_task.c` re-arms the I2C slave receiver the instant the master finishes reading each
ACK, precisely because the nRF writes the next packet immediately. There is no version
handshake and no fallback, so a mismatched pair fails silently rather than degrading.

Found on 18 August while testing the merged result through the mobile app, and fixed by
merging [ww-hardware#26] and flashing BLE firmware 0.30.47. Both transfers then completed
with every packet acknowledged and CRC verified.

The general lesson, now recorded in the agent skill: **two-sided protocol changes must be
released together**, and neither side currently detects the mismatch.

## Open items

- [#168] `adjustInactivityPeriod()` to pair with `restoreInactivityPeriod()`
- [#170] separate engineering-only code from production builds

Related but filed from the same testing session: [#195] (no capability or protocol-version
query, so consumers cannot detect a mismatch) and [ww-mobile-app#243].

Full set on the [project board](https://github.com/orgs/wildlifeai/projects/3), labelled
`review-finding`.

## Files

| File | What it is |
|---|---|
| [`review_responses_pr142_pr140.md`](review_responses_pr142_pr140.md) | Responses to **both** the #142 and #140 reviews. Kept whole rather than split across two folders, because it was written as one exchange: §1 is #142, §2 is #140's provenance questions, §3 and §4 span both |
| [CGP's review report](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/CGP_Code_Review_PR142.md) | Charles's review report for this PR, if present at the branch tip |

## Related threads

- [PR #141 review](../2026-08-06_pr141-camera-features-review/README.md)
- [PR #140 review](../2026-08-10_pr140-rp3-image-quality-review/README.md)

[#168]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/168
[#170]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/170
[#195]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/195
[ww-hardware#26]: https://github.com/wildlifeai/ww-hardware/pull/26
[ww-hardware#27]: https://github.com/wildlifeai/ww-hardware/pull/27
[ww-mobile-app#243]: https://github.com/wildlifeai/ww-mobile-app/issues/243
