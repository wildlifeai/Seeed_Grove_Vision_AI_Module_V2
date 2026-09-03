# An inactivity event during a capture sequence puts the device to sleep mid-sequence

#### File: explanation.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026, mechanism corrected and reproduced on demand on 4 September

**Repo:** Seeed_Grove_Vision_AI_Module_V2, `ae_review` at e8b7feb5. **Labels:** bug, review-finding.
**Severity:** medium. A multi-image capture whose gap exceeds op8, or any capture whose first frame
is late, is abandoned in DPD; the requester never gets `Captured`, and the nRF and the app are
told `Sleep` while the camera is still working.

## 1. What is the problem

The inactivity detector is the FreeRTOS idle hook (`inactivity.c:161` to `188`): it fires after
op8 ms of continuous idle, reset only by a task switch-in (`:206` to `215`). A capture waiting
for its next frame is idle, so with a gap longer than op8 the detector fires in the middle of
the sequence. What happens next depends on the camera.

**HM0360 (the original sighting).** Between images the image task stays in `Capturing`
(`USE_HM0360_CAPTURE_TIMER`, `image_task.c:131` to `135`, `:1222` to `1226`), and that state
rightly ignores the event (`:1074` to `1080`, "Inactive - expect WDT timeout soon?"). The IF
task does not: on `APP_MSG_IFTASK_INACTIVITY` it sends the `Sleep` line to the nRF, sets
`lastMessageSent` and, when that transmission completes, calls `barrier_ready()`
(`if_task.c:922` to `937`, `:1061` to `1089`). `lastMessageSent` is never cleared (`:273` is its
only other mention), so the IF task calls `barrier_ready()` again after every later
transmission. The shutdown barrier is two-party (`ww500_md.c:865`) but counts calls, not tasks
(`barrier.c:36` to `53`). The second call, here the `HM0360 AE regs` telemetry after the next
image, fires `image_sleepNow()` with the image task still capturing.

```
[+ 0.725] himax | Image capture 1/3 took 51ms
[+ 0.725] himax | Sending 124 bytes: ... 'HM0360 AE regs:
[+ 1.965] himax | Inactive for 1000ms
[+ 1.965] himax | Inactive - expect WDT timeout soon?
[+ 1.965] himax | IF task ready to sleep.
[+ 2.067] nrf   | BLE out: Sent   6 bytes: 'Sleep'
[+ 2.568] himax | Image capture 2/3 took 1864ms
[+ 2.568] himax | Sending 124 bytes: ... 'HM0360 AE regs:
[+ 2.568] himax | IF task ready to sleep.
[+ 2.568] himax | >>> Entering DPD at 2024:01:01 00:00:06
```

On 2 September the trigger was different and the outcome the same: the first frame never came,
the detector fired during the 5 s frame-timeout wait, the IF task sent `Sleep` and declared
ready, and two data-path events the `Capturing` state does not handle produced the transmission
that completed the barrier, 1.8 s into the retry
([`logs/original_sighting_2026-09-02.txt`](logs/original_sighting_2026-09-02.txt)).

**RP3.** Between images the image task waits in `Wait For Timer` (`image_task.c:1233` to
`1235`), whose handler for the event stops the timer and the sensor and goes to Save State
(`:1362` to `1388`, "Probably the timer interval is greater than the inactivity interval = bad
planning"). The sequence ends 1.5 s in:

```
[+ 0.615] himax | IMAGE Task state changed from 'NN Processing' (3) to 'Wait For Timer' (4)
[+ 1.547] himax | Inactive for 1000ms
[+ 1.547] himax | IMAGE Task state changed from 'Wait For Timer' (4) to 'Save State' (5)
[+ 1.547] himax | Image task ready to sleep.
[+ 1.547] himax | IF task ready to sleep.
[+ 1.547] himax | >>> Entering DPD at 2024:01:01 00:00:10
```

`Operational_Parameters.md` says of op6 "Must be less than `OP_PARAMETER_INTERVAL_BEFORE_DPD`",
so this face is a known constraint; nothing enforces it (`setop` and `capture` accept any
interval), and it does not cover the HM0360 face or the late-frame case, where the gap is not
the user's choice.

## 2. How to reproduce

Engineer Console, op8 at its default 1000: `AI capture 3 3000` on the RP3 slot, `AI capture 3
1900` on the HM0360 slot (op6's documented ceiling for that camera is about 2000). Watch the
Himax console for `Inactive for 1000ms` and `Entering DPD` before `Captured 3 images`.

[`repro_C.py`](repro_C.py) sends the command over adb and prints the ordered events and the
verdict. 4 September: RP3, DPD after image 1 of 3; HM0360, DPD after image 2 of 3; `Captured`
never sent in either. The app received `Sleep` and nothing else.

## 3. Where in the code

- `if_task.c:922` to `937`: `APP_MSG_IFTASK_INACTIVITY` sends `Sleep` and sets `lastMessageSent`;
  `:1061` to `1089`: every `TX_DONE` with `lastMessageSent` calls `barrier_ready()`; `:273`: the
  flag is never cleared.
- `barrier.c:36` to `53`: `readyCount++`, callback when it reaches `totalTasks`; `ww500_md.c:865`.
- `image_task.c:1074` to `1080` (`Capturing` ignores the event), `:1362` to `1388` (`Wait For
  Timer` sleeps on it), `:131` to `135` and `:1214` to `1235` (which wait each build uses),
  `:1082` to `1127` (the frame-timeout retry, idle throughout).
- `inactivity.c:161` to `188`, `:206` to `215`: idle is the only measure; no task calls
  `inactivity_reset()`.

## 4. Suggested fix

Two parts. First, make a capture count as activity from `STARTCAPTURE` to
`DISK_WRITE_COMPLETE` of the last image: disable the detector for that span, or have the
image task refuse the event while `g_cur_jpegenc_frame < g_captures_to_take` in every state.
Second, fix the half-sleep so the same class of bug cannot recur: clear `lastMessageSent` (or
have the IF task not act on the event at all) when the image task rejects it, and make
`barrier_ready()` per party, so one task cannot satisfy a two-party barrier. Then `Sleep` is
only ever sent when the device is actually going to sleep.

## Evidence

| File | What it is |
|---|---|
| [`logs/repro_C_bench.txt`](logs/repro_C_bench.txt) | Three-way bench log (app, nRF, Himax) of both runs, the slot switch between them, and the switch back. Hex dumps and the app's sync noise removed, nothing else |
| [`logs/repro_C_run1.txt`](logs/repro_C_run1.txt), [`logs/repro_C_run2_hm0360.txt`](logs/repro_C_run2_hm0360.txt) | The script's timeline and verdict for each run |
| [`logs/original_sighting_2026-09-02.txt`](logs/original_sighting_2026-09-02.txt) | The 2 September sighting on the HM0360 slot with op8 at 3000: the late frame, the retry, and the sleep 1.8 s into it |
| [`repro_C.py`](repro_C.py) | The reproduction |
