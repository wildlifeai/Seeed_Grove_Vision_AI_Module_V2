# Unresponsive BLE Processor Detection and Mitigation

#### File: CLAUDE_Unresponsive_BLE_Processor_Detection_and_Mitigation.md
#### Author: Claude (Opus 5.5), reviewed by Charles Palmer
#### 28 September 2026

## Summary

The AI processor (`ww500_md`) now finds out, at every boot, whether the BLE processor is there, and copes when it is
not. The first message after a boot is used as a probe with a short timeout. If it is not read, the BLE processor is
treated as unresponsive: nothing more is sent to it, the device still enters DPD, and the condition is reported on the
console (`ble` command) and in the self test bits (bit 14). A related bug is fixed on the way: `ww500_md` never entered
DPD when its "Sleep" message was not read.

Branch `minimalFreeRTOS`. Built and tested by Charles on 28 September 2026 on a WW500 board that deliberately has no BLE
processor: it behaves as intended. Not yet tested on a board with a BLE processor.

## Why

Charles was running `ww500_md` on a board without a BLE processor (the board used for `ww500_minimal`). Every message to the
BLE processor waited `MISSINGMASTERTIME` (4000 ms) for an I2C read that never came, and the device never entered DPD.

The DPD part is the same fault as the field report in [CLAUDE_BLE_processor_unresponsive.md](CLAUDE_BLE_processor_unresponsive.md)
(a camera that never slept). `shutdownBarrier` has two participants, the image task and the IF task. The IF task only
reported to it when the "Sleep" message had been read (`APP_MSG_IFTASK_I2CCOMM_TX_DONE`). When `MISSINGMASTERTIME` expired
instead (`APP_MSG_IFTASK_I2CCOMM_MM_TIMER`) it went back to IDLE without reporting, so `image_sleepNow()` was never called.
Any device whose BLE processor stops answering would stay awake.

