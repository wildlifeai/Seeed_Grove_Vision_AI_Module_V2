# Minimal FreeRTOS image (`ww500_minimal`) for HX6538 power measurement

#### File: README.md
#### Author: Claude (Sonnet 5), reviewed by Charles Palmer
#### 20 September 2026

## Status

**Steps 1-8 are built and working on the bench (22 September 2026); the power work is documented; open items remain.** Steps 1-6 (the minimal app and the power
investigation) are complete and what was learnt is in the power document. Step 7 (FatFS task, boot count) and step 8 (HM0360 image task, `capture` saves a JPEG) work
on the bench. What is still to be measured is listed under "Step 8: outstanding" and Open items below. No GitHub issues have been filed yet for the open items. Charles
committed the work at the end of 22 September 2026 (check `git log` for the commit); nothing has been pushed by Claude.

## Outcome

- `ww500_minimal` is a new app (`EPII_CM55M_APP_S/app/ww_projects/ww500_minimal`) for a WW500_C00 board carrying only the HX6538. It has no camera, FatFS, BLE
  interface, neural network or flash manager, so that sleep (DPD) current and operating current can be measured on their own, as was done for the BLE processor.
- It boots, blinks two LEDs (PB9, PB10), runs a console CLI, and enters DPD through the same inactivity mechanism as `ww500_md`. Wake sources are power-on, the RTC alarm
  and the WAKE pin (PA0). How it works is in
  [`ww500_minimal/doc/README.md`](../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/README.md).

**Where power results are recorded:** all of them, in one place -
[`power_investigation.md`](../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md) (its
"Short summary" section for the headline numbers, the rest for the detail, the known/unknown lists and the
evidence). This README and `ww500_minimal/doc/README.md` point to it rather than repeating figures, so there is
only one place to update as new results (camera current, DPD with the camera and card fitted, ...) come in.
Headline so far: **DPD 10 uA**, awake-idle 4.8-17.9 mA depending on clock state, Power-down with retention 1.5 mA.

Decisions and why (details in the proposal):

- **Keep the `ww500_md` inactivity mechanism** rather than write a separate power task. The
  blinky task stops after a run time, which makes every task idle, and inactivity then
  triggers DPD. The same route works from the CLI (`blink off`, `dpd`).
- **New `rtc_util`** instead of `exif_utc`, which drags in FatFS types and EXIF formats.
- **Settings revert to defaults after DPD**: RAM is lost and there is no SD card to hold them.
- **`ww.mk` is switched by editing `APP_TYPE`**, as for the other apps, rather than a
  command-line override.
- **Source files follow [`c_file_format.md`](../../c_file_format.md)**, except the
  third-party `FreeRTOS_CLI.c/.h`, which are copied unchanged.

## Step 7: the FatFS task (built and working on the bench, 22 September 2026)

Charles's brief asks for a light-weight FatFS task that checks for the SD card and keeps a boot count, to confirm the low DPD current survives adding
the SD card (step 8 will add the HM0360 image task). Answers to the questions asked before starting:

- **Hardware:** the card is powered from 3V3_WE, which is on only while the processor runs (PA1 low). No card-detect switch. SPI on PB2 to PB5. A 32 GB FAT32 card.
- **Boot count:** `BOOTS.TXT` in the root directory (8.3 names), one total for every boot. The count is to be incremented before a JPEG is written and used in its
  file name; if the increment fails, skip the JPEG. Implemented as: incremented once at every start-up, with `fatfs_task_bootCountValid()` telling the image task whether it
  worked.
- **No card:** the app works with or without one, and the card is not changed while running. The FatFS task owns the card. File time stamps come from the RTC.
- **Clean entry to DPD:** the `ww500_md` barrier mechanism: a `shutdownBarrier` with the FatFS task and the blinky task as participants; the last to report calls
  `blinky_task_sleepNow()`.
- **Power measurements:** to be made once running, and adjusted if necessary. The conservative assumption made now: `power_diag`'s `clkoff lsc` group no longer switches off
  the SPI master clock and DMA2/3.
- **CLI:** `sd`, `bootcount`, `sdwrite`, `sdread`. Also `setutc` and `sleep` wait for an SD operation to finish.

