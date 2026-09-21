# Minimal FreeRTOS image (`ww500_minimal`) for HX6538 power measurement

#### File: README.md
#### Author: Claude (Sonnet 5), reviewed by Charles Palmer
#### 20 September 2026

## Status

**Experiments complete and documented (21 September 2026); open items remain.** The firmware is built and working on the bench, the
power measurements it was written for have been made, and what was learnt is in the power document. No GitHub issues have been filed yet for the open
items below. Nothing has been committed or pushed.

## Outcome

- `ww500_minimal` is a new app (`EPII_CM55M_APP_S/app/ww_projects/ww500_minimal`) for a WW500_C00 board carrying only the HX6538. It has no camera, FatFS, BLE
  interface, neural network or flash manager, so that sleep (DPD) current and operating current can be measured on their own, as was done for the BLE processor.
- It boots, blinks two LEDs (PB9, PB10), runs a console CLI, and enters DPD through the same inactivity mechanism as `ww500_md`. Wake sources are power-on, the RTC alarm
  and the WAKE pin (PA0). How it works is in
  [`ww500_minimal/doc/README.md`](../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/README.md).

**What the power measurements showed** (short version; the full record, the known/unknown lists and the evidence are in
[`power_investigation.md`](../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md)):

- **DPD: 10 uA**, and it resumes on the timer and the WAKE pin. After a wake, the first bootloader line to a running CLI task takes 24 to 30 ms.
- **Awake and idle it drew 17.3 to 17.9 mA**, although FreeRTOS tickless idle works. WFI stops only the CPU clock; the PLL, buses, SRAMs and peripheral clocks stay on.
- **Run-time changes cut that to 4.8 mA (72 %)**: 24 MHz RC oscillator, PLL off, unused clock enables off, UART moved to the RC oscillator and the crystal off. It is reversible, the
  console still works and DPD still works from that state. Below 24 MHz gains little more. About 4 to 5 mA is a static floor.
- **Under 1 mA is not available while running code.** The datasheet offers sub-mA only in Power-down with retention and DPD.
- **Power-down with retention works** (after making it wake on the RC oscillator, as the application note asks): 1.5 mA asleep, RAM kept, and the application starts about
  10 ms sooner than after a DPD wake. The full wake latency was not measured.
- **The RTC and sleep timers are about 4.1 % fast** (32 kHz RC oscillator, no crystal): a 30 s alarm is about 28.8 s.
- **DPD is the right low-power state** on what we know. Power-down with retention is a latency option at 150 times DPD's sleep current.

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

## Open items

No GitHub issue has been filed for these. File them with the `review-finding` template if wanted (ask Charles first).

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
- Optionally reintroducing the HM0360 later (Charles).

## Files in this thread

- [`CLAUDE_Minimal_FreeRTOS.md`](CLAUDE_Minimal_FreeRTOS.md): the task brief
- [`CLAUDE_Minimal_FreeRTOS_proposal.md`](CLAUDE_Minimal_FreeRTOS_proposal.md): the proposal, Charles's answers (section 10) and the implementation notes (sections 12 and 13)
- [`power_investigation.md`](../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md): the power findings, with the short summary and the lists of what is
  and is not known
- Bench logs (Tera Term, host timestamps): `part_b_log.txt`, `part_b_run1_log.txt` (truncated), `part_b_run2_log.txt`, `part_b_slow_24MHz_log.txt`, `part_b_fast_400MHz_log.txt`
  (FreeRTOS tick and RTC accuracy), `partd_log.txt` (DPD from the slow state), `parte_1_retention1_wake_hung_log.txt`, `parte_2_retention1_wake_ok_log.txt` and
  `parte_3_retention_tests_log.txt` (Power-down with retention)
