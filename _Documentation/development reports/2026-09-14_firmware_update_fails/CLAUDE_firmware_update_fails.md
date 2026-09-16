# Task: Fix Firmware Update Failure

#### File: CLAUDE_firmware_update_fails.md
#### Author: Charles Palmer
#### Date: 14-17 September 2026

## Summary

After debugging the problem seems related to the BLE processor not always seeing the interrupt on the GPIO pin
from the AI processor. Fixed by changing interrupt code in the BLE processor.

At the same time, we cleared out some unused code and states, fixed bugs, and identified a new
issue to be dealt with as a separate task (Understand and fix Unresponsive BLE processor).

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

## Progress (Claude, 16 September 2026): AI-side ISR/FreeRTOS-API bug found and fixed

Follow-up question on the `MISSINGMASTERTIME` print from the log-correlation section
above: is it a genuine 4.0s timer expiry, does traffic actually flow before it fires,
and is there a bug in the timer itself? Checked directly against the code:

- The print corresponds exactly to `timerHndlMissingMaster`, a one-shot FreeRTOS timer
  (period `MISSINGMASTERTIME` = 4000ms, `if_task.c:74`), started in
  `i2ccomm_write_enable()` (`:792`, now `:798` after the fix below) every time this
  processor sends anything, and reported via `missingMasterExpired()`
  (`:1888`-ish)/`APP_MSG_IFTASK_I2CCOMM_MM_TIMER`. All three captured stall episodes
  fired 4.00-4.02s after their triggering send — exactly on schedule, no drift.
- Confirmed many prior successful exchanges before each stall (episode 1: at least 8
  ack round-trips succeeded in the ~2.6s immediately before it) — an intermittent
  mid-stream failure, not a cold-start bug.
- Found a real bug while checking the timer's own management: the *only* place that
  stops this timer, `i2csTxDoneEvent()` (`if_task.c:307`) — the I2C driver's slave-TX-done
  callback, registered into `hx_lib_i2ccomm_init()`'s callback table and documented in
  its own comment as running "in the ISR context" — called the **non-ISR** `xTimerStop()`
  instead of `xTimerStopFromISR()`. Calling a task-context-only FreeRTOS API from real
  interrupt context is undefined per FreeRTOS's own rules. The failure path is guarded
  by `configASSERT(0)`, and this build's `configASSERT` (`FreeRTOSConfig.h`) is a hard
  hang (disables interrupts, spins forever) rather than a no-op — so this specific call
  evidently didn't outright fail during the captured episodes (no hang observed), but
  "didn't fail this time" isn't proof it's safe.

**Exhaustive check for the same mistake elsewhere:** identified every function in
`ww500_md` (36 `.c` files) actually registered as a hardware ISR callback — via driver
registration calls (`hx_drv_gpio_cb_register`, the `I2CCOMM_CFG_T` callback table) and
the code's own "ISR context" doc comments — then checked each for FreeRTOS calls that
have `FromISR` counterparts. Found exactly 5 such callbacks, all in `if_task.c`:
`i2csTxDoneEvent()`, `i2csRxReadyEvent()`, `i2csErrorEvent()` (I2C driver callbacks), and
two board-variant (`#if`/`#else`, WW500.A00 vs A01) copies of `interprocessor_interrupt_cb()`
(GPIO ISR for `/IP_INT`). Only `i2csTxDoneEvent()` had the bug — the other four already
correctly use `xQueueSendFromISR()` + guarded `taskYIELD()`. Not checked: HX SDK/driver
internals (vendor code) and `ww130_cli`'s separate, less-active copy of `if_task.c`.

**Fixed** (`if_task.c:307-343`): `xTimerStop()` → `xTimerStopFromISR()`. Also initialised
`xHigherPriorityTaskWoken = pdFALSE` at declaration — it was previously read
uninitialised whenever `xQueueSendFromISR()` didn't need to set it (that function, like
all FreeRTOS `...FromISR` calls, only ever writes `pdTRUE` into the flag, never resets it
to `pdFALSE`), and now that two `FromISR` calls feed the same flag in sequence this was
no longer just latent but load-bearing.

