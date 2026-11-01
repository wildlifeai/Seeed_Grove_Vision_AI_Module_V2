# The AI processor's clock (RTC)

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-11-01_AI_RTC\README.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 1 November 2026

## Summary

Branch `261101_AI_RTC`, from `dev` after PR #283 was merged.

**What this thread is for.** Photos get the right time. The AI processor stamps each photo from
its own clock, which is stale after a motion wake (#56), reset to 2024 by every restart (#152) and
2 to 4.6 % fast between updates. The BLE processor has an accurate clock, but only corrects the AI
processor's when they differ by more than 300 s, and only after the photos are taken.

**Where it starts.** A way for the AI processor to get the time from the BLE processor early in
every wake, before the first photo is saved:
[CLAUDE_BLE_time_proposal.md](CLAUDE_BLE_time_proposal.md).

**Carried over** from the previous thread
([2026-10-27_XIP_save_state](../2026-10-27_XIP_save_state/README.md)), which corrected the sleep timer
(#245, closed) and left the time of day for here. Its analysis of the two time problems is in
[CLAUDE_time_accuracy.md](../2026-10-27_XIP_save_state/CLAUDE_time_accuracy.md); Problem 2 there is
this thread's starting point.

## The documents

| Document | What it is |
|---|---|
| [CLAUDE_BLE_time_proposal.md](CLAUDE_BLE_time_proposal.md) | Getting the time from the BLE processor early in each wake: options and questions |
| [CLAUDE_wake_sequence_spec.md](CLAUDE_wake_sequence_spec.md) | The spec for both processors: wake message first, `setutc` every wake without the RTC write, state machine changes, tests |

## Status

Open. Proposal written and its questions answered, 1 November 2026. Step 1 logs done: the RTC stopped
in DPD whenever no alarm was set. Test 1 (RTC0 left unmasked, alarm off) keeps it running with no
extra wakes: now permanent in `sleep_mode.c` (not yet built). Then the spec
(`CLAUDE_wake_sequence_spec.md`): AI processor side coded 1 Nov, not yet built; the BLE processor side
is being coded in a separate session. AI processor side benched 1 Nov (ai5/ble5): wake message first,
held commands, fallback stamps and `setutc` (no RTC write, written at sleep when over 600 s) all pass.
Waiting for the BLE processor side: brief for that session in `CLAUDE_BLE_processor_brief.md`.

## Outcome

None yet.

## The TODO list: where each item stands

Carried over from the previous thread, 1 November 2026.

| # | Item | Where it stands |
|---|---|---|
| [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56) | Photo times fall behind: the clock is stale after a motion wake. Also takes the clock's drift from #245 | **This thread** |
| [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152) | Every restart sets the clock to 2024 | **This thread** |
| [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247) | HM0360 keeps taking frames with MD off | Parked, with the power work |
| [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246) | Self-test bit 14 never reaches the app | Waiting on a decision |
| [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) | Detection tests class 1 alone | Waiting on a decision |
| [#262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262) | LoRaWAN message content | Waiting on a decision |

## Open items

- [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56),
  [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152): this thread.
- [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247),
  [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246),
  [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270),
  [#262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262): as in the table.
- Ask Victor: with no time anywhere, leave the EXIF time out instead of the 2024 stamp? (Decisions
  in the proposal.)
- Not yet filed, carried over (Charles to decide):
  - a fast paste into the console hung it, then the IF task stayed in `I2C RX State` and the
    camera never slept (previous thread's step log, "Parked: console stopped responding");
  - not tested with a second make of SD card since the card's settling wait was removed;
  - ww-hardware: ops 37 and 38 in `aiProcessor.h` (an issue in that repo).
