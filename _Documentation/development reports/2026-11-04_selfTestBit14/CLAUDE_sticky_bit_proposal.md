# Self-test bit 14: proposal and tests

#### File: `CLAUDE_sticky_bit_proposal.md`
#### Path: `_Documentation\development reports\2026-11-04_selfTestBit14\CLAUDE_sticky_bit_proposal.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 4 November 2026

## 1. What changes

Bit 14 becomes a record that the 300 ms check fired, kept until a `selftest` reply has carried it.
AI processor only; no change to the BLE processor or the app.

| | Today | Sticky |
|---|---|---|
| Set | First message after boot not read within 300 ms (`BLE_PROBE_TIME`; `if_task.c`, MM_TIMER while `bleProbePending`) | Same |
| BLE processor reads that message late (300 ms to 4 s) | Clears the bit | Bit kept |
| BLE processor sends any command | Clears the bit | Bit kept |
| `ble clear` console command | Clears the bit | Bit kept |
| `selftest`, from the BLE processor or the console | No change, so the BLE processor's reply says 0 | Reply carries the bit, then it is cleared |
| Reboot (DPD wake or reset) | Cleared (RAM) | Same |

In practice the BLE processor is the one that reads it: it sends `selftest` about 100 ms after reading the
wake message, before anyone at the console could.

`bleUnresponsive`, the flag that stops messages to the BLE processor, keeps working as now: it is still
cleared when the BLE processor turns up, so messages resume. Only the bit is decoupled from it.

## 2. Code

- `if_task.c`: remove `selfTest_clearErrorBits(1 << SELF_TEST_AI_NO_BLE)` from the RX_READY path (BLE
  processor sent a command), the late TX_DONE path, and `ifTask_clearBleUnresponsive()` (`ble clear`).
  Update the comment on `bleUnresponsive`.
- `CLI-commands.c`, `prvGetSelfTest()`: after building the reply, clear bit 14. Same for every caller: no
  check of where the command came from.
- `_Documentation/ble_commands.md`: describe bit 14 as kept until a `selftest` reply has carried it.

## 3. Decisions

Three separate decisions, not alternatives to each other. All three decided by Charles on 4 Nov 2026.

**Background.** `ble` was added on 28 Sep 2026
([2026-09-17_BLE_processor_unresponsive](../2026-09-17_BLE_processor_unresponsive/README.md)) for
running `ww500_md` on a board with no BLE processor, where every message waited 4 s and the camera never
entered DPD. It is a console diagnostic: no script uses it, and the app has no need for it (an app that
can send `AI ble` has a working BLE processor). `selftest` is also sent over the console by the ship check
script `_Tools/ww500_ship_check.py`, which records the four hex digits in `summary.csv` (it fails a board
only on the camera bits, not bit 14).

1. **Should a console `selftest` clear the bit? Decided (Charles, 4 Nov): yes.** Any `selftest` clears it,
   wherever it came from. A command that behaves differently depending on its source is untidy, and the
   gain is small: the BLE processor asks about 100 ms after the wake message, before a console reading
   could. The cost: on a board with no BLE processor the console shows `4000` once, then `0000` (`ble`
   still shows the flag).
2. **Should `ble clear` clear the bit? Decided (Charles, 4 Nov): no**, it clears only the flag (today it
   clears both). Then the bit has one rule: only a `selftest` reply clears it.
3. **When is the bit cleared: when the reply is built, or when the BLE processor has read it? Decided
   (Charles, 4 Nov): when it is built.** It is a one-line change. Clearing on the read would mean tracking the
   reply through the IF task to TX_DONE; the only gain is the rare case where the BLE processor sends
   `selftest` and then fails to read the reply.

Not covered, by design: if the BLE processor never reads during the wake, the bit is lost at sleep. The BLE
processor reports a dead AI processor itself (bit 1, `SELF_TEST_AI_PROC`, and its WAKE retry).

## 4. Bench tests

Logs go in `logs/` as `aiN.txt` and `bleN.txt` pairs. Bit 14 is `4000` in the `selfTest` reply (other bits
may also be set, e.g. `4800` with no SD card; only the `4` matters here).

### Test 1: normal wakes (nothing should change)

Normal build, both processors connected.

1. Let the camera wake a few times (motion or timer).
2. In the AI log, look for `BLE processor did not read the first message`. Expected: not there.
3. In the BLE log, find each `selftest` reply. Expected: `selfTest 0000` (bit 14 clear).
4. On the AI console, while awake, type `selftest`. Expected: `selfTest 0000`.

Pass if bit 14 is never set.

**Result (4 Nov, `logs/ai1.txt` + `ble1.txt`): PASS.** RP3 image (`Camera: RP v3 (IMX708)`), built
19:11:35 4 Nov with this change. 3 motion wakes. No probe messages in the AI log. Each wake the BLE
processor sent `selftest` 80 to 210 ms after the wake message and got `selfTest 0000` (BLE log: `AI self
test bits = 0x0000`). Step 4 (console `selftest`) not done; test 3 covers the console. The BLE log's
`BLE out: Failed to send ... 'Error bits = 0x0000'` is expected: no phone was connected.