**Not yet fixed / worth a decision:** the same uninitialised-`xHigherPriorityTaskWoken`
pattern (declared but never explicitly set to `pdFALSE`) exists in the other 4 ISR
functions too, each of which only makes one `FromISR` call. There it's lower-stakes (only
ISR: harmless spurious yield, or in the worst case a missed one delaying that queue
message's processing until the next scheduling point — a latency effect, not corruption
or a hang) since there's nothing to OR together — but it's the same class of relying on
unspecified stack contents. Not fixed pending Charles's call on whether it's worth
touching now or leaving alone.

Whether this bug is a contributor to the actual open stall (the TWI-race hypothesis on
the BLE side, documented above) is still unknown — it's a separate, independently-found
correctness issue on the AI side. Returning to the main BLE-stall problem next.

## Progress (Claude, 16 September 2026): duplicate-reschedule mystery resolved, retest shows big improvement, and a new pkt-time-variability lead

**Instrumented** `aiStateMachine.c` (BLE processor, `ww-hardware` repo) behind a single
`TXFILE_RESCHEDULE_DIAG` macro (`#ifdef`/`#else`, trivially revertible): logged the
*triggering* event at the file-FIFO-drain reschedule site (`aiStateMachine.c:897-907`),
and gated `deferredAiBump()`'s previously-ungated synchronous log+flush behind
`!g_fileTxActive` (`:718-737`), matching the sibling "bumped with..." print a few lines
into `aiStateMachine_bump()` that was already gated for exactly this cost reason.

**Retest (`ai_log_4.txt`/`ble_log_4.txt`) vs. a same-day baseline on the old build
(`ai_log_3.txt`/`ble_log_3.txt`, 0 diagnostic lines, still 9 stalls — confirms it's a
fair before/after):**

- Stall count dropped from 9 to **1** on the new build.
- Worst per-packet round-trip ("pkt time") dropped from 478ms to **76ms** — no more
  multi-hundred-ms outliers outside the one real stall.
- The "duplicate reschedule" mystery is resolved: 170 of 172 diagnostic lines are the
  identical, correct, one-per-real-ack case (`trigger event 14 'TxFile response'`,
  `oldState PROCESSING`) — the design working as intended, not a bug. The 2 exceptions
  are the one real stall's recovery (`event 15 'Response timeout'`) and one harmless
  startup RTC-sync exchange (`event 6 'UTC Rx'`). The earlier appearance of "many
  identical prints in a burst" was real acks that had backed up during the (now-removed)
  synchronous-flush cost and were drained in a rapid catch-up once unblocked — not
  duplicate scheduling.
