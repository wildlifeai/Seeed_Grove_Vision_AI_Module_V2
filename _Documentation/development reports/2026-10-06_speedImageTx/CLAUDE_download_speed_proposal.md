# Proposal: faster file transfer from the SD card to the app

#### File: `CLAUDE_download_speed_proposal.md`
#### Path: `_Documentation\development reports\2026-10-06_speedImageTx\CLAUDE_download_speed_proposal.md`
#### Author: Claude, for Charles Palmer
#### Date: 6 October 2026

## Summary

A download is strictly stop-and-wait, one 241-byte chunk at a time, through the CLI.

**Status, 7 October 2026:** the AI processor work is done. The transfer is instrumented, and
the per-packet console output is silenced (18.5 ms down to 0.5 ms per packet). The SD card read
is 0.4 ms per packet. The download still runs at about 1.5 KB/s, because the link moves one
packet per BLE connection interval, about 155 ms. Faster downloads now need work in the BLE
processor code, and maybe the app. See
[`Fast_SD_to_app_transfer_BLE_processor_work.md`](Fast_SD_to_app_transfer_BLE_processor_work.md).

The rest of this document is the original proposal (6 October), updated with status and
results.

## How a download works today (AI processor side)

1. The app sends `txfile <name>` (or `txfile .` for the last image). It reaches
   `processCommand()` (`CLI-commands.c:2582`) through the CLI task.
2. Per packet, `processCommand()` waits on `xI2CTxSemaphore`, then calls
   `prvTxFileCommand()` (`CLI-FATFS-commands.c:584`), which runs `f_read()` for 241 bytes into
   `cliOutBuffer`. It queues `..._CLI_BINARY_CONTINUES` to the IF task.
3. The IF task prints an event trace, then `sendI2CMessage()` (`if_task.c:1469`):
   **assert** `/IP_INT`, build the frame and print "Sending..." plus a 16-line hex dump, then
   **negate** `/IP_INT`.
4. The BLE processor reacts to the rising edge, reads 256 bytes over I2C at 400 kHz (about 6 ms), and
   queues a BLE notification.
5. The AI processor gets `I2CCOMM_TX_DONE`, prints the event and state changes, and gives
   `xI2CTxSemaphore`. Go to 2.

Nothing overlaps: the SD read, AI processor console output, I2C read and the BLE processor's work all happen in
series for every packet.

## Where the time goes

| Per packet | Cost | Source |
|---|---|---|
| BLE processor console (hex dump + flush) | about 130 ms | measured 4 Sept, [finding I](../2026-09-03_capture_bench_findings/I_transfer_throughput_console/explanation.md). Removed in BLE processor release 0.30.55, not yet measured |
| AI processor console | about 17 ms | measured 4 Sept: 1.9 s of Himax output over 110 packets. About 1,500 characters at 921600 baud |
| I2C read, 256 bytes at 400 kHz | about 6 ms | calculated |
| BLE processor wait for its previous BLE send | median 16 ms | measured 4 Sept (`we waited` line) |
| SD card `f_read()` of 241 bytes | probably under 1 ms | estimate, not measured (see A4) |
| Total measured, BLE processor release 0.30.48 | 308 ms (1.05 KB/s) | finding I |

For comparison, an upload runs at 45 ms per packet (5.2 KB/s), *including* an SD card write
(13 ms median) per packet, because both consoles are quiet and the app keeps 12 packets in
flight.

**Your SD read suspicion.** `FF_FS_TINY` is 0, so each open file has its own 512-byte sector
buffer. A 241-byte `f_read()` only touches the card when it crosses a sector boundary, about
every second packet, with one single-sector read. Reading larger chunks saves command overhead
but not data. I expect it to be small, but A1 will show it.

## Part (a): AI processor codebase only

**Status, 7 October 2026: A1 and A2 are done. A3 and A4 are not worth doing.** The AI processor
now takes 0.5 ms of each 158 ms packet (0.3%). The rest is the BLE processor waiting for a
connection event, so nothing more on the AI processor will speed up downloads until part (b)
is done. See the results sections below.

