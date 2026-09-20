# Proposal: `ww500_minimal` — minimal FreeRTOS image for HX6538 power measurement

#### File: CLAUDE_Minimal_FreeRTOS_proposal.md
#### Companion to: CLAUDE_Minimal_FreeRTOS.md (the task brief)
#### Date: 20 September 2026
#### Status: DRAFT for review — no code has been written

## 1. Goal and decisions so far

An image for a WW500_C00 board fitted with the HX6538 and almost nothing else, so that
sleep (DPD) and operating current can be measured on their own.

Agreed in conversation:

| Item | Decision |
|---|---|
| Wake sources | Power-on, RTC timer, and the WAKE signal on PA0 (level high). No HM0360 interrupt. |
| LEDs | PB9 and PB10, **active high** (1 = on). Hardware to be wired by Charles. |
| CLI | Yes, on console UART0, 921600 baud, as in `ww500_md`. |
| Variants | One image. No camera variants (`CIS_SUPPORT_INAPP` unset). HM0360 may return later. |
| Excluded | Camera, FatFS/SD, BLE interface (I2C slave), neural network, PCA9574/LED flash, XIP flash manager. |

## 2. Behaviour

```
Power-on / wake ──> banner, wake reason ──> blinky + CLI running
                                             │
                          run window expires (default 10 s cold, 3 s after a wake)
                          CLI activity extends it to 60 s (as in ww500_md)
                                             │
                                             ▼
                        LEDs off, UART drained, enter DPD
                        wake on: PA0 high | RTC alarm (default 30 s)
```

Cold boot is recognised as in `ww500_md.c` (both PMU wakeup registers read zero) and gets
a distinctive LED pattern. Warm boot prints the wake reason (`WAKE signal` / `RTC Timer`).

Because the aim is measurement, the run window and RTC alarm period are runtime settings
(CLI), not compile-time only. There is no SD card to hold `CONFIG.TXT`, and DPD loses RAM,
so **settings revert to defaults after every DPD cycle** unless we choose to keep them
somewhere (see question Q6).

## 3. Files

New folder: `EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/`

| File | Origin | Purpose |
|---|---|---|
| `ww500_minimal.c/.h` | Adapted from `ww500_md.c/.h` | `app_main()`, pin setup, wake-reason logic, task creation |
| `ww500_minimal.mk` | Adapted from `ww500_md.mk` | Build config; see §4 |
| `ww500_minimal.ld` | Copy of `ww500_md.ld` | Identical memory map (checked: `ww500_md_test_1.ld` differs only in comments). Not shrinking SRAM use; that gains nothing here. |
| `mk/image_gen.mk` | Copy of `ww500_md/mk/image_gen.mk`, no camera-variant naming | Image generation (RC24M/DPD profile) |
| `blinky_task.c/.h` | New, small | Alternates PB9/PB10 |
| `power_task.c/.h` | New, small | Owns the run window; enters DPD (see §5) |
| `CLI-commands.c/.h` | Cut down from `ww500_md` | CLI task and a short command list |
| `FreeRTOS_CLI.c/.h` | Verbatim | FreeRTOS+CLI parser |
| `freertos_app.c` | Trimmed | Static-allocation hooks; stack overflow hook |
| `hardfault_handler.c` | Verbatim | |
| `sleep_mode.c/.h` | Trimmed | Keep `sleep_mode_enter_dpd()` and `sleep_mode_print_event()`. Drop `sleep_mode_enter_sleep()` (PD mode, unused). |
| `pinmux_cfg.c/.h` | Trimmed | UART0 only, plus a new PA0/PB9/PB10 setup |
| `printf_x.c/.h` | Verbatim | Console colours |
| `app_msg.h` | Cut down | Only the CLI/power events we use (the original is mostly DP/camera events) |
| `rtc_util.c/.h` | **Decision needed (Q3)** | RTC read/set/alarm helpers |

Everything else in `ww500_md` (about 20 files: image, fatfs, if_task, xip, hm0360, ledFlash,
lightSensor, pca9574, cis_*, cvapp, exif_*, sw_jpeg, camera_switch, fileRx, directory_manager,
CLI-FATFS, preview, ae, img_correct, crc16, memory_manage, selfTest, timer_task) is **not
copied**.

Each task keeps the `ww500_md` model: `xxx_createTask(priority, wakeReason)`,
`xxx_getState()`, `xxx_getStateString()`, registered in `internalStates[]` so the `states`
CLI command still works.

## 4. Build changes

