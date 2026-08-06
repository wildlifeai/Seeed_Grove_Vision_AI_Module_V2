# 0003. Defaults for automatic camera switching (op26/op24)

Status: Proposed (decision expected at the CGP review call, Aug 2026)
Thread: [../development reports/2026-07_pr141-review-cgp/call_preread.md](../development%20reports/2026-07_pr141-review-cgp/call_preread.md) §5

## Context

op26=1 (automatic day/night switching) requires periodic light checks: an RTC wake every
op24 minutes (default 15) plus a ~1.9 s AE burst around captures. PR #141 ships
battery-neutral defaults (op26=0); PR #142 flips op26 to 1, which turns the 15-minute
heartbeat on for every deployment — measured at roughly 8–16 minutes of extra awake time
per day on an otherwise idle device, the dominant idle-battery term. A single-image device
with op26=1 wakes forever only to log "staying".

## Decision (proposed options — pick one)

a) Ship op26=0; the app enables it on dual-camera installs.
b) Ship op26=1 with op24 raised (60 min: dawn/dusk needs ~4 checks/day, not 96).
c) Ship op26=1 but arm the periodic wake only when the other slot is labelled with the
   opposite variant (no wasted wakes on single-image devices).

## Consequences

(To be completed when accepted: chosen option, battery estimate, app-side implications,
doc updates — `Operational_Parameters.md`, `config_file.md`, PR #142 defaults commit.)
