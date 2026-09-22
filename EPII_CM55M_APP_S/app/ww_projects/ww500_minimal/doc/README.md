# ww500_minimal

#### File: README.md
#### Author: Claude (Sonnet 5), reviewed by Charles Palmer
#### 20 September 2026

A minimal FreeRTOS image for the HX6538 (AI processor) on a WW500_C00 board that carries
almost nothing else. It exists so that DPD (sleep) current and operating current can be
measured on their own. It is not a product firmware: it has no camera, SD card, BLE
interface, neural network or firmware-update code.

How the work happened is in
`_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/`. What the power measurements showed, and
what is and is not known, is in [power_investigation.md](power_investigation.md) (start with its short summary).

## Behaviour

1. Power-on or wake: prints a banner and the wake reason, and flashes PB9/PB10.
2. Blinks PB9 and PB10 alternately (`BLINKY_TASK_PERIOD_MS`), printing the RTC time every
   `WW500_MINIMAL_TIME_PRINT_PERIOD_MS`.
3. After the run time (`WW500_MINIMAL_RUN_TIME_COLD_MS` after a cold boot,
   `WW500_MINIMAL_RUN_TIME_WARM_MS` after a wake) the blinking stops, so all tasks are idle.
4. After `WW500_MINIMAL_INACTIVITY_MS` of inactivity the processor prints `Inactive for <n>ms`,
   drives the LEDs low and enters DPD.
5. Wake sources: the WAKE pin (PA0, level high) or the RTC alarm
   (`WW500_MINIMAL_ALARM_PERIOD_S`).

Every boot prints a `Retention check` line: the app keeps a value in the `.noinit` section, which the start-up code does
not clear, and reports whether it survived (`the RAM was kept`, after a `sleep <seconds> 1`) or not (first boot, DPD,
power-cycle, `sleep <seconds> 0`). After a wake it also prints `RTC before synchronising`, which is the time DPD or
Power-down was entered, not the wake time (see `WW500_MINIMAL_SYNC_RTC_AFTER_DPD`).

Any character typed at the console extends the inactivity period to
`WW500_MINIMAL_INACTIVITY_CLI_MS`. This matters after a cold boot: the run time is short, so
type a character to keep the console available. Cold boot sets the RTC to
`WW500_MINIMAL_DEFAULT_TIME`. The RTC keeps time through DPD but not through power loss.

Settings changed at the CLI are lost in DPD and revert to the defaults below.

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
| Blue LED (LED2, R20) | PB10 (U2 pin 1) | U1 pin 23 to U2 pin 1 | GPIO1, active high (1 = on) |
| Green LED (LED3, R40) | not assigned | U1 pin 10 to a pin to be decided | Possible future addition. Not used by this firmware. |
| WAKE switch (SW1) | PA0 | U1 pin 12 to pin 24 | Fitted in place of /BLE_WAKE. Level high wakes from DPD. |

PB9 and PB10 are the PDM_CLK and PDM_DATA pins (pinmux function 1) when not used as GPIO.
GPIO0-GPIO2 signals are also available on PB6-PB8 and the SWD pads. Only one pad should be
assigned to each.

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
- **Initialisation:** at every boot, in the image task. After a cold boot (or any wake that is not the WAKE pin or the RTC) the long register table is written (`cisdp_sensor_init(true)`). After a wake from DPD it is not (`cisdp_sensor_init(false)`): the sensor should have kept its registers, and the mode it was in when the boot began is printed (`Image: HM0360 was in mode N when the boot began`) as the evidence. The data path is set up at every boot, as the HX6538 loses it in DPD. If the sensor does not answer at 0x24 the task says so and the app carries on without it (`states` shows `No camera`).
- **Resting mode:** whenever the sensor is not taking a picture it is in the resting mode, by default mode 2 (`MODE_SW_NFRAMES_SLEEP`, 1 frame, longest sleep interval, motion detection interrupt off). It is left in that mode for DPD. `hm0360_md.c` records about 270 uA for mode 2 against about 700 uA for mode 0: to be checked with a meter. In mode 2 the sensor keeps producing a frame about every 2 s with nobody listening (as in `ww500_md` in DPD).
- **`capture`:** refused if there is no camera, no card, no valid boot count (see "SD card and FatFS") or 100 pictures have been saved in this boot. Otherwise it does what `ww500_md` does for a picture: `cisdp_dp_init()`, `hm0360_md_setMode(MODE_SW_NFRAMES_SLEEP, 1 frame)` with sleep time zero, `cisdp_sensor_start()`. The first bench capture took 33 ms to the frame (the sleep interval does not delay the first frame in mode 2) and 88 ms to write a 9240 byte JPEG; the wait times out after 5 s. On a data path failure the console names the event (for example `EDM WDT2 timeout`) and says whether the sensor's frame counter moved (it did not when the connector soldering was faulty). The JPEG (VGA, `jpg_ratio` 10, no EXIF) is passed unchanged to the FatFS task and saved as `Bnnnnnnn.JPG`: `B`, then the boot count modulo 100000 in 5 digits, then the number of the picture in this boot in 2 digits (e.g. boot 1203, picture 3 is `B0120303.JPG`). The sensor is put back in the resting mode before the write starts.
- **`cam`:** with no parameter prints the mode the sensor is in, its model ID and its frame counter (0xFFFF until it has output a frame); `cam <0-4|6|7>` sets the resting mode so that its current can be measured; `cam init` writes the register table again.
- **DPD:** the image task is in the shutdown barrier with the blinky and FatFS tasks; DPD is not entered while a picture is being taken or written.
- **Caution:** `clkoff image` and `clkoff hsc` (see `power_investigation.md`) switch off the data path, JPEG and xDMA clocks, so a `capture` after them times out. Use `clkon` first.

