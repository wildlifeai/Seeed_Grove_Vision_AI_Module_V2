# WW500 BLE File Transfer — Himax side
#### Claude (with Victor) - 11 July 2026 (branch `feat/ble-fast-transfer`)

How the HX6538 receives files (firmware images, AI models, labels, config)
from the mobile app over BLE, what was changed to make it fast and reliable,
and — importantly for reviewers — what we tried that did NOT work and why.

The nRF52832 side of the same work is documented in the ww-hardware repo
(`BLE_Fast_File_Transfer.md`); the app-side analysis lives in the mobile repo
(`documentation/development reports/fast_file_transfer_proposal.md` and
`empty_sd_update_architecture.md`).

## Where we started / where we ended

| | Before | After |
|---|---|---|
| Throughput | ~0.24 KB/s (stop-and-wait, ~1 s/packet) | 6-8 KB/s bursts, ~1.5 KB/s sustained on large files |
| 512 KB transfer | ~35 min, frequently failed | ~5 min, CRC-verified, repeatable |
| Overwriting an existing large file | failed on packet 1 (`ftx err 7`) | works |
| Full dual-image firmware update over BLE | not viable | ~10-15 min end-to-end from the app |

## Protocol recap (unchanged framing)

The app sends `FILE_START` (7) / `FILE_DATA` (8) / `FILE_END` (9) packets over
BLE; the nRF relays each as an I2C frame (max 255 B, so 241 B payload per
packet); we write to `/MANIFEST/<8.3 name>` on the SD card and ACK each packet
back (`ftx ack N`). `FILE_END` carries a whole-file CRC16-CCITT which we
verify before keeping the file (`ftx ack end` / `ftx err 9`). The speed came
from the app streaming a 12-packet window and the nRF buffering it — the
HX↔nRF exchange itself stays strictly stop-and-wait, one frame in flight.

## Changes on this branch, and the failure that motivated each

### 1. SD write path (`fatfs_task.c`)

- `f_sync` every 16 writes instead of every write (per-packet sync cost
  dominated at the old speeds).
- **`f_sync` immediately after `f_open(FA_CREATE_ALWAYS)`.** Overwriting a
  large existing file truncates it, freeing ~1000 clusters; the card is still
  busy with that housekeeping when packet 1's write arrives, and `f_write`
  returned `FR_DISK_ERR` → `ftx err 7`. Reproduced reliably by re-sending
  LARGE.BIN. The sync commits the truncate before data flows.
- Transient write errors retried 3× with 15 ms backoff. **Short writes
  (FR_OK but `bw < length`) are NOT retried**: the file pointer has already
  advanced, so a retry would duplicate bytes mid-file; and FatFs documents
  short-write-with-FR_OK as volume-full, so a retry cannot succeed anyway.
- File size cap raised to 10 MB (model files).

### 2. I2C slave "deaf window" (`if_task.c`) — the big one

**Symptom:** at streaming rates the transfer stalled dead after some tens of
packets; the nRF eventually reported "AI processor not responding". At the
old stop-and-wait pace it never happened.

**Diagnosis:** timestamped console captures showed a packet arriving with its
4-byte frame header missing — raw payload bytes where `80 08 f2 00 <pktnum>`
should be. CRC failed, the frame was silently dropped, no ACK was sent,
deadlock. Root cause: after transmitting an ACK we re-armed the I2C slave
receiver via a task-queue hop (`APP_MSG_IFTASK_I2CCOMM_TX_DONE` →
`i2cTransmissionComplete()` → `enable_read`). The nRF, with a full FIFO,
writes the next packet the instant it finishes reading our ACK — inside that
task-scheduling window. At low rates the FIFO was empty and the gap between
packets hid the race.

**Fix:** re-arm the receiver in the TX-done interrupt callback
(`i2csTxDoneEvent`) itself, before the task hop. The re-arm was removed from
`i2cTransmissionComplete()` so a packet already received cannot be clobbered
by a second `enable_read`.

### 3. Console logging throttles the packet loop

