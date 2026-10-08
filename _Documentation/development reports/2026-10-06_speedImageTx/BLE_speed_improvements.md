# What the BLE processor does to speed up image transfer to the app

#### File: `BLE_speed_improvements.md`
#### Path: `_Documentation\development reports\2026-10-06_speedImageTx\BLE_speed_improvements.md`
#### Author: Claude, for Charles Palmer
#### Date: 6 October 2026

**Status, 9 Oct 2026:** a record of 6 October, now out of date. The AI processor's download
output is quiet since #264, and the BLE processor holds the fast connection interval for a
download from 0.30.57 (ww-hardware #34). See [`txfile.md`](../../txfile.md).

## Summary

The main cost on both processors is console output on the per-packet path. On the BLE processor
(the nRF52832 in the MKL62BA module, [ww-hardware](https://github.com/wildlifeai/ww-hardware)
repo) the per-packet logging is now suppressed in **both** directions. On the AI processor
(the HX6538, this repo) it is suppressed
for **uploads only** (app to SD card). A JPEG **download** (SD card to app) still prints a full
event trace and a 16-line hex dump for every 247-byte packet. That is the next thing to fix
here.

## The problem: ww-hardware issue #34

From [ww-hardware #34](https://github.com/wildlifeai/ww-hardware/issues/34) (line numbers are
as they were when the issue was written):

> What the user sees: downloading a photo from the camera to the app runs at about 1.3 KB/s:
> about 11 s for a 14 KB photo and 17 s for a 22 KB one. Sending a file the other way, from the
> app to the camera, runs at about 5.3 KB/s over the same BLE link. Most of the time a user waits
> for a capture in the app is this download.
>
> Why: for every 241-byte packet of a download, the BLE processor (nRF) prints the packet as a
> hex dump on its debug console before sending it to the app, about 1,500 characters per packet
> (aiProcessor.c:301-307). The console is a 115200-baud serial line, so the print takes about
> 130 ms, and the BLE processor waits for it to finish (NRF_LOG_FLUSH() just before the send,
> ble_actions.c:1258-1260). Uploads already skip this print; the comment above it says the dump
> inflates the per-packet time from about 15 ms to about 175 ms. But the switch that turns it
> off, g_fileTxActive, is only set for uploads (fileTx.c:380).

Section 3 below is the BLE processor's fix for this. The issue covers only the BLE processor. The AI processor has
the same pattern: its switch, `g_fileRxActive`, is also set for uploads only (see the last
section).

## What was done on the BLE processor

### 1. Uploads: release 0.30.47, July 2026

[PR #27](https://github.com/wildlifeai/ww-hardware/pull/27), branch `feature/ble-fast-transfer`,
commit `18ffc89`:

- The per-packet logging (I2C hex dumps, state machine traces, deferred-log flushes) is gated
  behind `g_fileTxActive`, which is set for the length of an upload session. The deferred
  `NRF_LOG` backend at 115200 baud dominated the per-packet time: **156 ms down to 45 ms**.
- The relay FIFO went from 4 to 16 slots, and only every 4th data ACK is forwarded to the app
  (`FILETX_ACK_EVERY`).
- Fast connection parameters (15 to 30 ms) are held for the whole transfer session.
- It was verified against this repo's `feat/ble-fast-transfer` at `3c0baafa` (where
  `g_fileRxActive` came from) and the app's `feat/ble-fast-transfer` at `5cdc45a`.

### 2. Reliability: release 0.30.51, September 2026

[PR #49](https://github.com/wildlifeai/ww-hardware/pull/49), branch
`fix/ip-int-missed-interrupts` (released through
[PR #50](https://github.com/wildlifeai/ww-hardware/pull/50)): this isn't a speed change. While
BLE is connected, the BLE processor catches the `/IP_INT` pulse with a dedicated GPIOTE channel, because
under load the low-power PORT event lost edges. A missed edge stalled a transfer until both
sides timed out. The pulse width (about 16 ms) is now a contract between the two processors.

### 3. Downloads: release 0.30.55, 4 October 2026

The fix for [#34](https://github.com/wildlifeai/ww-hardware/issues/34). Commit `98393a1`, whose
message notes that the change "might not have worked". It's on branches `260930_Charles_Testing`
and `261005_SaveState`, and **not yet merged to `dev`**. The PR is still to be opened from
`261005_SaveState`.

In `aiProcessor.c` and `ble_actions.c`:

- No log line and no hex dump for any `AI_PROCESSOR_MSG_RX_BINARY` frame received from the
  AI processor, whether or not `g_fileTxActive` is set.
- No `NRF_LOG_FLUSH()` per download packet. The deferred log drains when the main loop is idle.
- New: one summary line when a download ends (the first non-binary message after it):
  `Download: <bytes> bytes in <n> packets, <ms>ms, <rate> bytes/s`. Use it to measure any
  change made here.

The effect has **not been measured yet**. Other BLE processor per-packet log lines are still gated only
on `g_fileTxActive`, which is false during a download. A download packet is forwarded straight
from the I2C handler to BLE, without going through the AI state machine, so those lines
probably don't fire. This hasn't been confirmed.

## What is still missing on the AI processor (this repo)

In `EPII_CM55M_APP_S/app/ww_projects/ww500_md/if_task.c` (branch `261003_victorsPriorities`),
the per-packet output is gated only on `g_fileRxActive`. That flag is set at one point
(around line 1351), after the first `ftx ack 0` of an **upload**. A download
(`APP_MSG_IFTASK_I2CCOMM_CLI_BINARY_RESPONSE` / `..._BINARY_CONTINUES`) never sets it, so all of
this prints for every packet:

| Output | Where (approx.) | Gated today |
|---|---|---|
| `IF Task received event 'Binary Continues'` | line 1536 | `g_fileRxActive` only |
| `Assert` / `Negate inter-processor interrupt.` | lines 1776, 1813 | `g_fileRxActive` only |
| Hex dump of the sent buffer (about 16 lines) | `sendI2CMessage()`, line 824 | `g_fileRxActive` only |
| `Sending %d bytes: Header ...` | `sendI2CMessage()`, line 805 | not gated |
| `IF Task state changed from ...` | state change print | check |
| `I2C transmission complete.` | line 459 | not gated |

Suggested approach, matching the BLE processor: suppress by **message type** rather than by session flag.

- In `sendI2CMessage()`, skip the "Sending" line and the hex dump when the message is binary.
- Skip the event trace for `BINARY_RESPONSE` / `BINARY_CONTINUES`.
- Optionally, print one summary line at the end of a download (bytes, packets, time).

Then compare against the BLE processor's `Download:` line, before and after.

At 921600 baud a 16-line dump costs a few milliseconds per packet, and the `g_fileRxActive`
comment itself says this logging "measurably throttles the transfer loop". How much it limits
the download rate, compared with BLE and I2C, has not been measured.

## Rule worth keeping

From ww-hardware `traps.md`: **adding a log line to a packet path is a performance change.
Treat it as one.**
