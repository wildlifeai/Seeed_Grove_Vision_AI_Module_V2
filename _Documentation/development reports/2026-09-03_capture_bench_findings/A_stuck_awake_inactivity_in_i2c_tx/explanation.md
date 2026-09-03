# An inactivity event during an I2C reply leaves the device awake until it is power-cycled

#### File: explanation.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026

Filed as wildlifeai/Seeed_Grove_Vision_AI_Module_V2#205. **Bench:** WW500 `WILD-CNKW`, `ae_review` at e8b7feb5 on both slots, three-way logger (app over `adb logcat`, nRF console at 115200, Himax console at 921600; `bench_log.py` from the light sensor thread), mobile app wildlifeai/ww-mobile-app#264 as a debug build. Log times are MM:SS.mmm from 18:27:23 NZST.

**Repo:** Seeed_Grove_Vision_AI_Module_V2, `ae_review` at e8b7feb5. **Labels:** bug, review-finding.
**Severity:** high. A camera that hits this never sleeps again: no DPD, no motion detection, no
scheduled reset, battery drained, until someone power-cycles it. Seen at 18:55 on the bench, still
looping at 19:05 when the device was unplugged.

## 1. What is the problem

`handleEventForStateI2CTx()` in `if_task.c` has no case for `APP_MSG_IFTASK_INACTIVITY`. When the
inactivity detector fires while a reply to the nRF is going out, the event is flagged unexpected
and dropped. The image task has already handled its copy of the event: Save State, then
`sleepWhenPossible()`, which calls `barrier_ready(&shutdownBarrier)` and leaves the task
`Uninitialised`. The barrier is two-party (`ww500_md.c:865`). The IF task never sends its
`Sleep ...` line, never reaches `barrier_ready()`, and the callback `image_sleepNow()` never runs.

From then on the loop feeds itself, once per inactivity period:

1. The detector fires. The image task, `Uninitialised`, sends the event to `flagUnexpectedEvent()`,
   which calls `sendMsgToMaster()` with the "unhandled event" text.
2. That message puts the IF task into `I2C TX State` to send it to the nRF.
3. The IF task's own `Inactivity` event arrives while it is in that state. Dropped again.

```
[27:36.490] himax | Set OpParam 8 = 3000
[27:36.490] himax | Error 12 saving config
[27:36.490] himax | Image task ready to sleep.
[27:36.490] himax | IMAGE Task state changed from 'Save State' (5) to 'Uninitialised' (0)
[27:36.490] himax | IF Task unhandled event 'Inactivity' in 'I2C TX State'
[27:38.009] himax | IMAGE task unhandled event 'Image Event Inactivity' in 'Uninitialised'
[27:38.009] himax | IF Task unhandled event 'Inactivity' in 'I2C TX State'
(the last two lines repeat every second: close to 600 times in ten minutes)
```

The nRF relays each "unhandled event" line to the phone, so the app sees the device talking
and healthy. `AI reset` cannot rescue it: `app_setResetRequest()` is only consumed inside
`image_sleepNow()` (`image_task.c:2658`), which is the function that never runs. `AI enable` is
not handled in `Uninitialised` either.

## 2. How to reproduce

The detector is the idle hook (`inactivity.c`): it fires after op8 ms of continuous idle. Its
callback posts `Inactivity` to the image task first; that task's Save State (about 300 ms, an SD
write) runs before the callback gets to post to the IF task. Any command that reaches the Himax
inside that Save State is answered from the IF task's transmit state, and the `Inactivity` then
lands there. So the window is about 300 ms wide and opens op8 after the last activity.

Reproduced on demand at 19:54 with [`repro_A.py`](repro_A.py), which drives the app's Engineer
Console over adb, op8 at its default 1000:

1. Send `AI slots` (the anchor; it wakes the device if asleep) and watch the nRF console for its
   reply, `BLE out: Sent ... 'Active slot ...'`.
2. Send a second command 650 ms after that line appears (the nRF console lags the Himax by a few
   hundred ms, which is why 650 rather than 1000). Anything with a reply will do; the automation
   garbled this one into `slotsAI slots`, and the Himax's `Unrecognised` was enough.
3. `IF Task unhandled event 'Inactivity' in 'I2C TX State'` prints, and the device never sleeps
   again. That was the second attempt of a sweep from 600 ms in 50 ms steps; the first missed.

