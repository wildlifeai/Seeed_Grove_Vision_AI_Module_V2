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
| 4 | Fast-start settings, still waiting for the card | Done 28 Oct, benched on both images |
| 5 | Stop waiting for the card | Done 29 Oct, benched on both images |
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

---

## Step 4. Fast-start settings, still waiting for the card, coded 28 October 2026

Not built or benched yet. With a card in, the camera behaves as before: CONFIG.TXT still
overrides everything, and the record is only compared with it.

### What it does

- **Boot:** at the very start of the FatFS task, before the card is mounted,
  `xip_state_loadFastStart()` reads the newest fast-start record and puts its ops into
  `op_parameter[]`. CONFIG.TXT then loads over them as before. Once the card has loaded,
  `xip_state_compareFastStart()` prints whether the record matched.
- **Sleep:** in the FatFS task's save-state handler, after CONFIG.TXT is saved,
  `xip_state_saveFastStart()` writes a new record only if something has changed. The NN is not
  running then (the image task is in its save-state state).
- **What is in the record:** ops 5, 6, 9 to 18, 25, 27 to 31, 34 to 38 (23 ops), and the staged
  extra camera registers as their own block (count, then up to 24 entries), labelled with the
  camera image that wrote them. 144 of the 240 payload bytes.
- **"Changed"** means any op or register differs, except the RP3 exposure and gain (ops 37, 38):
  they count as changed only if the total light (exposure x gain, as in `ae.c`) has moved more than
  25 %. The bench run in step 2 saw the settled exposure wander about 15 % in steady light.
- **`xipstate`** also prints the newest record's values (`Newest: 5=1 6=500 ...`), to compare
  with `getop`.
- **Boot timing** has a new point, `flash record read`.

### Decisions made in this step

- **The record is read at the start of the FatFS task, not in `app_main()`** as the proposal said.
  Before the scheduler starts the flash code cannot run: `init_flash()` calls `vTaskDelay()` and
  `disable_xip()` takes the SPI mutex. The FatFS task start is the earliest safe point, and nothing
  that needs the settings runs before it (the image task waits for the FatFS task). Step 5 must keep
  the image task waiting for the record, though no longer for the card.
- **"For state the flash wins" (ops 25, 37, 38) is not applied yet.** In this step the card wins
  for everything and differences are only printed. Applying it changes behaviour, so it belongs
  in step 5.
- **The extra registers are labelled with the camera image.** Both images share the one record,
  but each has its own register file for its own sensor (`RPV3_EX.BIN`, `HM0360EX.BIN`). Step 5
  must apply registers only from a record written by the same image. A side effect: after each
  change of image the first sleep writes a new record.
- **No record is written when there is no card,** since the save-state message is only sent when
  the card is mounted. The record is a copy of the card, so this is right for now.

### Bench test (RP3 image first)

The step 3 test left the fast-start sector blank, so the first boot has no record.

1. Build both images. Flash the RP3 image and power cycle. During boot the console shows
   `Fast-start record: none, using the C defaults`, and after the card has loaded
   `Fast-start record: none at boot, nothing to compare`.
2. Let it sleep (or type `dpd`). After `Saved state to SD card.` the console shows
   `Fast-start record: written, seq 1 slot 0`.
3. At the next wake: `Fast-start record: seq 1 loaded (23 ops, N extra camera registers)`, then
   `Fast-start record: seq 1 matches CONFIG.TXT`. The `Boot timing` line includes
   `flash record read`, and `SD ready` should be within about 15 ms of before (195 ms on 28 Oct).
4. Let it sleep again without changing anything: `Fast-start record: unchanged (seq 1)`.
5. Type `xipstate`. The fast-start sector shows slot 0 valid and a `Newest:` line. Check a few
   values against `getop` (for example `getop 6`, `getop 29`).
