# XIP save state: the steps and their results

#### File: `CLAUDE_XIP_save_state_steps.md`
#### Path: `_Documentation\development reports\2026-10-27_XIP_save_state\CLAUDE_XIP_save_state_steps.md`
#### Author: Claude (Opus 5.5), with Charles Palmer
#### Date: 27 October 2026

A log of the "Proposed steps" in [CLAUDE_XIP_save_state_proposal.md](CLAUDE_XIP_save_state_proposal.md),
done one at a time on branch `261027_XIP_save_state`. Each step records what was done, what was
found, and how it was tested (with the firmware build, `AI ver`).

| Step | What | Status |
|---|---|---|
| 1 | Checks: the flash chip, and the model area bounds | Done 27 Oct (code reading only); model size check coded 28 Oct |
| 2 | RP3 exposure as ops 37 and 38 (#158) | Done 28 Oct, benched (RP3) |
| 3 | The flash record module and its test | Done 28 Oct, benched on both images |
| 4 | Fast-start settings, still waiting for the card | Not started |
| 5 | Stop waiting for the card | Not started |
| 6 | Optional extras | Not started |
| 7 | Docs | Not started |

---

## Step 1. Checks (27 October 2026)

No code was changed. Done by reading the code and the existing logs.

### The flash chip

- **Part:** Winbond W25Q128JW, 16 MB, 1.8 V. The boot console prints `Flash ID 0xEF`
  (`init_flash()`, `xip_manager.c`), and 0xEF is Winbond's manufacturer code. 124 boot logs in the
  repo agree. `swd_debugging/swdflash/CLAUDE.md` names the part.
- **What matters for the design** (W25Q JW family datasheet figures as I know them; there is no copy
  of the datasheet in the repo, so these are **to be confirmed**):

  | | Typical | Maximum |
  |---|---|---|
  | Erase / program cycles per sector | at least 100,000 | |
  | 4 KB sector erase | about 45 ms | about 400 ms |
  | Page program (up to 256 bytes) | about 0.4 ms | about 3 ms |

- **What this means:**
  - **Wear is not a problem.** A fast-start sector holding 32 records of 128 bytes is erased once
    every 32 writes. At 100,000 erases that is 3.2 million writes. Even one write at every wake,
    with a motion wake every minute all day, would last about 6 years. Writing only when something
    has changed makes it far longer.
  - **The erase is slow but rare.** It happens once in 32 writes, at DPD entry, off the path to the
    first photo. The image task must allow up to about 400 ms for it.
  - **A record must not cross a 256-byte page,** since one program command cannot cross a page
    boundary. Records of 128 bytes, or any size that divides 256, placed on matching boundaries,
    are safe.

### Can loading a model erase into the new area?

**Yes, in principle. This needs fixing before the new area is used.**

- The model area is 0x00200000 to 0x00EFFFFF (13 MB). `xip_copy_model_from_sd_to_flash()` erases
  `align_up(sizeof(ModelMetaData), 16) + fileSize` bytes from 0x00200000
  (`xip_erase_model_flash_area()`, `xip_manager.c:482`), with **no upper limit**.
  `FLASH_MODEL_AREA_SIZE` (13 MB) is defined at `xip_manager.c:84` but never used.
- So a model file larger than about 13 MB would erase into 0x00F00000, the new fast-start area. A
  file larger than about 14.9 MB would also erase the slot selector at 0x00FFF000, and the camera
  might not boot.
- Real models are far smaller (the person detector is a few hundred KB). But a wrong file named
  `.TFL` is not impossible: #241 was a ZIP file loaded as a model.
- **Suggested fix:** in `xip_copy_model_from_sd_to_flash()`, refuse a model whose
  `flashSizeRequired` is larger than `FLASH_MODEL_AREA_SIZE`, before anything is erased. About five
  lines.

### Other flash writers

- Only `xip_manager.c` erases or writes the flash.
- **Firmware updates are bounded:** a file larger than a slot is refused (`xip_manager.c:1332`), and
  the erase stays inside the slot (`:1182`).
- **The slot selector** writes only its own sector (0x00FFF000).

So once the model size check is in, nothing else can reach 0x00F00000 to 0x00FEFFFF.

### Result

- The flash is suitable, with the figures above to be confirmed against the datasheet.
- **One fix is needed first:** the model size check. It is a small, separate change, best done at the
  start of step 3 (the flash record module), since that is the first step that uses the new area.

### Model size check, coded 28 October 2026

`xip_copy_model_from_sd_to_flash()` (`xip_manager.c`) now refuses a model file larger than the model
area, less the metadata, before anything is erased. It prints
`Model file too big for the model area (N bytes max)` and returns false. The check is on the file
size before the metadata is added, so a huge file cannot wrap the sum. 13 MB is a whole number of
64 KB blocks, so an accepted file's erase stays inside the area.

---

## Step 2. RP3 exposure as ops 37 and 38 (#158), coded 28 October 2026

Built and benched on the RP3 image, 28 October 2026 (see "Bench result" below).

### The names

| Op | Name | Range | Meaning |
|---|---|---|---|
| 37 | `OP_PARAMETER_CAM_AE_EXPOSURE` | 0, or 8 to 5000 | Exposure (lines) the AE loop last reached. 0 = none saved |
| 38 | `OP_PARAMETER_CAM_AE_GAIN` | 0 to 960 | Analog gain code reached with op 37 (0 = 1x, 960 = 16x) |

The `CAM_AE_` prefix matches ops 29 and 30. Both are state, not settings, like op 25. Both default
to 0.

### What changed

- **`ae.c`**: after each step, `ae_process()` copies the exposure and gain into ops 37 and 38. They
  reach CONFIG.TXT with the rest at sleep (`APP_MSG_FATFSTASK_SAVE_STATE`). At sensor init,
  `ae_notifySensorInit()` writes them back to the sensor and the loop carries on from them,
  printing `AE: restored exposure N gain code M`. Nothing is restored when op 29 = 0 (AE off) or
  op 37 = 0, so a first boot or a new card behaves as before.
- **Order is safe:** the restore runs after `cisdp_sensor_init()` (which writes the 2368-line table
  value) and after the staged register file, and before streaming starts. Stream-on writes only
  register 0x0100. The sensor init runs after the image task's wait for the card, so ops 37 and 38
  have been read from CONFIG.TXT by then. **Step 5 removes that wait, so the flash record must carry
  ops 37 and 38 (planned in step 4).**
- **A saved exposure overrides a staged exposure** in `RPV3_EX.BIN` (the interim fix for #158).
  Before, AE assumed 2368 lines while the staged value was in the sensor.
- **`ae.h`**: the exposure and gain limits moved here from `ae.c`, since they are also the op ranges.
- **`fatfs_task.c/.h`**: the two ops, defaults 0, ranges in `fatfs_isValidOperationalParameter()`
  (so `setop` rejects a bad value), and a bad pair in CONFIG.TXT is replaced by 0.
- **HM0360 image:** does not use them, and passes them through CONFIG.TXT unchanged.
- **`if_task.c`, the "Sleep" message:** it lists every op for the BLE processor. The buffer was 150
  bytes, and each value was written with the full buffer size from its offset, so a long list could
  overrun it. It is now 245 bytes (the 244-byte payload limit plus the NUL), static, and each write
  is bounded by the space left. 39 ops of at most `65535 ` plus `Sleep ` is 240 characters, so 39
  ops always fit. In practice the message is about 110 bytes (160 for a busy camera), so more ops
  fit with ordinary values; past 39, unusually large values could cut the last ops off (safely).
- **`MANIFEST/CONFIG.TXT` and `config_file.md`**: ops 37 and 38 added.

### Cross-repo

- The BLE processor reads the "Sleep" message up to its own op count (`aiProcessorSaveStats()`,
  ww-hardware `aiProcessor.c`), so the two extra values are ignored and nothing breaks.
- To do, ww-hardware: add ops 37 and 38 to `aiProcessor.h`, which mirrors the op list.
- The app and website do not need to show them (state, not settings).

### Bench test (RP3 image, in steady lamp light)

Captures are refused until the first sleep after a cold boot, so timelapse wakes do the work.

1. Build and flash the RP3 image. Use an SD card whose CONFIG.TXT has no lines 37 and 38 (an
   existing card), and power cycle.
2. Type `getop 37`. It shows 0: nothing saved yet.
3. Type `setop 7 30`, so the camera wakes for a timelapse picture 30 s after each sleep. Let it
   sleep. The console shows `Saved state to SD card.`
4. At the first wake there is no `AE: restored` line, and the `AE:` lines start from 2368
   (`exposure 2368->...`).
5. While it is awake, type `getop 37` and `getop 38`. They show the exposure and gain code from the
   last `AE:` line.
6. At the next wake, the console shows `AE: restored exposure N gain code M`, with the values from
   step 5. The first `AE: p75=` line is close to the target 110, or there is none (already within
   the deadband). Before this change, every wake started from 2368.
7. Type `setop 37 6000`. It replies `Error: 6000 is not a valid value for op 37.`
8. Type `setop 29 0` (AE off) and wait for the next wake. No `AE: restored` line. Then type
   `setop 29 1`.
9. Type `setop 7 0` (or the timelapse value you had before) to finish.

### Bench result (28 October 2026, RP3 image, build 08:23 28 Oct)

- **Timelapse wakes alone** (`logs/log1.log`): no `AE:` lines and ops 37/38 stayed 0. Correct: at
  the table exposure (2368 lines) the frame was already within the AE deadband (p75 110 +/- 8), and
  `ae_process()` neither prints nor saves when it makes no change. `setop 37 6000` was rejected,
  and the "Sleep" message carried all 39 values (114 bytes).
- **Live view** (AE not limited to 3 steps): op 37 = 2645, op 38 = 0.
- `setop 30 200`: op 37 = 4866 (about 1.84 times, as expected for the higher target with the
  pedestal), op 38 = 0. `setop 30 110`: op 37 = 2670.
- **Restore:** after DPD the wake printed `AE: restored exposure 2315 gain code 0`.

**Watch for step 4 (Charles):** in the same scene the settled exposure wandered between 2315 and
2670, about 15 %, because the deadband lets AE stop anywhere in a band of about +/- 8 %. If the flash
record were written whenever op 37 changed, it could be written at almost every wake. The proposal's
"write when it moves more than 10 %" is too tight for this. In step 4, log each record write and
count them over a day of wakes, and set the threshold from that (25 % is a likely start).

### Draft comment for #158 (for Charles to post when the bench test passes)

> Fixed on `261027_XIP_save_state`. Two new op parameters hold the RP3 auto-exposure state:
> op 37 `OP_PARAMETER_CAM_AE_EXPOSURE` (lines, 8-5000, 0 = none saved) and op 38
> `OP_PARAMETER_CAM_AE_GAIN` (gain code 0-960). `ae.c` updates them after each step, they are saved
> to CONFIG.TXT before DPD, and at the next sensor init they are written back to the sensor, so each
> wake starts from the last exposure instead of 2368 lines. Not used when op 29 = 0. Benched on
> 28 October 2026: values saved and restored across DPD (`AE: restored exposure 2315 gain code 0`). The ww-hardware `aiProcessor.h` op list still needs ops 37 and 38.
> Later, step 4 of the XIP save state work copies them into flash so they are available before the
> card is ready.

---

## Step 3. The flash record module and its test, coded 28 October 2026

Built and benched 28 October 2026 (see "Bench result" below). No change to how the camera behaves: nothing writes a real record yet
(that is step 4).

### What changed

- **`xip_manager.c/.h`:** three functions, `xip_state_area_read()`, `_write()` and `_erase()`, the
  only way into the saved-state area (0x00F00000 to 0x00F01FFF, `XIP_STATE_AREA_ADDR/SIZE`). They
  refuse any address outside it. They use the existing `disable_xip()`/`enable_xip()` (SPI mutex,
  watchdog suspended). The flash map comment in `xip_manager.h` now shows the area.
- **New `xip_state.c/.h`:** the records.
  - **Record:** 256 bytes, one flash page: magic `WWXS`, format version, type, payload length,
    sequence number, 240 bytes of payload, CRC-16/CCITT (the existing `crc16_ccitt.c`).
  - **16 slots per 4 KB sector.** Fast-start records at 0x00F00000. The board record sector
    (0x00F01000) is in the format but `xip_state_write()` refuses it for now.
  - **Write:** scan the sector, use the slot after the last used one (a part-written slot is never
    reused), erase first if the sector is full, then read the record back and compare.
  - **Read:** the valid record with the highest sequence number, or "none" (the caller uses the C
    defaults).
  - Reads use SPI commands, not the XIP window, so they never see cached data.
  - Working memory (about 4.4 KB) comes from the heap for each call, not a task stack.
- **`CLI-commands.c`:** the `xipstate` command. `xipstate` lists every slot of both sectors;
  `xipstate test` runs the bench test. Both are refused unless the image task is idle (`Init`),
  since switching XIP off while the NN runs from flash would fault it.

### Changed from the proposal

- **256-byte records, not 128.** Step 4's payload is about 30 bytes of ops plus the extra camera
  registers (up to 24 entries of 3 bytes, 73 bytes). 128-byte records would leave little room.
  With 256: 16 records per sector, an erase every 16 writes, still about 1.6 million writes before
  wear-out.
- **So the test is smaller:** 33 writes and 2 erases fill the sector twice, plus an erase at the
  start and one for the fall-back check.

### Not tested by step 3

- **Writing the saved record back** at the end of the test: until step 4 there is no real record,
  so the test always starts and ends with a blank sector.
- **Reading at boot before the scheduler starts** (step 4 needs it): `disable_xip()` takes a FreeRTOS
  mutex, which needs looking at for that case.

### Bench test (either image)

1. Build and flash. At the console, type `xipstate`. The reply lists:
   - `Fast-start sector 0x00f00000 (format version 1):`, then `slots 0-15: blank` and
     `No valid record: the C defaults are used`.
   - The same for `Board sector 0x00f01000`.
   - Then `Done.`
2. Type `xipstate test`. In about 2 seconds the console shows:
   - `xipstate test: no record before the test`
   - `Erase 1: at the start`
   - `Erase 2: before write 17 (sector full)`
   - `Erase 3: before write 33 (sector full)`
   - `Bad-CRC record in slot 1 skipped: OK`
   - `Erase 4: to check the fall-back`
   - `Erased sector: no record, the C defaults would be used: OK`
   - `xipstate test: PASS (34 writes, 4 erases)`
   - and the reply `xipstate test: PASS`.
3. Type `xipstate`. Both sectors are blank again, as in step 1.
4. Let the camera sleep and wake, then start a capture with a long burst (`capture 20 1000`) and
   type `xipstate` while it runs. It replies `Error: not while the camera is busy (...)`, with the
   image task's state in the brackets.
5. Check the rest of the camera is unaffected: let it sleep and wake, and take a picture.

### Bench result (28 October 2026)

Steps 1 and 2 of the bench test gave exactly the expected output: both sectors blank, then
`xipstate test: PASS (34 writes, 4 erases)` with every erase at the expected write.

After the reformat to `c_file_format.md` (comments and layout only): built, `xipstate test` PASS
again, both sectors blank afterwards. During `capture 20 1000` the command was refused with
`Error: not while the camera is busy (NN Processing)` or `(Wait For Timer)`, depending on when it
was typed. After a sleep and wake the RP3 (colour) image took and saved a picture as normal. The same checks
(test PASS, sectors blank, refused while busy, picture after a wake) passed on the HM0360 (mono)
image. **Step 3 complete.**