1. **`ww.mk`**: add `#APP_TYPE = ww500_minimal` beside the others. Because `APP_TYPE` is
   assigned with `=`, a command-line `make APP_TYPE=ww500_minimal` also overrides it, so the
   committed default can stay `ww500_md` and CI is unaffected. (Q8.)
2. **`ww500_minimal.mk`** is the main work. Differences from `ww500_md.mk`:
   - `APPL_DEFINES += -DWW500_MINIMAL` (matching the `APP_TYPE` line, as the existing comment requires).
   - `LIB_SEL`: drop `tflmtag2412_u55tag2411`, `sensordp`, `spi_ptl`, `spi_eeprom`, `i2c_comm`. Start from `pwrmgmt` alone and add back only if the link fails.
   - `MID_SEL =` (empty): no FatFS, so no `FATFS_PORT_LIST` / `CMSIS_DRIVERS_LIST = SPI`.
   - No `CIS_SUPPORT_INAPP*` block, no `USE_HM0360*` / `USE_RP*` / `CIS_IMX` defines.
   - `WW500_FAST_LIBS` prebuilt-library switch becomes moot without TFLM/CMSIS-NN; keep the block unless it causes trouble.
   - Keep: git-info defines, force-rebuild of `__TIME__`/`__DATE__`, `stage_elf`, `OUT_DIR_ROOT` workaround, `OS_SEL := freertos_10_5_1`, TrustZone secure settings, `EPII_USECASE_SEL`.
3. **`drv_user_defined.mk`**: `ww500_md` has one that lists all driver IPs. Trimming it would
   remove unused drivers, but it risks link errors against SDK code that assumes them. Proposal:
   **copy unchanged first, prune later** as a measured optimisation.
4. **CI**: no change now. When the image is proven, add a `ww500_minimal` build job.

`ww.mk` includes every `*.mk` sitting directly in the project folder, which is why
`image_gen.mk` lives in `mk/`. The same layout is kept.

## 5. Design of the two new tasks

**`power_task`** replaces the `inactivity.c` + idle-hook + `barrier.c` machinery of
`ww500_md`. That machinery exists so DPD is entered only once *every* task is idle. Here the
blinky task is never idle by design, so "idle detection" is the wrong test. A single task
that sleeps for the run window (extended by `power_extendRunWindow()` from the CLI) and then
calls `sleep_mode_enter_dpd()` is simpler, and touches no shared FreeRTOS config. See Q4.

Sequence before DPD: stop blinky, both LEDs low, wait for the UART transmitter to drain,
call `sleep_mode_enter_dpd(WAKE_PIN | RTC, alarmSeconds, false)`.

**`blinky_task`**: alternates PB9/PB10 at a configurable rate (default 500 ms). A `blink
off` CLI command lets you measure operating current with the LEDs dark.

## 6. Reuse and what I deliberately left out (power review)

Items in `ww500_md` that would add current or wake things, and are excluded:

| `ww500_md` item | Why excluded |
|---|---|
| `hx_drv_i2cm_init(...)` in `app_main` and `checkForCameras()` | I2C master to PCA9574/HM0360/camera; sensor-enable pin toggling; a 100 ms+ power-up delay |
| `i2cs0_pinmux_cfg`, `aon_gpio0_interrupt_init` in `if_task.c` | BLE link. PA0 is set up only inside `sleep_mode_enter_dpd()` |
| `spi_m_pinmux_cfg` | SD card / inter-board SPI |
| `xip_manager_preinit()` and flash SPI mutex | Only needed to update firmware from SD; no SD here |
| Inactivity idle-hook and `vApplicationTaskSwitchedIn` | Runs on every task switch; see §5 |
| `selfTest_*` | Reports camera/SD/flash faults that cannot exist here |
| `exif_utc_init` bookkeeping (`timeHasBeenSet`, EXIF strings) | Image metadata |

Kept and worth a second look for power:

- **UART0 receive interrupt**: only wakes the core if a character arrives. Fine.
- **Console transmit**: chatty logging costs current. Add a `quiet` CLI toggle so operating-current measurements are not dominated by printing.

## 7. CLI command set

`help` (built in), `ver`, `tasks` (`prvTaskStatsCommand`), `states`, `dpd` (enter DPD now),
`reset`, `getutc`, `setutc`, `wake <seconds>` (RTC alarm period), `awake <seconds>` (run
window), `blink <ms|off>`, `led <b9|b10> <0|1>`, `quiet <0|1>`, `int` (PA0 level), `assert`.

Not included: everything touching camera, FatFS, model, op-params, GPS/deployment ID, flash
LED, slots, firmware update, preview.