**A1. Instrument the download. Done.** At the end of `txfile`, the AI processor console
prints a `Download timing` block: bytes, packets, total time and rate, then the average,
maximum and total for each of three parts.
- `f_read()`: the SD card read.
- `AI`: from the BLE processor reading one packet (the TX done interrupt) to `/IP_INT` for
  the next.
- `BLE`: from `/IP_INT` to the BLE processor's read.

The code is `printDownloadTiming()` in `CLI-FATFS-commands.c` and `ifTask_getDownloadTiming()`
in `if_task.c`. It uses FreeRTOS ticks (1 ms), because tickless idle stops the cycle counter.
Averages over a download are good to a fraction of a ms.

**A2. Silence per-packet console output for binary packets. Done.** In `if_task.c`,
`binaryTxActive` is set by a binary message and cleared by the next non-binary one.
`consoleQuiet()` (`g_fileRxActive || binaryTxActive`) now gates every per-packet print:
- the event trace and state change lines
- Assert/Negate
- "Sending..." and the hex dump
- "I2C transmission complete"

The AI processor time fell from 18.5 ms to 0.5 ms per packet. The download rate didn't change,
because the time saved is spent waiting for the next connection event. It will matter once
the interval is short. The `/IP_INT` pulse is now microseconds long, as for uploads. That's
safe from BLE processor release 0.30.51 on, because `/IP_INT` uses a GPIOTE channel while a
phone is connected.

**A3. Read ahead. Not worth doing.** This would read chunk N+1 while the BLE processor reads
chunk N. It saves at most the `f_read()` time, measured at 0.4 ms per packet. Even at a 15 ms
interval, that's under 3%.

**A4. Larger SD reads. Not worth doing,** for the same reason. FatFS already reads each
512-byte sector once into its own buffer.

Also noted: `FF_FS_REENTRANT` is 0, but `txfile` calls FatFS directly from the CLI task while
`fatfs_task` also uses it. This is an existing risk, not a speed problem.

## Part (b): with the app and the BLE processor

Written up for the BLE processor developers in
[`Fast_SD_to_app_transfer_BLE_processor_work.md`](Fast_SD_to_app_transfer_BLE_processor_work.md).

**B1. Fast connection settings for downloads. The next step.** The measurements confirmed the
cause: a download moves one packet per connection interval, about 155 ms on the bench. The
BLE processor holds 15 to 30 ms only for upload sessions (`ble_actions_setFastConnParams()`).
The suggested changes:
- The BLE processor holds the fast interval from the `AI txfile` command until the
  `Finished sending` string.
- The app requests high connection priority for downloads too.
- The BLE processor's `we waited` log reports real milliseconds.

Expected result: 15 to 30 ms per packet, about 8 to 16 KB/s, at least as fast as uploads.

**B2. Several packets per connection event. Later, if needed.** With only one packet in flight,
the SoftDevice has one notification queued at a time. A larger `hvn_tx_queue_size` would let
several packets go in one connection event. Using it needs more than one packet ready at a
time:
- either bigger I2C frames (for example 1 KB, split by the BLE processor into 4 notifications
  in the existing packet format), or
- the BLE processor reading ahead.

Both processors need a larger payload size. `WW130_MAX_PAYLOAD_SIZE` also sets the receive
buffers. The 1-byte packet counter already wraps for files over 61 KB.

**B3. A download session that mirrors uploads. Later, for integrity rather than speed.** This
would be a dedicated `frx`-style exchange instead of the CLI. The app requests a file, the AI
processor streams it, and the session ends with a whole-file CRC (`compute_file_crc()` already
exists). The app can then re-request missing ranges by offset. Charles: no CRC for now. It is a
cross-repo contract (app, BLE processor, AI processor), so it needs agreement first.

## Recommended order

