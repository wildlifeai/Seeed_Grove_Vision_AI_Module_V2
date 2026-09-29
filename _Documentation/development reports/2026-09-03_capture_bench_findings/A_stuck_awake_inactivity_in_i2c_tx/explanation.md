# An inactivity event during an I2C reply leaves the device awake until it is power-cycled

#### File: explanation.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026

Filed as wildlifeai/Seeed_Grove_Vision_AI_Module_V2#205. **Bench:** WW500 `WILD-CNKW`, `ae_review` at e8b7feb5 on both slots, three-way logger (app over `adb logcat`, nRF console at 115200, Himax console at 921600; `bench_log.py` from the light sensor thread), mobile app wildlifeai/ww-mobile-app#264 as a debug build. Log times are MM:SS.mmm from 18:27:23 NZST.

**Repo:** Seeed_Grove_Vision_AI_Module_V2, `ae_review` at e8b7feb5. **Labels:** bug, review-finding.
**Severity:** high. A camera that hits this never sleeps again: no DPD, no motion detection, no
scheduled reset, battery drained, until someone power-cycles it. Seen at 18:55 on the bench, still
looping at 19:05 when the device was unplugged.

**Status (4 September, 19:03):** fixed by Charles in `ae_review` 4bcb722c and bench-verified on
the device with the same on-demand reproduction, three hits, all slept. See
[Verification of the fix](#verification-of-the-fix) below.

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

## Verification of the fix

Commit 4bcb722c on `ae_review` adds `case APP_MSG_IFTASK_INACTIVITY:` to the five IF-task
states that dropped it (I2C RX, I2C TX, I2C slave TX and RX, PA0), deferring it into
`savedMessage` the way the state already defers RX_READY and the CLI responses, so Idle replays
it and sends the `Sleep` line. Charles's own write-up is in
`_Documentation/development reports/2026-09-04_issue205_inactivity_during_i2c/README.md` on that
branch. He could not test it; this is that test.

**Bench, 4 September 19:00 to 19:04.** Same device, the fix built and flashed by Victor (banner
`WW500 MD. (WW500_C02) Built: 16:45:52 Sep  4 2026`, `Git branch: 'nogit'`), a freshly formatted
SD card (so op8 = 1000 and motion detection off), the phone connected through the Engineer
Console. Driver: [`repro_A_fix.py`](repro_A_fix.py), the original sweep with the pass condition
changed: an attempt counts as a window hit when the probe's `MKL62BA command received: 'slots'`
prints after `Inactive for 1000ms`, and the fix passes when `Deferring event 0x070e` follows and
the device still reaches `Entering DPD`. The old `IF Task unhandled event 'Inactivity'` line is
the fail condition.

Three of the first five attempts landed in the window (520, 560 and 600 ms after the anchor
line), and all three slept:

```
[02:13.875] himax | Inactive for 1000ms
[02:13.875] himax | IMAGE Task state changed from 'Init' (1) to 'Save State' (5)
[02:13.875] himax | MKL62BA command received: 'slots'
[02:13.875] himax | IF Task state changed from 'I2C RX State' (2) to 'I2C TX State' (3)
[02:13.875] himax | IMAGE Task state changed from 'Save State' (5) to 'Uninitialised' (0)
[02:13.875] himax | IF Task received event 'Inactivity' (0x070e). Rx data = 0x00000000
[02:13.875] himax | Deferring event 0x070e
[02:13.875] himax | I2C transmission complete.
[02:13.875] himax | IF Task state changed from 'I2C TX State' (3) to 'Idle' (1)
[02:13.875] himax | Issuing deferred event 0x070e 'Inactivity'
[02:13.875] himax | IF Task received event 'Inactivity' (0x070e). Rx data = 0x00000000
[02:13.875] himax | Sending 103 bytes: Header 4, payload 97, checksum 2 'Sleep 0 0 0 1 2 1 500 ...'
[02:14.176] himax | IF task ready to sleep.
[02:14.176] himax | >>> Entering DPD at 2024:01:01 00:00:08
```

That is the exact sequence of section 1 with the one missing case filled in: the `Inactivity`
arrives in `I2C TX State`, is held, and is replayed once the reply has gone out. The other two
hits are at `[02:38.257]` and `[03:02.008]` in
[`logs/fix_verification_2026-09-04.txt`](logs/fix_verification_2026-09-04.txt) (filtered: hex
dumps, nRF state chatter and the app's raw receive lines removed); the driver's per-attempt
summary is [`logs/fix_verification_sweep.txt`](logs/fix_verification_sweep.txt). No
`unhandled event` line appears anywhere in the session.

Two things learned getting there, for the next person running the sweep:

- The nRF answers a command with `Sleep` on its own once it has the Himax's `Sleep` message, so a
  probe that arrives after the Save State is never forwarded. The window is only the Save State,
  0 to 300 ms wide on this build, and `adb input tap` adds about 170 ms of jitter; a 20 ms step
  hit three times in five where the earlier 50 ms sweep hit nothing in sixteen.
- The sweep needs a quiet device and a quiet phone. With motion detection on (op11 = 1000 from
  an earlier deployment) every wake streamed captures and the dev app fell minutes behind, then
  sent the taps it had queued as one doubled `AI slotsAI slots`; that is the app backlog of
  wildlifeai/ww-mobile-app#273, not a firmware matter. The formatted card fixed both.

The single-slot `savedMessage` caveat in section 4 still stands: Charles's README notes it as a
pre-existing limitation and it is not exercised by this test.

**Evidence:** [`logs/capture_retest_bench.txt`](logs/capture_retest_bench.txt) lines 805 to 828,
three-way bench (app, nRF, Himax). The app side of the same minute is in wildlifeai/ww-mobile-app,
`documentation/development reports/2026-09-03_capture-flash-and-keep-awake/README.md`, eighth run.
The app cannot avoid this; it holds a 3 s timer while its capture screen is open, but the device
wakes on whatever op8 it last saved.
