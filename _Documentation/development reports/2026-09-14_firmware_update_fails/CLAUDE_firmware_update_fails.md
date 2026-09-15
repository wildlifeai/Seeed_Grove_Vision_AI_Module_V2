# Task: Fix formware update failure

#### File: CLAUDE_firmware_update_fails.md
#### Author: Charles Palmer
#### Date: 14 September 2026

## Background

The system supports updates of the AI processor firmware via the app. The app collects 
the latest firmware images (one for each camera) and attempts to install them on the 
AI processor using the 'AI firmware<filename>' command.

The process was introduced in PR #142 and is described in part in ww500_md/doc/WW500_ble_file_transfer.md

This fails, at least in some situations.

## Evidence of Failure

Two log files are placed in this folder:

* `ble_log.txt` - the console output of the BLE processor
* `ai_log.txt` - the console output of the AI processor

The SD card contains a partial file: R6905J22.IMG, 39k size.

The app shows some (expected) messages, then "Error: No transfer response for 15 seconds - device may be stuck"

## Possible Cause

Since the tranfers presumably works with Victor's setup but fails with mine, the differences could be:

1.	Different speeds of BLE transfer. I have an older phone which might have negotiated smaller BLE packet sizes.
2. 	Different SD card write speeds. Different card are known to take different times to write data.

Transfer of packets are:
```
app -> BLE processor -> AI processor -> SD  card
```
And some acknowlegements flow back in the opposite direction. 
However I suspect that at least some of these transfer steps occur without 
acknowledegments and rely on assumtions that acks are unnecessary. Such assumptions might be invalid.

## Analysis Required

1.	Claude should attempt to understand the cause of the failure, and document this.
2.	Claude should propose a fix, but not until the analysis is complete.
3.	Claude should ask any questions to help find the fault.
 
 ## Tools
 
 The code for this large file transfer was created by Claude at Victor's site. It may be that the other Claude
 was able to monitor the console output of both devices by itself. 
 Tools may exist for this - e.g. in th e`_Tools` folder.
 
 This Claude may use such tools (if they exist) - but ask first. I might need to shut down
 teh terminal emulators to facilitate this.



## Progress so far (Claude, 15 September 2026)

Two bugs found and fixed while investigating (both in `ww500_md`, committed
`5452fcde` on `firmwaretx`):

1. **Firmware update silently "succeeds" with no SD card fitted.** `fatfs_task.c`'s
   uninitialised-state message handler reported the SD-card-absent error in the wrong
   place (a message field nothing reads instead of the struct field `if_task.c`
   actually checks), so the AI processor sent `ftx ack 0` for every packet regardless.
   Fixed so the app now gets `ftx err 6` and aborts instead of transferring the whole
   file into nowhere.
2. **Build date/time on the console went stale on incremental Windows builds.** The
   existing force-rebuild rule for `ww500_md.c` (which prints `__TIME__`/`__DATE__`)
   targeted the wrong object path once builds redirect to `D:\hxbuild`. Fixed in
   `ww500_md.mk`.
3. **`fatfs_mounted()` stayed "true" after the SD card was unmounted for sleep.**
   The flag it reads was only ever set in the mount routine, never cleared when the
   card is unmounted before DPD. Fixed (`fatfs_task.c`).

Not yet resolved — parked, to come back to: the original reported failure (BLE
transfer stalling mid-stream with a card actually fitted, `ble_log.txt`/`ai_log.txt`)
is a separate issue, still open. Working hypothesis: `MISSINGMASTERTIME` (4000 ms)
being exceeded by connection-interval renegotiation stalls on the test phone, with no
ack retry on a dropped packet.

## Open issue: sleep can race a starting file transfer

A second, distinct failure was found on 15 Sep testing with the SD card actually
fitted: a firmware-update transfer can fail with `ftx err 6` (file open failed) even
though nothing is wrong with the card.

**What happens:** the device goes to sleep after a short 2-second gap of no BLE
activity, and as its very last step before sleeping it unmounts the SD card. If the
app happens to send `FILE_START` in that same narrow window — after the device has
already started falling asleep but before its "keep the file transfer awake" logic has
had a chance to run — the file-open request arrives just as (or just after) the card
is unmounted, and genuinely fails.

In the captured case, the app took about 2.1 seconds after its last exchange with the
device (`slots`) before sending `FILE_START` — just over the device's 2-second
inactivity timeout — so the device had already begun shutting down when the transfer
request arrived, missing it by roughly 200ms.

**Not yet fixed.** Candidate approaches: lengthen the inactivity window before sleep,
or make the device's sleep sequence aware of (and pause for) an in-flight BLE request.
Needs a decision before implementing.

## Progress (Claude, 16 September 2026): BLE/AI log correlation, and a dead-code finding

