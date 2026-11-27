# #207: a setop after Save State replies OK but is not saved

#### File: `CLAUDE_207_setop_after_save_state.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/CLAUDE_207_setop_after_save_state.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 27 November 2026

## In short

- **The issue:** [#207](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/207),
  raised in September. Thread [README](README.md).
- **The fault.** On the way to sleep the AI processor saves its state (CONFIG.TXT, then the card
  is unmounted; the flash records). A `setop` after that changed RAM and replied
  `Set OpParam N = V`, but nothing saved it, so the next wake had the old value.
- **Why now.** The #296 fix ([record](CLAUDE_296_sleep_order.md)) put the RTC work, up to about
  5 s, between Save State and "Sleep". Commands in that time now reach the AI processor, so the
  window grew from milliseconds to seconds whenever RTC work is due. Before, such a command was
  lost outright ("not responding"); now it would get a false OK.
- **The fix.** From the start of Save State to DPD, the commands that change saved settings
  (`setop`, `setdid`, `setgps`, `md`) reply `Error: going to sleep, try again.` and change
  nothing. Sent again after "Sleep", the command wakes the AI processor and is saved as usual.
- **State: fixed and benched** (27 November 2026). Not yet committed.

## 1. The change (`CLI-commands.c`)

`settingRefused()` checks `image_isGoingToSleep()`, which the image task sets when it starts Save
State and never clears before DPD. The four commands call it before changing anything. `capture`
and `light` were already refused the same way (#296).

Not changed: commands that only read (`getop`, `getdid`, `getgps`, `ver`) are answered as usual.

## 2. Considered and dropped: moving the RTC work before Save State

The plan of 27 November was first to move the RTC work before Save State, so a `setop` during it
would still be saved, and then to refuse `setop` in the short gap left. Dropped:

- The RTC write must not overlap an SD card write (the JPEG write fault, `WW500_file_write_fail.md`:
  `hx_drv_rtc_set_time()` blocked interrupts for about 1.4 s; `g_sdWriteActive` guards it). After Save State the card
  is idle; before it, a photo write can still be under way (for example after a frame fault).
- Refusing for the whole time from Save State to DPD fixes #207 on its own, with no change to the
  sleep order that #296 benched.

The cost: a settings change in the up-to-5 s of RTC work is refused instead of saved. The sender
gets an error and can send it again.

## 3. Bench results, 27 November 2026 (HM0360 image)

| Step | Result |
|---|---|
| `AI reset`, then `AI setop 9 90` right after `Inactive for 1000ms` | Arrived after Save State, during the RTC work (between `RTC was out by ...: set` and `RTC calibration: ... saved`). Replied `Error: going to sleep, try again.` at once; "Sleep" and DPD followed normally |
| After that sleep, `AI getop 9` | Old value, unchanged |
| `AI setop 9 90` on an ordinary wake, sleep, `AI getop 9` | `Set OpParam 9 = 90`, saved, and 90 after the sleep. Put back to 5 |

The refused run had no card mounted (Save State replied `FR_NO_FILESYSTEM`); the refusal does not
depend on the card. The save on an ordinary wake was checked with the card in.