6. Type `setop 6 600` and let it sleep: `Fast-start record: written, seq 2 slot 1`. At the wake,
   `seq 2 matches CONFIG.TXT`. Then `setop 6 500` to put it back.
7. **Card edited:** power off, and on a PC change op 5 in CONFIG.TXT (for example `5 1` to `5 2`).
   Power on. The console shows `Fast-start record: seq ... differs from CONFIG.TXT:` and
   `  op 5: record 1, CONFIG.TXT 2`. The next sleep writes a new record. Put op 5 back.
8. Type `xipstate test`. It now starts `xipstate test: saved the current record` and ends with
   `Saved record written back` and PASS. `xipstate` afterwards shows the same `Newest:` values.
9. **Other image:** type `switchslot` and let it sleep. The HM0360 image boots, loads the record and
   prints `extra camera registers: the record's are from the other camera image`. The next sleep
   writes a new record. Repeat steps 4 and 5 on this image.

### Bench result, steps 1 to 6 (28 October 2026, RP3 image, build 11:21, `logs/log2.txt`)

Commands sent over BLE. Everything as expected:

- Cold boot: `none, using the C defaults`, `none at boot, nothing to compare`. First sleep:
  `written, seq 1 slot 0`.
- Each following wake: `seq 1 loaded (23 ops, 0 extra camera registers)`, `seq 1 matches
  CONFIG.TXT`, and `AE: restored exposure 2242 gain code 0`. Five sleeps with nothing changed:
  `unchanged (seq 1)` each time.
- `xipstate`: slot 0 valid, slots 1-15 blank, `Newest:` values agree with `getop 6` (500) and
  `getop 29` (1).
- `setop 6 600`: `written, seq 2 slot 1`, then `seq 2 matches` at the wake. `setop 6 500`:
  `written, seq 3 slot 2`.
- **Cost of reading the record:** `FatFs setup took` went from 170 ms (log1) to 182 ms, about
  12 ms, mostly `init_flash()`'s fixed 10 ms wait. Before this step that same `init_flash()` ran
  later, in the image task's NN start-up (log1 shows `Flash ID` there), so the time to the first
  photo should be about the same. Not yet confirmed: these were BLE wakes with no capture, so no
  `Boot timing` line. A timer or motion wake will show it.

### Bench result, steps 7 and 8 and a capture (28 October 2026, RP3 image, `logs/log3.txt`)

- **Step 7, card edited** (op 5 changed to 2 on a PC): `seq 3 differs from CONFIG.TXT:` and
  `op 5: record 1, CONFIG.TXT 2`; `getop 5` = 2 (the card won); the sleep wrote seq 4. After
  `setop 5 1`, seq 5, then `matches`.
- **Step 8, `xipstate test`** with a real record: `saved the current record`, all checks OK,
  `Saved record written back`, `PASS (35 writes, 4 erases)`. The record written back is seq 1
  (the sector was erased), with the same values, and the next wakes load and match it.