By hand from the phone it is luck (the app's screen entry hit it once in six); the script sweeps
the delay. Log in [`logs/repro_A_sweep.txt`](logs/repro_A_sweep.txt):

```
[87:19.755] himax | Inactive for 1000ms
[87:19.755] himax | IMAGE Task state changed from 'Init' (1) to 'Save State' (5)
[87:19.755] himax | FatFS Task received event 'Save State' (0x0904)
[87:20.055] himax | MKL62BA command received: 'slotsAI slots'
[87:20.055] himax | Unrecognised
[87:20.055] himax | Saved state to SD card. Image sequence number = 27
[87:20.055] himax | Image task ready to sleep.
[87:20.055] himax | IMAGE Task state changed from 'Save State' (5) to 'Uninitialised' (0)
[87:20.055] himax | IF Task received event 'Inactivity' (0x070e)
[87:20.055] himax | IF Task unhandled event 'Inactivity' in 'I2C TX State'
```

`AI dpd` looked like a one-line reproduction (`prvDpd()` sets the period to 1 ms, so its own reply
should be the collision) and was tried at 19:35: the device slept. The reply had finished its I2C
transmission inside the 1 ms, and the IF task took its `Inactivity` in Idle. With a 1 ms tick it
is a coin toss, not a recipe; the sweep above is the one to use. That attempt is in
[`logs/ai_dpd_attempt.txt`](logs/ai_dpd_attempt.txt).

## Also seen since filing (4 September)

- **It does not need a timed probe.** After a power cycle the device was woken by a single
  console command. The nRF's wake flow sent `selftest`, then the command; the reply was still
  going out when the detector fired, 1000 ms after the boot's last activity, and the loop
  started: `IF Task unhandled event 'Inactivity' in 'I2C TX State'` at `04:03.734`. An ordinary
  wake-and-command from the phone is enough.
- **In the loop the device also looks dead.** The nRF's AI state machine never left SELFTEST,
  and from then on it answered every app command on its own console with `DEBUG: Ignore this in
  SELFTEST for now` and nothing over BLE (`05:38.253`). So the app cannot even ask the device
  what is wrong; the only way out is the power cycle. That half is ww-hardware's.
- **Two related findings share the machinery.** #207 (a `setop` in the same window is
  acknowledged but never saved) hits together with this one, as in the original sighting. #208
  (the device sleeps mid-capture) is the mirror image: there the IF task calls `barrier_ready()`
  twice, because `lastMessageSent` is never cleared and the barrier counts calls, not tasks.
  Fixing the barrier per party closes both ends.

Both excerpts: [`logs/reconnect_burst_and_selftest_drop.txt`](logs/reconnect_burst_and_selftest_drop.txt).

## 3. Where in the code

- `if_task.c`, `handleEventForStateI2CTx()`: cases for TX_DONE, MM_TIMER, ERR, PA0_INT_IN, then a
  group that is deferred into `savedMessage` and replayed on the return to Idle (RX_READY,
  MSG_TO_MASTER, the CLI responses; `if_task.c:1112` to `1126`, replay at `:1571` to `1575`; the
  `dpd` attempt shows it working: `Deferring event 0x070d`), then `default: flagUnexpectedEvent()`.
  `APP_MSG_IFTASK_INACTIVITY` is the one event of the set that falls to `default`. The Idle-state
  handler (`if_task.c:922`) is the one that sends the `Sleep` line and sets `lastMessageSent`.
- `image_task.c:1786`: `APP_IMAGE_TASK_STATE_UNINIT` dispatches straight to `flagUnexpectedEvent()`,
  and that function sends the text to the nRF (`sendMsgToMaster`), which is what keeps the loop fed.
- `image_task.c:2094` `sleepWhenPossible()`, `ww500_md.c:865` `barrier_init(&shutdownBarrier, 2,
  image_sleepNow)`, `barrier.c:36` to `53`: `readyCount` only ever goes up, and the callback fires once.

## 4. Suggested fix

Add `APP_MSG_IFTASK_INACTIVITY` to the group `handleEventForStateI2CTx()` already defers: on the
return to Idle the existing handler sends the `Sleep` line and sets `lastMessageSent`, and the
barrier completes. One caveat: `savedMessage` holds a single event, and in the stuck case a
`MSG_TO_MASTER` and an `Inactivity` arrive during the same transmit, so give the inactivity its own
pending flag or let it win the slot. Two smaller changes close the loop from the other side: do not
send unhandled-event text to the nRF from `Uninitialised` (console only), or have that state
re-signal the barrier instead of complaining. The reliable reproduction is the test for this: post
`APP_MSG_IFTASK_INACTIVITY` while `if_task_state == APP_IF_STATE_I2C_TX` and check DPD is still
entered.

**Evidence:** [`logs/capture_retest_bench.txt`](logs/capture_retest_bench.txt) lines 805 to 828,
three-way bench (app, nRF, Himax). The app side of the same minute is in wildlifeai/ww-mobile-app,
`documentation/development reports/2026-09-03_capture-flash-and-keep-awake/README.md`, eighth run.
The app cannot avoid this; it holds a 3 s timer while its capture screen is open, but the device
wakes on whatever op8 it last saved.