1. A1 and A2. Done, 7 October 2026.
2. Measure with BLE processor release 0.30.55. Done: the BLE processor's connection interval
   accounts for 99.7% of the time.
3. B1. Handed to the BLE processor developers.
4. Measure again. Then B2, only if the rate still falls short of uploads. B3 if integrity is
   wanted.

## Answers (Charles, 7 October)

1. Yes, the BLE processor runs 0.30.55.
2. No end-to-end CRC for now. Perhaps as part of part (b).
3. No target rate, but the aim is the same rate in both directions.

## Result of A1, 7 October 2026

A1 is implemented (`txfile` prints a `Download timing` block at the end). One download, BLE processor
release 0.30.55, AI processor console still verbose:

| | Per packet, avg | Max | Share of 12.6 s |
|---|---|---|---|
| `f_read()` (inside the AI time) | 0.4 ms | 1 ms | 0.2% |
| AI: BLE read to next `/IP_INT` | 18.5 ms | 19 ms | 11% |
| BLE: `/IP_INT` to BLE read | 142.4 ms | 282 ms | 88% |

18,642 bytes in 78 packets: 12,581 ms on the AI processor and 12,485 ms on the BLE processor (`Download:` line),
about 1.48 KB/s and 160 ms per packet. Uploads run at 45 ms per packet.

- **SD card reads are not a factor.** A3 and A4 are not worth doing.
- **AI processor console output costs 18.5 ms per packet**, as predicted. A2 removes it, but that only
  takes 160 ms down to about 142 ms.
- **The BLE processor side takes 88% of the time.** Of the 142 ms, the I2C read accounts for about 6 ms.
  The rest is the BLE processor not starting the read. The 282 ms maximum is about twice the average,
  which suggests the time comes in whole BLE connection intervals (100 to 200 ms by default).
  The BLE processor's `we waited Xms for BLE transfer` log lines should confirm or rule this out. If they
  do, B1 (fast connection parameters for downloads) is the next step.

### The BLE processor's `we waited` lines (7 October)

The BLE processor logs `we waited 42ms for BLE transfer` for most packets, and sometimes `91ms`. The number
is really a count of retries, not milliseconds. Each retry is a 1 ms `TimerSetValue()`, and the
LoRa timer layer raises that to its minimum of 3 RTC ticks at 1024 Hz (`MIN_ALARM_DELAY`,
`WW500-C02/rtc-board.c`). So one retry takes about 2.9 ms:

- 42 retries is about 123 ms. Add about 6 ms for the I2C read and the result is close to the
  measured 142 ms average.
- 91 retries is about 267 ms, close to the measured 282 ms maximum.

So for each packet, the BLE processor waits for most of a connection interval before it reads from the AI processor. That is the time
until its previous BLE notification is accepted (`bleMsg_busy()`, waiting for TX_RDY), which
works out to about **one packet per BLE connection interval**. This confirms B1. With the
upload's 15 to 30 ms interval, the AI processor's 18.5 ms console time becomes a large part of each
packet, so A2 should be done together with B1.

### Result of A2, 7 October 2026

A2 is implemented (`binaryTxActive` and `consoleQuiet()` in `if_task.c`). One download:

| | Before A2 | After A2 |
|---|---|---|
| AI time per packet | 18.5 ms | 0.5 ms |
| BLE time per packet | 142.4 ms | 157.1 ms |
| Packet period | 161 ms | 158 ms |
| Rate (BLE processor `Download:` line) | 1,493 bytes/s | 1,526 bytes/s |

This was expected. The link moves one packet per connection interval, about 155 to 160 ms, so
the time saved on the AI processor is spent waiting for the next connection event. The AI processor side is now
0.3% of the download. The rest is up to B1, see
[`Fast_SD_to_app_transfer_BLE_processor_work.md`](Fast_SD_to_app_transfer_BLE_processor_work.md). A2 matters once
the interval is 15 to 30 ms. It would have been 18.5 ms of every packet.