Charles first asked for `MISSINGMASTERTIME` to be cut to 300 ms. The comment above it records why it was raised to 4000 ms
(duplicate interrupts during file transfer at 300 ms; Android's ~950 ms link renegotiation at 1000 ms), so the short
timeout is used only for the first message, which is a probe, and every other message keeps 4000 ms.

## What changed

All in `EPII_CM55M_APP_S/app/ww_projects/ww500_md`.

### `if_task.c` / `if_task.h`

- **Probe:** new `BLE_PROBE_TIME` (300 ms). The first message after a boot ("Wake ...", "Timer ..." or "MD ...", sent on
  `APP_MSG_IFTASK_FREERTOS_INIT`) sets `bleProbePending`, which gives it the short timeout. In `i2ccomm_write_enable()`,
  `xTimerStart()` became `xTimerChangePeriod()` with `BLE_PROBE_TIME` or `MISSINGMASTERTIME` (it also starts the timer).
  `TX_DONE` clears `bleProbePending`.
- **Detection:** if the probe times out (`MM_TIMER` with `bleProbePending`), `bleUnresponsive` is set and the console says
  `BLE processor did not read the first message within 300ms: treating it as unresponsive`.
- **Mitigation, while `bleUnresponsive` is set:**
  - `APP_MSG_IFTASK_MSG_TO_MASTER` (from `sendMsgToMaster()`: AE reports, camera-switch notices, ...) is dropped in
    `vIfTask()` before anything is sent: no `/IP_INT` pulse, no I2C data. `xI2CTxSemaphore` is given back so the sender does
    not block. Console: `BLE processor unresponsive - not sending message to master`.
  - On inactivity the "Sleep" message is not sent; the IF task reports to `shutdownBarrier` at once. Console:
    `IF task ready to sleep (BLE processor unresponsive, no Sleep message).`
- **Recovery:** the flag is cleared when the BLE processor sends a command (`RX_READY` in IDLE: `BLE processor has contacted
  us: messages to it resumed`), by `ble clear`, or at the next boot. It is RAM only, so every boot probes again (300 ms per
  wake on a board with no BLE processor).
- **DPD when the "Sleep" message is not read:** `MM_TIMER` with `lastMessageSent` now reports to `shutdownBarrier` too
  (`IF task ready to sleep (the Sleep message was not read).`).
- **Once per boot:** `barrier_ready()` counts calls, not tasks, and the inactivity detector can fire again after any
  activity. All three paths to DPD now go through `reportReadyToSleep()`, which reports at most once per boot.
- `if_task.h`: `ifTask_isBleUnresponsive()` and `ifTask_clearBleUnresponsive()` (and `#include <stdbool.h>`).

### `CLI-commands.c`

- New command **`ble [clear]`**: `BLE processor responsive` or `BLE processor unresponsive: messages to it are not sent`;
  `clear` clears the flag. `status` was left unchanged, as the app may parse its reply.

### `selfTest.h`

- New self test bit **14, `SELF_TEST_AI_NO_BLE` (0x4000)**, set and cleared with `bleUnresponsive`. `selftest` shows it.

### Found while testing: motion detection "on" but not working (`hm0360_md.c`, `image_task.c`)

On the same board the HM0360 never woke the processor although the console said motion detection was on. The cause was
operational parameter 17 (`OP_PARAMETER_MD_SENSITIVITY`) = 0 on the SD card: `cisdp_sensor_set_md_sensitivity(0)` writes
`MD_LIGHT_COEF` = 0 for both contexts, so the sensor never raises a motion interrupt, while the message in
`hm0360_md_prepare()` depended only on the frame interval (op 11).

- **`hm0360_md_prepare()`** now says `off (the frame interval, op 11, is 0)`, `off (the camera system is disabled)`,
  `off (the sensitivity, op 17, is 0), frames every N ms`, or `on! N ms frame interval, sensitivity S`. Messages only; the
  sensor is set up as before (it still takes frames at the interval with sensitivity 0). In the RP builds op 17 is not
  applied, so there the message ignores it.
- **`image_sleepNow()`** now applies op 17 (`cisdp_sensor_set_md_sensitivity()`) just before `hm0360_md_prepare()`, in the
  HM0360 build. Before, op 17 reached the sensor only at boot, so `setop 17 n` took effect one wake late while the new
  message already reported it.

### Found while testing: FatFS event name (`fatfs_task.c`)

- `setop` printed `FatFS Task received event '' (0x0908)`: `APP_MSG_FATFSTASK_SAVE_CONFIG` had no entry in
  `fatFsTaskEventString[]`. Added `"Save config"`. All four task event-name tables (IF, CLI, FatFS, image) were then checked
  against `app_msg.h` and match.
- `flagUnexpectedEvent()` in `fatfs_task.c` used the IF task's event range to check and index the names. Now uses
  `APP_MSG_FATFSTASK_FIRST/LAST`.

## Documentation

- `_Documentation/ble_commands.md`: `AI ble` row and note 6 (including self test bit 14).
- This document; a pointer from [CLAUDE_BLE_processor_unresponsive.md](CLAUDE_BLE_processor_unresponsive.md); the thread
  [README.md](README.md).

## Console, board with no BLE processor

```
I2C master did not read our I2C message
BLE processor did not read the first message within 300ms: treating it as unresponsive
...
BLE processor unresponsive - not sending message to master
...
HM0360 MD sensitivity 1
Preparing HM0360 for MD:   HM0360 Motion Detection on! 1000ms frame interval, sensitivity 1
IF task ready to sleep (BLE processor unresponsive, no Sleep message).
```

(Illustrative, from the code; the exact lines and their order depend on the build and the settings.)

## Open items

- **Test on a board with a BLE processor.** The only intended difference there is the 300 ms limit on the first message
  after each boot. Check a normal wake, a file transfer (`txfile`, firmware update) and DPD.
- **Self test bit 14 is a contract change.** `selfTest.h` says the list must be the same in the AI processor and the app.
  The BLE processor's copy (`ww-hardware`, `MokoTech/Workspace/WildlifeWatcher_1/selfTest.h`) and the app do not have bit 14
  yet; until they do it is only seen on the AI console.
- **Sensitivity 0 still costs power.** With op 17 = 0 and op 11 non-zero the HM0360 keeps taking frames that cannot detect
  motion. The TODO in `hm0360_md_prepare()` (should sensitivity 0 also stop the frames?) is still open.
- The earlier open items of this thread (port the GPIOTE fix to `dev`; the single-slot `savedMessage`) are unchanged; see
  [README.md](README.md).
- No GitHub issues filed yet.
