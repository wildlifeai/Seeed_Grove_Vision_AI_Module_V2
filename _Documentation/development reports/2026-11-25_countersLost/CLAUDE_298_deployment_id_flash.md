# #298: deployment ID and GPS position are lost if CONFIG.TXT is damaged

#### File: `CLAUDE_298_deployment_id_flash.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/CLAUDE_298_deployment_id_flash.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 26 November 2026, updated 27 November 2026

## In short

- The text of [#298](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/298), raised 26 November 2026, and the fix (last section).
- **The fix (27 November):** a third flash record, "deployment", holds a copy of the deployment ID and
  GPS position. It supplies whichever of them CONFIG.TXT doesn't give: no card, no file (a blank
  card), a damaged file, or a missing `I` or `G` line. After damage it restores them before the new
  image folder is chosen.
- **State: fixed.** Every case benched on HM0360 (`logs/ai12.txt` to `ai16.txt`), the main ones on
  RP3 (`ai17.txt`). Committed `e483dc7b`; #298 closed 27 November 2026.
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

---

## The fix, 27 November 2026

Decided (Charles), first: use the flash copy only when CONFIG.TXT is damaged or unreadable, so a
blank card for a new deployment would not inherit the old ID.

**Changed twice the same day, after testing.** First: that left two field cases broken. A **blank card** swapped
in during a deployment got the zero ID. And with **no card**, the save at sleep wrote the zero ID over
the flash copy, so the next card had nothing to restore. Since a new deployment is set by the app
anyway (`setdid`, plus ops 19 and 20), the rule is now: **use the flash copy whenever no readable
CONFIG.TXT was loaded** (no card, no CONFIG.TXT, damaged, unreadable). A readable CONFIG.TXT always
wins, even with no `I` or `G` line.

Second (Charles, after deleting a `G` line on the PC and getting zero): a readable file without the
line still gave the default, which was inconsistent with a missing file giving the flash copy. So
the rule is now **per line**:

| CONFIG.TXT | Deployment ID / GPS position from |
|---|---|
| Has the `I` / `G` line | CONFIG.TXT |
| Line missing (removed by hand) | Flash, for that line only |
| Missing (blank card) or no card | Flash |
| Damaged or unreadable | Flash, and bit 12 |

The camera writes both lines whenever it saves, so a line is missing only if removed by hand. To set
the zero ID on purpose, use `setdid` with the zero UUID. With the same card put back after a no-card
wake, everything resumes from its CONFIG.TXT. A `setgps` made with no card is lost when a card with a
`G` line goes back in, as for the settings.

| Where | Change |
|---|---|
| `xip_state.c/.h` | Record type `XIP_STATE_TYPE_DEPLOYMENT` in the sector at `0x00F02000`, own version `XIP_STATE_DEPLOYMENT_VERSION` 1. Payload: the deployment ID string and the three GPS structures (about 108 of 240 bytes). `xip_state_loadDeployment()`, `xip_state_applyDeployment()`, `xip_state_saveDeployment()`. `xipstate` lists it |
| `xip_manager.h` | Saved-state area 0x2000 to 0x3000 bytes |
| `fatfs_task.c` | Read at boot with the fast-start record, into RAM: the flash can't be read once the NN may be running. After a damaged CONFIG.TXT, applied before `dir_mgr_useNextImageDir()`, so the next image folder is looked for in the deployment's own folder. Written at sleep only if changed |
| Docs | `WW500_XIP_saved_state.md` (new section), the flash maps in `slot_selector.md` and `WW500_Neural_Network_Operation.md`, `hardware-traps.md` |

**Cost:** one more 4 KB flash read at every wake, before the camera starts. The boot timing line
(`flash record read N`) shows it; it was about 16 to 21 ms after `app_main()` before.

**Bench test:**
1. `setdid` a test UUID and `setgps` a position; let it sleep. Expect `Deployment record: written`.
2. Next wake: no `Deployment record: written` (unchanged). Note `flash record read` in the boot
   timing line.
3. Empty CONFIG.TXT on a PC, wake. Expect the red `Deployment ID ... and GPS position restored from
   flash`, then `Counters lost: new photos go in IMAGES.NNN` under `/MEDIA/<first 8 of the ID>/`.
4. `getdid` and `getgps` show the restored values; a photo's EXIF has them.
5. `xipstate` lists the deployment sector.

**Bench results, HM0360 image** ([`logs/ai12.txt`](logs/ai12.txt) to [`logs/ai14.txt`](logs/ai14.txt)), with
folders `/MEDIA/00000000/` (to `IMAGES.010`) and `/MEDIA/3F2A9C1E/` (to `IMAGES.015`) on the card:

| Test | Result |
|---|---|
| 1, `I 3f2a9c1e-...` and `G` lines added to CONFIG.TXT by hand | `Deployment record: written (deployment ID 3f2a9c1e-...)` at sleep |
| 2, unchanged | No rewrite. `getdid`, `getgps`, `xipstate` right. `flash record read 21` ms, as before #298 |
| 3, CONFIG.TXT emptied | Restored from flash; new folder `/MEDIA/3f2a9c1e/IMAGES.017`; bit `0x1000` |
| 4, EXIF of the photos | Deployment ID (tag `0xf200`) and GPS position right |
| 5, `I`/`G` lines deleted, op lines kept | (Old rule) CONFIG.TXT won: zero ID, flash copy rewritten. Under the per-line rule, expect the flash values instead: to retest |
| 6, CONFIG.TXT emptied again | Zero ID restored; new folder `/MEDIA/00000000/IMAGES.018` |
| 7, `setdid 3f2a9c1e-...` | `Deployment record: written` at sleep |

Found in testing, no code change (Charles): ops 19 and 20 belong to the card, not the deployment, so
a new deployment ID keeps the old folder number. The app should set ops 19 and 20 with a new ID
([note to Victor](CLAUDE_note_for_Victor_bit12.md)). The console hang pasting `setdid` is #285.

**No card and blank card, HM0360 image built 15:03** ([`logs/ai15.txt`](logs/ai15.txt)):

| Test | Result |
|---|---|
| No card, cold boot and a wake with `getdid` | `No SD card: deployment ID 4f2a9c1e-... and GPS position from flash`; `getdid` right; no `Deployment record: written` with zeros |
| Blank card | `'CONFIG.TXT' NOT found.`, `No CONFIG.TXT: deployment ID 4f2a9c1e-... from flash`, photos in `/MEDIA/4f2a9c1e/IMAGES.000` |

**Per-line rule, HM0360 image built 15:24** ([`logs/ai16.txt`](logs/ai16.txt)):

| Test | Result |
|---|---|
| `setgps` with no card | `Device GPS set`, then `Deployment record: written` at sleep; `getgps` right with no card |
| Card back, its CONFIG.TXT with both `I` and `G` lines deleted | `'CONFIG.TXT' has no 'I' or 'G' line: taken from flash (deployment ID 4f2a9c1e-...)`; photo in `/MEDIA/4f2a9c1e/IMAGES.018`; next wake normal, `getgps` right |

Not run: only one of the two lines missing. The code treats the lines separately.

`Error: MSB = 1` on a cold boot with no card comes from the SD driver (`mmc_we2_spi.c`): no card
answering. Harmless.

**RP3 image built 15:36** ([`logs/ai17.txt`](logs/ai17.txt)): no card (`getop`, `getgps`, `getdid`
right, from flash), then the card with both lines deleted (`has no 'I' or 'G' line: taken from
flash`), photos in `/MEDIA/4f2a9c1e/IMAGES.018`, later wakes normal. Passed.
