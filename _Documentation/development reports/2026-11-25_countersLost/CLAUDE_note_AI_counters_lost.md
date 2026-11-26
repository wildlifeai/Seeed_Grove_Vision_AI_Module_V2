# Note for the AI processor: state ops lost after /RST resets

#### File: `CLAUDE_note_AI_counters_lost.md`
#### Path: `_Documentation/development reports/2026-11-06_lorawanPing/CLAUDE_note_AI_counters_lost.md`
#### Author: Claude (Opus 5.5, BLE processor side), for Charles Palmer, to pass to the AI processor Claude
#### Date: 23 November 2026, sections 6 and 7 added 24 November 2026

## In short

After six BLE processor fault resets in a few minutes, each of which resets the AI processor,
the AI processor reported its photo number and both detection counts as 0. Its settings and its
cold boot count were kept. Probable cause: a reset landing while the AI processor saves its state
at sleep, or the order in which it reloads that state. Please investigate (questions in
section 3). Separately, one reset had no effect on the AI processor (section 5), and the AI
processor's clock was wrong three times in one day (section 6). Section 7 is a suggestion, to
discuss with Charles before any change: check every op's value in `setop`.

For investigation later. Nothing has been changed on either side because of this. Seen on the
bench while testing the new LoRaWAN uplink (phase 1 of
[CLAUDE_lorawanPayload_proposal.md](CLAUDE_lorawanPayload_proposal.md)), which reports ops 1 and 2.

## 1. What was seen

After a run of BLE processor fault resets, each of which resets the AI processor with /RST at the
next BLE boot, the AI processor reported its image sequence number and both NN counts as 0. Its
settings and its cold boot count were not affected.

`Sleep` list sent by the AI processor afterwards:

```
Sleep 0 0 0 18 1 1 500 0 1000 5 1 1000 100 0 20 1 60 1 0 0 0 2 5 65 15 0 0 286 326 1 110 1 15 5 0 0 0 5000 587
```

| Op | Meaning | Before the resets | After |
|---|---|---|---|
| 0 | Image sequence number | not recorded | **0** |
| 1 | NN analyses | 1131 | **0** |
| 2 | Positive NN analyses | 80 | **0** |
| 3 | Cold boots | not recorded | 18 (so kept, and still counting) |
| 4 | Warm boots | not recorded | 1 |
| 5 to 17 | Camera, flash, model and motion settings | | look intact |
| 32 | LoRaWAN ping minutes | 15 | 15 |

The BLE processor only copies ops 1 and 2 from the `Sleep` list (`aiProcessorSaveStats()`), so
the zeros come from the AI processor.

## 2. History on the bench, 23 November

1. The AI processor was updated to the Seeed branch `261106_lorawanPing` build (with `sdused`).
   Several BLE processor DFU loads followed, each with /RST at boot, and the AI processor woke
   and slept normally. Ops 1 and 2 stayed at 1131 and 80 throughout.
2. The BLE command `hack 6` (a deliberate `APP_ERROR_CHECK()` failure) was then run six times
   within a few minutes. Each one resets the BLE processor, and its next boot pulses /RST
   (P0.06, 100 ms, open drain) to the AI processor. The sixth hit the BLE processor's reset loop
   guard, which waits 10 minutes before resetting. The timing of each /RST relative to the AI
   processor's activity (awake, writing after `Sleep`, or in DPD) was not recorded.
3. From then on the `Sleep` list shows ops 0, 1 and 2 as 0.

## 3. Where to look

The loss is limited to the run-time counters, which the AI firmware already treats separately:

- `fatfs_task.c`, around line 1820: the fast-start record saved in flash at the last sleep is
  loaded first (`xip_state_loadFastStart()`), then CONFIG.TXT, which "overrides the settings but
  not the state ops" (`xip_state_keepStateOp()`).
- `xip_state.c`: `xip_state_loadFastStart()` uses the newest valid record, op by op through
  `fatfs_isValidOperationalParameter()`.

Questions:

1. Are ops 0, 1 and 2 state ops held in the fast-start record? If so, was the record lost or
   rejected, or was a record with zeros saved?
