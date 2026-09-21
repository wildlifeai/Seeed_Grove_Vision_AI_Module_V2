# ww500_minimal

#### File: README.md
#### Author: Claude (Sonnet 5), reviewed by Charles Palmer
#### 20 September 2026

A minimal FreeRTOS image for the HX6538 (AI processor) on a WW500_C00 board that carries
almost nothing else. It exists so that DPD (sleep) current and operating current can be
measured on their own. It is not a product firmware: it has no camera, SD card, BLE
interface, neural network or firmware-update code.

How the work happened is in
`_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/`.

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
| Red LED (LED1, R22) | PB9 (U2 pin 4) | U1 pin 21 to U2 pin 4 | GPIO0, active high (1 = on) |
| Blue LED (LED2, R20) | PB10 (U2 pin 1) | U1 pin 23 to U2 pin 1 | GPIO1, active high (1 = on) |
| Green LED (LED3, R40) | not assigned | U1 pin 10 to a pin to be decided | Possible future addition. Not used by this firmware. |
| WAKE switch (SW1) | PA0 | U1 pin 12 to pin 24 | Fitted in place of /BLE_WAKE. Level high wakes from DPD. |

PB9 and PB10 are the PDM_CLK and PDM_DATA pins (pinmux function 1) when not used as GPIO.
GPIO0-GPIO2 signals are also available on PB6-PB8 and the SWD pads. Only one pad should be
assigned to each.

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
| `clkslow <rc\|xtal>` | EXPERIMENT: runs the CPU and buses from the 24 MHz RC oscillator or crystal instead of the 400 MHz PLL. The FreeRTOS tick is retuned so time stays correct |
| `clkpll <0\|1>` | EXPERIMENT: switches the PLL off (refused unless `clkslow` is in force) or on |
| `clkuart <rc\|xtal>` | EXPERIMENT: moves the console UART's reference clock (normally the 24 MHz crystal) to the RC oscillator, so the crystal can be switched off without losing the console |
| `clkdiv <1-16>` | EXPERIMENT: divides the slow clock further (only after `clkslow`). 24 MHz divided by 16 is 1.5 MHz |
| `clkfast` | Returns the clocks to normal after `clkslow` and `clkpll` |
| `xtal <24\|32> <0\|1>` | EXPERIMENT: switches the 24 MHz or 32.768 kHz crystal oscillator off (0) or on (1). The 24 MHz one is refused while the PLL or CPU clock uses it |
| `sleep <seconds> <0\|1>` | EXPERIMENT: enters Power-down mode with retention off (0) or on (1). Wakes on the timer or the WAKE pin as a warm boot; with retention it should not reload from flash |
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
| `blinky_task.c/.h` | Blinks the LEDs, prints the time, owns entry to DPD |
| `CLI-commands.c/.h` | CLI task, UART receive callback and commands |
| `FreeRTOS_CLI.c/.h` | FreeRTOS+CLI parser (third-party, copied from `ww500_md`) |
| `inactivity.c/.h` | Inactivity detection using the idle hook (from `ww500_md`) |
| `power_diag.c/.h` | Power diagnostics for the `idle`, `clocks`, `clkoff`, `clkon`, `clkslow`, `clkpll` and `clkfast` commands |
| `barrier.c/.h` | Calls a function when every task is ready (from `ww500_md`) |
| `sleep_mode.c/.h` | DPD entry and wake-reason decoding (from `ww500_md`) |
| `rtc_util.c/.h` | RTC read/set, clocks around DPD, ISO strings, adding seconds |
| `freertos_app.c` | FreeRTOS static-allocation, idle, task-switch and stack overflow hooks |
| `hardfault_handler.c` | Fault handlers that report and halt |
| `printf_x.c/.h` | Console colours and buffer dump |
| `app_msg.h` | Queue messages between tasks |
| `mk/image_gen.mk` | Image generation (RC24M/DPD profile) |

The shared file `EPII_CM55M_APP_S/app/main.c` has a `WW500_MINIMAL` block, like the others.

Source files follow `_Documentation/c_file_format.md`, except the third-party
`FreeRTOS_CLI.c/.h`.