- The one remaining stall is structurally identical to the earlier ones (silence, then
  each side's own independent timeout, no NACK/error path) — still unexplained by this
  instrumentation; the TWI-race hypothesis (above) remains the leading candidate.

**New lead on the *other* symptom (pkt-time variability, distinct from the full stalls):**
traced exactly what "pkt time" measures (`fileTx.c:652`/`:706`/`:756`, BLE side) — the
full round trip from BLE dispatching a FILE_DATA chunk to receiving its ack, sampled only
1-in-4 (`FILETX_ACK_EVERY = 4`) and reflecting only the *last* of each sampled group of 4
(`startPacketTime` is overwritten on every dispatch, not just sampled ones). Checked
whether the AI processor's 241-byte I2C chunks get aggregated before hitting the SD card:

- Our own code (`fatfs_task.c:859`) does one `f_write()` per I2C packet, no batching.
- FatFS itself does aggregate: `ffconf.h` confirms `FF_FS_TINY = 0` (each open file has
  its own 512-byte write-behind buffer), so only roughly every 2nd `f_write()` actually
  reaches the card.
- Our own code separately forces an `f_sync()` every 16 writes
  (`TRANSFER_WRITES_PER_SYNC`, `fatfs_task.c:183`) — a much heavier FAT+directory flush,
  run synchronously before that packet's ack goes out. 16 is an exact multiple of the
  BLE side's 4-packet sampling interval, so **every periodic sync boundary is guaranteed
  to land on a sampled pkt-time value** — a strong structural candidate for the recurring
  37-76ms bumps (as opposed to the one full 10s stall, a separate phenomenon).
- Ruled out with direct evidence: the pre-existing transient-SD-busy retry path
  (`fatfs_task.c:854-873`, up to 3 retries with a 15ms delay, logs
  `"SD write err %d, retry %d/3"` when it fires) never fired in either `ai_log_3.txt` or
  `ai_log_4.txt` — not the explanation here.

**Two small changes made as a result, not yet retested:**
1. `fatfs_task.c:183` — added a comment noting `TRANSFER_WRITES_PER_SYNC = 16` was a
   deliberate fix for a real prior bug (unbounded dirty state causing non-deterministic
   `"ftx err 7"` on transfers beyond ~3-7KB, per the existing comment there), but is worth
   retuning (larger, e.g. 64/128) now that we have direct cost evidence — flagged as
   needing a retest for `"ftx err 7"` recurrence before loosening, not changed yet.
2. `if_task.c` — added `SLOW_DISK_OP_WARNING_MS` (100ms) and a warning print in the
   `APP_MSG_IFTASK_DISK_WRITE_COMPLETE` handler (`:1243-1254`): the existing per-operation
   timing print is normally fully suppressed during a transfer (`g_fileRxActive`), so we
   currently have zero visibility into individual SD-write duration for the bulk of any
   transfer; this surfaces only the rare slow one (by construction infrequent, so
   shouldn't itself perturb the timing it reports on) without reintroducing the
   ~40ms/packet cost of printing every operation.

## Progress (Claude/Charles, 16 September 2026): likely root cause found and fixed on the BLE side — /IP_INT pulse too short for a low-priority, low-accuracy GPIO interrupt

Charles asked how the AI-side `MISSINGMASTERTIME` handshake actually works mechanically
(assert/negate the shared `/IP_INT` line, BLE reacts on the rising edge). Tracing the
BLE-side detection path (`ww-hardware` repo) found it uses the nRF52832's low-power
**PORT event** mechanism (`hi_accuracy = false`, hardcoded in
`WW500-C02/gpio-board.c`), at NVIC priority 6 (low — the code's own `IRQ_HIGH_PRIORITY`
request is documented as ignored: "priority is set in sdk_config.h"). Traced Nordic's
actual PORT-event handler: it only recognises a transition if the pin is still at the
new level when the CPU eventually services the shared event — a pulse shorter than that
servicing latency under load (active BLE + TWI traffic, exactly a file transfer) is
**silently missed, not delayed**.

Charles added a 1ms `vTaskDelay()` either side of driving the pin high in
`interprocessor_interrupt_negate()` (AI side, stretching the pulse) as a quick empirical
test: 3 runs (log files 8/9/10) gave 0, 1, and 0 stalls respectively — down from the
usual 6-9 per run, strong support for the theory, though not a full fix.

**Fix implemented on the BLE side (`ww-hardware` repo, not yet tested)**: `/IP_INT` now
switches to a dedicated, hardware-latched GPIOTE channel (`hi_accuracy = true`) only for
the duration of a file-transfer session, reverting to the default low-power interrupt
afterward — mirrors the existing `ble_actions_setFastConnParams()` session lifecycle in
`fileTx.c`. New `GpioSetInterruptHiAccuracy()` in `gpio-board.h`/`WW500-C02/gpio-board.c`
(other boards unaffected), new `aiInt_setHighAccuracy()` in `main.c` (declared in
`aiProcessor.h`), called from `fileTx.c` alongside the connection-parameter switch. Kept
transfer-scoped deliberately: a dedicated GPIOTE channel costs a small continuous current
draw, negligible during an already power-hungry transfer but not worth paying 24/7 on a
battery/solar device that spends most of its life asleep. Charles is removing the 1ms
pulse-stretch delay to test this fix in isolation.