## 8. Risks and things I have not yet verified

- **`platform_driver_init()` / `board_init()` run before `app_main()`** and I have not read
  them. They may initialise peripherals (I2C, SPI, clocks) that draw current regardless of
  this app. If so, reducing them means editing shared SDK code, which affects `ww500_md`,
  so I would stop and ask.
- **`FreeRTOSConfig.h` is shared** (`os/freertos_10_5_1/TZ_Sec/config/`). My grep for
  `configUSE_TICKLESS_IDLE` / `configUSE_IDLE_HOOK` returned nothing there, so those come from
  elsewhere and I need to find where. Tickless idle matters directly for operating current,
  and this project must not change the shared config.
- **Linker-script/section dependencies**: I expect an unmodified `.ld` to work, but a link
  failure after dropping libraries is the most likely first build problem.
- **Boot and slot model**: the image will be burned via X-Modem/SWD like any other. Without
  `xip_manager`, nothing labels the slot on boot, so `slots` on a later `ww500_md` image
  will show `unknown`. I believe this is harmless (§4 of `SKILL.md` says it self-heals),
  but I have not confirmed the bootloader itself never needs the label.
- **DPD pin state**: I do not know whether PB9/PB10 hold their driven level through DPD.
  Driving them low first avoids the question for LED current, but check on the bench.
- **Reset**: `ww500_md` implements deliberate reset as a deferred watchdog reset executed in
  `image_sleepNow()`. The minimal image has no `image_sleepNow()`, so `reset` must start the
  watchdog directly (Q5).

## 9. Questions for you

- **Q1. RTC alarm default.** 30 s is my suggestion for convenient measurement. Longer, or should the very first 
wake after cold boot be different?
- **Q2. Run window defaults.** 10 s cold, 3 s after a wake, 60 s after CLI activity. OK?
- **Q3. RTC helper.** `exif_utc.c` is what `sleep_mode.c` calls for time and alarm maths, but it also holds `get_fattime()` 
(needs `ff.h`), EXIF string formats and GPS-era bookkeeping. I recommend a small new `rtc_util.c` (about 100 lines) 
with just read, set, add-seconds and a string formatter, instead of dragging FatFS headers in. Agree, or would you 
rather keep `exif_utc.c` for consistency?
- **Q4. Inactivity.** Agree that `power_task` replaces `inactivity.c` and `barrier.c`? The alternative is reusing them as-is, 
at the cost of the idle hook and a task-switch hook running in a build that has nothing to be idle about.
- **Q5. Reset.** Keep a `reset` command (direct watchdog reset), or omit it?
- **Q6. Settings across DPD.** Should `wake`/`awake`/`blink` values survive DPD? RAM is lost. Options: (a) revert to defaults 
each cycle (simplest, my default), (b) keep in AON retention registers (there is a scratch word used by `sleep_mode_enter_sleep` 
for a boot counter, so retention is possible), (c) compile-time only. I recommend (a).
- **Q7. Boot counter and time.** Cold boot sets the RTC to a fixed date as `ww500_md` does. Fine for measurement?
- **Q8. `ww.mk`.** Leave the committed default as `ww500_md` (build with `make APP_TYPE=ww500_minimal`), or switch the default 
while you work on this? Every switch means a dirty `ww.mk` on the branch.
- **Q9. Branch.** You are on `minimalFreeRTOS`. Is that the branch for this work? Per project rules I will not commit or push 
without asking, and I will ask before running `make`.

## 10. Answers from Charles

- **Q1.** OK for now. Make is a #define.
- **Q2.** OK for now. Make them #define.
- **Q3.** RTC helper is OK. The HX6538 gets its time from the BLE processor, which is missing. I might be interested in
using this build to test RTC accuracy, so you could start with a default ime at reset (say 1/1/2024, midnight) and 
allow for time and date to be set by CLI, and printed periodiacally.
- **Q4.** I think the existing inactivity mechanism is working and worth preserving. Perhaps we can cause inactivity by
a mechanism to stop the blinky task - e.g. after a set time or by CLI command.
- **Q5.** a WDT reset could be useful. Retain.
- **Q6.** (a)
- **Q7.** OK - see Q3 above.
- **Q8.** switch project using the ww.mk model currently in use. For consistency and to prevent confusion in the future.
- **Q9.** Yes. I am unsure whether you can run teh compiler, but anyway it is easy fro me, so let me run make and the compiler, for now.
 
## 11. Proposed order of work (after your review)