The description of the code is in `ww500_minimal/doc/README.md` ("SD card and FatFS"). What to check on the bench is listed in the reply that accompanied the code and repeated
under Open items below.

## Open items

No GitHub issue has been filed for these. File them with the `review-finding` template if wanted (ask Charles first).

- **SD card became unreadable in Windows (cause not established).** After the step 7 build ran, Windows said `F:\ is not accessible. The file or directory is corrupted and
  unreadable`, while the WW500 read the card fine. A read-only `chkdsk` reported nonvalid links on nearly every entry, including the root, and showed the card held `ww500_md` data and
  `FOUND.000` (so Windows had repaired it before). `chkdsk /F` fixed it. `BOOTS.TXT` was not among the entries reported. Not investigated further: the card was not imaged first and no fresh-card
  test was run. If it recurs, image the card (raw read of the first 16 MB) before repairing it, and try a new known-brand card formatted in Windows. One precaution was taken: the boot count is now
  overwritten in place (one data sector and the directory entry) instead of recreating the file every boot.
- **Step 7 remaining bench checks** (Charles reports the FatFS functions work and the boot count increments; these were not all reported): build; boot with and without a card (`sd`, `bootcount`; the boot count should go up at every boot, including DPD and Power-down
  wakes); `sdwrite` and `sdread`; a wake by timer and by the WAKE switch with the card fitted; DPD current with the card fitted (it should stay near 10 uA); the time the mount and the
  boot count write take (printed at start-up); and what happens with no card (the mount can take a second or more before giving up).
- **Measure the wake latency** (WAKE edge or alarm to the application running) for DPD, `sleep 30 1` and `sleep 30 0`, with an oscilloscope or logic analyser on the
  WAKE pin and PB10. This decides whether Power-down with retention is worth its 1.5 mA.
- **Test the Power-down wake by the WAKE switch** on the fixed build (only the timer wake was tested).
- **Why Power-down draws 1.5 mA** when the datasheet's typical is several times lower, and the same with and without retention. Untried: the PMU settings copied from the Himax
  example (DC-DC, I/O retention, the external-supply pin), pin states, the board.
- **Trim the RTC** (`hx_drv_scu_set_RC32K1K_trim()` is unused) or take the time from the BLE processor: the RTC is about 4 % fast.
- **Look at DVFS and SRAM power-down** in the SDK, for the static floor.
- **Which block of the `hsc` clock group stops a DPD resume** when left off (U55, I3C, PUF, DMA or SDIO). Not needed if the clocks are always restored before DPD.
- **Whether any of this applies in `ww500_md`**, with its camera, SD card and BLE link. The experiments ran only in `ww500_minimal`. Nothing has been changed in `ww500_md`.
- **Before production builds:** set `APP_TYPE` in `ww.mk` back to `ww500_md`. The experiment commands in `ww500_minimal` change nothing unless used and can stay or be removed.
- `platform_driver_init()` and `board_init()` (shared with `ww500_md`) were not touched. They were not found to matter: with the app's own clock changes the current fell to 4.8 mA.
- The HM0360 has now been reintroduced (step 8): see "Step 8: outstanding" for what still has to be measured.

## Files in this thread

- [`CLAUDE_Minimal_FreeRTOS.md`](CLAUDE_Minimal_FreeRTOS.md): the task brief
- [`CLAUDE_Minimal_FreeRTOS_proposal.md`](CLAUDE_Minimal_FreeRTOS_proposal.md): the proposal, Charles's answers (section 10) and the implementation notes (sections 12 and 13)
- [`power_investigation.md`](../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md): the power findings, with the short summary and the lists of what is
  and is not known
- Bench logs (Tera Term, host timestamps): `part_b_log.txt`, `part_b_run1_log.txt` (truncated), `part_b_run2_log.txt`, `part_b_slow_24MHz_log.txt`, `part_b_fast_400MHz_log.txt`
  (FreeRTOS tick and RTC accuracy), `partd_log.txt` (DPD from the slow state), `parte_1_retention1_wake_hung_log.txt`, `parte_2_retention1_wake_ok_log.txt` and
  `parte_3_retention_tests_log.txt` (Power-down with retention)