Each packet produced ~16 console lines (hex dumps of the frame and the ACK,
IF/FatFS task state and event traces) at 921600 baud. Measured effect: the
device-side per-packet time was dominated by logging, not the SD write
(3 ms). All per-packet logging is now suppressed while a transfer session is
active (`g_fileRxActive`, set at FILE_START, cleared at session end and reset
by the DPD reboot) and normal at all other times. The same class of fix was
needed on the nRF (deferred NRF_LOG flushing) and even in the app (a Redux
log store growing per ACK) — every layer's "harmless" per-packet logging
became the bottleneck once the layer below it got faster.

### 4. Missing-master window 1 s → 4 s

While streaming, the Android BLE stack renegotiates connection parameters at
certain points; the link stalls ~950 ms. Our "I2C master did not read our
message" watchdog was 1000 ms — right on the edge — and a firing watchdog
tears the session down. 4 s rides the renegotiation out while staying well
under the 15 s session inactivity hold and the app's 15 s abort.

**What did NOT work:** the app originally re-requested Android's HIGH
connection priority every 20 s to keep the fast interval. That renegotiation
did not just stall the link — it desynchronised the nRF↔HX exchange badly
enough that transfers died at ~25 s no matter how long this window was. The
refresh was removed app-side; the cost is that Android drops from ~8 KB/s to
~1.3 KB/s after ~25 s (OS behaviour, phone-specific), which we accept for
reliability. This is why "fast bursts, slower sustained".

### 5. Session inactivity hold

A transfer session holds off DPD (15 s inactivity extension at FILE_START,
restored at close/abort) so the device does not sleep between packets.

The hold was originally 5 s, which iOS defeated (bench 19 Jul 2026, app
0.0.60 with write-with-response): ~28–30 s into a large transfer iOS
renegotiates the connection interval and CoreBluetooth stalls the in-flight
write for >5 s. The device saw 5 s of silence, logged
`WARNING: incomplete file transfer — closing before DPD`, and slept at the
exact moment the phone recovered — LARGE.BIN died at packet 96/2125, while
the same transfer completes on Android (its stalls are sub-second). 15 s
matches the app's own silence budget: both ends now tolerate the same
worst-case gap, so a renegotiation stall is a pause, not a death. An
abandoned session costs one delayed DPD entry, nothing more.

### 6. Slot-label robustness (`camera_switch.c`)

Seen once in the field: after a dual-image firmware update the freshly
flashed image's brief mid-update boot lost its slot-label write silently, and
the app's camera switching was stuck on "unknown" with no path forward.
`cameraSwitch_labelBootSlot()` failure paths are now loud on the console and
the flash write retries once. (The app now also allows an explicit-confirm
switch into an unlabelled slot, since `xip_switch_slot()` itself refuses
slots without a valid secure-boot image.)

### 7. New defaults: OP26 = 1, OP22 = 50

- `OP_PARAMETER_SLOT_SWITCH` (op26) defaults to **1**: automatic day/night
  camera switching after each AE light check. Requires both slots labelled —
  a dual-image update leaves them so.
- `OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT` (op22) defaults to **50**: 5 %
  proved too dim for night-time motion detection. Power note: the IR LED is
  STROBE-gated by the HM0360 to each MD frame's integration window (~15 ms
  pulses, and only when the AE decision says dark) — brightness scales the
  pulse amplitude, not the duty cycle. The LED current at 50 % vs the battery
  budget still wants a hardware sanity check.

## What to test when reviewing

1. 512 KB transfer (app "File Transfer Test" screen, sliding window) —
   completes, CRC verified; repeat immediately (overwrite case).
2. Full cloud firmware update from the app (both images, empty SD card).
3. Interrupt a transfer mid-stream (walk away / kill app) — device cleans up
   via the session watchdog + inactivity, next transfer works.
4. Console noise: per-packet logs absent during a transfer, present outside.

## Known limits / future work

- No transfer resume: an interrupted file restarts from packet 1 (worst case
  ~5 min lost). Worth adding only if >1 MB models become routine.
- Android drops to ~1.3 KB/s after ~25 s (see §4). iOS unmeasured.
- `firmware <file> [0xCRC]` always writes the inactive slot; updating both
  variants needs the flash→reset→flash dance. A slot-targeted variant would
  halve the wall time.
- Bootloader behaviour on a corrupted slot-selector sector (power loss during
  the ~ms selector write) is untested — worth a bench session.
