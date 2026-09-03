# Capture Picture bench, 3 September 2026: firmware findings

#### File: README.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026

Findings from a day of three-way bench logging (app, nRF, Himax) while the mobile app's Capture
Picture flow was rebuilt, plus one found while proving them the next day. Each has its own folder
or file in the shape of issues #202 to #204: what the problem is with the log lines that show it,
how to reproduce it, where in the code, and a suggested fix. Every one was reproduced on demand
before filing, except D and E, which are decisions; each carries its issue number in the table.
A ninth draft, an op7 timelapse that could not be explained, turned out to be our own doing and
was dropped.

**Device:** WW500 `WILD-CNKW`, `ae_review` at e8b7feb5 on both slots (HM0360 and RP3), nRF code
read at ww-hardware f3ea286.
**App:** wildlifeai/ww-mobile-app, branch `feat/capture-keep-awake`, debug build over Metro.
**Logger:** `bench_log.py` from the light sensor thread
(`2026-08-24_light_sensor_review/mobile_app_three_way_bench/`, on PR #206 into `ae_review`), Himax
console at 921600 and nRF console at 115200 interleaved with `adb logcat`. Times in the logs are MM:SS.mmm from each capture's start.

## The findings, in the order they should be fixed

| | Repo | Finding | Severity |
|---|---|---|---|
| [A](A_stuck_awake_inactivity_in_i2c_tx/explanation.md) | Seeed | An inactivity event during an I2C reply leaves the device awake until it is power-cycled. Filed as Seeed #205 | high |
| [H](H_command_during_binary_send/explanation.md) | ww-hardware | A command received while a file is streaming is forwarded at once and restarts the packet counter. Reproduced on demand, filed as ww-hardware #33 | medium |
| [B](B_setop_during_save_state_lost/explanation.md) | Seeed | A `setop` that lands after Save State replies success, but the value is not saved. Reproduced on demand, read back after a power cycle. Filed as Seeed #207 | medium |
| [C](C_inactivity_during_frame_retry/explanation.md) | Seeed | An inactivity event during a capture sequence puts the device to sleep mid-sequence: the IF task half-sleeps and its next transmission completes the barrier. Reproduced on both cameras with `AI capture 3 <gap>`. Filed as Seeed #208 | medium |
| [D and E](D_E_flash_mode_index_and_wake_applied_params.md) | Seeed | Decide: the flash-mode op index, and whether `setop` should apply what is only read at wake. Filed as Seeed #209 | decision |
| [I](I_transfer_throughput_console/explanation.md) | ww-hardware | Image transfer is held near 1 KB/s by the nRF's own console output. Measured against the quiet upload path on the same device: 1.05 against 5.24 KB/s. Filed as ww-hardware #34 | medium |
| [J](J_finished_sending_failed_twice/explanation.md) | ww-hardware | `Finished sending` is logged as failed twice before it goes out, every time. Re-checked against `dev`: the log calls SoftDevice back-pressure a failure. Filed as ww-hardware #35 | low |
| [K](K_loopback_never_echoes.md) | ww-hardware | `FILE_LOOPBACK` packets are never echoed, so the app's BLE benchmark always times out. Filed as ww-hardware #36 | low |

A leads because a camera that hits it in the field drains its battery until someone drives out
to it, and the app cannot avoid it: the device wakes on whatever op8 it last saved, and the
app's entry burst is enough to hit the window.

## Filing them

One issue per file, in the repo named in its header, title as written, labels as written, the
four sections pasted as they are. Link the evidence by line number into these logs once this
folder is on `main`; do not paste the logs. Then one message to Charles with the table above and
the sentence under it.

## The logs

| File | What it shows |
|---|---|
| [`logs/capture_retest_bench.txt`](logs/capture_retest_bench.txt) | 18:32 and 18:54: the app's leave, re-enter, capture rounds; the stream corrupted by `slots` (H); the stuck-awake loop from 27:36 (A, B) |
| [`logs/capture_flash_bench.txt`](logs/capture_flash_bench.txt) | 12:21: the flash tests; the HM0360 initialisation failure and the frame-timeout retry that ended in sleep (C) |
| [`logs/capture_final_bench.txt`](logs/capture_final_bench.txt) | 16:26: the ten-step flash and camera matrix; transfer timings (I) |
| [`J_finished_sending_failed_twice/logs/`](J_finished_sending_failed_twice/logs/finished_sending_failed_to_send.txt) | Every `Finished sending` line of the day, failed twice then sent (J) |
| [`A_stuck_awake_inactivity_in_i2c_tx/`](A_stuck_awake_inactivity_in_i2c_tx/explanation.md) | A's own copy of the retest log, the 19:54 sweep that reproduced it, the 19:35 `AI dpd` attempt, and `repro_A.py` |
| [`H_command_during_binary_send/`](H_command_during_binary_send/explanation.md) | 22:33 and 22:35: H reproduced on demand twice, the filtered three-way log, both script reports, and `repro_H.py` |
| [`I_transfer_throughput_console/`](I_transfer_throughput_console/explanation.md) | 4 September 08:31 to 08:44: the nRF version, a timed download, the 500 KB upload through the quiet path, the loopback benchmark that never echoed (K), and `measure_I.py` |

The app side of the same day, with what changed in the app and why, is in
wildlifeai/ww-mobile-app, `documentation/development reports/2026-09-03_capture-flash-and-keep-awake/`.
