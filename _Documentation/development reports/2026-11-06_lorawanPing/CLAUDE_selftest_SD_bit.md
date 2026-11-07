# Self-test SD card bit missing after a warm boot

#### File: `CLAUDE_selftest_SD_bit.md`
#### Path: `_Documentation\development reports\2026-11-06_lorawanPing\CLAUDE_selftest_SD_bit.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 7 November 2026

## Finding

On a warm boot with no SD card, `selftest` replies `0000`. Bit 11 (`SELF_TEST_AI_NO_SD_CARD`) should be set,
so the reply should be `0800`. A cold boot with no card gives `0800`, which is correct.

**Cause.** Since the AI RTC change (`393dee35`, 1 Nov 2026), a warm boot sends "Wake" as the IF task starts,
before the SD card is mounted (`if_task.c:1712`). The BLE processor answers "Wake" with `selftest` at once.
Bit 11 is set only when the mount fails (`fatfs_task.c:1777`), about 650 ms later. A cold boot still sends
"Wake" from `startupBarrier`, after the mount.

From [`logs/ai1.txt`](logs/ai1.txt) (first wake, no card):

| Time | Event |
| --- | --- |
| 08.803 | `Wake` sent |
| 08.877 | BLE processor's `selftest`, reply `selfTest 0000` |
| 09.476 | `SD card initialisation failed (reason 3)`, bit 11 set |

**Why it matters.** The BLE processor uses the bits only to report to a connected phone. Victor's LoRaWAN
health uplink ([`WW500-LoRaWAN-health-message.md`](WW500-LoRaWAN-health-message.md)) would send them after
the first `selftest` reply of a wake, so a camera with no card would report "SD OK". Bit 11 is one of the
four bits for the red "Fault" state.

## Proposed fix: bit 15 = provisional

A new self-test bit 15, `SELF_TEST_AI_PROVISIONAL`: "these AI bits are not final, another `selfTest` follows".

1. **AI processor.** A `selftest` reply sent before the FatFS task has finished the mount attempt has bit 15
   set. When the mount attempt finishes, if a provisional reply went out, send `selfTest xxxx` unprompted
   with bit 15 clear. A cold boot never sets bit 15 (the mount is done before "Wake").
2. **Bit 14 is cleared only by a final reply.** The BLE processor replaces all AI bits with each reply, so if
   a provisional reply cleared bit 14, the final one would lose it.
3. **BLE processor.** Accept an unprompted `selfTest ` in any awake state: call `processAiSelfTestBits()`
   and keep the state. Today IDLE and SETUTC log `UNHANDLED event` and drop it (`aiStateMachine.c:675`,
   `:626`). The mask `SELF_TEST_AI_ALL_BITS` is already `0xff00`, so bit 15 gets through.
4. **Health uplink** waits while bit 15 is set.
5. **Contract.** Add bit 15 to both `selfTest.h` files, the app's `deviceSelfTest.ts` and Victor's health
   layout (show as "checking", not a fault).

**Order.** The BLE processor change must ship first, or bit 15 stays set all wake.

**Alternatives considered.**
- Send an unprompted `selfTest` only if the bits change. No extra message, but the BLE processor cannot tell
  a provisional reply from a final one, and a lost follow-up hides a missing card.
- Set bit 11 until the mount succeeds. Fails safe, but every working camera reports "no SD card" briefly on
  each warm wake, which would raise a false health alert.
- Hold the reply until the mount is done. Delays `setutc` by 200 to 700 ms on every wake, the time the
  1 Nov change saved.

**Cost.** One extra `selfTest` message per warm wake, after the mount, off the path to the first photo.

## Status, 7 Nov 2026

AI processor side coded, not yet built or benched: `selfTest_getReplyBits()` and `selfTest_setChecksDone()` in
`selfTest.c`, called from `prvGetSelfTest()` and where the FatFS task sets `sdReady`; new IF task event
`APP_MSG_IFTASK_SELFTEST_FINAL`, sent from IDLE and deferred in the other states (not dropped, unlike
`sendMsgToMaster()` mid-exchange). BLE processor side not started, and it must ship first. Benched 7 Nov ([`logs/ai3.txt`](logs/ai3.txt)): the
AI side sends `8000` then `0000` on every warm wake, but on a motion wake the BLE processor is IDLE and drops the
final one. Change requested in ww-hardware, same thread folder, `CLAUDE_selftest_bit15_brief.md`.

**Benched with both changes, 7 Nov** ([`logs/ai4.txt`](logs/ai4.txt), [`logs/ble4.txt`](logs/ble4.txt)): 10 wakes
(2 cold, 4 BLE, 4 motion; card in and out). Cold boots: one `selfTest`, no bit 15. Every warm wake: `8000`, then
the final `0000` (card) or `0800` (no card), merged by the BLE processor in IDLE (motion) or PROCESSING (app
command), no `UNHANDLED event`, and the app got both `Error bits` lines. A PROCESSING command still got its own
reply afterwards. Fixed on both sides; not yet committed.

## Issue text

Raised as [#290](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/290) on 7 Nov 2026.

> **Title:** `selftest` misses the SD card bit after a warm boot
>
> On a warm boot with no SD card, `selftest` replies `0000` instead of `0800` (bit 11,
> `SELF_TEST_AI_NO_SD_CARD`). A cold boot is correct.
>
> Since `393dee35` (1 Nov 2026) a warm boot sends "Wake" before the SD card is mounted. The BLE processor's
> `selftest` follows at once and is answered about 650 ms before the mount fails and sets bit 11. Nothing
> asks again on that wake.
>
> Today this only affects what a connected app sees. The planned LoRaWAN health uplink (#262) sends the
> bits after the first `selftest` reply, so it would report a missing card as OK.
>
> **Proposed fix:** new self-test bit 15, "provisional" (cross-repo contract: both processors, app,
> health layout).
> - AI processor: a `selftest` reply sent before the mount attempt finishes has bit 15 set. When the mount
>   attempt finishes, send `selfTest xxxx` unprompted with bit 15 clear. Bit 14 is cleared only by a final
>   reply.
> - BLE processor: handle an unprompted `selfTest ` in any awake state by merging the bits
>   (`processAiSelfTestBits()`) without a state change (IDLE and SETUTC drop it today). Don't send the
>   health uplink while bit 15 is set.
> - App: show bit 15 as "checking", not a fault.
> - The BLE processor change ships first, or bit 15 stays set all wake.
>
> Details: `_Documentation/development reports/2026-11-06_lorawanPing/CLAUDE_selftest_SD_bit.md`.
