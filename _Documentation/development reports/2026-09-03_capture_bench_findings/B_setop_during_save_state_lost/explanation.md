# A `setop` that lands after Save State replies success, but the value is not saved

#### File: explanation.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026, reproduced on demand on 4 September

**Repo:** Seeed_Grove_Vision_AI_Module_V2, `ae_review` at e8b7feb5. **Labels:** bug, review-finding.
**Severity:** medium. The app trusts `Set OpParam N = V`; the device wakes with the old value.

## 1. What is the problem

Save State writes CONFIG.TXT and then unmounts the volume (`fatfs_task.c:707` to `709`:
`save_configuration()`, `f_unmount(DRV)`), but the `mounted` flag is only maintained by the mount
routine (`:969`, `:972`), so `fatfs_mounted()` (`:1809`) keeps answering true. A `setop` that
arrives in the window between Save State and DPD posts `APP_MSG_FATFSTASK_SAVE_CONFIG`
(`CLI-commands.c:1931`); the handler (`fatfs_task.c:727`) passes its `fatfs_mounted()` check
(`:734`), calls `save_configuration()` on an unmounted volume, and gets `FR_NOT_ENABLED` (12, "the
volume has no work area"). The reply to the sender was already `Set OpParam 9 = 90`
(`CLI-commands.c:1939`): the RAM copy changed, the file did not, and the next wake loads the old
value (`fatfs_task.c:1580`).

```
[02:14.324] himax | Inactive for 1000ms
[02:14.324] himax | MKL62BA command received: 'setop 9 90'
[02:14.324] himax | Saved state to SD card. Image sequence number = 27
[02:14.324] himax | Error 12 saving config
[02:14.636] nrf   | BLE out: Sent  19 bytes: 'Set OpParam 9 = 90'
```

Two smaller things in the same lines: the FatFS event name table (`fatfs_task.c:201` to `210`)
has no entry for `0x0908`, so the trace prints `FatFS Task received event '' (0x0908)`; and
`setop: failed to queue config save` (`CLI-commands.c:1935`) is the only failure the sender could
ever hear about, and it goes to the console, not the reply.

## 2. How to reproduce

The window is the same as finding A's (#205), so the two usually arrive together, as they did in
the original sighting and again here. When A does not bite the device sleeps normally and the
write is simply gone; when it does, a power cycle reloads the file and shows the same thing.

1. `AI getop 9` (op9, LED brightness, harmless): `OpParam 9 = 89`.
2. `AI slots`, and 600 ms after its reply `AI setop 9 90`. Reply: `Set OpParam 9 = 90`. Console:
   `Error 12 saving config`.
3. Let the device sleep, or power-cycle it if A bit. `AI getop 9`: `OpParam 9 = 89`.

[`repro_B.py`](repro_B.py) does 1 and 2 from the PC over adb through the app's Engineer Console,
sweeping the delay, and its `verify` step does 3. On 4 September the first attempt at 600 ms hit,
together with A; after the power cycle:

```
[03:12.981] nrf   | BLE in: Received  10 bytes 'AI getop 9'
[03:13.897] nrf   | BLE out: Sent  15 bytes: 'OpParam 9 = 89'
[03:18.496] nrf   | BLE in: Received  13 bytes 'AI setop 9 89'
[03:19.413] nrf   | BLE out: Sent  19 bytes: 'Set OpParam 9 = 89'
[03:19.413] himax | Config saved (op params persisted).
```

The last two lines are the control: the same `setop` with the device awake and not about to
sleep is saved, and says so on the console.

## 3. Where in the code

`fatfs_task.c:696` to `725` (Save State: `save_configuration()` then `f_unmount()`, `mounted`
untouched), `:727` to `745` (the `SAVE_CONFIG` handler and its `fatfs_mounted()` check), `:969`,
`:972` (the only writes to `mounted`), `:1809` (`fatfs_mounted()`), `:1580` (the load at wake),
`:201` to `210` (the event name table); `CLI-commands.c:1922` to `1940` (`setop` queues the save
and returns the reply at once); `app_msg.h:209` (`0x0908`).

## 4. Suggested fix

Clear `mounted` where Save State unmounts (or have `fatfs_mounted()` ask FatFS), so
`SAVE_CONFIG` after Save State either remounts and writes, or is refused. Then make the refusal
visible: reply `Set OpParam 9 = 90 (not saved)` or hold the reply until the save result is
known. Adding `SAVE_CONFIG` to the event name table is a one-line extra.

## Evidence

| File | What it is |
|---|---|
| [`logs/repro_B_bench.txt`](logs/repro_B_bench.txt) | Three-way bench log (app, nRF, Himax) of the sweep: the read at `02:09`, the hit at `02:14`, and the first seconds of the stuck-awake loop that followed (A), cut at `02:22`. Hex dumps and the app's sync noise removed, nothing else |
| [`logs/repro_B_verify.txt`](logs/repro_B_verify.txt) | After the power cycle: the read-back at `03:12` and the control write at `03:18` |
| [`logs/repro_B_sweep.txt`](logs/repro_B_sweep.txt) | The script's own report, including two read-back attempts that failed for bench reasons (the nRF ignoring commands while A held it in SELFTEST, and the console's send tap outrunning the typed text) |
| [`repro_B.py`](repro_B.py) | The reproduction |
| [`../A_stuck_awake_inactivity_in_i2c_tx/logs/capture_retest_bench.txt`](../A_stuck_awake_inactivity_in_i2c_tx/logs/capture_retest_bench.txt) | The original sighting, `27:36.490`: `setop 8 3000` acknowledged, `Error 12 saving config` |
