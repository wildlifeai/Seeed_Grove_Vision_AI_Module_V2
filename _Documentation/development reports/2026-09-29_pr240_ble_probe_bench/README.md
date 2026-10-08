# PR #240 bench: the 300 ms BLE probe on a board that has a BLE processor

#### File: README.md
#### Author: Claude (Fable 5.1), run with and reviewed by Victor Anton
#### 29 September 2026

## Status

Closed. The result went into the review of PR #240.

## Outcome

PR #240 makes the AI processor's first message after every boot ("Wake", "Timer" or "MD") a probe: if the BLE
processor does not read it within 300 ms (`BLE_PROBE_TIME`, `if_task.c`), the AI processor treats it as unresponsive,
sends it nothing more and enters DPD without it. Charles had tested this only on a board with no BLE processor, so this
bench checked it on one that has one: WILD-7VQI (WW500-C02, HM0360 and RP3 fitted, SD card, the board's current nRF
firmware), running the PR's `ww500_md` HM0360 build (c95aee62, built locally, 466,944 bytes) flashed over XMODEM.

| Leg | Boots | Probe fired | Missing-master timer expired | nRF read the first message and sent `selftest` |
|---|---|---|---|---|
| Cold boots (`reset` then `dpd`), app not connected | 20 | 0 | 0 | 20 of 20 |
| DPD wakes by the timelapse timer (op 7 = 20 s), nothing typed | 9 | 0 | 0 | 9 of 9 |
| Cold boots with the phone app connected over BLE | 10 | 0 | 0 | 10 of 10 |
| File transfer from the app (File Transfer Test, `LARGE.BIN`, 512000 bytes), one wake | 1 | 0 | 0 | 1 of 1 |

In the app leg the console was also timed: the nRF read the "Wake" message 10 to 20 ms after the AI processor sent it
(`Sending ... 'Wake` to `I2C transmission complete`) and its `selftest` command arrived 60 to 90 ms after the send. On the
timer wakes the `selftest` arrived 0.32 to 0.37 s after the boot banner. After every boot the IF task's sleep path was
"Sleep message read", no message to the BLE processor was dropped, and the device slept or took its scheduled watchdog
reset. The nRF log shows the app connected for 7 of the 10 boots of the app leg (its "Wake" and "Sleep" notifications went
out over BLE); on the other 3 it was reconnecting.

The file transfer (14:04 to 14:06 in the logs) completed normally: `FileTX: Received 'LARGE.BIN' OK (85 packets, 512000
bytes, CRC 0x0300)`, nRF packet times 11 to 15 ms, `ftx done`, no missing-master timeout during it (the 4000 ms window
still applies after the first message), and the device entered DPD afterwards.

Not covered: other nRF firmware versions.

## Open items

None from this bench. PR #240's other open items were benched the next day, in
[`2026-09-30_pr240_bench_findings`](../2026-09-30_pr240_bench_findings/README.md).

## Files

- `ble_probe_bench.py`: the script (phases `flash`, `cycles`, `app`, `watch`, `restore`). Needs pyserial and the
  `Console` class of `_Tools/ww500_ship_check.py`; the operator presses RESET at one beep. Each cycle waits for the boot
  banner, types `reset` once the CLI is up and `dpd` once the nRF's first command has arrived (console typing otherwise
  holds the board awake for 60 s), and scans the console for `BLE processor did not read the first message`,
  `I2C master did not read our I2C message`, `MKL62BA command received`, the IF task's sleep path and
  `>>> Entering DPD` or `>>> Reset by watchdog`. The nRF console is logged alongside with a host timestamp per chunk.
- `ble_probe_results_cycles.csv`, `ble_probe_results_app.csv`: one row per boot. The first `timer` row is the boot in
  which the timelapse was set, not a wake, and is left out of the counts above.
- `status_cycles.txt`, `status_app.txt`: the run logs with the per-boot lines and the summaries.
- `ai_console.log`: the AI processor console for the whole day, including the flash and two aborted script runs (NULs
  stripped). `nrf_console.log`: the nRF console; it flushes its log in bursts, so order events by the AI lines.
