# Re-bench of the Todo issues, 2 October 2026

Before the Todo issues go to Charles, each bug was run again on current `dev`. All five still
reproduce. #246 (a decision) and #251 (a design task) were not benched. The BLE side of the same
session, ww-hardware #33, #34, #35, #36, #52 and #58, is in ww-hardware under
`_Documentation/development reports/2026-10-02_todo_rebench/`.

**Setup:** WILD-KOMB, nRF 0.30.51. Both Himax images built from `dev` 5750f8b5 (the same firmware
as `main` 0ecbd6a5) and flashed over X-Modem; the board went back to the 23 September production
images afterwards. The SD card started empty, so settings are the defaults except where a log
shows `setop`. Stamps are the bench PC's clock (NZDT); the device keeps UTC.

| Issue | Result | Log |
|---|---|---|
| #152 Watchdog reset rewinds the clock | Reproduces. Clock at `2026-10-02T01:25:50Z`, then `reset` + `dpd`: the board announces `Wake 2024-01-01T00:00:02Z`. | [152](152_rtc_reset_by_watchdog.txt), [after an update](152_after_firmware_update.txt) |
| #153 EXIF Model on RP3 photos | Reproduces. A photo taken by the RP3 image (Software `WW500_C02 14:06:13`) says Model `WW500 HM0360`. | [153](153_exif_model_rp3.txt), [photo](ABF08401.JPG) |
| #158 AE lost across DPD | Reproduces. Four wakes in lamp light all start from 2368 lines; the first frame of each reads p75 156 to 236 against a target of 110. | [158](158_ae_restarts_each_wake.txt) |
| #211 MD sensitivity on the RP3 image | Reproduces. `md 2` is `Command not recognised` and op17 = 3 is never applied. On the HM0360 image both work. | [211](211_md_sensitivity_rp3.txt) |
| #247 MD off still takes frames | Reproduces. op17 = 0 and op11 = 0 both leave the HM0360 in mode 2, and waving did not wake it; with op17 = 1 it woke on every wave. | [247](247_md_off_frames.txt) |

Also seen in the same logs:

- **#56 (closed in December 2025) still happens.** After DPD the clock carries on from the time it
  went to sleep, so each sleep is lost: 29 s over four short sleeps. A photo from a motion wake
  carries the time the camera went to sleep unless the BLE processor's time update lands first.
  [56](56_rtc_loses_dpd_time.txt). Re-tested that afternoon on the CI build: after a motion or BLE wake
  the clock never catches up, so the error grows with every sleep; after a timer wake only the first
  reading is stale and the clock catches up within about 2 s ([56, motion and timer](56_retest_motion_vs_timer.txt)).
- **#245:** two 40 s timer sleeps woke after 37.3 s and 37.9 s ([247](247_md_off_frames.txt)).

## Files

- `bench_daemon.py` holds both consoles open, logs them with timestamps, and runs commands
  appended to `cmd.txt`: type a command, catch the next boot's console, flash both images.
- `ec.py` types into the app's Engineer Console over adb; `ec2.py` sends a second command a set
  time after the first (used for #33).
- `merge3.py` merges the Himax, nRF and app logs into one timeline for a time window.
- `retest58.py` is the timing test for ww-hardware #58.

Ports and the log folder are set at the top of each script.
