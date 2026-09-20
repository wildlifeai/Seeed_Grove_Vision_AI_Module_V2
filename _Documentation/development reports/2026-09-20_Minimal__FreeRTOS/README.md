# Minimal FreeRTOS image (`ww500_minimal`) for HX6538 power measurement

#### File: README.md
#### Author: Claude (Sonnet 5), reviewed by Charles Palmer
#### 20 September 2026

## Status

**Open.** The firmware is built and working on the bench. The power measurements it was
written for have not been made yet.

## Outcome

So far:

- `ww500_minimal` is a new app (`EPII_CM55M_APP_S/app/ww_projects/ww500_minimal`) for a
  WW500_C00 board carrying only the HX6538. It has no camera, FatFS, BLE interface, neural
  network or flash manager, so that sleep (DPD) current and operating current can be
  measured on their own, as was done for the BLE processor.
- It boots, blinks two LEDs (PB9, PB10), runs a console CLI, and enters DPD through the same
  inactivity mechanism as `ww500_md`. Wake sources are power-on, the RTC alarm and the WAKE
  pin (PA0). Bench-verified on 20 September 2026: cold boot, the CLI, DPD entry, and wake by
  the WAKE pin (switched by hand in place of the missing BLE processor).
- How it works now is in
  [`ww500_minimal/doc/README.md`](../../EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/README.md).

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

- Sleep and operating current measurements, with results and the method recorded in this
  thread. Not yet done. No GitHub issue has been filed for them: propose filing one with the
  `review-finding` template if you want it on the project board.
- Whether `platform_driver_init()` / `board_init()` (which run before `app_main()` and are
  shared with `ww500_md`) leave peripherals running that add current. Unverified; it would
  only matter if the measurements are higher than expected.
- Optionally reintroducing the HM0360 later, after the first measurements (Charles).

## Files in this thread

- [`CLAUDE_Minimal_FreeRTOS.md`](CLAUDE_Minimal_FreeRTOS.md): the task brief
- [`CLAUDE_Minimal_FreeRTOS_proposal.md`](CLAUDE_Minimal_FreeRTOS_proposal.md): the proposal,
  Charles's answers (section 10) and the implementation notes (sections 12 and 13)
