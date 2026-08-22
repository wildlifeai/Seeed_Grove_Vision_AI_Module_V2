# PR #140 review: RP3 image quality, live preview, AE and AWB

**Status:** closed, 18 August 2026. Merged to `dev` as `8cbb6a2b`.

Charles Palmer's review of `feat/uart-live-preview` (PR #140): live image preview over the
console UART, highlight-metered auto-exposure, auto white balance and the full colour
pipeline for the RP3 (IMX708) camera. Reviewed alongside PR #142, so the response document
lives in that thread.

Review-for-direction rather than line-by-line: the work is best judged by running the live
preview (`live_view.py`) against the hardware.

## Outcome

**Merged**, 27 files, +2,589 lines. Four conflicts when bringing it forward, and their
shape is worth recording: all but one were **cosmetic**, created by CGP's C file formatting
work reordering functions and rewriting doc comments on top of code that had also changed
on `dev`.

Checked function by function with comments and whitespace stripped: identical inventory on
both sides, and the only real differences were punctuation inside two log strings. The
resolution was to take #140's layout wholesale. Only `ww500_md.mk` had genuine content on
both sides.

**Position on image quality:** #141's op27/op28 fixed gains were the interim fix; this PR
is the endpoint (highlight-metered AE op29/op30, auto-WB op31 with op27/op28 fallback, full
RPi-order pipeline; bench G/R 0.97 to 1.01). Method was instrumentation first: live preview
plus `live_view.py` gave a ~1 fps tuning loop, `tune_stats.py` scored frames against a
phone reference.

**Still not solved:** deployed integration-preview photos showed a remaining gap. `ae.c`
state is RAM-only and limited to 3 steps per wake from the 0x0940 default, so bright scenes
never converge on the wake path. Camreg-staged exposure was validated on the bench as a
zero-code mitigation; the durable fix is persisting converged exposure across DPD, which is
[#158] and remains open.

**A defect this PR's tooling introduced**, found while updating the build guide:
`device_image` clears `R*.IMG` and `H*.IMG` before writing, so building the second camera
variant silently deletes the first variant's image. Confirmed by building both legs in
sequence, after which only the HM0360 image remained. That is [#190], one line to fix.

## Open items

- **Image quality:** [#153] (EXIF Model tag says HM0360 on RP3 photos), [#159] (record
  IMX708 exposure telemetry in the MakerNote), [#172] (collate the RP3 deficits into one
  document), [#173] (profile the colour-correction pipeline), [#174] (verify against a
  colour test card), [#175] (decide: on-device or server-side correction), [#161]
  (benchmark `library/JPEGENC`).
- **Build tooling from this PR:** [#190] (`device_image` deletes the other variant), [#194]
  (`2>NUL` writes a stray file on Linux), [#179] (build guide, fixed in [#191]).
- **Blocked on [#143] and [#144]:** [#158] (persist AE exposure and gain across DPD, the
  wake-path white-outs), [#154] (sampling burst runs once per image instead of once per
  wake), [#160], [#180].

Full set on the [project board](https://github.com/orgs/wildlifeai/projects/3), labelled
`review-finding`.

## Files

| File | What it is |
|---|---|
| [`review_responses_pr142_pr140.md`](../2026-08-08_pr142-ble-fast-transfer-review/review_responses_pr142_pr140.md) | Responses to both the #142 and #140 reviews. §2 is this PR's provenance questions, §3 and §4 span both. Lives in the #142 thread because it was written as one exchange |
| [CGP's review report](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/CGP_Code_Review_PR140.md) | Charles's review report for this PR |
| [`rp3-image-quality-plan.md`](../../rp3-image-quality-plan.md) | Durable topic doc produced by this work |
| [`live_preview.md`](../../live_preview.md) | Durable topic doc for the live preview and `live_view.py` |

## Related threads

- [PR #141 review](../2026-08-06_pr141-camera-features-review/README.md)
- [PR #142 review](../2026-08-08_pr142-ble-fast-transfer-review/README.md)

[#143]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/143
[#144]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/144
[#153]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/153
[#154]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/154
[#158]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/158
[#159]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/159
[#160]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/160
[#161]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/161
[#172]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/172
[#173]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/173
[#174]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/174
[#175]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/175
[#179]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/179
[#180]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/180
[#190]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/190
[#191]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/191
[#194]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/194