### Test 2: BLE processor late (the case this change is for)

**How the fault is simulated.** The AI processor's first message after each boot is a probe: if the BLE
processor has not read it within `BLE_PROBE_TIME` (300 ms), bit 14 is set. Since the `/IP_INT` handshake
(3 Nov) the message then stays waiting for up to 4 s, so a late BLE processor still reads it. A healthy BLE
processor reads it in tens of milliseconds, so the bit never fires on the bench. The test build sets
`#define BLE_PROBE_TIME 1` in `if_task.c`: the probe now always times out after 1 ms, but the BLE processor
reads the message at its normal time, just as a late BLE processor would. Everything after that (the late
read, `selftest`, the reply, the app) runs as it would for real. Undo after the test.

**How it could happen for real.** Anything that keeps the BLE processor from reading for more than 300 ms
after the AI processor boots:
- the BLE processor is busy: erasing or writing flash (FDS), or handling a LoRaWAN transmit or receive;
- the BLE processor is itself still starting, e.g. both processors powered up together, or the BLE
  processor reset by its watchdog;
- the BLE processor misses the `/IP_INT` signal and only reads at its 3 s check (the missed-interrupt fault
  of 3 Nov, fixed in ww-hardware; see `2026-11-01_AI_RTC`);
- no BLE processor, or one that has stopped (then nothing carries the bit, see test 3).

Only the first three can reach the app, because the BLE processor recovers within the same wake.

1. Let the camera wake 3 times.
2. In the AI log, each wake should show, in this order:
   - `BLE processor did not read the first message within 1ms: treating it as unresponsive`
   - `BLE processor read the first message late: no longer treated as unresponsive`
3. In the BLE log, find the `selftest` reply for each wake. Expected: bit 14 set (`selfTest 4000`).
   Before the change it says `0000`: this is the bug.
4. After the BLE processor's `selftest` reply, on the AI console type `selftest`. Expected:
   `selfTest 0000` (the reply to the BLE processor cleared it).
5. If the app is connected: it should show bit 14 once per wake.

Pass if every wake's BLE `selftest` reply has bit 14 set and the console shows it cleared afterwards.

**Result (4 Nov, `logs/ai2.txt` + `ble2.txt`): PASS.** RP3 image built 19:19:24 with `BLE_PROBE_TIME 1`.
5 boots: a cold boot, 3 motion wakes, and a wake for `AI ver` from a phone connected at 19:23:12.
- Every boot: `did not read the first message within 1ms`, then `read the first message late` 0 to 40 ms
  later (the late read no longer clears the bit).
- Every boot the BLE processor's `selftest` got `selfTest 4000` (BLE log: `AI self test bits = 0x4000`), 5/5.
  Before the change this was `0000`.
- **The bit reached the app.** With the phone connected, the BLE processor passed it on: `BLE out: Sent
  20 bytes: 'Error bits = 0x4000'`. Without a phone the same message fails to send, as in test 1.
- Console `selftest` after the last wake: `selfTest 0000` (the reply to the BLE processor cleared it).
- Not exercised: the "BLE processor sends a command" path, because the late read always cleared the flag
  first (`has contacted us` never printed). Same one-line removal, checked by reading the code.

Set `BLE_PROBE_TIME` back to 300 before test 3.

### Test 3: no BLE processor (`ble clear` keeps the bit, `selftest` clears it)

Normal build. Use the board with no BLE processor, or halt the BLE processor in the debugger.

1. Reset the AI processor.
2. In the AI log: `BLE processor did not read the first message within 300ms`.
3. Type `ble`. Expected: `BLE processor unresponsive: messages to it are not sent`.
4. Type `ble clear`. Expected: `BLE processor flag cleared: messages to it will be sent`.
5. Type `selftest`. Expected: `selfTest 4000` (`ble clear` cleared the flag, not the bit).
6. Type `selftest` again. Expected: `selfTest 0000` (the first reply carried it, so it was cleared).

Pass if step 5 shows `4000` and step 6 shows `0000`.

**Result (4 Nov, `logs/ai3.log`, AI log only): PASS.** RP3 image built 19:24:32, `BLE_PROBE_TIME` back
to 300 (log: `within 300ms`). No BLE processor answering, and no cameras either (`HM0360 not present`,
`Main camera not present`), so bits 8 and 9 are also set (`0x0300`); only the `4` matters here.
- Cold boot and an RTC timer wake: both `did not read the first message within 300ms`, then `I2C master did
  not read our I2C message` at 4 s (the message waited, as since 3 Nov). Both entered DPD.
- `ble`: `BLE processor unresponsive: messages to it are not sent`.
- `ble clear`: `BLE processor flag cleared: messages to it will be sent`.
- `selftest`: `selfTest 4300` (bit 14 kept by `ble clear`).
- `selftest` again: `selfTest 0300` (bit 14 cleared by the first reply; the camera bits stay, as they should).
- After `ble clear` the "Sleep" message was sent again, not read for 4 s, then DPD (`the Sleep message was not
  read`): expected, the flag was cleared.
