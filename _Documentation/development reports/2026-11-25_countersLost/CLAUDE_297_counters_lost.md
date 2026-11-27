# #297: operational parameter counters lost from CONFIG.TXT

#### File: `CLAUDE_297_counters_lost.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/CLAUDE_297_counters_lost.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 26 November 2026, updated 27 November 2026

## In short

- **The issue:** [#297](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/297). Thread [README](README.md).
- **The fault.** On 23 November, after a run of BLE processor resets, the AI processor's counters
  (photo number, NN counts, boot counts) went to 0 while its settings survived.
- **Why it matters.** The LoRaWAN uplink then reports NN counts of 0, which the server sees as a
  reset camera. The image folder count also restarts, so new photos go into the wrong folders:
  back into `IMAGES.000` and on, mixed in with the photos already there, and folders grow past
  their 100-photo limit. File names come from the photo time, so nothing is overwritten (except in
  the #295 case, section 2). No error was reported, so the loss went unnoticed.
- **The cause.** The counters (ops 0 to 4, 19, 20) are kept only in CONFIG.TXT; the settings are
  also in a flash record. A save emptied CONFIG.TXT before rewriting it, so a reset during the save,
  or a boot that failed to read the file and then saved its zeros, lost the counters.
- **The fix, in three parts** (section 2):
  1. **Make a loss much less likely.** The new CONFIG.TXT is written in full as `CONFIG.TMP` before it
     replaces the old one, so a reset part way through leaves a complete file, old or new. A CONFIG.TXT
     that can't be read is not saved over.
  2. **If the counters are lost anyway, keep new photos apart.** They go into a new image folder, after
     the highest on the card, instead of back into `IMAGES.000`.
  3. **Report it.** Self-test bit 12 goes to the BLE processor, which sends it to the LoRaWAN server
     straight away in a health uplink (and to the app, if connected), plus a red console line.
- **Found on the way.** The deployment ID and GPS position are also only in CONFIG.TXT:
  [#298](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/298) ([text](CLAUDE_298_deployment_id_flash.md)).
- **State.** Part 1 benched on the RP3 image, parts 2 and 3 on the HM0360 image. Every test passed.
  Committed `52e69958`, 26 November.

## 1. Evidence

- **23 November**, found by the BLE processor Claude
  ([CLAUDE_note_AI_counters_lost.md](CLAUDE_note_AI_counters_lost.md), sections 1 to 4, with its
  questions). From the BLE processor session's pasted console: after the resets the `Sleep`
  list began `0 0 0 18 1`. Op 4 (warm boots) at 1 means every counter was reset, not just the NN
  counts; op 3 at 18 had been counting again since.
- **26 November** ([`logs/ai7.txt`](logs/ai7.txt), [`logs/ble7.txt`](logs/ble7.txt)): eight
  `hack 6` resets did not reproduce it. Every /RST came while the AI processor was in DPD, and the
  save window is tens of milliseconds, too short to hit by hand.

## 2. The fix

### Part 1: make a loss much less likely (`fatfs_task.c`)

`CONFIG.TMP` is not a backup kept alongside CONFIG.TXT. It exists only during a save:

1. The new contents are written to `CONFIG.TMP` and every write is checked.
2. Only then is CONFIG.TXT deleted and `CONFIG.TMP` renamed to CONFIG.TXT.

So at every moment the card holds one complete copy. A reset before step 2 leaves the old
CONFIG.TXT (and a part-written `CONFIG.TMP`). A reset between the delete and the rename leaves only
the complete `CONFIG.TMP`. At the next boot `recoverConfigTmp()` deals with what is left:

| On the card at boot | Action | Counters |
|---|---|---|
| CONFIG.TXT only | None (the usual case) | Kept |
| `CONFIG.TMP` only | Renamed to CONFIG.TXT | Kept |
| Both | `CONFIG.TMP` deleted | Kept, from CONFIG.TXT |

The old code opened CONFIG.TXT with `FA_CREATE_ALWAYS`, which empties it first: a reset then left
an empty file.

Also: if CONFIG.TXT exists but can't be read, it is not saved over at the next sleep (the RAM holds
default counters, not its values). The next boot tries to read it again.

`STATE_FILE_TMP` is defined next to `STATE_FILE` in `directory_manager.h`.

### Part 2: if the counters are lost, keep new photos apart (`directory_manager.c`)

Ops 19 and 20 choose the image folder. At 0, new photos would go back into `IMAGES.000` on top of
the photos already there. Instead, `dir_mgr_useNextImageDir()` finds the highest `IMAGES.NNN` in the
deployment's folder and starts the next one. Nothing is overwritten either way, as file names come
from the photo time, except in the #295 case (2024 times after a reset can repeat).

### Part 3: report it (`selfTest.h`, `fatfs_task.c`)

- **Self-test bit 12** (`0x1000`, `SELF_TEST_AI_CONFIG_DAMAGED`) is set for that wake. The BLE
  processor sends a health uplink to the LoRaWAN server as soon as a self-test bit changes, so the
  server sees it within seconds. The app sees it only if connected during that wake;
  [ww-hardware #83](https://github.com/wildlifeai/ww-hardware/issues/83) asks the BLE processor to
  keep it until reported ([details](CLAUDE_issue_83_BLE_selftest_bit12.md)).
- **Console:** red lines `'CONFIG.TXT' has no op parameters (damaged)` or `could not be read`, and
  `Counters lost: new photos go in IMAGES.NNN`.

Bit 12 was the PDM microphone, never fitted. It is a cross-repo contract: Victor (app and website
labels, [note](CLAUDE_note_for_Victor_bit12.md)) and the BLE processor's `selfTest.h` must follow.

### What it can't do

The lost counters themselves can't be rebuilt on the AI processor: they were only in the damaged
file. The NN counts could be restored by the BLE processor, which has the last good values
(ww-hardware #83, part 3, optional).

## 3. Bench results

Save and recovery, RP3 image, logs [`logs/ai8.txt`](logs/ai8.txt) and [`logs/ai9.txt`](logs/ai9.txt), card prepared on a PC:

| Test | Card | Result |
|---|---|---|
| 1 | Unchanged | Saves work, counters count, no `CONFIG.TMP` left |
| 2 | CONFIG.TXT renamed to `CONFIG.TMP` | `using 'CONFIG.TMP'`, counters kept |
| 3 | A part-written `CONFIG.TMP` beside CONFIG.TXT | `'CONFIG.TMP' ... deleted, 'CONFIG.TXT' kept`, counters kept |
| 4 | CONFIG.TXT with no op lines (the old damage) | Red `has no op parameters (damaged)`; counters 0, settings kept |

Bit 12 and the new folder, HM0360 image built 12:35 ([`logs/ai10.txt`](logs/ai10.txt),
[`logs/ble10.txt`](logs/ble10.txt)). The card had `IMAGES.000` to `IMAGES.009`, but op 20 was 0: the
camera had been writing into `IMAGES.000` again since an earlier loss, probably 23 November.

| Test | Result |
|---|---|
| A, good CONFIG.TXT | Normal boot, no red lines, self-test `0x0000` |
| B, CONFIG.TXT emptied | Red `has no op parameters (damaged)`, `Counters lost: new photos go in IMAGES.010`, folder created, final `selfTest 1000`. The BLE processor sent a health uplink with self-test `0x1000`, acknowledged |
| After B | The next wake read the CONFIG.TXT saved after the damage: no bit 12, still `IMAGES.010`. A photo (`B077CA70.JPG`) went there and op 0 counted 1 |

At the next wake bit 12 cleared, but the BLE processor held that change back until the next ping
(one health event per ping period). So the server shows `0x1000` until then.

After a loss op 0 is 0, also the "no SD card" value, until the next photo. Left as it is.
