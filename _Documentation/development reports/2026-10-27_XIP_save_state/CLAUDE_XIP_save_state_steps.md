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
| 6 | Optional extras: shorten the card's path, then the frame | Changes 1 and 2, image task priority (29 Oct), NN start-up after the capture and NN counters (30 Oct): all benched on both images |
| 7 | Docs | Done 30 Oct: `ww500_md/doc/WW500_XIP_saved_state.md`, flash map in `slot_selector.md`, `WW500_Neural_Network_Operation.md`, `ble_commands.md` (`camreg`) |

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

---

## Step 6. Shorten the card's path, changes 1 and 2 coded 29 October 2026

Not built or benched yet.

### Why step 6 changed

Step 5 showed the photo now waits for the card on the HM0360 (queued within 8 ms of `SD ready`),
while on the RP3 the software colour correction and JPEG encoding after the frame take longest.
So only time on the card's path brings the HM0360 photo forward. The proposal's idea of starting
the NN after the capture is dropped: it would make the frame earlier, not the photo.

On the HM0360 motion wakes in log10, `SD ready` (about 230 ms after `app_main()`) is made of: boot
to scheduler (30 ms on a motion wake, 11 on a timer wake), reading the record (13 ms, mostly
`init_flash()`'s fixed 10 ms wait), the FatFS task's fixed 10 ms "settling" wait, the mount (about
165 ms, mostly the card answering ACMD41), and CONFIG.TXT, the image folder and the register file
(about 10 ms).

### The candidates (agreed with Charles 29 Oct)

| | Change | Saves | Risk | Decision |
|---|---|---|---|---|
| 1 | Remove the FatFS task's 10 ms settling wait before mounting | About 10 ms, HM0360 photo | Low | **Done** |
| 2 | Cut `init_flash()`'s 10 ms wait (before reading the flash ID) to 1 ms | About 9 ms on the card's path, both images; RP3 camera starts about 9 ms sooner too | Low | **Done** |
| 3 | Move the motion-wake HM0360 grid read (`hm0360_md_saveWakeGrid()`, likely the 19 ms extra before the scheduler) from `app_main()` to the image task | About 19 ms, motion wakes only | Moderate: the grid must still be read before anything reconfigures the HM0360 or clears its interrupt, in both images | Later: first confirm the 19 ms with a boot timing point |
| 4 | Read the record in the image task, overlapping the mount | About 3 ms after change 2 | Moves the read onto the camera's path, slower RP3 | Not done |

### Changes 1 and 2

- **1. `fatfs_task.c`:** the `vTaskDelay(10)` before `fatFsInit()` is removed (it was marked
  `TODO - experiment - do I need settling time for 3V3_WE?`). The card is powered from 3V3_WE,
  which the hardware switches on as the chip leaves DPD, at least 40 ms before this point. The card
  driver's power-up sequence (`SD_powerUpSeq()`) still waits 10 ms and sends 80 clocks before the
  first command; the SD specification asks for 1 ms. **If a card needs longer** it fails to mount
  (`SD card initialisation failed`) and the photo is not saved.
- **2. `xip_manager.c`:** in `init_flash()`, the wait between opening the SPI flash and reading its
  ID is one tick (up to 1 ms), was 10 ms. The flash is the boot flash, powered all along. **If it is
  not ready** the ID read fails and is reported (`init_flash: failed to read flash ID`): the record
  is not read, so that boot waits for the card as before, and the NN start-up tries `init_flash()`
  again. The other 10 ms wait in `xip_manager.c` (printing the model metadata) runs only on a cold
  boot and is unchanged.

### Bench test (both images)

1. Build both images and flash one. Over many wakes of every kind (cold boot, timer, motion, BLE)
   the console shows `Flash ID 0xEF initialised` and `Mounting FatFS on SD card ... OK` every time:
   no `failed to read flash ID`, no `SD card initialisation failed`.
2. Compare the `Boot timing` lines with step 5 (log7 and log10): `flash record read` about 9 ms
   earlier (33 to about 24 on the RP3, 43 to about 34 on a motion wake), `SD ready` about 20 ms
   earlier (227-233 to about 210 on an HM0360 motion wake), and `photo queued` on the HM0360 the
   same amount earlier.
3. Repeat with a second SD card, a different make if possible, since the settling wait was for the card.
4. Repeat on the other image.


### First run, RP3 image (29 October 2026, build 12:47; its log was saved as `logs/log12.txt` but later overwritten, the figures below are copied from it)

Two BLE wakes, one with photos (`capture` over BLE). `Flash ID 0xEF initialised` and the mount OK
both times; record loaded and `matches`.

| ms since `app_main()` | Step 5 (log9, BLE wake) | Step 6 (log12, BLE wake) |
|---|---|---|
| flash record read | 33 | 25 |
| SD mounted | 199 | 187 |
| SD ready | 212 | 198 |
| camera ready | 122 | 118 |

- **The record read is 8 ms sooner,** as expected from change 2.
- **`SD ready` is 14 ms sooner, not the 18 to 20 expected.** From the record read to `SD mounted`
  took 162 ms, against 166 ms in step 5 with the 10 ms settling wait in it. So removing the wait
  saved only about 4 ms: the card then spent about 6 ms longer answering its start-up command
  (ACMD41). That suggests the card needs a fixed time from power-on before it is ready, and starting
  sooner just means polling for longer. To confirm on more wakes and a second card.
- BLE wakes take the photo when the command arrives, so `capture started` and later do not compare.

### First run, HM0360 image (29 October 2026, build 12:48, `logs/log11.txt`)

Two BLE wakes and three motion wakes, each motion wake a photo saved. No flash ID or mount failures.

| ms since `app_main()` | Step 5 (log10, motion) | Step 6 (log11, motion) |
|---|---|---|
| flash record read | 43 | 35 |
| SD mounted | 215, 221 | 197, 208, 208 |
| SD ready | 227, 233 | 208, 220, 220 |
| frame ready | 129, 141 | 141, 128, 128 |
| photo queued | 233, 241 | 229, 228, 228 |

- **The photo is about 5 to 13 ms sooner,** half the 20 ms hoped for.
- **The same pattern as the RP3:** the record read is 8 ms sooner (change 2), but from the record
  read to `SD mounted` took 162 to 173 ms against 172 to 178 in step 5, which still had the 10 ms
  wait. So most of the 10 ms removed by change 1 went into the card taking longer to answer its
  start-up command. **The card seems to need a roughly fixed time from power-on** (about 200 ms after
  `app_main()` here), and the firmware cannot shorten that.
- Both changes are kept: they cost nothing, and change 2 also brings the RP3 camera forward.
- **Not tested with a second card** (bench step 3): no card of another make was to hand. Change 1
  removed a wait before the card's start-up, so a card that needs more time after power-on would
  fail to mount (`SD card initialisation failed`). The driver's own 10 ms power-up wait remains.
  Worth checking when another make of card is available.

### The objective, restated by Charles (29 October 2026)

**What matters is the time from the wake (motion) to the frame being ready** (`frame ready`, the
FRAME_READY message from the Himax library): that is when the scene is captured. When the photo is
saved does not matter. So the card is off the critical path since step 5, and the comments above
about the photo waiting for the card, or for the RP3's colour correction, do not bear on the
objective. Changes 1 and 2 still help a little, since they free the CPU sooner (see below).

**The path to `frame ready` now** (ms since `app_main()`):

| | HM0360 motion wake (log11) | RP3 timer wake (log7) |
|---|---|---|
| cameras checked | 7 | 16 |
| scheduler started | 30 (motion grid read and RTC before it) | 20 |
| flash record read (image task released) | 35 | 33 (25 in log12) |
| camera ready | 52 | 121 (118 in log12) |
| capture started | 90 | 154 |
| frame ready | 128 to 141 | 217 |

**Candidates for the frame:**
- **Start the NN after the capture.** Between `camera ready` and `capture started` the image task
  spends 33 to 38 ms, mostly the NN start-up (`NN Initialisation took 19ms` on the HM0360 even with
  no model: project ID 0). Dropped earlier in step 6 because it did not bring the photo forward; it
  does bring the frame forward.
- **The FatFS task runs at a higher priority than the image task,** and the card's reads and SPI
  transfers busy-wait. So during the mount the camera start-up is slowed (19 ms for an NN start-up
  that does nothing points to that). Running the FatFS task below the image task would let the
  camera go first; the card would then be later, which no longer matters.
- **The motion grid read in `app_main()`** (change 3, about 19 ms on motion wakes) is before the
  scheduler, so it delays everything, the frame included.
- **The RP3 sensor start-up** takes about 90 ms from the record to `camera ready`. Worth a look
  separately.

### Frame candidate 2: the image task above the FatFS task, coded 29 October 2026

Agreed with Charles: candidate 2 first, then candidate 1 (the NN after the capture), measured
separately. Not built or benched yet.

- **The change (`ww500_md.c`, `app_main()`):** the image task is now created before the FatFS task.
  Tasks get descending priorities in creation order, so the image task is now priority 2 and the
  FatFS task priority 1 (they were the other way round). The CLI (4) and IF (3) tasks are unchanged.
  The boot shows `Created task 'IMAGE' Priority 2` and `Created task 'FAT' Priority 1`.
  (`fatfs_task_PRIORITY` and `image_task_PRIORITY` in the task files are not used.)
- **Expected:** `camera ready`, `capture started` and `frame ready` sooner, on both images. The card
  (`SD mounted`, `SD ready`) later, since the FatFS task now waits whenever the image task is busy.
- **Risks:** the card's start-up and the photo's save run later, most on the RP3 where the colour
  correction and JPEG encoding (330 ms of CPU) now run before the FatFS task can continue. Nothing
  waits for the card except the image task itself, at the point where it blocks for it, so this
  cannot deadlock. The BLE processor's `Awake` message (sent by the FatFS task after CONFIG.TXT) may
  also come later.

**Bench test (both images):**
1. Check the boot shows the new priorities.
2. Several timer wakes on the RP3 and motion wakes on the HM0360. Compare `camera ready`,
   `capture started` and `frame ready` with log7 (RP3: 121, 154, 217) and log11 (HM0360: 52, 90,
   128 to 141). Also note `SD ready` and `photo queued`, expected later.
3. Check every photo is still saved, and the console shows no errors.

### Bench result, candidate 2, HM0360 image (29 October 2026, build 13:46, `logs/log12.txt`)

A cold boot, three motion wakes (each a photo saved) and BLE wakes. The boot shows `IMAGE Priority 2`,
`FAT Priority 1`. No errors.

| ms since `app_main()` | log11 (before) | log12 (image task above FatFS) |
|---|---|---|
| camera ready | 52 | 50 |
| capture started | 90 | 70 |
| **frame ready** | **128 to 141** | **103, 103, 117** |
| SD ready | 208 to 220 | 236 to 239 |
| photo queued | 228 to 229 | 238 to 241 |

- **The frame is 25 ms sooner** (median 103 against 128). The NN start-up now takes 8 ms, against
  19 ms when the FatFS task could interrupt it.
- The card is about 20 ms later, as expected. It no longer matters.


### Bench result, candidate 2, RP3 image (29 October 2026, build 13:51, `logs/log13.txt`)

A cold boot, three motion wakes and a timer wake (each a photo saved), and BLE wakes. No errors.

| ms since `app_main()` | log7 (step 5, timer) | log13 timer wake | log13 motion wakes |
|---|---|---|---|
| scheduler started | 20 | 20 | 40 |
| flash record read | 33 | 24 | 44 |
| camera ready | 121 | 82 | 102 |
| capture started | 154 | 101 | 121 |
| **frame ready** | **217** | **125** | **145 to 147** |
| SD ready | 210 | 258 | 272 to 277 |
| photo queued | 547 | 533 | 548 to 555 |

- **The frame is 92 ms sooner on a timer wake** (125 against 217). The RP3 sensor start-up
  (`camera ready`) is 39 ms quicker without the FatFS task interrupting it, and the frame itself
  arrives 24 ms after the capture starts, against 63 ms before.
- **Motion wakes on the RP3 image** (first measurement): frame ready 145 to 147. The 20 ms extra
  before the scheduler is the motion grid read (`scheduler started` 40 against 20 on a timer wake).
- The card is about 50 ms later and the photo is saved at about the same time as before.

### Progress on the objective (wake to frame ready)

| | Before this work | Step 5 | Step 6, candidate 2 |
|---|---|---|---|
| RP3 timer wake | 329 (log1) | 217 (log7) | 125 (log13) |
| HM0360 motion wake | about 270 (estimate: no measurement) | 128 to 141 (log10, log11) | 103 to 117 (log12) |
| RP3 motion wake | | | 145 to 147 (log13) |

## Frame candidate 1: the NN start-up after the capture (30 October 2026)

### Setup: a model in flash

Until now every run had project ID 0, where `cv_init()` prints one line and returns, so the 8 ms
`NN Initialisation` measured nothing real. A baseline with a model is measured first.

- CONFIG.TXT set to op 14 = 20, op 15 = 1 (person detect, `20V1.TFL`, 2 classes). The record
  noticed the change (`op 14: record 0, CONFIG.TXT 20`) and wrote seq 38.
- The model was already in flash from an earlier load, with `WARNING: Number of classes and labels
  unmatched`: the flash metadata held 1 label (`unknown`) from an older `20V1.TXT`. The card's
  `20V1.TXT` (`logs/20V1.TXT`) has 2 lines, `no person` and `person`, CR LF. Since the named model
  was in flash, `cv_init()` never read the new file. Fixed with `erasemodel` then `loadmodel 20 1`;
  `capture` now reports `no person` and `person`.
- First figure: `NN Initialisation took 13ms` (Ethos-U start-up, model found in flash,
  interpreter built). Baseline wakes to follow.

### Baseline with the model, RP3 image (30 October 2026, `logs/log14.txt`)

A cold boot, then five timer wakes (20 s interval), each a photo saved, `Target object not detected`
(scores -9 to -91, threshold 18). No errors. All five timer wakes gave identical figures:

| ms since `app_main()` | log13 (no model) | log14 (model `20V1.TFL`) |
|---|---|---|
| flash record read | 24 | 25 |
| camera ready | 82 | 82 |
| capture started | 101 | 105 |
| **frame ready** | **125** | **131** |
| SD ready | 258 | 265 |
| photo queued | 533 | 588 to 591 |

- **`NN Initialisation took 12ms` on every wake,** against 8 ms with no model. The real start-up
  adds only 4 ms, and the frame is 6 ms later. **Moving the NN start-up after the capture would
  bring the RP3 frame forward by about 12 to 19 ms** (the 12 ms of start-up plus the console output
  around it; `camera ready` to `capture started` is 23 ms).
- **`NN processing took 140ms` on a wake, against 16 ms after a `capture` command.** The timing
  includes `processNNOutput()`, which waits for the card before counting the result
  (`fatfs_waitForSdReady()`): 131 + 140 = 271, just after `SD ready` at 265. So on the RP3 the image
  task now sits idle until the card is ready before starting the colour correction and JPEG
  encoding, and the photo is queued about 55 ms later than in log13. Not on the path to the frame,
  but it could be avoided by counting the result later. To decide with Charles.

### Baseline with the model, HM0360 image (30 October 2026, `logs/log15.txt`)

Four timer wakes (op 7 still 20 s from the RP3 run; no motion wakes), each a photo saved, `Target
object not detected` (scores -25 to -99), then a BLE wake. `There are 2 classes (2)`: the labels
now match. No errors.

| ms since `app_main()` | HM0360 timer wake, step 5 first run (no model) | log15 timer wakes (model) |
|---|---|---|
| scheduler started | | 11 |
| flash record read | 24 | 16 |
| camera ready | 40 | 31 |
| capture started | 71 | 55 |
| **frame ready** | **109** | **90 to 93** |
| SD ready | 202 | 227 to 242 |
| photo queued | 209 | 277 to 292 |

- **`NN Initialisation took 12ms`,** as on the RP3. `camera ready` to `capture started` is 24 ms,
  so moving the NN start-up after the capture should bring the frame forward by about 12 to 19 ms
  here too.
- **`NN processing took 140 to 157ms`,** again mostly the wait for the card in `processNNOutput()`
  (frame 91 + 142 = 233, just after `SD ready`). The photo is queued about 50 ms after `SD ready`.
- These are the first HM0360 timer-wake figures since step 6; motion wakes add the grid read
  before the scheduler (about 19 ms). The comparison after the change uses the same wake type.

### Candidate 1 and the NN counters, coded 30 October 2026

Agreed with Charles: both. Not built or benched yet. All in `image_task.c`.

- **The NN start-up after the capture.** The `cv_init()` block moved into a new `initNN()`. On a
  timer or motion wake that will capture, `vImageTask()` sets `nnInitAfterCapture` instead of
  calling it, and `handleEventForInit()` calls `initNN()` just after `configure_image_sensor(
  CAMERA_CONFIG_RUN)` and the frame timer start. `FRAME_READY` waits in the queue until it returns;
  the frame timeout is 7 s, far longer. The boot summary then says `Neural network starts after the
  capture.`
- **Not deferred** on cold, BLE and light-check wakes (as before), or when the model is not in
  flash: `cv_init()` then copies it from the card, which can take seconds.
- **The counters no longer hold up the image task.** `processNNOutput()` reports the result at
  once (`NN+`/`NN-` to the BLE processor, console) and no longer waits for the card; the threshold
  (op 16) is in the fast-start record. The counts (ops 1 and 2) are added by the new
  `countNNResult()`, called just after the existing `fatfs_waitForSdReady()` before the photo is
  saved. That wait is reached for every frame, so no count is lost.
- The `NN Initialisation took` message lost its "TODO - consider doing this after taking the
  picture!".

**Bench test (both images, timer wakes, `setop 7 20`):**
1. The boot summary shows `Neural network starts after the capture.` and the `NN Initialisation
   took` line comes after `capture started`, before the frame.
2. Compare `capture started` and `frame ready` with log14 (RP3: 105, 131) and log15 (HM0360: 55, 90
   to 93). Expected about 12 to 19 ms sooner.
3. `NN processing took` should be about 16 ms, not 140. On the RP3 `photo queued` should come
   sooner than 588.
4. Counters: `getop 1` and `getop 2` before and after a few wakes. Op 1 goes up by one per photo;
   op 2 only on a detection (stand in front of the camera for one wake).
5. A BLE wake then `capture`: the NN still starts at boot (`Neural network enabled.`), detection
   works.

### Bench result, candidate 1, RP3 image (30 October 2026, build 14:31, `logs/log17.txt`)

A cold boot, a BLE wake, three timer wakes (each a photo saved, `Neural network starts after the
capture.`), then a BLE wake with `capture 2`. No errors. (`logs/log16,txt` was the HM0360 image, but
still the build of 29 October: a repeat of the log15 baseline.)

| ms since `app_main()` | log14 (NN before the capture) | log17 (NN after) |
|---|---|---|
| camera ready | 82 | 82 |
| capture started | 105 | 92 |
| **frame ready** | **131** | **115 to 119** |
| SD ready | 265 | 267, 320, 322 |
| photo queued | 588 to 591 | 544, 597, 598 |

- **The frame is 12 to 16 ms sooner** (115 to 119 against 131), as expected. The NN start-up (12
  ms) now runs while the sensor takes the frame: the capture takes 24 to 27 ms, as before.
- **`NN processing took 15 to 16ms`,** against 140: the counters no longer wait for the card.
- **The photo is not sooner.** The image task still waits for the card before the colour
  correction and JPEG encoding (the `fatfs_waitForSdReady()` before the photo is prepared, needed
  for the file name, sequence number and EXIF). On two wakes the card was also about 55 ms slower to
  mount (`SD mounted` 309 and 311, against 254 to 256). Seen on the HM0360 too (below), cause not
  known yet.
- The counters and a detection: checked on the HM0360 image (below).

### Bench result, candidate 1, HM0360 image (30 October 2026, build 14:37, `logs/log18.txt`)

A cold boot, a BLE wake, three timer wakes (each a photo saved, `Neural network starts after the
capture.`), then a BLE wake with `capture 3`. No errors.

| ms since `app_main()` | log15/log16 (NN before the capture) | log18 (NN after) |
|---|---|---|
| camera ready | 31 | 31 |
| capture started | 55 | 42 |
| **frame ready** | **89 to 93** | **75, 75, 76** |
| SD mounted | 216 to 231 | 271 to 273 |
| photo queued | 277 to 292 | 284 to 286 |

- **The frame is 14 to 17 ms sooner** (75 against 89 to 93). `NN processing took 16ms`, against
  140 to 157.
- **Detection works:** in the `capture 3` burst the third photo gave `TARGET OBJECT DETECTED!`,
  score 47 (threshold 18). The first scored exactly 18, not detected: the test is "above".
- **The card mounted about 50 ms later on all three timer wakes** (271 to 273 against 216 to 231),
  and on two of three RP3 wakes (log17). The photo is no later, since it now waits for the card
  anyway, but the cause is not known: the image task uses about the same CPU as before, only in a
  different order. Not investigated (Charles): it does not delay the frame.
- **The counters are right,** read from the `Sleep` message (ops 1 and 2 are its second and third
  values): op 1 went 350, 351, 352, 353 over the three timer wakes, then 356 after the 3-photo
  burst; op 2 stayed 6, then 7 after the one detection.

**Candidate 1 and the counter fix pass on both images.** Wake to frame ready: RP3 timer 131 to 115
to 119, HM0360 timer 89 to 93 to 75 (with a model in flash).

## Frame candidate 3: the motion-wake grid read (30 October 2026)

### Measured, HM0360 image (build 14:37, `logs/log19.txt`)

Five motion wakes (op 7 = 0, op 11 = 1000), each a photo saved, NN started after the capture, one
`TARGET OBJECT DETECTED!` (score 37). No errors.

| ms since `app_main()` | log18 timer wakes | log19 motion wakes |
|---|---|---|
| cameras checked | 7 | 7 |
| **scheduler started** | **11** | **30** |
| camera ready | 31 | 50 |
| capture started | 42 | 61 |
| **frame ready** | **75 to 76** | **92 to 94** (107 on the first) |

- **The motion wake costs 19 ms before the scheduler**, and everything after it moves by the same
  amount. The only motion-only work there is `hm0360_md_saveWakeGrid()` (`ww500_md.c:702`), so
  that is the grid read.
- **Why it is slow:** `hm0360_md_getMDOutput()` reads the 32 `ROI_OUT` registers one at a time
  (`hx_drv_cis_get_reg()`), each a separate I2C transaction at 100 kHz (the HM0360 needs 100 kHz
  in motion detection mode), about 0.5 ms each.
- **It cannot simply move later on the HM0360 image:** the registers must be read before the
  HM0360 is reconfigured for the capture, which is the first thing the image task does.

### Measured, RP3 image (build 14:31, `logs/log20.txt`)

A cold boot, three motion wakes (each a photo saved, NN started after the capture), then a BLE
wake. No errors.

| ms since `app_main()` | log17 timer wakes | log13 motion wakes (NN before the capture) | log20 motion wakes |
|---|---|---|---|
| cameras checked | 16 | 16 | 16 |
| **scheduler started** | **20** | 40 | **40** |
| camera ready | 82 | 102 | 102 |
| capture started | 92 | 121 | 112 |
| **frame ready** | **115 to 119** | 145 to 147 | **135, 135, 136** |

- **The grid read costs 20 ms on the RP3 image too**, the same as on the HM0360 (19 ms).
- The NN change brought the RP3 motion-wake frame 10 ms sooner (145 to 147, now 135 to 136).

### Does the wake grid survive? A temporary diagnostic, coded 30 October 2026

Asked by Charles: is there evidence that `ROI_OUT` is lost when the HM0360 is set up for the
capture? Not directly. The first photo's grid is always empty (26 Oct), but that does not say
whether the set-up or the first new frame changed it. And `app_main()` already writes
`MD_CTRL1 = 0` at the wake (`hm0360_md_disableInterrupt()`), which may stop the updates.

**TEMPORARY, to be removed once answered** (`WAKE_GRID_DIAG`, defined in `hm0360_md.h`):
`hm0360_md_diagWakeGrid()` and `hm0360_md_diagWakeGridReport()` in `hm0360_md.c` (and its
`<stdio.h>`), called from four places in `image_task.c`. On a motion wake it re-reads `ROI_OUT`
before the sensor set-up, after the capture starts, after the frame and when the photo is queued,
and prints one line after the first photo:

`Wake grid (N blocks at wake): before setup same (N blocks); after capture start ...; after frame ...; photo queued ...;`

Each re-read costs about 19 ms, so the `Boot timing` figures are not valid while it is in.

### Diagnostic result, HM0360 image (build 16:30, `logs/log21.txt`)

Three motion wakes (71, 52 and 8 blocks at the wake), each a photo saved, no errors:

`before setup same; after capture start same; after frame DIFFERS (0 blocks); photo queued DIFFERS (0 blocks)`

on all three. **The grid survives the sensor set-up and the capture start, and is lost when the
first new frame completes** (0 blocks: no earlier frame to compare with). So on the HM0360 image
the read can move to just after the capture starts, into the wait for the frame (about 33 ms),
and off the path to the frame.

### Diagnostic result, RP3 image (build 16:35, `logs/log22.txt`)

Four motion wakes (67, 61, 8 and 75 blocks), each a photo saved, no errors:

`before setup same; after capture start same; after frame same; photo queued same`

on all four. **On the RP3 image the grid survives to the photo being queued** (595 to 694 ms after
`app_main()`), so it could be read after the frame. But op 11 was 1000 ms, so the HM0360 may simply
not have taken its next motion-detection frame yet: this does not show whether `MD_CTRL1 = 0` at
the wake stops the updates. To settle it, the same test with op 11 = 200 (several HM0360 frames
before the photo is queued).

### Diagnostic result, RP3 image, op 11 = 200 ms (`logs/log23.txt`)

Three motion wakes (139, 15 and 34 blocks), each a photo saved, no errors:

| Wake | before setup | after capture start | after frame | photo queued |
|---|---|---|---|---|
| 1 | same | DIFFERS (80) | DIFFERS (93) | DIFFERS (76) |
| 2 | same | DIFFERS (56) | DIFFERS (56) | DIFFERS (62) |
| 3 | same | DIFFERS (61) | DIFFERS (61) | DIFFERS (31) |

- **`MD_CTRL1 = 0` at the wake does not stop the grid changing.** On the RP3 image the HM0360
  carries on taking motion-detection frames while the AI processor is awake, and each one rewrites
  `ROI_OUT` (non-zero, changing counts). With op 11 = 200 the wake grid was gone by the time the
  capture started (131 ms). log22 only kept it because op 11 was 1000.
- So the grid must be read before the HM0360's next frame: within one op 11 interval of the wake,
  on either image. Reading it in `app_main()` is the only point that is always safe, unless the
  HM0360 is stopped at the wake (Charles's MODE_SELECT idea). On the RP3 image that would also stop
  the `HM0360 motion in N blocks` reports for the 2nd and later photos of a burst, which rely on
  the HM0360 running.

### Burst read of the grid: a temporary check, coded 30 October 2026

The wake-grid diagnostic (`WAKE_GRID_DIAG`) is removed: `hm0360_md.c/.h` and `image_task.c` are
back as committed. Charles doubts the HM0360 increments the register address in a burst read (the
datasheet does not say it does), but agreed to try it.

**TEMPORARY, to be removed once answered** (`WAKE_GRID_BURST_CHECK`, defined at the top of
`hm0360_md.c`): `readRoiOutBurst()` reads the 32 `ROI_OUT` registers in one
`hx_drv_i2cm_write_restart_read()`. At a motion wake `hm0360_md_saveWakeGrid()` reads the grid both
ways and prints

`Wake grid burst read: same (ret 0). 32 single reads N us, burst M us`

and, if they differ, both sets of 32 bytes. The grid saved is still the one from the single reads.

### Burst read result, RP3 image (build 16:48, `logs/log24.txt`)

Three motion wakes (97, 80 and 58 blocks), each a photo saved, no errors:

`Wake grid burst read: same (ret 0). 32 single reads 18852 us, burst 4020 us`

on all three. **The HM0360 does increment the register address in a burst read**: the three grids
had different, well-filled patterns and matched byte for byte. **The burst takes 4.0 ms against
18.9 ms**, about 15 ms saved per read (at 100 kHz, with the RP3 image's bus-speed switch included).
Tested on the RP3 image only; the HM0360 image uses the same bus at the same speed.

### Burst read result, HM0360 image (build 16:53, `logs/log25.txt`), and the change

Three motion wakes (95 blocks on the first): `same (ret 0). 32 single reads 18832 us, burst 4007 us`
on all three. So the burst read works on both images.

**Coded 30 October 2026** (agreed with Charles: only for the MD grid). The temporary check is
removed. In `hm0360_md.c` a new `static readRoiOutBurst()` does the one-transaction read, and
`hm0360_md_getMDOutput()`, the only function that reads the grid, uses it, falling back to the 32
single reads with `MD grid: burst read failed, reading one register at a time` if the I2C transfer
fails. Its two callers both gain: the wake grid in `app_main()` (before the scheduler, so on the
path to the frame) and the per-frame grid in `image_task.c` (also the `md` bytes sent to
`live_view.py`). Charles's fallback idea, skipping the wake-grid read (engineering information
only), is parked.

**Bench test:**
1. `live_view.py` with Motion detection on: the grid over the image follows a hand moved in front
   of the camera, on each image. No `MD grid: burst read failed` lines.
2. Motion wakes on both images (op 7 = 0, op 11 = 1000): `HM0360 woken by motion in N blocks`
   still printed, and `scheduler started` about 15 ms earlier than log19 (HM0360, 30) and log20
   (RP3, 40), with `frame ready` the same amount earlier.

Bench step 1 passed (Charles, 30 October 2026): `live_view.py` with Motion detection on, the grid
follows the motion.

### Bench result, burst read, HM0360 image (build 16:58, `logs/log26.txt`)

A BLE wake, then three motion wakes (101, 25 and 6 blocks; one `TARGET OBJECT DETECTED!`), each a
photo saved and the wake grid printed. No errors, no `burst read failed`. All three identical:

| ms since `app_main()` | log19 (32 single reads) | log26 (burst read) |
|---|---|---|
| scheduler started | 30 | **15** |
| camera ready | 50 | 35 |
| capture started | 61 | 46 |
| **frame ready** | **92 to 94** (107 on the first) | **92** |
| `Image capture 1/1 took` | 32 (47 on the first) | 47 |

- **The burst read saves 15 ms before the scheduler, as expected, but the HM0360 frame is not
  sooner.** The capture now takes 47 ms instead of 32: the 15 ms saved are spent waiting for the
  sensor. On a motion wake the HM0360 delivers its first frame about 92 ms after `app_main()`
  however early the capture starts (log19's first wake, which started late, also took 47 ms). Why
  is not known: perhaps the sensor finishing its motion-detection cycle, or a frame boundary of
  its own. On a timer wake the capture takes 33 to 35 ms.
- So on the HM0360 image the motion-wake frame is now set by the sensor, not the firmware. The
  RP3 image, whose camera is separate from the HM0360, should still gain (log27).

### The same with op 11 = 200 ms (`logs/log27.txt`)

To see whether the HM0360's 92 ms is tied to its motion-detection interval. Two motion wakes (104
and 46 blocks): `capture started 46`, `frame ready 97` and `92`, capture 53 and 47 ms. **The same
as at 1000 ms, so the interval is not the cause.** The first HM0360 frame after a motion wake
still comes at about 92 ms after `app_main()`. Cause unknown.

### Bench result, burst read, RP3 image, op 11 = 200 ms (build 17:40, `logs/log28.txt`)

Four motion wakes (28, 7, 19 and 7 blocks), each a photo saved, no errors, no `burst read failed`.

| ms since `app_main()` | log20 (single reads, op 11 = 1000) | log28 (burst, op 11 = 200) |
|---|---|---|
| scheduler started | 40 | **25** |
| camera ready | 102 | 87 |
| capture started | 112 | 97 |
| **frame ready** | **135 to 136** | **150** |
| `Image capture 1/1 took` | 24 to 25 ms | 54 ms |

- The burst read saved its 15 ms, but **the RP3 capture took 54 ms instead of 24**, so the frame
  was 15 ms later than log20. Two things changed at once: the burst read and op 11 (200 here, 1000
  in log20). Not explained yet. Next: the same with op 11 = 1000, to separate them.

### Bench result, burst read, RP3 image, op 11 = 1000 ms (`logs/log29.txt`), and the explanation

Three motion wakes: the same as log28 (`capture started 97`, `frame ready 150`, capture 54 ms).
So op 11 is not the cause. **Neither is the burst read: the light is.** The RP3 capture time
follows the exposure restored at the wake (`AE: restored exposure N`), which rose as the
afternoon got darker:

| Log (time) | Restored exposure (lines) | `Image capture 1/1 took` |
|---|---|---|
| log14 (13:06), log17 (14:33), log20 | 273 to 588 | 24 to 27 ms |
| log28, log29 (17:45) | 2548 | 54 ms |
| log24 (16:49), log23 (16:42) | 3816 | 71 to 88 ms |

- **The burst read does what it should on the RP3:** the capture starts 15 ms sooner (97 against
  112 in log20). In log20's light the frame would come at about 121 ms (97 + 24); it was not
  measured again in that light.
- **The HM0360 result stands:** its integration time was 376 lines in log19 and in log26/27, so
  the light was the same, and its capture still grew by exactly the 15 ms saved.
- Comparisons of `frame ready` between logs must check the RP3's restored exposure.
