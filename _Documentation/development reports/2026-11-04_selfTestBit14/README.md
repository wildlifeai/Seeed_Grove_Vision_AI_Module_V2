# Self-test bit 14: keep it until a selftest reply has carried it

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-11-04_selfTestBit14\README.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 4 November 2026

## Summary

Branch `261104_selfTest`, from `261101_AI_RTC` (PR #284).

**What this thread is for.** Issue
[#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246): self-test bit 14 never
reaches the app.

**Result.** Bit 14 now stays set until it has been read with `selftest` (in practice by the BLE processor,
which passes it on to the app), then it is cleared.

**The issue.** Bit 14 (`SELF_TEST_AI_NO_BLE`, added by PR #240) means "the BLE processor did not read the
AI processor's first message after boot within 300 ms". It is the only way a field camera could tell us
that this check fired (#229). But nobody outside the AI processor ever sees it: the bit is cleared as soon
as the BLE processor turns up, and the BLE processor only learns the bits by sending `selftest`, so the
reply always says 0. On the bench (30 Sep) the AI console showed `selfTest 4000` while the BLE processor
was told `0000`.

**Discussion on #246.** Charles (4 Oct): if the BLE processor never reads, nothing can carry the bit to it
or the app; the BLE processor already sets all AI bits to "error" at its own reset. Victor (10 Oct)
decided option 1: keep the bit until one `selftest` reply has carried it. The app (ww-mobile-app#433) and
the BLE processor (ww-hardware#60) already know bit 14, so only the AI processor changes.

**Why sticky is still worth doing.** The bit also fires when the BLE processor is only late (booting,
erasing flash, handling LoRaWAN), and then comms return in the same wake. That case can be reported, and
today it is lost.

## The documents

| Document | What it is |
|---|---|
| [CLAUDE_sticky_bit_proposal.md](CLAUDE_sticky_bit_proposal.md) | What changes (how the bit is set and cleared), the decisions, how the fault was simulated, and the bench tests with their results |
| [logs/](logs/) | Tera Term logs: `ai1`/`ble1` test 1, `ai2`/`ble2` test 2, `ai3.log` test 3 (AI only, no BLE processor) |

## Status

Closed, 4 November 2026. Coded and benched on branch `261104_selfTest`: tests 1 to 3 on the RP3 image
(logs above), and the HM0360 image tested OK by Charles (no log kept). Issue #246 closes when the branch
is merged.

## Outcome

Self-test bit 14 now reaches the BLE processor and the app when the BLE processor was late reading the
AI processor's first message after boot. Before, it was always cleared before anyone outside the AI
processor could see it.

1. **The bit is set as before** (first message not read within 300 ms), but it is **no longer cleared with
   the `bleUnresponsive` flag**: not by the late read, not by a command from the BLE processor, not by
   `ble clear`. The flag still clears, so messages to the BLE processor resume as before.
2. **Any `selftest` reply carries the bit and then clears it**, from the BLE processor or the console. A
   command that behaves differently depending on its source was rejected as untidy; in practice the BLE
   processor asks about 100 ms after the wake message, before anyone else.
3. **Benched with a simulated fault**: a test build with the probe time cut to 1 ms makes every wake look
   like a late BLE processor. All 5 wakes reported `selfTest 4000` to the BLE processor, and it passed
   `Error bits = 0x4000` to the connected app. For real it would come from a busy BLE processor (flash
   writes, LoRaWAN), one still starting up, or a missed `/IP_INT`.
4. **Not covered, by design**: a BLE processor that never reads during the wake. Nothing can carry the bit
   then, and it is lost at sleep; the BLE processor reports a dead AI processor itself (bit 1).

Code: `if_task.c`, `CLI-commands.c` (`prvGetSelfTest()`), `selfTest.h` (comment). Durable docs:
`_Documentation/ble_commands.md` (the `ble` command, item 6) and the agent layer's
`cross-repo-contracts.md` (self-test bits).

## Open items

None. #246 is closed by the merge. Related, already closed: #229 (the 300 ms check that sets this bit).
