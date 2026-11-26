# LoRaWAN ping and health message

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-11-06_lorawanPing\README.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 6 November 2026, updated 7 November 2026

## Status

Open. AI processor part done, benched and committed (`1f900bcf`, 7 Nov 2026), branch `261106_lorawanPing`.
Stays open, with no PR, until the BLE processor work (ww-hardware thread of the same name) is done.

## Summary

**What this thread is for.** How the WW500 sends its LoRaWAN ping, and adding a health uplink.
Victor's design of 29 Oct 2026 is in
[`WW500-LoRaWAN-health-message.md`](WW500-LoRaWAN-health-message.md). It answers the open
questions in [ww-hardware #72](https://github.com/wildlifeai/ww-hardware/issues/72) and
[Seeed #262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262).
Downlinks ([Seeed #263](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/263))
are out of scope.

___Most of the work is in the BLE processor code. The AI code needs to provide information on how full the SD card is. 
We also had to fix a bug that was not sending accurate selftest reports.___ 

**The problem (from reading the code, not yet benched).** The ping is 12 bytes. At DR2, the data rate
used after every join, AS923 with uplink dwell time allows 11 bytes, and the DeviceTimeReq MAC command
sent with each ping takes one of them. A payload that does not fit goes out as an empty frame, and the
BLE processor's state machine only logs "TODO - what now?". The server sees no content until ADR moves
the camera to DR3.

**The proposal.**

- **Ping (FPort 2) shrinks to 9 bytes** by dropping the 3-byte firmware-version field. The
  deployment record already holds both firmware versions. The backend decoder needs no change.
- **New 7-byte health uplink on FPort 4:** self-test bits (u16), SD card free %, temperature,
  last fault reason, unexpected-reset count, AI processor failure count.
- **Sent** after every join (once the AI processor's first `selftest` reply is in), when a self-test
  bit changes (at most once an hour), and every 24 h. Confirmed, and queued rather than dropped when
  the MAC is busy.
- **Website and mobile app** show health with shared status rules (Silent, Fault, Warning, Unknown,
  OK). Red states raise an alert.

**Work by repo.** Most of it is on the BLE processor (ww-hardware): shrink the ping, add the health
uplink and its triggers, queue uplinks, hold fault counts until acknowledged. Then ww-backend,
ww-website and ww-mobile-app.

**AI processor (this repo) work.** Report SD card free % to the BLE processor as a new op parameter
in the `Sleep` message (op 39, after op 0 to 38). Add it to the shared op list
(`MANIFEST/config_file.md`), which is a cross-repo contract. The BLE processor sends 0xFF until this
firmware is out. _(Superseded 7 Nov: an on-demand `sdused` command instead, see the 7 Nov proposal.)_

**Open questions for the ww-hardware team** (from Victor's doc):

- Is dropping the ping's firmware field acceptable? On 16 Oct the answer was "don't change the ping".
- Bench check: does a 12-byte ping at DR2 go out empty, and does the state machine then stay in
  REPORTING with no timer to leave it?
- Is the BLE processor's die temperature close enough to enclosure temperature to be worth a byte?
- Which resets count as unexpected: watchdog and fault only, or brown-outs too? Self-test bit 4
  (brown-out) is never set today.

## Discussion, 6 Nov 2026: getting SD free % to the BLE processor

**Proposal: ask on demand instead of op 39 in `Sleep`.** Needs Victor's agreement, as it changes his
design.

**Why not op 39.** The `Sleep` payload limit is 244 bytes on both processors (AI processor
`WW130_MAX_PAYLOAD_SIZE`; BLE processor `I2CCOMM_MAX_PAYLOAD_SIZE`, which is tied to the BLE MTU). The
theoretical worst case (39 values of 5 digits) is 240, and op 39 takes it to 244. Real messages in the
bench logs are about 125 bytes, so it would fit in practice. Two real issues remain:

- If the AI processor's `snprintf` loop (`if_task.c`, `APP_MSG_IFTASK_INACTIVITY`) ever fills the
  buffer, it silently cuts the last value mid-number, and the BLE processor stores a wrong value.
  Worth a guard (stop at the last whole value, log a warning) whatever is decided here.
- SD free % is a reading, not a setting, but op parameters are saved in CONFIG.TXT.

**On-demand design.** The BLE processor sends a request a few times a day, when it is about to build a
health message (for example with the `selftest` request). It wakes the AI processor itself, so no
capture is running and nothing touches the time to first photo. `f_getfree()` cost does not matter
then, even the 1 to 2 s full FAT scan when the card's FSINFO is invalid.

**Points to settle:**

1. **Reply format.** `info`'s reply is human-readable and starts "Label: ...". Proposed: add a
   used/free % line to `info` for people, plus a short command (e.g. `sdfree`, reply `sdfree 41`) that
   the BLE processor matches in `aiCommands[]`. Same behaviour from the console or the BLE processor.
2. **No card or `f_getfree()` failure** still replies, with 255 (Victor's 0xFF = unknown).
3. **Request during a capture.** Simplest rule: the BLE processor asks only when it woke the AI processor
   itself. (Corrected 7 Nov: there is no FatFS lock, `FF_FS_REENTRANT` is 0. See the 7 Nov proposal.)
4. **Energy.** One extra AI processor wake per request, negligible at a few per day.
5. **`info` code notes.** `used * 100` overflows 32 bits on large cards (use 64-bit). Round so that
   0 % and 100 % mean exactly empty and full. Keep existing `info` lines, add the new one at the end
   (the app may parse it). `info` is missing from `_Documentation/ble_commands.md`.

## Finding, 7 Nov 2026: SD card bit missing from `selftest` after a warm boot

The early "Wake" (1 Nov) lets `selftest` be answered before the SD mount fails, so bit 11 is lost and the
health uplink would report a missing card as OK. Cause, logs, fix and issue text:
[`CLAUDE_selftest_SD_bit.md`](CLAUDE_selftest_SD_bit.md). Logs in [`logs/`](logs/).
Raised as [#290](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/290).

## Proposal, 7 Nov 2026: SD card fullness on demand

Replaces op 39 in Victor's design, so needs his agreement. Refines the 6 Nov discussion above.

- **New command `sdused`**, reply `sdused <n>`, the same from the console or the BLE processor.
  - **n = 0 to 100:** % of the card used. 100 only when no free cluster is left (writes will fail), 0 only
    when nothing is used. Anything between is rounded, then kept within 1 to 99, so a nearly full card
    never shows as full. In practice a working card is never 0 (`MANIFEST/`, CONFIG.TXT).
  - **n = 255:** unknown. No card, mount failed, or `f_getfree()` failed. Bit 11 tells a missing card
    apart. Matches Victor's 0xFF.
- **Always registered** (`CLI-commands.c`), not in the SD-only set from `cli_fatfs_init()`, so it answers
  when there is no card. `info` stays SD-only, which is why it is "unrecognised" with no card.
- **Waits for the mount attempt** if asked early in a wake (done about 200 ms after "Wake" with a card,
  700 ms without), so the answer is never provisional (compare #290). Acceptable as the request is on
  demand, not on the path to the first photo.
- **`f_getfree()` runs in the FatFS task**, not the CLI task. `FF_FS_REENTRANT` is 0, so FatFS calls from
  two tasks are not protected. `info` already calls it from the CLI task: same risk, fix it at the same
  time.
- **`info`** gets a line `SD card: 59% used` after its existing lines, same rule.
- **Maths:** 64-bit for sectors x 100 (32 bits overflow on large cards).
- **Health byte 2** stays "SD free" as Victor defined it: the BLE processor sends 100 - n, and 255 as 255.
- **Docs:** add `sdused` and `info` to `_Documentation/ble_commands.md`.

**Coded and benched 7 Nov 2026** ([`logs/ai4.txt`](logs/ai4.txt)): `sdused 255` with no card (the request
was held until the mount had failed), `sdused 1` with a card. `sdused` in `CLI-commands.c`;
`fatfs_getSdUsedPercent()` and the new `APP_MSG_FATFSTASK_GET_SD_USED` message in `fatfs_task.c` (answered in
every FatFS task state, 255 with no card); `AI sdused` added to `ble_commands.md`.

**Dropped (Charles, 7 Nov):** the `info` % line. Moving `info`'s `f_getfree()` alone is not worth it: about 30
FatFS calls in the SD commands run in the CLI task. The fix for all of them (`FF_FS_REENTRANT` with a FreeRTOS
mutex, or inhibiting captures during transfers) is [#291](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/291).

## Outcome

**AI processor side (7 Nov 2026):**

- **SD card use on demand.** New command `sdused`, reply `sdused N`: % of the card used (100 = full, 255 =
  unknown or no card). The BLE processor asks for it when it builds a health message, instead of op 39 in
  the `Sleep` message as in Victor's design. That keeps the `Sleep` message and the op list unchanged and keeps
  the work off the path to the first photo. Works with no card. Documented in `ble_commands.md`.
- **#290 fixed:** on a warm boot `selftest` was answered before the SD card mount, so bit 11 (no SD card)
  was lost. New self-test bit 15, provisional: the early reply has it set, and the AI processor sends the final
  bits unprompted once the mount is done. The BLE processor now accepts that in every awake state (ww-hardware,
  same thread). Benched on both processors ([`CLAUDE_selftest_SD_bit.md`](CLAUDE_selftest_SD_bit.md)).

**Still to do elsewhere:** the ping and the health uplink (BLE processor), then backend, website and app. Victor
to agree `sdused` in place of op 39, and bit 15 (app decoder and health layout show it as "checking").

## Open items

- [ww-hardware #72](https://github.com/wildlifeai/ww-hardware/issues/72): BLE processor side (private repo, title not checked).
- [Seeed #262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262): Decide LoRaWAN uplink message content.
- [Seeed #263](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/263): Decide LoRaWAN downlink message content (out of scope here).
- [Seeed #290](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/290): `selftest` misses the SD card bit after a warm boot. Fixed both sides, to be closed.
- `sdused`: no issue (Charles, 7 Nov); Victor's agreement to use it instead of op 39 belongs with #262.
- [Seeed #291](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/291): FatFS is called from more than one task (`FF_FS_REENTRANT` 0). Not known to cause a problem.
