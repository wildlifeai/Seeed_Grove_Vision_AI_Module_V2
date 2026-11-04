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
| [CLAUDE_BLE_time_proposal.md](CLAUDE_BLE_time_proposal.md) | Getting the time from the BLE processor early in each wake: options, questions, the RTC findings (steps 1 and 1b, test 1) |
| [CLAUDE_wake_sequence_spec.md](CLAUDE_wake_sequence_spec.md) | The spec for both processors, what was coded where, every bench result (ai5 to ai13) and the decisions |
| [CLAUDE_BLE_processor_brief.md](CLAUDE_BLE_processor_brief.md) | The brief given to the ww-hardware session for the BLE processor side |
| [logs/](logs/) | Tera Term logs, `aiN.txt` and `bleN.txt` in pairs (PC times are NZDT, UTC+13) |
| ww-hardware, `_Documentation/development reports/2026-11-01_AI_RTC/` | The BLE processor side: its review of the brief, what it coded, and `CLAUDE_IP_INT_while_awake_brief.md` |

## Status

Closed, 2 November 2026. Both processors coded and benched on branch `261101_AI_RTC` in each repo, board
WILD-CMSS: HM0360 image for ai4 to ai11, RP3 image (with the file name fix) for ai12 and ai13. Before
merging: build the HM0360 image with the final code, and Charles to commit both repos.

## Outcome

Photos get the right time (#56, #152), from the BLE processor, on every wake.

1. **The RTC kept counting through DPD only when an alarm was set.** With the timer off (op 7 = 0)
   the clock lost every sleep: the main cause of #56. `sleep_mode.c` now leaves RTC0 unmasked and
   moves the old alarm to 2020 (turning its interrupt off is not enough: an old alarm still woke the
   camera).
2. **The wake message goes first**, as soon as the IF task starts, so the BLE processor no longer drops
   `NN-` and the motion messages; it also handles them in every state now (ww-hardware).
3. **The BLE processor sends `setutc` on every wake** when it has the time (from LoRaWAN or the app).
   The AI processor keeps it in RAM and replies at once; it writes its own RTC only at sleep, when more
   than 10 minutes out or before 2025, because the write blocks it for 1 to 2 s. A cold boot's 2024 is
   corrected on the first wake with `setutc` (#152).
4. **One time reference per wake.** Photo time = reference + ticks to the frame. The first photo waits
   up to 300 ms for `setutc` (it arrives about 110 to 160 ms after the wake message), else uses the RTC
   once it has ticked, else 2024. The file name, file date, `getutc` and time-of-day flash use the same
   time.
5. **Settings commands are held** until CONFIG.TXT has loaded, so a `setop` sent while the camera slept
   is no longer overwritten.
6. **BLE processor (ww-hardware):** retries the WAKE pulse once after 3 s, for a pulse sent during the
   RTC write at sleep.

Decided: no new BLE command (reuse `setutc`); 10-minute RTC write limit; the 300 ms wait counts from
when the photo is ready; no protection against overwriting a file of the same name; app commands
during the wake handshake are still dropped (rare). Durable docs: `ble_commands.md` (`setutc`),
`.agents/skills/references/hardware-traps.md` (the RTC entry), `cross-repo-contracts.md` (the wake
sequence).

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
  [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152): fixed here; close
  when merged.
- [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247),
  [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246),
  [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270),
  [#262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262): as in the table.
- To file (Charles):
  - ww-hardware: the BLE processor misses the AI processor's interrupt while busy with no phone
    connected (ai13). Brief: ww-hardware thread, `CLAUDE_IP_INT_while_awake_brief.md`.
  - Ask Victor: with no time anywhere, leave the EXIF time out instead of the 2024 stamp?
  - Carried over: a fast paste into the console hung it (previous thread); not tested with a second
    make of SD card since the card's settling wait was removed; ww-hardware ops 37 and 38 in
    `aiProcessor.h`.

## Reopened 3 to 4 November: the `/IP_INT` handshake

After the thread closed, ai13 showed the BLE processor missing the AI processor's interrupt while busy
(LoRaWAN, no phone). The fix became a change to the inter-processor handshake, on both processors, on
the same branches. Spec and bench record: ww-hardware thread `IP_INT_handshake_spec.md`; the AI
processor side is in [CLAUDE_wake_sequence_spec.md](CLAUDE_wake_sequence_spec.md) (logs ai14 to ai21).

1. **Held-low handshake.** The AI processor drives `/IP_INT` low only once a message is armed, and holds
   it low until the message is read (or 4 s). The BLE processor reads on the low, so a busy BLE
   processor finds it still low. Before, it was a pulse, lost if the BLE processor was busy.
2. **A missed low (ai17).** On the nRF52 all PORT interrupt pins share one DETECT signal. The LoRa
   radio's DIO1, set for one edge, held DETECT high during a receive, so the `/IP_INT` low gave no
   interrupt. Fixed on the BLE processor: every PORT pin now uses both edges; and if `/IP_INT` is low at
   its 3 s wait timeout, it reads anyway. Proven with a pin held on purpose (`hack 12`, run 20).
3. **Events deferred while a message waits (ai20).** A late read released a stale `Inactivity`, so
   `Sleep` went before `selftest`, and a single slot let one deferred event overwrite another.
   `if_task.c` now keeps a queue of 4, drops a deferred `Inactivity` when a message is read, and drops
   anything deferred once `Sleep` is sent. Run 21: the wake completes with every interrupt missed.
4. **`NN-` arrived as `HM0` (ai22).** `sendMsgToMaster()` passed a pointer to a buffer the image task
   reused at once, so each message carried the next one's text. It now copies the message. Benched OK.

Left: a message sent from IDLE whose interrupt is missed is still lost (no 3 s check there); only
seen with `hack 12`. ww-hardware: remove the temporary `hack` code. The ai13 item under "To file"
above is done.