2. Could a /RST arriving while the AI processor saves at sleep (the record, the RTC write after
   `Sleep`, or CONFIG.TXT) leave the counters at 0, while settings and the cold boot count
   survive?
3. After a cold boot, which source wins for these ops: the fast-start record or CONFIG.TXT? Is
   that what is intended?
4. Image sequence number 0: can new photos now reuse the names of photos already on the card?

Useful evidence: the AI processor console from one cold boot, in particular the
`Fast-start record: ...` line and `'CONFIG.TXT' found. (Next image #N)` or `... NOT found`. Also
`AI getop 0`, `AI getop 1` and `AI getop 2`.

## 4. Why it matters

- The BLE processor resets the AI processor at every BLE boot (error recovery step 6). In the
  field that includes every BLE watchdog or fault reset, so any of them could lose the counts.
- The LoRaWAN uplink sends ops 1 and 2 (`PAYLOAD_TYPE_NN_TOTAL`, `PAYLOAD_TYPE_SENSOR_EVENT_COUNTER`).
  The server would see the counts drop to 0, which looks like a factory reset.
- Phase 2 of the uplink work triggers an uplink when op 2 rises, and assumes the count survives
  reboots (proposal section 3.8).

## 5. Related, lower priority: one /RST the AI processor ignored

Earlier the same day, one `hack 6` reset (the first in a row) and one BLE `reset` were followed by
no activity on the AI processor console. The BLE processor reported
`AI processor not responding: nothing after power-on or reset`, and the AI processor answered a
later `AI ver` normally, so it had been asleep, not reset. Later /RST pulses worked, including
from DPD, and the scope shows a clean 100 ms low pulse.

On the BLE side the pulse is the second statement of `main()` (`fault_resetAiProcessor()`), with
no condition, so the `hack 6` case should have produced one. The BLE `reset` command only resets
when the phone disconnects, which may explain that case. If the AI processor side knows of any
state in which RESETN is ignored or the boot is silent, that would explain the `hack 6` case.
Otherwise it needs catching in the act: scope triggered on /RST plus the AI console.

## 6. Related: the AI processor's clock

Three observations on 23 November, all corrected by the `setutc` the BLE processor sends at each
wake. None has been looked into.

1. **Back to 2024 after each BLE boot.** The BLE processor resets the AI processor at every boot,
   and the AI processor's clock then starts from 2024 until its next `setutc`.
2. **12 seconds fast** five minutes after a `setutc`.
3. **11 hours 47 minutes slow.** At 18:39 UTC the AI processor woke from DPD (for an `AI ver`)
   and its `Wake` message gave `2026-11-23T06:52:05Z`. The BLE processor logged
   `BLE and AI time differ by 42429s - too much!`. The previous `setutc` was earlier that day; the
   AI processor had been reset (cold boots rose from 18 to 23 since section 1) and had woken
   and slept several times in between.

Questions: does the AI processor's RTC keep counting through DPD and through a /RST? Is the
`Wake` time read from the RTC, or from a value saved at sleep? Is 2 (fast by 12 s in 5 minutes,
4%) a clock source or a calibration problem?

## 7. Suggestion, to discuss with Charles: check every op's value in `setop`

The LoRaWAN uplink's phase 3 will let the server read and set ops by downlink. The BLE processor
relays the index and value unchecked, by design: it should not need changing when an op's
meaning changes. So the AI processor's `setop` is the only check.

`fatfs_isValidOperationalParameter()` (`fatfs_task.c:2226`) checks five ops (16, 17, 33, 37,
38) and returns `true` for every other index. So a `setop` from the app or a downlink can, for
example:

- overwrite the state ops 0 to 4 (image number, NN counts, boot counts), which gives the same
  symptom as section 1;
- set an enable, mode or period op to a value outside its meaning (op 10 camera enabled = 7, a
  flash mode out of range, op 32 = 65535).

Suggested: a range for every op in `fatfs_isValidOperationalParameter()`, and a decision on
whether the state ops can be set at all (the app may rely on setting some of them). The replies
need not change: the BLE processor treats any reply starting `Error:` as a rejection.
