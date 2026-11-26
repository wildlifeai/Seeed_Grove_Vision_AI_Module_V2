# #298: deployment ID and GPS position are lost if CONFIG.TXT is damaged

#### File: `CLAUDE_298_deployment_id_flash.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/CLAUDE_298_deployment_id_flash.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 26 November 2026

## In short

- The text of [#298](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/298), raised 26 November 2026. Not yet worked on.
- Found while fixing [#297](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/297) ([record](CLAUDE_297_counters_lost.md), thread [README](README.md)).

---

**Title:** Deployment ID and GPS position are lost if CONFIG.TXT is damaged

## Problem

The deployment ID (`I` line) and the GPS position (`G` line) are kept only in `/MANIFEST/CONFIG.TXT`.
The settings (ops 5 to 38) are also kept in the fast-start record in flash, so they survive a damaged
or unreadable CONFIG.TXT. The deployment ID and GPS position do not.

If CONFIG.TXT is damaged:

- The deployment ID falls back to the zero UUID. New photos go into `/MEDIA/00000000/` and carry the
  zero UUID in their EXIF `DeploymentID`, so the website can't attribute them to the deployment.
- The GPS position falls back to its default, and the photos' EXIF position is wrong.
- Both stay wrong until the app sets them again (`setdid`, `setgps`).

Since #297 (26 November 2026) a damaged CONFIG.TXT is much less likely (saved via `CONFIG.TMP`),
and is reported by self-test bit 12. But a card fault or a file edited on
a PC can still damage it.

## Proposal

Keep both in flash too, and use the flash copy when CONFIG.TXT has none:

- They rarely change, so the flash copy is rarely written.
- The fast-start record payload (`xip_state.c`) is 162 of 240 bytes. The UUID as 16 bytes plus the
  GPS position may fit in the 78 left. That needs a new `XIP_STATE_FORMAT_VERSION`. Otherwise use a
  third record type in the saved-state area, which has room (`xip_manager.h`: 0x00F02000 onwards is
  unused).
- At boot, a CONFIG.TXT `I` or `G` line still wins, as for the settings.

Not a cross-repo change: the EXIF fields and the `setdid`/`setgps` commands stay the same.

## Done when

- With CONFIG.TXT cut to its `#` lines on a PC, the next photo carries the deployment ID and GPS
  position set before, and goes into the deployment's own `/MEDIA` folder.
- Benched on both camera images.