- **Capture** (`capture 1 1` over BLE; `capture 1` was rejected, it needs both parameters):

  | ms since `app_main()` | log1 (timer wake, before step 4) | log3 (BLE wake, step 4) |
  |---|---|---|
  | flash record read | | 33 |
  | SD mounted | 184 | 196 |
  | SD ready | 195 | 208 |
  | camera ready | 254 | 268 |
  | capture started | 280 | 477 |
  | frame ready | 329 | 526 |

  **No time saved yet, and none expected:** step 4 still waits for the card. Up to `camera ready`
  the record read adds about 13 ms (20 to 33 ms, mostly `init_flash()`'s fixed 10 ms wait). The
  later `capture started` is because this capture was started by a BLE command after the wake,
  not by the wake itself, so those two rows do not compare. The saving comes in step 5, when the
  image task stops waiting for the card (about 170 ms).
- **Possible saving of about 10 ms, for step 6:** the FatFS task waits a fixed 10 ms before
  mounting the card ("settling time for 3V3_WE"), and the record read now runs just before it.
  The read could use that wait instead of adding to it.

### Bench result, step 9 (28 October 2026, HM0360 image build 11:28, `logs/log4.txt`)

The HM0360 image was loaded with xmodem rather than `switchslot` (it boots from slot B,
`flash_offset 0x00100000`). The test only needs the other image to boot on a board holding an
RP3 record, so this is equivalent.

- **An xmodem load leaves the saved-state area alone:** the first boot loaded `seq 1`, the RP3's
  record. (Worth knowing for the docs: loading firmware does not clear the saved state.)
- It printed `differs from CONFIG.TXT:`, `extra camera registers: the record's are from the other
  camera image`, and `op 37: record 2242, CONFIG.TXT 1944`. The sleep wrote seq 2; the following
  wakes `matched`, sleeps were `unchanged`, and `xipstate` showed `Newest: ... 37=1944 ...
  (camera 1)`.
- **Why op 37 differed:** the last RP3 capture (log3) moved the exposure from 2242 to 1944, 13 %.
  CONFIG.TXT saved 1944, but the record stayed at 2242 (`unchanged`), since 13 % is within the
  25 % tolerance. That is the tolerance working as intended. **But the op 37 line should not have
  been printed:** the comparison is meant to list the exposure only when it is outside the
  tolerance. Fixed after this test in `xip_state_compareFastStart()`: it now checks ops 37 and 38
  together with `aeSimilar()`, as the sleep-time write does, and lists them only if outside.
- **Boot timing (BLE capture):** `scheduler started 11, flash record read 24`: the read costs
  13 ms on this image too.
- `Error: File has no contents` after `HM0360 MD sensitivity 1` comes from `cis_file_process()`
  finding an empty `HM0360EX.BIN`. Not from this work.

### Bench result, the comparison fix (28 October 2026, HM0360 image build 12:10, `logs/log5.txt`, `logs/log6.txt`)

- `setop 37 2100` (8 % from the record's 1944): sleep `unchanged (seq 2)`.
- Op 5 changed to 2 on a PC: `seq 2 differs from CONFIG.TXT:` and only `op 5: record 1,
  CONFIG.TXT 2`, with no op 37 line (2100 against 1944 is within the tolerance). Sleep wrote seq 3.
- Op 37 changed to 3000 and op 5 back to 1 on a PC: `op 5: record 2, CONFIG.TXT 1` and
  `op 37: record 2100, CONFIG.TXT 3000` (outside the tolerance, so listed). Sleep wrote seq 4.
- `setop 37 1944`: sleep wrote seq 5 (1944 against 3000 is outside the tolerance).

**Step 4 is complete on both images.**

**Decided for step 5 (Charles, 28 October 2026):** cards do not move between cameras. When
CONFIG.TXT loads part way through a wake:
- **Settings:** the card wins, as now.
- **State (ops 25, 37, 38):** the value in RAM is kept. It started from the record and may already
  have moved during this wake (an AE step, a light check), so it is newer than the card's, which
  was saved at the last sleep.
- **Exception:** with no record at boot, the card's values are used for state too, since RAM then
  holds only the C defaults.

---

## Step 5. Stop waiting for the card, coded 28 October 2026

Not built or benched yet.

### What changes

- **The image task starts as soon as the settings are known,** not when the card is ready. A new
  semaphore (`fatfs_waitForSettings()`) is given right after the fast-start record is read, about
  30 ms after `app_main()`. Without a record (first boot, new format) it is given when the card is
  ready, so the camera behaves exactly as before.
- **It waits for the card only where the card is needed** (`fatfs_waitForSdReady()`):
  - before deciding whether and how to save the photo (file name, folder, sequence number,
    deployment ID, GPS). The frame and the NN result wait in RAM;
  - before the NN counters are incremented (`processNNOutput()`), or loading CONFIG.TXT would
    undo the count;
  - before the NN start-up, only if the named model is not already in flash (`cv_modelNeedsCard()`);
  - before deciding to save state at sleep (`sdMountedForSave()`), so the camera never sleeps part
    way through the card's start-up.
- **The SD card start-up yields the CPU.** Its waits (`mmc_disk_initialize()`, about 150 ms of
  polling) busy-waited (`hx_drv_timer_cm55x_delay_ms()`), and the FatFS task has a higher priority,
  so nothing else could run. They now call `vTaskDelay()` once the scheduler runs
  (`DELAY_YIELD()`, `middleware/fatfs/port/mmc_spi/mmc_we2_spi.c`). The waits used when reading
  and writing files are unchanged.
- **Extra camera registers before the card is ready:** applied from the staged table, filled from
  the record (`cis_file_setStagedFromRecord()`, `cis_file_applyStaged()`), only if the record was
  written by the same camera image. Once the card is ready, `cis_file_process()` reads the file as
  before.
- **State ops (25, 37, 38) keep the RAM value** when CONFIG.TXT loads (`xip_state_keepStateOp()`
  in `load_configuration()`), as decided. The comparison still uses the card's values.
- **The record now holds every op except the counters** (32 ops, not 23). The image task reads
  more ops before the card is ready than step 4 stored: op 7 (timelapse) decides whether a timer
  wake is a light-check-only wake, ops 23 and 26 are used at the first light check, op 33 sets up
  motion detection. **Format version 2:** the step 4 records are ignored, so the first boot of this
  build has no record and waits for the card; the first sleep writes a version 2 record.
- **Camera off on the card:** if CONFIG.TXT says op 10 = 0, the frame taken from the record's
  settings is not saved.
- **Boot timing** is now printed when the first photo is queued for the card (new point
  `photo queued`), so the card's points are included even when the frame comes first.

### Decisions made in this step

- **No new `SD_READY` message or `WAIT_FOR_SD` state** (proposal section 3). The image task blocks on
  the existing semaphore at the first point that needs the card. At that point it holds a frame and
  has nothing else to do: no frame or data path events can arrive, and inactivity is not running
  until the card is ready. Much smaller change, same effect. Easy to change to a state later if
  needed.
- **The SD driver is middleware shared by every app,** so `DELAY_YIELD()` falls back to the busy
  wait before the scheduler starts, or when `INCLUDE_xTaskGetSchedulerState` is 0.
- **Where the driver change lives:** `mmc_we2_spi.c` is compiled into the FatFS library
  (`libfatfs.a`), which every build makes from source (`middleware/fatfs/fatfs.mk`). Its copy in
  `prebuilt_libs/gnu/` is ignored by git, so there is nothing extra to commit.

### Not covered

- **More than 24 extra camera registers:** the record holds the first 24, as `camreg` can edit.
  A longer file is applied in full only once the card is ready.
- **After a change of camera image** the record's registers are for the other sensor, so none are
  applied until the card is ready. With the RP3 that means none for that wake (its sensor starts
  once per wake).

### Bench test (RP3 image first)

1. Build both images. Flash the RP3 image. At the first wake the console shows
   `Fast-start record: none, using the C defaults`: the step 4 records (format 1) are ignored. The
   boot carries on as before, waiting for the card.
2. Let it sleep: `Fast-start record: written, seq 1 slot N`.
3. Type `xipstate`. The step 4 slots show `NOT VALID` (version 1); the newest is valid with 32
   values in its `Newest:` line.
4. Type `setop 7 30` and let it sleep. At the timer wake look at the `Boot timing (timer wake)`
   line. **`frame ready` should now come before `SD ready`.** Compare `frame ready` with 329 ms
   (log1). The photo is saved as usual (`Wrote ... byte image to SD`).
5. Let it run a few more timer wakes. Each shows `AE: restored`, `seq N matches CONFIG.TXT`, and
   the photo saved. Check on a PC that the photos have the right time, GPS and deployment ID.
6. Type `getop 0` twice, a wake apart: the sequence number goes up by one per photo.
7. **No card:** remove the card and let it make a timer wake. It takes the frame, reports
   `SD card initialisation failed`, saves nothing and goes back to sleep (no hang). Put the card back.
8. **Camera off on the card:** on a PC set op 10 to 0 in CONFIG.TXT. At the next timer wake the
   frame is taken but `Skipping file save.` Then type `setop 10 1`.
9. **Extra registers:** type `camreg 0x0101` to read a register, then `camreg 0x0101 <the same
   value>` to stage it unchanged. After the next sleep and wake, the console shows
   `Processed 1 staged settings (from the fast-start record)`. Then `camreg clear`.
10. Type `setop 7 0`. Then the HM0360 image: load it, and after its first sleep check a motion wake's
   `Boot timing (motion wake)` the same way.

### First runs (28 October 2026, HM0360 image build 13:23, console pasted by Charles)

A timer wake with a photo, then a BLE wake. Both loaded the format 2 record (`seq 2 loaded (32
ops ...)`), the image task started before the card was mounted, and the card's start-up was
interleaved with the camera's (`Mounting FatFS on SD card sensor_type: 8 ... OK`). `FatFs setup
took 186ms`, as in step 4, so yielding did not slow the mount. `Timelapse interval: 20s` came from
the record, before CONFIG.TXT loaded. The photo (`59200F90.JPG`, sequence 1315) was saved with 12
EXIF tags; the record `matches` and was `unchanged` at sleep.

**Boot timing, timer wake:** `flash record read 24, camera ready 40, capture started 71, frame
ready 109, SD mounted 191, CONFIG.TXT loaded 196, image dir ready 200, SD ready 202, photo queued
209` (ms since `app_main()`).

- **The frame is ready at 109 ms, about 93 ms before the card.** Before step 5 the camera could
  not start until `SD ready` (about 200 ms on this image), so its frame came at about 270 ms.
- **The photo is now limited by the card:** it is queued 7 ms after `SD ready`. Making the camera
  side faster (step 6's "NN start-up after the capture") would no longer bring the photo forward.
  What would: the card's start-up path, about 180 ms from the scheduler starting to `SD ready`:
  the record read (13 ms, mostly `init_flash()`'s fixed 10 ms wait), the FatFS task's fixed 10 ms
  "settling" wait, and the mount itself (about 165 ms, mostly the card answering ACMD41).

### Bench test, steps 3 to 10 (29 October 2026, RP3 image build 18:49 28 Oct, `logs/log7.txt`)

Commands over BLE. Steps 1 and 2 were already covered by the HM0360 runs above (format 1 records
ignored, format 2 written).

| Step | Result |
|---|---|
| 3. `xipstate` | Slots 0-4 `NOT VALID` (version 1), 5-8 valid (version 2, length 162), slot 8 newest, `Newest:` with 32 values, camera 2. **Pass** |
| 4. Timer wake | `frame ready 217`, against 329 in log1. **Pass**, see the timing below |
| 5. More timer wakes | Three photos (`59200940`, `59200B40`, `59200D40.JPG`), each `AE: restored`, `matches`, `unchanged`. **Pass.** EXIF: after `setdid` and `setgps`, three timer-wake photos (`59201360`, `592014C0`, `59201620.JPG`, `logs/log8.txt`, `logs/exif_step5_gps.csv`) carry the deployment ID `3f2a9c1e-...`, GPS `S 36/1, 49/1, 2506/100`, `E 174/1, 47/1, 4865/100`, altitude `30/1`, and times matching the `Wrote` lines. The folder is `/MEDIA/3f2a9c1e/IMAGES.005` (named from the ID), so the card's values reached the photo. **Pass** |
| 6. Sequence number | 1316 to 1319, one per photo. **Pass** |
| 7. No card | Two wakes: `SD card initialisation failed (reason 3)` after about 650 ms, frame taken, `Image not written. Error: 13`, back to sleep about 1 s later. No hang, no record written. **Pass** |
| 8. Camera off on the card | `op 10: record 1, CONFIG.TXT 0`, `Skipping file save.`, new record seq 6. The next timer wake took no photo (camera off from the record). `setop 10 1`: seq 7, then photos again. **Pass** |
| 9. Extra register | (`logs/log9.txt`) `camreg 1010` read 0x00, `camreg 1010 00` staged it and saved `RPV3_EX.BIN`; the sleep wrote seq 13. The next two wakes: `seq 13 loaded (32 ops, 1 extra camera registers)`, then `Processed 1 staged settings (from the fast-start record)` while the card was still mounting, then `Loaded 1 staged register(s)` from the card and `matches`. `camreg clear`: seq 14 with 0 registers, loaded at the next wake. **Pass** |
| 10. HM0360 motion wake | (`logs/log10.txt`, HM0360 build 18:26 28 Oct, op 11 = 1000) Three motion wakes, each a photo saved and the record `matches`/`unchanged`. Boot timing: `camera ready 59, frame ready 129` and `141`, `SD ready 227` and `233`, `photo queued 233` and `241`. **Pass** |

**Timing, RP3 timer wake** (ms since `app_main()`):

| | log1 (before step 4) | log7 (step 5) |
|---|---|---|
| flash record read | | 33 |
| camera ready | 254 | 121 |
| frame ready | 329 | 217 |
| SD ready | 195 | 210 |
| photo queued | about 656 (`Writing` 327 ms after the frame) | 547 |

- **About 110 ms sooner** for both the frame and the photo.
- **On the RP3 the card is ready just before the frame** (210 against 217), so the photo never
  waits for the card. The camera starts more slowly than before (`camera ready` 121, 88 ms after the
  record, against 59 ms after `SD ready` in log1), since it shares the CPU with the card's start-up.
- **After the frame, 330 ms go on the RP3's software colour correction (100 ms) and JPEG encoding
  (187 ms),** as before. That is now the largest part of the time to the photo on this image.


### Parked: console stopped responding (29 October 2026)

Not part of this work; parked by Charles to come back to. During the step 5 tests a paste into Tera
Term (no transmit delay) hung after a few characters, and the console stopped responding. A `ver`
then arrived over BLE: the IF task passed it to the CLI task and waited in `I2C RX State` for the
reply, which never came. From then on it deferred every inactivity event (`Deferring event 0x070e`)
and refused every BLE message (`unhandled event 'I2C Rx' in 'I2C RX State'`), so the camera stayed
awake until reset. The awake watchdog did not reset it.

To look at later:
- why the console (CLI task or UART receive, one character at a time) stopped after a fast paste;
- whether a CLI task that stops answering should trigger a watchdog reset (see the camera errors
  work), and whether the IF task needs a timeout while waiting for a CLI reply.

### Step 5 result (29 October 2026)

**All ten bench steps pass on the RP3 image, and the timer and motion wakes on the HM0360.**

| ms since `app_main()` | RP3 timer wake | HM0360 motion wake |
|---|---|---|
| flash record read | 33 | 43 |
| camera ready | 121 | 59 |
| frame ready | 217 (329 before) | 129 to 141 |
| SD ready | 210 | 227 to 233 |
| photo queued | 547 (about 656 before) | 233 to 241 |

- **The camera no longer waits for the card.** On the HM0360 the frame is ready about 95 ms before
  the card; the photo is queued within 8 ms of `SD ready`, so the card now decides the time.
- **On the RP3** the card is ready just before the frame; the 330 ms of software colour correction
  and JPEG encoding after the frame is now the largest part.
- On a motion wake `scheduler started` is 30 ms (11 on a timer wake): the HM0360 checks in
  `app_main()` before the tasks start.

