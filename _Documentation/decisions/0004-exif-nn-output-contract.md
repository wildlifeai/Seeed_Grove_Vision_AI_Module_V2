# 0004. EXIF NN output contract (logits vs percentages)

Status: Proposed (decision expected at the CGP review call, Aug 2026)
Thread: [../development reports/2026-07_pr141-review-cgp/review_response_and_next_prs.md](../development%20reports/2026-07_pr141-review-cgp/review_response_and_next_prs.md) §3

## Context

PR #141 enabled `USE_PERCENTAGE`, so the EXIF UserComment carries percentages; the
ww-website MakerNote-ingestion branch consumes label+confidence in that form. CGP argues
for restoring raw logits: simpler, no floating point on the device (the only FP user in
the codebase), and the server can derive percentages at any time. Separately: the flash
field must stay EXIF-spec in tag 0x9209 (LED *type* belongs in the MakerNote), and RP3
photos currently record the HM0360 light-sensor's AE registers while the IMX708's own
exposure state is recorded nowhere — field photos cannot self-diagnose exposure bugs.

## Decision (proposed)

Decide the device↔cloud contract with the website work in view: logits or percentages,
one place. Whichever is chosen: fix the EXIF `Model` `#ifdef` ordering (RP3 photos
currently mislabelled `WW500 HM0360`); keep 0x9209 spec-compliant; extend the RP3
MakerNote with IMX708 exposure/gain and op29–31 state.

## Consequences

(To be completed when accepted: chosen encoding, website parser alignment, removal or
retention of `USE_PERCENTAGE`/`NN_confidence_EXIF_not_written.md`, app/website heads-up
for the Flash-field change.)
