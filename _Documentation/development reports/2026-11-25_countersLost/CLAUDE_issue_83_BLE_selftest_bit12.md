# ww-hardware #83: self-test bit 12 now means "CONFIG.TXT damaged"

#### File: `CLAUDE_issue_83_BLE_selftest_bit12.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/CLAUDE_issue_83_BLE_selftest_bit12.md`
#### Author: Claude (Opus 5.5, AI processor side), for Charles Palmer and the BLE processor Claude
#### Date: 26 November 2026

## In short

- The details of [ww-hardware #83](https://github.com/wildlifeai/ww-hardware/issues/83), raised 26 November 2026, for the BLE processor side. Not yet worked on.
- Part 1 is a rename only. Part 2 answers Victor's bit 14 question of 21 November. Part 3 is optional.
- Follows [#297](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/297) ([record](CLAUDE_297_counters_lost.md), thread [README](README.md)).

---

**Title:** Self-test bit 12 now means "CONFIG.TXT damaged"; keep AI event bits until reported

## Background

On 23 November the AI processor's counters (photo number, NN counts) went to 0 after repeated
resets (`CLAUDE_note_AI_counters_lost.md`, lorawanPing thread). Cause: the counters are kept only
in CONFIG.TXT, and a save emptied the file before rewriting it. Fixed on the AI processor side in
Seeed #297 (branch `261125_countersLost`).

## 1. Bit 12 renamed (needed)

AI self-test bit 12 (`0x1000`) was `SELF_TEST_AI_PDM_ERROR`, a microphone that was never fitted.
It is now `SELF_TEST_AI_CONFIG_DAMAGED`: CONFIG.TXT was damaged or unreadable at boot, the counters
(ops 0 to 4) restarted from 0, and new photos started a new image folder.

- Rename it in ww-hardware's `selfTest.h` (and `selfTest_test()` in `selfTest.c`).
- No logic change: it already passes the BLE processor's `0xff00` mask into the app and the
  LoRaWAN health uplink.

## 2. Keep AI event bits until reported (proposed)

The AI processor sets bit 12 for one wake only, like bit 14 (BLE processor missed the first
message). The BLE processor replaces its copy of the AI bits at every wake. So:

- **LoRaWAN:** fine. The health uplink goes at once on a self-test change.
- **App:** sees the bit only if a phone is connected during that wake (Victor's question about bit
  14, 21 November).

Proposal: treat AI bits 12 and 14 as events. OR them into a latched copy, and clear the latched copy
only once it has gone in an acknowledged uplink and been read by the app (or one of the two, to
decide).

## 3. Restore the NN counts (optional, for discussion)

When bit 12 arrives, the AI processor's NN counts (ops 1, 2) restart at 0. The BLE processor still
has the last good values from the previous `Sleep` list. It could send them back (`AI setop 1 <n>`,
`AI setop 2 <n>`) so the LoRaWAN counts don't fall to 0. Needs agreement on whether that is wanted.