Returned to the parked mid-stream stall (previous section), now with a correct
branch checkout of the BLE processor code (`ww-hardware` repo — the earlier session
had been looking at a branch missing `Ai_Event_ResponseTimeout`). Correlated
`ai_log.txt` and `ble_log.txt` timestamp-for-timestamp across three stall episodes.
Each episode shows total silence on both sides, followed by each processor's own
*independent* timeout firing separately (AI's `MISSINGMASTERTIME` = 4000 ms,
`if_task.c:74`; BLE's `AI_RESPONSE_TIMEOUT_MS` = 10000 ms, `aiStateMachine.c:73`) with
no NACK/I2C-error path triggered on either side. Leading hypothesis: a TWI driver race
on the BLE side — `aiProcessorTxBinaryMsg()` (the FILE_DATA send path, `aiProcessor.c`
~line 829) has no busy check before calling `nrf_drv_twi_tx()`, and `twi_handler()`
unconditionally calls `twi_master_uninit()` as its first line (the "anomaly 89" fix) —
so a dispatch racing a not-yet-completed prior transfer can wedge the peripheral with
no callback ever firing. Connection-interval renegotiation and I2C bus/electrical NACK
were both ruled out (neither occurs in/near the stall windows). Not yet fixed — next
step, not yet implemented, is an ungated busy/event counter (no per-packet print, so it
won't perturb the timing) to confirm or rule out the race.

While reading `if_task.c` for this, found unrelated dead code: **`APP_IF_STATE_I2C_SLAVE_TX`
and `APP_IF_STATE_I2C_SLAVE_RX` are unused states.** `if_task_state` is never assigned
`APP_IF_STATE_I2C_SLAVE_TX` anywhere at runtime — the only line that would do so is
commented out (`if_task.c:874`, under a TODO: "think carefully whether we need this
state..."). The AI-initiated send path (`APP_MSG_IFTASK_MSG_TO_MASTER`, e.g. `ftx ack N`)
was instead wired to the existing `APP_IF_STATE_I2C_TX` state (line 875), shared with
MKL62BA-initiated exchanges. Since the SLAVE_TX state is never entered, its handler
`handleEventForStateI2CSlaveTx()` (`if_task.c:1170-1232`) never runs — including the
`APP_IF_STATE_I2C_SLAVE_RX` assignment inside it (line 1180) — so `APP_IF_STATE_I2C_SLAVE_RX`
and its handler `handleEventForStateI2CSlaveRx()` (`if_task.c:1240-1280`) are dead too,
transitively. Both dispatcher cases (`if_task.c:1591-1594`, `:1596-1599`) and the two
enum values (`if_task.h:47-48`) are included in the removal.

**Plan: remove this dead code, in stages, gently.** Charles is committing the working
tree first as a checkpoint before any of it is touched, so the pre-removal state is
recoverable. The removal procedure itself (order of edits, how to verify nothing was
silently reachable some other way) is still to be agreed before Claude touches anything.

## Progress (Claude, 16 September 2026): dead SLAVE_TX/RX states removed

Before removing, checked two more states Charles queried against the same suspicion:
**`APP_IF_STATE_PA0` and `APP_IF_STATE_DISK_OP` are both live, not dead.** `DISK_OP` is
assigned from six sites in the `AI_PROCESSOR_MSG_FILE_START`/`FILE_DATA`/`FILE_END`
handlers and its handler `handleEventForStateDiskOp()` is exactly the file-receive
protocol (`ftx ack N`/`ftx err N`) seen firing in `ai_log.txt` during the `LARGE.BIN`
transfer above. `PA0` is gated behind `#ifdef TEST_INT_PULSE`, but that macro is
unconditionally defined (`if_task.c:56`) in every build, and it's reachable from the CLI
`int` command (`CLI-commands.c:1341`) — live, just narrow-purpose (manually pulsing the
inter-processor interrupt line to test the handshake). Neither touched.

Removed `APP_IF_STATE_I2C_SLAVE_TX`/`APP_IF_STATE_I2C_SLAVE_RX` and everything only
reachable through them, in `if_task.h`/`if_task.c`:

- Both enum values, and the two dispatcher `case` labels in `vIfTask()`'s switch.
- The whole `handleEventForStateI2CSlaveTx()` and `handleEventForStateI2CSlaveRx()`
  functions (including their forward declarations) — this also removed the unreachable
  duplicate of the `"I2C master did not read our I2C message"` print that lived only in
  the dead `SlaveTx` copy; the live copy (`handleEventForStateI2CTx()`) is untouched.
- The two now-orphaned entries in `ifTaskStateString[]`. Since that array is indexed
  positionally by the enum (not designated initializers), removing two values from the
  middle required renumbering the survivors (`PA0` 0x0006→0x0004, `DISK_OP`
  0x0007→0x0005, `NUMSTATES` 0x0008→0x0006) to keep the array aligned — this changes the
  numeric state code shown in console logs (e.g. "Disk Op State" was `(7)`, now `(5)`),
  which is a pure display value with no other consumer (`ifTask_getState()`'s only
  external callers, in `ww500_md.c`, are generic per-task status callbacks that print
  whatever they're given).
- The stale commented-out `if_task_state = APP_IF_STATE_I2C_SLAVE_TX;` line and its TODO
  in the `APP_MSG_IFTASK_MSG_TO_MASTER` case, replaced with a comment explaining why that
  path shares `APP_IF_STATE_I2C_TX` instead.

Checked and left alone: every `APP_MSG_IFTASK_*` event the dead handlers touched
(`TX_DONE`, `MM_TIMER`, `ERR`, `INACTIVITY`, `CLI_STRING_RESPONSE`, `RX_READY`) is still
used by live handlers elsewhere, so nothing in `app_msg.h` needed to change.

**Both camera variants (`cis_imx708`, `cis_hm0360`) built and ran clean on device, 16
September 2026.** Charles is committing this as a checkpoint; the mid-stream BLE-transfer
stall (the actual open problem — see the two sections above) is still unfixed and is
what we return to next.

