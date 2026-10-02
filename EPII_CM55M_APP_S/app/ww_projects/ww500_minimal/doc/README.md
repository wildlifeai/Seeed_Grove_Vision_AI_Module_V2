# ww500_minimal

#### File: README.md
#### Author: Claude (Sonnet 5), reviewed by Charles Palmer
#### 20 September 2026

A minimal FreeRTOS image for the HX6538 (AI processor) on a WW500_C00 board that carries
almost nothing else. It exists so that DPD (sleep) current and operating current can be
measured on their own. It is not a product firmware: it has no BLE interface, neural network
or firmware-update code. It started with no camera or SD card either; the SD card (step 7)
and the HM0360 camera (step 8) have since been added, to check that the low DPD current
survives adding them, and then (27 September 2026) an RP3 camera build, to measure the RP3.

Changes found or proven here that should also be made in `ww500_md` are listed at the end of this file:
[Changes to transfer to ww500_md](#changes-to-transfer-to-ww500_md).

How the work happened is in
`_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/`. What the power measurements showed, and
what is and is not known, is in [power_investigation.md](power_investigation.md) (start with its short summary).

## Behaviour

1. Power-on or wake: prints a banner and the wake reason, and flashes the red and blue LEDs.
2. Blinks the red (PB9) and blue (PB11) LEDs alternately (`BLINKY_TASK_PERIOD_MS`), printing the RTC time every
   `WW500_MINIMAL_TIME_PRINT_PERIOD_MS`.
3. After the run time (`WW500_MINIMAL_RUN_TIME_COLD_MS` after a cold boot,
   `WW500_MINIMAL_RUN_TIME_WARM_MS` after a wake) the blinking stops, so all tasks are idle.
4. After `WW500_MINIMAL_INACTIVITY_MS` of inactivity the processor prints `Inactive for <n>ms`,
   drives the LEDs low and enters DPD.
5. Wake sources: the WAKE pin (PA0, level high) or the RTC alarm
   (`WW500_MINIMAL_ALARM_PERIOD_S`). In the HM0360 build, HM0360 motion detection also wakes it
   through PA0 once it is turned on with `context B` and `mdint <ms>` (25 September 2026; see
   "Motion detection" under "HM0360 camera").

Every boot prints a `Retention check` line: the app keeps a value in the `.noinit` section, which the start-up code does
not clear, and reports whether it survived (`the RAM was kept`, after a `sleep <seconds> 1`) or not (first boot, DPD,
power-cycle, `sleep <seconds> 0`). After a wake it also prints `RTC before synchronising`, which is the time DPD or
Power-down was entered, not the wake time (see `WW500_MINIMAL_SYNC_RTC_AFTER_DPD`).

Any character typed at the console extends the inactivity period to
`WW500_MINIMAL_INACTIVITY_CLI_MS`. This matters after a cold boot: the run time is short, so
type a character to keep the console available. Cold boot sets the RTC to
`WW500_MINIMAL_DEFAULT_TIME`. The RTC keeps time through DPD but not through power loss.

Settings changed at the CLI are lost in DPD and revert to the defaults below. The exception is the HM0360's resting mode,
context and motion detection interval, which the sensor itself keeps and which are read back from it after a wake.

## Hardware assumed

The HX6538 is on a WW500_C00 board whose BLE processor module is not fitted. Wire links
connect the HX6538 to the LEDs and switch that the BLE processor normally drives and reads.
These are described in the "Wire Links" section of
`_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/CLAUDE_Minimal_FreeRTOS.md`
and summarised here.

| Signal | HX6538 pin | Wire link (from the brief) | Note |
|---|---|---|---|
| Console UART0 | PB0 (RX), PB1 (TX) | none | 921600 baud |
| SD card (SPI) | PB2 (DO), PB3 (DI), PB4 (SCLK), PB5 (CS) | soldered socket | Powered from 3V3_WE (on only while the processor runs) |
| Red LED (LED1, R22) | PB9 (U2 pin 4) | U1 pin 21 to U2 pin 4 | GPIO0, active high (1 = on) |
| Blue LED (LED2, R20) | PB11 (U2 pin 22) | U1 pin 22 to R20 | GPIO2, active high (1 = on). Normally /IP_INT so can't be used if MKL62BA is present. |
| SENSOR_ENABLE | PB7 | none | GPIO1, output, low from `pinmux_cfg_init()` on (the RP camera enable; not used by the HM0360). The RP3 build drives it high only while the camera is in use |
| Green LED (LED3, R40) | not assigned | U1 pin 10 to a pin to be decided | Possible future addition. Not used by this firmware. |
| WAKE switch (SW1) | PA0 | U1 pin 12 to pin 24 | Fitted in place of /BLE_WAKE. Level high wakes from DPD. |

PB9 and PB10 are the PDM_CLK and PDM_DATA pins (pinmux function 1) when not used as GPIO. PB10 is now set to function 0 (an input).
GPIO0-GPIO2 signals are also available on PB6-PB8 and the SWD pads. Only one pad should be
assigned to each: GPIO0, GPIO1 and GPIO2 are each one signal that can appear on two pads (PB6/PB9, PB7/PB10, PB8/PB11; datasheet section 4.5, note 3). With the blue LED on PB10 as GPIO1, anything driving SENSOR_ENABLE on PB7 also drove the LED. All the pins are set in `pinmux_cfg.c`. **PB7 and PB8 are also the SWD pins** (SWCLK, SWDIO), which the bootloader sets up: PB8 stays SWDIO, but once the app has made PB7 a GPIO, SWD can only connect in the short time between the bootloader and `pinmux_cfg_init()`.

## SD card and FatFS

The SD card socket is on PB2 (SPI data out), PB3 (data in), PB4 (clock) and PB5 (chip select, driven as GPIO16 by the SD card
driver). The card is powered from the 3V3_WE rail, which is on only while the processor is running (PA1 low), so it is off in DPD.
There is no card-detect signal. Use a 32 GB SDHC card formatted FAT32 from a known brand (see
`ww500_md/doc/WW500_FATFS_Behaviour.md` for a counterfeit card that could not be used).

`fatfs_task.c/.h` is a light-weight FatFS task, much smaller than the one in `ww500_md`:

- **It owns the card.** All FatFS calls are made by this task. It mounts the card at start-up (after a 10 ms wait for the supply). If the mount
  succeeds a card is present. The app works with or without a card. The card must not be changed while the app is running.
- **Boot count.** `BOOTS.TXT` in the root directory holds one decimal number: one total for every boot (cold, timer wake, WAKE-pin wake, Power-down
  wake). It is incremented once at every start-up, and printed (`Boot count is N`). The number is written as a fixed 11 bytes (ten digits and a line end)
  over the start of the existing file, so an update changes one data sector and the directory entry and nothing in the FAT. (FatFS is not safe against power
  loss part way through an update, and the card's supply is cut at DPD; recreating the file every boot took six to eight sector writes.) If the file is missing the count starts at 1. If it cannot be updated
  `fatfs_task_bootCountValid()` is false, and the image task (step 8) is to skip the image. The count is meant to be used in the names of the JPEG files.
- **File operations.** Other tasks send `APP_MSG_FATFSTASK_WRITE_FILE` or `APP_MSG_FATFSTASK_READ_FILE` with a `fileOperation_t` (8.3 name in the
  root directory, buffer, length, and the queue and event for the reply). With no card the reply is `FR_NOT_READY`.
- **Clean entry to DPD.** The FatFS task and the blinky task are the two participants in `shutdownBarrier` (as `image_sleepNow` is in `ww500_md`).
  When all tasks have been inactive each one finishes what it is doing and reports to the barrier; the last to report calls `blinky_task_sleepNow()`,
  which enters DPD. Files are closed after every operation, so the card is safe to lose power.
- **Configuration.** `ffconf.h` is the `ww500_md` one, trimmed: one volume, 8.3 names, no directories, no `f_mkfs`, labels, string functions,
  locking or TRIM. File time stamps come from the RTC (2024-01-01 at a cold boot, and note the RTC is about 4 % fast; the first RTC read after a
  DPD wake is the time DPD was entered).
- **Left out from `ww500_md`:** the operational parameters and `CONFIG.TXT`, the directory manager and image folders, GPS, the deployment ID, model
  labels, the manifest unzipper, and the file CLI commands.
- **Setting the RTC** (`setutc`) and entering Power-down (`sleep`) wait, for up to 2 s, for an SD card operation in progress.

Files added: `fatfs_task.c/.h`, `ffconf.h`. `ww500_minimal.mk` selects `MID_SEL = fatfs`, `FATFS_PORT_LIST = mmc_spi` and `CMSIS_DRIVERS_LIST = SPI`.

## HM0360 camera (step 8: built; camera found, `capture` saves a JPEG on the bench, 22 September 2026)

`image_task.c/.h` is a light-weight image task, much smaller than the one in `ww500_md`. The design decisions and the answers to the questions are in `_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/CLAUDE_Step8_camera_proposal.md`.

- **Hardware:** the HM0360 wiring is the same as in `ww500_md` (I2C address 0x24, `USE_DW_IIC_1`). At start-up the image task also checks for the PCA9574 I2C expander (0x20 or 0x21) on the same bus, as a test that the bus works when the camera does not answer. Its supply is always on, so it is never power-cycled, not even by DPD.
- **Initialisation:** at every boot, in the image task. After a cold boot (or any wake that is not the WAKE pin or the RTC) the long register table is written (`cisdp_sensor_init(true)`). After a wake from DPD it is not (`cisdp_sensor_init(false)`): the sensor should have kept its registers, and before anything is written the motion detection interrupt is reported and cleared, and the mode, context and motion detection interval are read back from the sensor and printed (`Image: HM0360 kept mode N, context C, motion detection on/off ...`; since 25 September 2026, see "Motion detection" below) as the evidence. The data path is set up at every boot, as the HX6538 loses it in DPD. If the sensor does not answer at 0x24 the task says so and the app carries on without it (`states` shows `No camera`).
- **Resting mode:** whenever the sensor is not taking a picture it is in the resting mode, by default mode 2 (`MODE_SW_NFRAMES_SLEEP`, 1 frame, longest sleep interval, motion detection interrupt off). It is left in that mode for DPD. Measured current: see `power_investigation.md` ("Where is the HM0360 power going?"); `hm0360_md.c`'s own comment gives about 270 uA for mode 2 against about 700 uA for mode 0 as a starting expectation. In mode 2 the sensor keeps producing a frame about every 2 s with nobody listening (as in `ww500_md` in DPD).
- **`capture`:** refused if there is no camera or 100 pictures have already been saved in this boot. Otherwise it does what `ww500_md` does for a picture: `cisdp_dp_init()`, `hm0360_md_setMode(MODE_SW_NFRAMES_SLEEP, 1 frame)` with sleep time zero, `cisdp_sensor_start()`. The first bench capture took 33 ms to the frame (the sleep interval does not delay the first frame in mode 2); the wait times out after 5 s. On a data path failure the console names the event (for example `EDM WDT2 timeout`) and says whether the sensor's frame counter moved (it did not when the connector soldering was faulty). **Whether the SD card is fitted (or FatFS code present) is checked only after the frame is in hand**, not before the capture (23 September 2026 - was checked first, so `WW500_NO_FATFS` builds could not test the camera): with no card, no valid boot count, or 100 pictures already saved, the frame is taken (so its size and timing can still be seen) and then discarded, printed as e.g. `frame after 33 ms, JPEG is 9240 bytes, but there is no SD card, so it was not saved`. Otherwise the JPEG (VGA, `jpg_ratio` 10, no EXIF) is passed unchanged to the FatFS task and saved as `Bnnnnnnn.JPG`: `B`, then the boot count modulo 100000 in 5 digits, then the number of the picture in this boot in 2 digits (e.g. boot 1203, picture 3 is `B0120303.JPG`, 88 ms to write on the bench). The sensor is put back in the resting mode before the write starts, or immediately if there was no write.
- **`cam`:** with no parameter prints the mode the sensor is in, its model ID and its frame counter (0xFFFF until it has output a frame); `cam <0-4|6|7>` sets the resting mode so that its current can be measured; `cam init` writes the register table again.
- **Motion detection (`context`, `mdint`; 25 September 2026, built and tested on the bench: the motion wake works):** `context B` then `mdint <ms>` puts the resting mode in the state `ww500_md` uses before DPD (`hm0360_md_prepare()`: context B, mode 2, 1 frame, the interval, motion detection interrupt enabled). The cold-boot register table already turns motion detection on (`MD_CTRL` 0x2080 = 0x31) with low sensitivity for context B. The settings are **kept through DPD by reading them back from the sensor** at a warm boot (the HX6538 RAM is lost; the sensor keeps its registers): mode from `MODE_SELECT`, context from `PMU_CFG_3`, interval from the sleep count in `PMU_CFG_8/9` when `MD_CTRL1` is non-zero. At a warm boot, before anything is written, `INT_INDIC` is read and printed with the number of motion blocks, and the console says whether a WAKE pin wake came from the HM0360; then all interrupt bits are cleared (`INT_CLEAR` 0xFF). As in `ww500_md`, the interrupt is **disabled while the processor is awake** (the sensor keeps taking its motion detection frames): before DPD the interrupt bits are cleared (a raised interrupt holds PA0 high and would end DPD at once) and then the interrupt is enabled. (First bench test, 25 September: motion wake works, `INT_INDIC` 0xC8; with the interrupt left enabled while awake, motion was also reported just before DPD, hence this change.) `cam` also prints the context, the interval and `INT_INDIC`.
- **Context B timing experiment (parked 26 September 2026):** the aim was to cut the motion detection current by shortening context B's frame. Context B is already QVGA (sub-sample 2, outputs off) but the `.i` table gives it context A's VGA frame and line lengths. Code left in place, all switched off (the defaults): `CIS_CONTEXT_B_TIMING` in `cisdp_cfg.h` (0 = `.i` table, 1 = line length `0x0300`, 2 = also frame length `0x011C`, written with `COMMAND_UPDATE` at cold boot by `HM0360_contextB_timing[]` in `cisdp_sensor.c`); `CIS_CONTEXT_B_OUTPUT` (1 = context B outputs on, so VSYNC can be seen); `PRINT_REGISTERS_BEFORE_DPD` in `image_task.c` (dumps mode, context, PMU, MD, exposure and the three timing blocks before DPD). Result: the registers were written and in use (`0x0340-43` read back the new context B values) but the PPK2 showed little or no change. Leads for a return, not yet tested: the exposure (`INTEGRATION` read 0x0178 = 376 lines, longer than the 284-line frame, so auto-exposure may stretch the frame); pre-metering (`PMU_CFG_5` 0x3026 = 0x03 = at power-up and at every wake; the number of STROBE pulses per wake varied), try 0x01 or 0x05; the unexplained third timing block at `0x35B4` (frame length `0x0094`); an exposure cap (`MAX_INTG` 0x2029/2A). Charles's notes and PPK2 screenshots are in `power_investigation.md` under "Changes made to context B timing".
- **DPD:** the image task is in the shutdown barrier with the blinky and FatFS tasks; DPD is not entered while a picture is being taken or written.
- **Caution:** `clkoff image` and `clkoff hsc` (see `power_investigation.md`) switch off the data path, JPEG and xDMA clocks, so a `capture` after them times out. Use `clkon` first.

Files added: `image_task.c/.h`, `hm0360_md.c/.h` and `hm0360_regs.h` (copied from `ww500_md`; the only change is in `hm0360_md.c`: the unused `fatfs_task.h` include is removed and the register table include path corrected), and `cis_sensor/cis_hm0360/` (an unchanged copy of the `ww500_md` folder). `ww500_minimal.mk` adds `sensordp` to `LIB_SEL`, sets `CIS_SUPPORT_INAPP = cis_sensor` and `CIS_SUPPORT_INAPP_MODEL = cis_hm0360`, and defines `USE_HM0360`.

## RP3 camera (27 September 2026; built and used on the bench 28 September)

Built with `CIS_SUPPORT_INAPP_MODEL=cis_imx708`, which defines `USE_RP3` (and `CIS_IMX`, as `ww500_md` does) instead of
`USE_HM0360`. For measuring the power of the RP3 (IMX708) fitted in place of the HM0360, including taking a picture.

- **Power:** the RP3 is powered by SENSOR_ENABLE (PB7, GPIO1, `pinmux_cfg_rpSensorEnable()`, the equivalent of
  `ww500_md`'s `rp_sensor_enable()`). PB7 is low from `pinmux_cfg_init()` on, so the camera is off except while it is in use,
  and always off for DPD. It loses its registers when off, so they are written every time it is powered, as `ww500_md` does
  after every DPD.
- **At start-up** (cold or warm): SENSOR_ENABLE high, `CIS_POWERUP_DELAY` (10 ms since 28 September; was 100 ms), read at I2C address 0x1A and print the
  model ID (0x0708 expected), then off again.
- **`capture`:** SENSOR_ENABLE high, `cisdp_sensor_init()` (all the IMX708 tables), `cisdp_dp_init()` (the same 640x480
  JPEG data path as the HM0360 and `ww500_md`), stream on (`cisdp_sensor_start()`), first frame, stream off and MIPI off
  (`cisdp_sensor_stop()`), SENSOR_ENABLE low, then the JPEG is saved as for the HM0360. The console gives the power-up and
  initialisation time and the frame time (from stream on) separately.
- **Power-up time and the I2C speed (28 September 2026):** `cisdp_sensor_init()` makes about 255 I2C transactions (145
  table writes, 108 PDAF pixel-correction gain writes, one read, one or two others), each a separate transaction. At the
  HM0360's 100 kHz they took 111 ms (blue LED markers on the PPK2) and the whole power-up 130 ms. The RP3 build now runs the
  sensor I2C at 400 kHz (`SENSOR_I2C_SPEED` in `image_task.c`): 40 ms and 57 ms. Compiling out the progress messages in
  `cisdp_sensor_init()` (`CISDP_DBG_TYPE` in `cis_sensor/cis_imx708/cisdp_sensor.c`; the failure messages still print) brought
  the power-up to 51 ms. Not yet tried: writing the two blocks of 54 PDAF gains as burst writes, or leaving them out (see the
  image first: they correct the phase-detection pixels). The timing of every stage of a `capture`, and what else might be
  done, is in `power_investigation.md` ("RP3 camera", "Time from `capture` to the frame").
- **Timing markers:** the blue LED is lit around the register writes (calls added by Charles in `cisdp_sensor_init()`), and
  the red LED from stream on until the data path reports the frame (`CAPTURE_TIME_ON_RED_LED` in `image_task.c`, 1 = on).
  Use `blink off` first so the blinky task does not also drive the LEDs. The LED current adds to the measured current.
- **`cam on` / `cam off`:** keeps the camera powered and initialised (not streaming) between pictures, or not (the
  default), so its standing current can be measured. `cam` alone says which. `cam init` checks again that it answers.
- **Not ported from `ww500_md`:** auto-exposure (`ae.c`) and the software white balance (`img_correct`, `sw_jpeg.c`). The
  exposure is the fixed one in the register tables and the picture will be green, as the raw sensor has no white balance.
  `context` and `mdint` are HM0360 only, and the HM0360 is not used for motion detection in this build.

Files: `cis_sensor/cis_imx708/` is a copy of the `ww500_md` folder, changed since only in `cisdp_sensor.c` (FreeRTOS includes first; console messages filtered by `CISDP_DBG_TYPE`, failures only by default; `ww500_minimal.h` included and the blue LED timing markers) and `cisdp_cfg.h` (`IMX708_POWERUP_DELAY` 10 ms): see "Changes to transfer to ww500_md". `image_task.c` keeps one capture and save
flow, with the camera-specific parts in `USE_HM0360` and `USE_RP3` blocks. `hm0360_md.c` (still built, not used) includes
its register table as `../cis_hm0360/...`, as `ww500_md` does, so it is found whichever camera folder is selected.

## RTC accuracy and the 32.768 kHz crystal

The board has a 24 MHz crystal but no 32.768 kHz crystal (confirmed by Charles). Two separate things follow from
that, and they have different answers.

**Why the RTC is about 4 % fast.** The RTC, and the sleep/wake timers, are clocked from the 32.768 kHz domain.
With no 32.768 kHz crystal fitted, that domain runs from the internal RC32K oscillator instead, and the RC32K
oscillator is what `power_investigation.md` measured as about 4 % fast (4.05 % at 400 MHz CPU clock, 3.96 % at
24 MHz - see its "Timing accuracy" section). The datasheet (section 5.8.2, and the clock-sources list) describes
this RC oscillator as "factory trimmed for accuracy", but a real crystal is normally two to three orders of
magnitude more accurate than any RC oscillator, trimmed or not, so a 4 % error is not surprising. **Fitting a
32.768 kHz crystal, and switching the RTC's clock source to it, should fix this** - see "What would have to
change" below.

**Confirmed on the bench (23 September 2026):** the RTC's own clock source selector,
`SCU_PDAON_CLK_CFG_T.aonclk` (`hx_drv_scu_get/set_pdaon_clk_cfg()`, separate from the HSC/LSC 32 kHz selectors,
since the RTC is in the always-on power domain - datasheet 5.8.2), reads `RC32K1K` every time `clocks` has been
run, as expected (nothing in this app or `ww500_md` ever selects the crystal source). The error at the default
trim was measured directly at +4.4 % fast, and the RC32K1K trim register brings it to about 0 % at trim 33 - see
"The RC32K1K trim register" below. (Not yet checked: whether the RC32K1K oscillator itself is running in its
32.768 kHz or 1 kHz submode - `clocks` shows this too, as "RC32K1K oscillator ..., selected for ...".)

**Why `setutc` takes about 1.4 s: not fixed by a crystal.** This is a separate mechanism from the RTC's running
accuracy. Per `ww500_md/doc/WW500_file_write_fail.md` ("Root cause"), `hx_drv_rtc_set_time()` suppresses ARM
interrupts for about 1357 ms while it busy-polls a hardware status register, because the WE2 RTC hardware only
captures a new counter value at a 32.768 kHz clock boundary - worst case one full cycle of that clock, plus
further polling margin in the driver. This is a synchronisation delay between clock domains (the fast CPU clock
writing into the slow RTC clock domain), and it applies for one edge of whichever 32.768 kHz clock the RTC uses,
**whether that clock is accurate or not**. A crystal is far more stable than the RC oscillator, but it still only
ticks at 32.768 kHz, so the wait for the next edge is the same order of magnitude either way. **Fitting a crystal
would not be expected to shorten `setutc`.** The driver code that causes the delay is prebuilt and was not
modified by that investigation.

**What would have to change to use a crystal**, if one were fitted (XTAL_32K_I/O pins, per the datasheet pinout -
not currently populated on this board):
1. Confirm the 32.768 kHz crystal oscillator is enabled (`xtal 32 <0|1>` in this app; the datasheet says both
   crystal oscillators default to enabled, and `power_investigation.md` found the 32.768 kHz one enabled at a
   cold boot even with no crystal fitted).
2. Run `clocks` to confirm the RTC's clock source is `RC32K1K` (see above).
3. If it is, set it to `SCU_AONCLKSRC_XTAL32K` with `hx_drv_scu_set_pdaon_clk_cfg()`, early at boot, before the
   RTC is read or written. There is no CLI command for this yet (it needs a crystal fitted first to be worth
   trying); ask for one when the hardware is ready.
4. Re-measure the RTC rate against the host clock (see "Measuring RTC accuracy" below) to confirm the
   improvement.

This (fitting a crystal) has not been coded or tried. It needs a hardware change (populating the XTAL_32K_I/O
pins), so the RC32K1K trim register below is the thing to try first, since it needs no hardware change.

### The RC32K1K trim register (23 September 2026: measured, not adopted in code)

The RTC's RC oscillator can be adjusted in place, no hardware change, with `hx_drv_scu_get/set_RC32K1K_trim()`
(CLI: `rc32ktrim [0-255]`, also shown by `clocks`). The direction and step size are not documented, so a short
sweep was run (five commands: `rc32ktrim <value>`, `inactivity 150`, `awake 150`, `blink 500`, `timeprint 10`,
then measure the RTC rate against the host clock for 100 s+ - all of these revert on the next DPD/reboot).

| Trim | Result |
|---|---|
| 31 (default) | +4.4 % fast |
| **33** | **~0 % (best found)** |
| 34 | -2.2 % slow |
| 63 | -37.0 % slow (badly overshot) |

**The trim does not survive DPD or a power-cycle** - it reads back at the hardware default after either, so to
use 33 it would need setting again every boot (e.g. in `app_main()`/`rtc_util_init()`). Not done; no code changed.

### FreeRTOS tick accuracy: crystal vs the 24 MHz RC oscillator

The FreeRTOS tick is accurate when the CPU runs from the crystal-fed PLL (about -10 ppm at 400 MHz - see
`power_investigation.md`, "Part B, long runs"), and about 0.96 % slow when `clkslow rc` moves it to the internal
24 MHz RC oscillator instead - the oscillator's own inaccuracy, not a software bug.
`ulTimerCountsForOneTick = configSYSTICK_CLOCK_HZ / configTICK_RATE_HZ` (`configTICK_RATE_HZ` is 1000) is used
correctly throughout, including in the tickless-idle SysTick reload path (`vPortSuppressTicksAndSleep()`); there
is no `1024`-vs-`1000` mismatch.

## Building

Select the app in `EPII_CM55M_APP_S/app/ww_projects/ww.mk` (uncomment one `APP_TYPE`):

```
APP_TYPE = ww500_minimal
```

Then build as described in `_Documentation/building_firmware.md`. There are two camera
variants (27 September 2026), chosen as in `ww500_md`, with `make clean` between them:

```bash
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_hm0360   # HM0360 (the default)
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_imx708   # RP3 (IMX708)
```

The build generates the flashable image, named `M<YMDDHMM>.IMG` (same scheme as the other
apps; the leading letter is `M` for both cameras, so note which one you built). Flash and recover as in
`_Documentation/firmware_update_and_recovery.md`.

After changing a `#define` in a header (such as those in `ww500_minimal.h`), run `make clean`
before building. On 20 September 2026 an incremental build kept an old value in an object file:
only `ww500_minimal.c` is force-rebuilt, so a file that merely uses the changed define was not
recompiled.

Remember to set `APP_TYPE` back to `ww500_md` before building the production firmware. CI
builds `ww500_md` only and does not build this app.

`ww500_minimal.mk` uses the GNU toolchain only (there is no `.sct` for armclang) and links
only the `pwrmgmt` library, with no middleware.

### Building without the camera or FatFS code (23 September 2026)

Two lines near the end of `ww500_minimal.mk`, normally commented out, isolate whether the camera or SD card
code affects DPD current (added after the current rose from 10 uA to 13.8 uA with both removed and only the
PCA9574 added - which turned out to be neither: disabling the camera code alone made no difference; see
`power_investigation.md`):

```
WW500_NO_CAMERA = y
#WW500_NO_FATFS = y
```

Uncomment either or both, then clean and rebuild (a compile define - see "Building" above for why a clean
build matters; in Eclipse: `Project > Clean...`, then Build). With `WW500_NO_CAMERA`, the image task is never
created and `capture`/`cam` are not registered. With `WW500_NO_FATFS`, the FatFS task is never created and
`sd`/`bootcount`/`sdwrite`/`sdread` are not registered. Either way the library and source files are still
built and linked (excluding them from the build itself was judged too risky without a compiler here) - only
whether the task runs changes. The banner reports which are present:

```
Camera code: present
SD card code: absent (WW500_MINIMAL_NO_FATFS)
```

## Settings (`ww500_minimal.h`)

| Define | Default | Meaning |
|---|---|---|
| `WW500_MINIMAL_ALARM_PERIOD_S` | 30 | RTC alarm period for waking from DPD |
| `WW500_MINIMAL_RUN_TIME_COLD_MS` | 3000 | Blinking time after a cold boot |
| `WW500_MINIMAL_RUN_TIME_WARM_MS` | 10000 | Blinking time after a wake |
| `WW500_MINIMAL_TIME_PRINT_PERIOD_MS` | 1000 | How often the time is printed while blinking |
| `WW500_MINIMAL_INACTIVITY_MS` | 1000 | Idle time before DPD |
| `WW500_MINIMAL_INACTIVITY_CLI_MS` | 60000 | Idle time before DPD once a character has been typed |
| `WW500_MINIMAL_DEFAULT_TIME` | `2024-01-01T00:00:00Z` | RTC time set at cold boot |
| `WW500_MINIMAL_SYNC_RTC_AFTER_DPD` | 0 | 1 = the first RTC read after DPD waits about 1 s to synchronise, so "Woke at" is correct, at the cost of 1 s more awake time |

`BLINKY_TASK_PERIOD_MS` (500) is in `blinky_task.h`. Camera switches, all in the files named:

| Define | Default | Where | Meaning |
|---|---|---|---|
| `CAPTURE_TIME_ON_RED_LED` | 1 | `image_task.c` | Red LED lit from stream on to frame (timing marker) |
| `SENSOR_I2C_SPEED` | 100 kHz (HM0360), 400 kHz (RP3) | `image_task.c` | Sensor I2C bus speed |
| `PRINT_REGISTERS_BEFORE_DPD` | 0 | `image_task.c` | HM0360: dump its registers before DPD |
| `CIS_CONTEXT_B_TIMING`, `CIS_CONTEXT_B_OUTPUT` | 0, 0 | `cis_sensor/cis_hm0360/cisdp_cfg.h` | HM0360 context B timing experiment (parked) |
| `CISDP_DBG_TYPE` | `DBG_MORE_INFO` | `cis_sensor/cis_imx708/cisdp_sensor.c` | RP3: which console messages print (failures only) |

## CLI commands

Type `help` at the console for the list. Commands with their parameters:

| Command | Effect |
|---|---|
| `ver` | Board name and build time |
| `ps` | FreeRTOS task table |
| `states` | Internal state of each task |
| `getutc` | RTC time as an ISO 8601 string |
| `setutc <YYYY-MM-DDTHH:MM:SSZ>` | Set the RTC (takes 1-2 s and suppresses interrupts for about 1.4 s) |
| `wake <seconds>` | RTC alarm period for waking from DPD (1-65535). Default `WW500_MINIMAL_ALARM_PERIOD_S` |
| `awake <seconds>` | Blinking run time, measured from when blinking started. Defaults `WW500_MINIMAL_RUN_TIME_COLD_MS` / `WW500_MINIMAL_RUN_TIME_WARM_MS` |
| `blink <ms\|off>` | Start blinking with a period (50-10000 ms), or stop; once stopped, DPD follows. Default period `BLINKY_TASK_PERIOD_MS` |
| `timeprint <seconds>` | Time print period while blinking; 0 turns it off. Default `WW500_MINIMAL_TIME_PRINT_PERIOD_MS` |
| `led <red\|blue> <0\|1>` | Set an LED directly (use `blink off` first) |
| `inactivity <seconds>` | Idle time before DPD after a character is typed. Default `WW500_MINIMAL_INACTIVITY_CLI_MS`; the idle time with no typing is `WW500_MINIMAL_INACTIVITY_MS` |
| `idle` | Power diagnostic: measures the idle loop for 2 s to show whether tickless idle is sleeping the CPU. Use after `blink off` |
| `clocks` | Power diagnostic: prints the clock frequencies, which clock enables are set, the RTC's clock source and the RC32K1K trim value |
| `rc32ktrim [0-255]` | EXPERIMENT: reads or sets the RC32K1K trim register that clocks the RTC and sleep timers, to try to correct the 4 % error (see "RTC accuracy and the 32.768 kHz crystal") |
| `clkoff <image\|hsc\|flash\|lsc\|sb\|all>` | EXPERIMENT: switches off a group of unused clock enables to see what they cost (see `power_diag.c` for what is in each group). `all` = image + hsc + lsc + sb |
| `clkon` | Restores the clocks switched off by `clkoff` (a reset or DPD wake also restores them) |
| `clkslow <rc\|xtal>` | EXPERIMENT: runs the CPU and buses from the 24 MHz RC oscillator or crystal instead of the 400 MHz PLL. The FreeRTOS tick is retuned so time stays correct, but on the RC oscillator it is about 1 % slow. Use `rc` (it is what the application note wants before sleeping) |
| `clkpll <0\|1>` | EXPERIMENT: switches the PLL off (refused unless `clkslow` is in force) or on |
| `clkuart <rc\|xtal>` | EXPERIMENT: moves the console UART's reference clock (normally the 24 MHz crystal) to the RC oscillator, so the crystal can be switched off without losing the console |
| `clkdiv <1-16>` | EXPERIMENT: divides the slow clock further (only after `clkslow`). 24 MHz divided by 16 is 1.5 MHz |
| `clkfast` | Returns the clock speed, PLL, crystals and UART clock to normal after `clkslow`, `clkpll`, `clkdiv`, `clkuart` and `xtal`. It does **not** restore the clock enables switched off by `clkoff`: use `clkon` |
| `xtal <24\|32> <0\|1>` | EXPERIMENT: switches the 24 MHz or 32.768 kHz crystal oscillator off (0) or on (1). The 24 MHz one is refused while the PLL or CPU clock uses it |
| `sleep <seconds> <0\|1>` | EXPERIMENT: enters Power-down mode with retention off (0) or on (1). Wakes on the timer (`SB_timer_int`) or the WAKE pin as a warm boot. With retention the RAM is kept and the application starts about 10 ms sooner than after a DPD wake; about 1.5 mA while asleep either way. Any clock changes are undone first. The wake by the WAKE pin has not been tested on the fixed build |
| `sd` | Prints the state of the FatFS task, the card (file system type and capacity) and the boot count |
| `bootcount` | Prints the boot count from `BOOTS.TXT` (to change it: `sdwrite BOOTS.TXT 0`) |
| `sdwrite <name> <text>` | Writes the text (the rest of the command line) to an 8.3 file in the root of the SD card, replacing it |
| `sdread <name>` | Reads an 8.3 file from the root of the SD card (up to 127 bytes) and prints it as text |
| `capture` | Takes a picture and saves it as `Bnnnnnnn.JPG` on the SD card. HM0360: mode 2, one frame. RP3: powers up, one frame, powers down |
| `cam [mode\|init]` | HM0360: prints the mode, or sets its resting mode (0-4, 6, 7), or writes its registers again |
| `cam [on\|off\|init]` | RP3: prints whether it is powered, or keeps it powered between pictures (`on`) or not (`off`), or checks it again |
| `context <A\|B>` | Sets the HM0360 register context of the resting mode (default A; `ww500_md` uses B for motion detection). Pictures are always taken in context A |
| `mdint <ms>` | Sets the HM0360 motion detection interval (0 = off, the default; longest about 2000 ms). Non-zero enables the motion detection interrupt, which wakes the processor from DPD on PA0 |
| `dpd` | Stop blinking and enter DPD as soon as possible |
| `reset` | Reset by watchdog (the next boot is a cold boot) |

The values changed by `wake`, `awake`, `blink`, `timeprint` and `inactivity` revert to the
defaults named above after DPD.

To hold the processor awake and idle for an operating-current measurement:
`blink off`, then `inactivity 3600`.

## Structure

| File | Purpose |
|---|---|
| `ww500_minimal.c/.h` | `app_main()`, LED control, wake reason, task creation, watchdog reset |
| `pinmux_cfg.c/.h` | Pin functions: UART, SD card SPI, LEDs (PB9 red, PB11 blue), SENSOR_ENABLE (PB7, always low), PB8 SWDIO, PB10 input |
| `blinky_task.c/.h` | Blinks the LEDs, prints the time, and (`blinky_task_sleepNow()`) enters DPD when the shutdown barrier is satisfied |
| `fatfs_task.c/.h`, `ffconf.h` | The SD card and FatFS task, the boot count, and the FatFS configuration (see "SD card and FatFS") |
| `image_task.c/.h`, `hm0360_md.c/.h`, `hm0360_regs.h`, `cis_sensor/cis_hm0360/` | The camera task, the HM0360 mode control and the sensor and data path code (see "HM0360 camera") |
| `cis_sensor/cis_imx708/` | The RP3 sensor and data path code, used instead of `cis_hm0360` in the RP3 build (see "RP3 camera") |
| `CLI-commands.c/.h` | CLI task, UART receive callback and commands |
| `FreeRTOS_CLI.c/.h` | FreeRTOS+CLI parser (third-party, copied from `ww500_md`) |
| `inactivity.c/.h` | Inactivity detection using the idle hook (from `ww500_md`) |
| `power_diag.c/.h` | Power diagnostics and experiments behind the `idle`, `clocks`, `clkoff`, `clkon`, `clkslow`, `clkpll`, `clkdiv`, `clkuart`, `clkfast` and `xtal` commands |
| `barrier.c/.h` | Calls a function when every task is ready (from `ww500_md`) |
| `sleep_mode.c/.h` | DPD entry, Power-down entry (`sleep_mode_enter_sleep()`, used by the `sleep` command) and wake-reason decoding (from `ww500_md`) |
| `rtc_util.c/.h` | RTC read/set, clocks around DPD, ISO strings, adding seconds |
| `freertos_app.c` | FreeRTOS static-allocation, idle, task-switch and stack overflow hooks |
| `hardfault_handler.c` | Fault handlers that report and halt |
| `printf_x.c/.h` | Console colours and buffer dump |
| `app_msg.h` | Queue messages between tasks |
| `mk/image_gen.mk` | Image generation (RC24M/DPD profile) |

The shared file `EPII_CM55M_APP_S/app/main.c` has a `WW500_MINIMAL` block, like the others.

Source files follow `_Documentation/c_file_format.md`, except the third-party
`FreeRTOS_CLI.c/.h`.

## Changes to transfer to ww500_md

Things found or proven in `ww500_minimal` that should be made in `ww500_md` too. Not yet done there. Each is a change to
shared behaviour, so build and test both camera variants after making it.

**Status (30 September 2026):** items 2 to 5 have been made in `ww500_md` (issue #249, branch `260930_appCommsBugs`,
built and run on the bench with the RP3 build; item 5 only in the RP3 driver). Item 1: SENSOR_ENABLE is now an output, low, in
every `ww500_md` build (2 October 2026); the LED pin changes are not transferred: `ww500_md` never used the blue LED on PB10, and its
unused LED code has since been removed (2 October 2026). See `_Documentation/development reports/2026-09-30_App_Comms_Bugs/README.md`, "Transfer
improvements from `ww500_minimal`".

1. **SENSOR_ENABLE (PB7) an output, low, for both cameras** (27 September 2026). In `ww500_md` PB7 is set up (as GPIO1,
   by `rp_sensor_enable()` in `pinmux_cfg.c`) only in the RP builds (`USE_RP2`/`USE_RP3`). In the HM0360 build it is never
   set: the call in `checkForCameras()` (`ww500_md.c`) is commented out, and `rp_sensor_enable_gpio1_pinmux_cfg()` is
   commented out in `pinmux_init()` and is inside `#if 0`. In `ww500_minimal` it is set in `pinmux_cfg_init()`: GPIO1
   output low, then `SCU_PB7_PINMUX_GPIO1_1`, then low again, at start-up in every build.
   Two things come with it in `ww500_md`:
   - **The blue LED shares GPIO1.** `ledInit()` puts the blue LED on PB10 as GPIO1, and GPIO1 is one signal that can
     appear on PB7 and PB10 (datasheet section 4.5, note 3), so SENSOR_ENABLE and the blue LED would move together. In
     `ww500_minimal` the blue LED moved to PB11 (GPIO2, `SCU_PB11_PINMUX_GPIO2`; not PB8, which has a pull-up), and PB10 is
     set to function 0. That needs the board's LED wire link moved too.
   - **SWD:** PB7 is also SWCLK. Once the app makes it a GPIO, SWD can only connect in the short time between the
     bootloader and the pin set-up (this is why the call in `checkForCameras()` was commented out).

2. **`IMX708_POWERUP_DELAY` from 100 to 10 ms** (28 September 2026, tested in `ww500_minimal` with the RP3). In
   `ww500_md/cis_sensor/cis_imx708/cisdp_cfg.h`. It is the wait after SENSOR_ENABLE goes high before the first I2C access
   (`CIS_POWERUP_DELAY`, used in `cisdp_sensor_init()` and in `checkForCameras()`), so it is paid at every boot and every
   camera power-up in the RP3 build.

3. **`FreeRTOS.h` first among the includes** (28 September 2026). Experience shows the FreeRTOS headers should come
   before the other project and SDK headers (the reason is not known; `FreeRTOSConfig.h` itself includes `WE2_device.h`,
   so putting `FreeRTOS.h` first does not lose the device definitions). Done in `ww500_minimal` in both copies of
   `cisdp_sensor.c` (so `cis_sensor/cis_imx708/` is no longer an unchanged copy of the `ww500_md` folder) and in
   `freertos_app.c`. A check of `ww500_md` (any quoted include before `FreeRTOS.h`) finds:
   - `cis_sensor/cis_hm0360/cisdp_sensor.c`, `cis_sensor/cis_imx219/cisdp_sensor.c`, `cis_sensor/cis_imx708/cisdp_sensor.c`:
     all their project and SDK headers come first.
   - `fatfs_task.c`, `if_task.c`, `image_task.c`, `timer_task.c`: `WE2_device.h`, `WE2_core.h`, `board.h`, `printf_x.h`
     and others come first.
   - `img_correct.c`: `xprintf.h`, `printf_x.h`, `WE2_device.h`. `freertos_app.c`: `WE2_device.h`.
   - `cis_file.c`: only its own header, `cis_file.h`, comes first (check whether that header pulls in SDK headers).

4. **Sensor I2C at 400 kHz in the RP builds** (28 September 2026, tested in `ww500_minimal` with the RP3): the IMX708
   initialisation went from 111 ms to 40 ms, and the whole power-up from 130 ms to 57 ms. `ww500_md` runs the bus at
   100 kHz in every build (`hx_drv_i2cm_init(..., DW_IIC_SPEED_STANDARD)` in `ww500_md.c`), because Himax says the HM0360
   needs it once it is in motion detection mode. In the RP builds the HM0360 is still on the bus for motion detection, so
   the speed would have to be 400 kHz only while the RP camera is being set up, and back to 100 kHz before any HM0360
   access. Check that the HM0360 does not misbehave when it sees 400 kHz traffic addressed to another device.

5. **A per-file filter for `dbg_printf()`** (28 September 2026, in `ww500_minimal`'s RP3 `cisdp_sensor.c`, not yet
   measured). `app/WE2_debug.h`, shared by every app, defines `DBG_LESS` itself, so every `dbg_printf(DBG_LESS_INFO, ...)`
   prints and it cannot be turned off from the build. In `cis_sensor/cis_imx708/cisdp_sensor.c`, `dbg_printf()` is redefined
   after the includes to print only the levels in `CISDP_DBG_TYPE` (0, `DBG_MORE_INFO`, or both). The failure messages were
   changed to `DBG_MORE_INFO` and the default is `DBG_MORE_INFO`, so only they print; the ~37 progress messages are compiled
   out (they were about 6 ms of each RP3 power-up). The comment at the top of the file explains how to choose. For
   `ww500_md`, the same could be done per file (all the `cisdp_sensor.c` copies, `hm0360_md.c`, ...), or once as a build
   switch in `app/WE2_debug.h` (for example `#ifndef DBG_OFF` around its `#define DBG_LESS`), which would touch every app.