## Step 8: the HM0360 image task (built and working on the bench, 22 September 2026)

A proposal was written first ([CLAUDE_Step8_camera_proposal.md](CLAUDE_Step8_camera_proposal.md)) and Charles answered its questions (section 7 there).

### Step 8: done

- `image_task.c/.h`: a light-weight image task. At boot it initialises the sensor I2C master, checks for the PCA9574 I2C expander (0x20 or 0x21, a test that the bus works),
  checks for the HM0360 at 0x24, writes its register table after a cold boot only, sets up the data path, and leaves the sensor in a resting mode (default 2,
  `MODE_SW_NFRAMES_SLEEP`). `capture` takes one frame in mode 2 and has the FatFS task save the JPEG (no EXIF) as `Bnnnnnnn.JPG` (5 digits of boot count, 2 digits of picture number in this boot).
  `cam [mode|init]` shows the mode, model ID and frame counter, sets the resting mode (0-4, 6, 7) or rewrites the register table. The task is in the shutdown barrier.
- Copied from `ww500_md`: `hm0360_md.c/.h`, `hm0360_regs.h` (only edit: the unused `fatfs_task.h` include removed and the register table include path corrected) and `cis_sensor/cis_hm0360/` (unchanged).
  Changed: `ww500_minimal.mk` (`sensordp`, `cis_sensor`, `cis_hm0360`, `-DUSE_HM0360`), `app_msg.h`, `ww500_minimal.c/.h` (four tasks, three in the shutdown barrier), `CLI-commands.c`.
  Description: `ww500_minimal/doc/README.md`, "HM0360 camera".
- **Bench results:** the camera was not found at first. The I2C master, its clock and its pin mux were fine and the PCA9574 answered on the same bus, so the fault was at the camera end
  (the connector is hard to solder). The first captures then failed with `EDM WDT2 timeout` (data path event -76) and a sensor frame counter that did not move. After the connections were
  repaired: the camera answered at 0x24, and `capture` gave the frame after 33 ms, a 9240 byte JPEG saved as `B0010300.JPG` (boot 103, picture 0), write 88 ms.
- **Diagnostics kept in the code** because they were needed: data path events are named (with a hint), a failed capture reports whether the sensor's frame counter moved, and the PCA9574 check.

### Step 8: outstanding

- Open the saved JPEG on a PC and judge the picture (only the file size and the write result were seen). A second `capture` in the same boot should give `B0010301.JPG`.
- **Current in each HM0360 mode** (`cam 0`, `cam 2`, ...) with the 0R link lifted, to check the comment in `hm0360_md.c` (about 700 uA in mode 0, 270 uA in mode 2) and which mode is really lowest.
- **DPD current with the camera and the SD card fitted**, after a plain boot and after a `capture` (target: still about 10 uA). If it rises, look at the camera's control pins and I2C pads and the sensor's mode before DPD.
- Record the results of both in `power_investigation.md` (add a "Camera (HM0360)" entry to its summary table and detailed record), not here - see "Where power results are recorded" above.
- **Sensor state over DPD:** after a wake, `Image: HM0360 was in mode N when the boot began` should show the mode it was left in, and the ready line should say `warm init` (no register table).
  Not tested yet. Note the sensor is left in the resting mode, in which it keeps producing a frame about every 2 s with nobody listening (as in `ww500_md` in DPD).
- **Check the capture timing:** the 5 s wait for the frame is far longer than the 33 ms seen. Take more pictures, then decide whether to shorten it. (The doc says the first frame does not wait for the sleep interval in mode 2.)
- **The file name** was my reading of `Bnnnnnnn.JPG`: `B` + boot count modulo 100000 in 5 digits + 2 digits for the picture number. Confirm it is what was wanted.
- Not done, by decision: EXIF, exposure control, more than one frame per capture (1 frame for now), a `power_diag` check for `clkoff image/hsc` (a `capture` after them times out; use `clkon`).
- If the I2C probing is no longer wanted at boot, the PCA9574 check in `image_task.c` (`checkExpander()`) can go; it costs a few ms and prints one line.
- The `cam` and `capture` commands and the boot-count use are documented in `ww500_minimal/doc/README.md`. The step 7 bench checks in Open items still apply (they were not all reported).
