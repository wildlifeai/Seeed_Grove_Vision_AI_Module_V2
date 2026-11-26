# #296: commands lost between "Sleep" and DPD

#### File: `CLAUDE_296_sleep_order.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/CLAUDE_296_sleep_order.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 25 November 2026

## In short

- **The fault.** The AI processor sent "Sleep", then spent 3 to 5 s on RTC work before entering
  DPD. The BLE processor treats "Sleep" as "in DPD", so a command in that time became a WAKE pulse
  that was ignored. The command was lost and the app showed "AI processor is not responding".
- **More often than #296 says.** Not only after a cold boot: also once a day, and whenever the RTC
  had drifted more than 10 minutes, which at its 4.6 % drift is every few hours of sleep.
- **The fix.** The RTC work now comes before "Sleep", which is the last thing before DPD (30 to
  90 ms later). A command during the RTC work reaches an awake processor and is answered.
  AI processor only; no change in the BLE processor or the app.
- **Also fixed: the calibration repeating.** The RTC calibration repeated on most sleeps. The RTC write now comes
  before it.
- **State.** Benched on both images, every test passed. Committed `ae19db33`; #296 closed on
  26 November. Thread [README](README.md).
- **Read on** for the evidence (section 1), the options weighed (2), the change (3) and the
  bench results (4).

## 1. The fault, reproduced

Reported by Victor ([#296](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/296),
his email [Victors_Issues.eml](Victors_Issues.eml)). Logs [`logs/ai1.txt`](logs/ai1.txt) and
[`logs/ble1.txt`](logs/ble1.txt), RP3 image, before the fix. `AI reset` from the app, then `AI ver` sent when the post-cold-boot "Sleep" appeared:

| Time | Processor | Event |
|---|---|---|
| 14:41:21.706 | AI | sends `Sleep ...` |
| 14:41:21.818 | BLE | marks the AI processor asleep |
| 14:41:22.953 | BLE | `AI processor is in DPD. Waking it. (ver)` |
| 14:41:24.101 | AI | `RTC calibration: ... saved` |
| 14:41:25.968 | BLE | `No Wake after WAKE pulse: trying once more` |
| 14:41:26.007 | AI | `RTC was out by ...: set` (1.9 s), `>>> Entering DPD` |
| 14:41:28.982 | BLE | `AI processor not responding` |

Both WAKE pulses came before DPD and were lost. The BLE processor then counted an AI processor
failure in its health uplink, so the server sees a fault as well.

The same log showed a 4.7 s window on an ordinary wake (calibration due, RTC 59 minutes out), and
the calibration running on 6 of 7 sleeps.

## 2. Options weighed

| | Option | Verdict |
|---|---|---|
| A | Do the RTC work before "Sleep" (Victor's fix in #296) | **Chosen.** One repo, and a late command is answered at once |
| B | A "Snooze" message before the RTC work, "Sleep" after it; the BLE processor holds commands in between (Charles) | Kept in reserve in case A failed its bench test. Needs both repos and a message change |
| C | The BLE processor retries the WAKE pulse for longer | A cheap safety net for the BLE processor side; not done |
| D | Make the RTC work rarer | Done in part: the calibration no longer repeats |

The risk in A was a command arriving during the RTC write, thought to block interrupts. Tested
first with today's firmware (`utctests` on the AI console, `AI ver` from the app at the same
moment, [`logs/ai2.txt`](logs/ai2.txt), [`logs/ble2.txt`](logs/ble2.txt)): 4 of 4 commands
arrived intact.

## 3. The change

- **`image_task.c`.** When state is saved, `rtcWorkBeforeSleep()` writes the RTC if far out, then
  calibrates if due, with the watchdog stopped. Only then is the IF task asked to send "Sleep".
- **`ww500_md.c`.** The inactivity detector tells the IF task only once the image task is ready
  (`image_isReadyToSleep()`).
- **`CLI-commands.c`.** `capture` and `light` while preparing to sleep reply
  `Error: going to sleep, try again.`
- **The calibration repeating.** RTC write before calibration, so the calibration's time is in the corrected RTC time.
- **Docs.** `ble_commands.md` note 7, `hardware-traps.md`.

## 4. Bench results, after the fix

| Run | Image | Test | Result |
|---|---|---|---|
| [`ai3`](logs/ai3.txt) / [`ble3`](logs/ble3.txt) | HM0360 | Two `AI reset` cycles | RTC work before "Sleep", DPD 44 ms after it. Warm sleeps after a cold boot no longer calibrate |
| [`ai4`](logs/ai4.txt) / [`ble4`](logs/ble4.txt) | HM0360 | `AI ver` 0.9 s into the cold-boot RTC write | Answered in 33 ms, then a normal sleep |
| [`ai5`](logs/ai5.txt) / [`ble5`](logs/ble5.txt) | HM0360 | `AI capture 1 1` during the calibration | Refused at once (`no captures until after the first sleep`, as it was a cold boot), then a normal sleep |
| [`ai6`](logs/ai6.txt) | RP3 | Cold boots, `AI reset`, warm wakes | Same order every time, DPD 30 to 90 ms after "Sleep" |

Not caught by hand: `Error: going to sleep, try again.` It needs a warm wake that does RTC work.

**Unexpected.** In `ai4` the AI processor answered during the RTC write, so the write did not
hold up the I2C interrupt or the IF and CLI tasks. `hardware-traps.md` says it blocks interrupts
for 1 to 2 s. One sample, so that entry is unchanged.