Files added: `image_task.c/.h`, `hm0360_md.c/.h` and `hm0360_regs.h` (copied from `ww500_md`; the only change is in `hm0360_md.c`: the unused `fatfs_task.h` include is removed and the register table include path corrected), and `cis_sensor/cis_hm0360/` (an unchanged copy of the `ww500_md` folder). `ww500_minimal.mk` adds `sensordp` to `LIB_SEL`, sets `CIS_SUPPORT_INAPP = cis_sensor` and `CIS_SUPPORT_INAPP_MODEL = cis_hm0360`, and defines `USE_HM0360`.

## Building

Select the app in `EPII_CM55M_APP_S/app/ww_projects/ww.mk` (uncomment one `APP_TYPE`):

```
APP_TYPE = ww500_minimal
```

Then build as described in `_Documentation/building_firmware.md`. There is no camera
variant, so no `CIS_SUPPORT_INAPP_MODEL` and no `make clean` between variants. The build
generates the flashable image, named `M<YMDDHMM>.IMG` (same scheme as the other apps; the
leading letter is `M` rather than a camera variant). Flash and recover as in
`_Documentation/firmware_update_and_recovery.md`.

After changing a `#define` in a header (such as those in `ww500_minimal.h`), run `make clean`
before building. On 20 September 2026 an incremental build kept an old value in an object file:
only `ww500_minimal.c` is force-rebuilt, so a file that merely uses the changed define was not
recompiled.

Remember to set `APP_TYPE` back to `ww500_md` before building the production firmware. CI
builds `ww500_md` only and does not build this app.

`ww500_minimal.mk` uses the GNU toolchain only (there is no `.sct` for armclang) and links
only the `pwrmgmt` library, with no middleware.

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

`BLINKY_TASK_PERIOD_MS` (500) is in `blinky_task.h`.

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
| `led <9\|10> <0\|1>` | Set an LED directly (use `blink off` first) |
| `inactivity <seconds>` | Idle time before DPD after a character is typed. Default `WW500_MINIMAL_INACTIVITY_CLI_MS`; the idle time with no typing is `WW500_MINIMAL_INACTIVITY_MS` |
| `idle` | Power diagnostic: measures the idle loop for 2 s to show whether tickless idle is sleeping the CPU. Use after `blink off` |
| `clocks` | Power diagnostic: prints the clock frequencies and which clock enables are set |
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
| `capture` | Takes a picture with the HM0360 (mode 2, one frame) and saves it as `Bnnnnnnn.JPG` on the SD card |
| `cam [mode\|init]` | Prints the HM0360 mode, or sets its resting mode (0-4, 6, 7), or writes its registers again |
| `dpd` | Stop blinking and enter DPD as soon as possible |
| `reset` | Reset by watchdog (the next boot is a cold boot) |

The values changed by `wake`, `awake`, `blink`, `timeprint` and `inactivity` revert to the
defaults named above after DPD.

To hold the processor awake and idle for an operating-current measurement:
`blink off`, then `inactivity 3600`.

## Structure

| File | Purpose |
|---|---|
| `ww500_minimal.c/.h` | `app_main()`, pin set-up, wake reason, task creation, watchdog reset |
| `blinky_task.c/.h` | Blinks the LEDs, prints the time, and (`blinky_task_sleepNow()`) enters DPD when the shutdown barrier is satisfied |
| `fatfs_task.c/.h`, `ffconf.h` | The SD card and FatFS task, the boot count, and the FatFS configuration (see "SD card and FatFS") |
| `image_task.c/.h`, `hm0360_md.c/.h`, `hm0360_regs.h`, `cis_sensor/cis_hm0360/` | The HM0360 camera task, its mode control and the sensor and data path code (see "HM0360 camera") |
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