1. Skeleton: folder, `.mk`/`.ld`/`image_gen.mk`, `ww.mk` entry, `app_main()` that prints the banner and wake reason. Build to prove the trimmed library list links.
2. `blinky_task` and pin setup; bench-check the LEDs.
3. `power_task` and trimmed `sleep_mode.c`; verify DPD entry and both wake sources (PA0 high, RTC).
4. CLI with the reduced command set.
5. Measure: first sleep current, then operating current with LEDs off and quiet console; record in this thread with logs.
6. Durable doc for the new app (`ww500_minimal/doc/`), and a thread README with Status/Outcome/Open items per `development reports/README.md`.

Steps 1 to 4 each end in a build (asking you first) and a bench check before the next starts.

## 12. Step 1 implementation notes (20 September 2026)

Written after the answers in §10, and following `_Documentation/c_file_format.md`.

Changes from the proposal:

- **Inactivity kept (Q4).** `inactivity.c/.h`, `barrier.c/.h` and `freertos_app.c` are ported, not replaced by a `power_task`. The blinky task will stop after `WW500_MINIMAL_RUN_TIME_*_MS`; the inactivity mechanism then sees idle time and calls `ww500_minimal_onInactivity()`. `inactivity_reset()` and the untested `USETIMER` alternative were left behind.
- **`pinmux_cfg.c/.h` dropped.** With only UART0 to configure, the pin setup is in `ww500_minimal.c` (`initPins()`), alongside the LEDs.
- **`app_msg.h` not created yet.** It is added in the step that first needs a queue.
- **`printf_x_test()` and the dead-code blocks** in the ported files were left behind (the format rules say to remove commented-out code).
- **`sleep_mode.c`** keeps `sleep_mode_print_event()` and `sleep_mode_enter_dpd()` only; it calls `rtc_util` instead of `exif_utc`.
- **`main.c` (shared SDK file) needed a small additive change:** a `#ifdef WW500_MINIMAL` block, identical to the `WW500_MD` one, which includes `ww500_minimal.h` and calls `app_main()`. `ww500_md` builds are unaffected.
- **`ww.mk`:** `APP_TYPE = ww500_minimal` is now active and `ww500_md` is commented out (Q8). Switch it back to build `ww500_md`.
- **Image name:** `mk/image_gen.mk` names the device copy `M<YMDDHMM>.IMG` (no camera variant letter).
- **Constants** requested as `#define`s (Q1, Q2) are in `ww500_minimal.h`.

Not yet written: `blinky_task`, CLI, `app_msg.h`, the periodic time print and the CLI time commands (Q3).
Not yet built or run: nothing here has been compiled.

## 13. Steps 2 and 3 notes (20 September 2026)

- **Step 2 built and run:** the LEDs did not light at first because of a wiring error, which was fixed. PB9 (GPIO0) and PB10 (GPIO1) work as outputs. The temporary pin diagnostic was removed afterwards.
- **DPD entry (proposal step 3) was folded into step 2:** `blinky_task` enters DPD when the inactivity mechanism reports inactivity (Q4).
- **Step 4, the CLI, is written and not yet built.** `CLI-commands.c/.h` (prefix `cli_`), plus `FreeRTOS_CLI.c/.h` copied from `ww500_md` unchanged except for the include that provides `configCOMMAND_INT_MAX_OUTPUT_SIZE`. The FreeRTOS+CLI files are third-party and were **not** reformatted to `c_file_format.md`; this is a deliberate exception, and can be reversed if you want them reformatted.
- **Commands:** `help`, `ver`, `ps`, `states`, `getutc`, `setutc`, `wake <s>`, `awake <s>`, `blink <ms|off>`, `timeprint <s>`, `led <9|10> <0|1>`, `inactivity <s>`, `dpd`, `reset` (watchdog, Q5).
- **Differences from §7:** `int` and `assert` were not ported (no PA0 interrupt code exists in this build; `assert` is a `ww500_md` development aid). `quiet` was replaced by `timeprint 0`, because the only periodic printing is the time print.
- **Time printing (Q3):** the blinky task prints the RTC time every `WW500_MINIMAL_TIME_PRINT_PERIOD_MS` (5 s) while blinking. It stops when blinking stops, because a task that printed while otherwise idle would prevent inactivity and DPD.
- **Holding the board awake for a current measurement:** `blink off` then `inactivity <seconds>` (DPD follows that long after the last idle moment; typing extends it). To hold it awake with the LEDs blinking, use `awake <seconds>`.
- **Settings revert after DPD** (Q6 option a), as agreed.
