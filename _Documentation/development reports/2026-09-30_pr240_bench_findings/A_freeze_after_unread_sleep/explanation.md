# After an unread Sleep message and `dpd`, the AI processor freezes until RESET

#### File: explanation.md
#### Author: Claude (Fable 5.1), run with Victor Anton
#### 30 September 2026

## 1. What is the problem

If the BLE processor does not read the AI processor's Sleep message, and the inactivity period is short (as it is after
`dpd`), the AI processor stops within 3 seconds. It does not enter DPD, the console stops answering, and a WAKE pulse from
the BLE processor does nothing. Only RESET brings it back. This happened in 2 runs out of 2.

With the normal inactivity period the same unread Sleep message is handled as PR #240 intends: after 4 seconds the AI
processor gives up on it and enters DPD (run 3).

## 2. How to reproduce

AI processor on `dev` ecb54946 (HM0360 build), BLE processor 0.30.51, phone app connected, AI console open.

1. Press RESET, then type any command on the AI console (typing keeps it awake for 60 seconds).
2. Type `ble`. It must say `BLE processor responsive`, which means the BLE processor read the first message.
3. In the app's Engineer Console send `dfu`, then disconnect. The BLE processor sits in its bootloader for 2 minutes and
   reads nothing.
4. Type `dpd` on the AI console.

The Sleep message goes out and is not read. The console shows 13 `Deferring event` lines, then nothing more.

## 3. Why it happens

Links are to `dev` ecb54946.

1. The image task has already finished, so it treats each further Inactivity event as unexpected and sends a message about
   it to the app
   ([`image_task.c:1543-1551`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c#L1543-L1551),
   the same relay as #219). Sending it takes the I2C semaphore
   ([`image_task.c:2185`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c#L2185)).
2. The IF task is still waiting for the Sleep message to be read, so it puts that message aside in its one-message slot,
   and the next Inactivity event overwrites it
   ([`if_task.c:1217-1227`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_md/if_task.c#L1217-L1227)).
   The message is lost, and the semaphore is never given back.
3. `dpd` sets the inactivity period to 200 ms
   ([`CLI-commands.c:848`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_md/CLI-commands.c#L848)),
   so Inactivity events keep coming while the image task waits for the semaphore, and its queue (10 places) fills.
4. Inactivity is detected in the FreeRTOS idle hook
   ([`inactivity.c:174-200`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_md/inactivity.c#L174-L200)),
   and `app_onInactivityDetection()` then waits up to 1 second to add to the full queue
   ([`ww500_md.c:520`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_md/ww500_md.c#L520)).
   The idle hook must never wait (`freertos_app.c` says so), and everything stops, including the 4 second timer that would
   otherwise have put the board to sleep.

With the default op 8 of 1000 ms the queue would take about 12 seconds to fill, and the 4 second timer comes first, so
deployed cameras should not freeze. The lost message (step 2) happens either way; the 16 Sep field log shows it 26 times.

## 4. Suggested fix

- Never wait in the idle hook: send from `app_onInactivityDetection()` without waiting, or have the hook wake a task.
- Hold more than one set-aside message in the IF task, or at least give the semaphore back when a message to the BLE
  processor is overwritten.
- #219: stop sending the same unexpected-event message to the app again and again.

## Evidence

Lines starting `#####` are the bench script's notes: the PC time and what it typed.

| Run | What happened | Logs |
|---|---|---|
| 1, 10:36 | `dpd` typed, Sleep not read: 13 set-aside messages, then silence. No reply to typing at 10:40. A WAKE pulse at 10:42:39 did nothing | [`run1_freeze.txt`](logs/run1_freeze.txt), [`run1_nrf.txt`](logs/run1_nrf.txt) |
| 2, 10:45 | The same, after RESET | [`run2_freeze.txt`](logs/run2_freeze.txt), [`run2_nrf.txt`](logs/run2_nrf.txt) |
| 3, 10:50 | No `dpd`: the board went idle by itself after the 60 second console period. Nothing set aside, the 4 second timer fired, DPD | [`run3_control_no_dpd.txt`](logs/run3_control_no_dpd.txt), [`run3_control_nrf.txt`](logs/run3_control_nrf.txt) |
