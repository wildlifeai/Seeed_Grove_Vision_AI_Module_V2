# Faster file transfer from the SD card to the app: work needed in the BLE processor

#### File: `Fast_SD_to_app_transfer_BLE_processor_work.md`
#### Path: `_Documentation\development reports\2026-10-06_speedImageTx\Fast_SD_to_app_transfer_BLE_processor_work.md`
#### Author: Claude, for Charles Palmer
#### Date: 7 October 2026

## In short

Fast file transfer from the SD card to the app needs **work in the BLE processor code
(ww-hardware repo), and maybe in the app**. The AI processor side is finished.

A transfer from the SD card to the app (a "download", `AI txfile <name>`) runs at about
**1.5 KB/s**. A transfer the other way runs at about **5.2 KB/s**. The download moves **one
packet per BLE connection interval**, about 155 ms on the bench. The fix is to hold the fast
connection interval (15 to 30 ms) during a download, as the BLE processor already does during
an upload. Expected result: about 8 to 16 KB/s.

## How a download works

1. The app sends `AI txfile <name>`. The BLE processor forwards it to the AI processor.
2. The AI processor replies with a string, `<size> bytes in <name>`.
3. For each 241-byte chunk of the file:
   - The AI processor prepares an `AI_PROCESSOR_MSG_RX_BINARY` frame and pulses `/IP_INT`.
   - The BLE processor reads the frame over I2C (256 bytes at 400 kHz, about 6 ms). It sends it
     to the app as one notification: type 0x06, packet counter, length, then data.
   - The AI processor doesn't prepare the next chunk until the BLE processor has read this one.
4. The AI processor ends with the string `Finished sending <n> bytes (<p> packets)`.

Only one packet is ever in flight. Each step waits for the one before it.

## Where the time goes

The AI processor's `txfile` now times each packet and prints a block at the end of the
download. Its figures agree with the BLE processor's `Download:` line. Measured on
7 October 2026 with BLE processor release 0.30.55:

| Per packet | Average | Max |
|---|---|---|
| AI processor: from the BLE processor reading one packet to `/IP_INT` for the next (includes the SD card read, 0.4 ms) | 0.5 ms | 1 ms |
| BLE processor: from `/IP_INT` to its I2C read of the packet | **157.1 ms** | 294 ms |

The download was 17,466 bytes in 73 packets. The BLE processor's line was
`Download: 17466 bytes in 73 packets, 11440ms, 1526 bytes/s`.

An earlier run printed more on the AI processor console, taking 18.5 ms per packet. The BLE
processor time was then 142 ms, and the packet period was the same, about 158 ms. Making the AI
processor faster only moved the wait. That is what you'd expect when each packet waits for a
connection event.

## Why: one packet per connection interval

Of the 157 ms, the I2C read takes about 6 ms. The rest is `executeI2cRead()` (`aiProcessor.c`)
retrying while `bleMsg_busy()` is true, that is, until the SoftDevice accepts the previous
notification. With one notification queued at a time, that's one packet per connection event.

The log line `DEBUG: we waited 42ms for BLE transfer.` (sometimes `91ms`) supports this, but the
number is a **retry count, not milliseconds**. Each retry is `TimerSetValue(&retryAIReadTimer, 1)`.
The LoRa timer layer raises that to its minimum of 3 RTC ticks at 1024 Hz (`MIN_ALARM_DELAY`,
`WW500-C02/rtc-board.c`), about 2.9 ms per retry. So 42 is about 123 ms, and 91 is about 267 ms.
That's one and two intervals.

Probable reason the interval is slow: the `ble_conn_params` module renegotiates to the
standard 100 to 200 ms 20 s after connection. Only an upload session stops it, through
`ble_actions_setFastConnParams(true)` in `fileTx.c`.

## Suggested changes

Function names come from the `feature/ble-fast-transfer` branch (July 2026). Check them against
the current branch.

1. **BLE processor:** call `ble_actions_setFastConnParams(true)` when a download starts and
   `ble_actions_setFastConnParams(false)` when it ends:
   - Start when forwarding an `AI txfile` command, so the fast interval is already agreed by
     the first packet. Alternatively, start on the first binary frame, the string-to-binary
     transition in `bleMsg_beginBinary()`.
   - End at `bleMsg_endBinary()` (the `Finished sending` string), and on disconnect or
     timeout.
   - `rxComplete()` runs in ISR context. Schedule the call through `app_sched`, as `fileTx.c`
     does from main context.
2. **App (Android):** call `requestConnectionPriority(HIGH)` for downloads too, if it is
   currently only made for uploads (`[FileTransfer] Requested high connection priority`).
3. **BLE processor, small:** make the `we waited` line report real milliseconds, from a
   timestamp difference, not the retry count.

## How to check the result

Download a JPEG with `AI txfile <name>` from the app or Engineer Console. Compare:

- the BLE processor's `Download: <bytes> bytes in <n> packets, <ms>ms, <rate> bytes/s` line
- the AI processor's console block, for example:

```
Download timing: 17466 bytes, 73 packets in 11534 ms (1514 bytes/s)
  f_read: avg 0.4 ms, max 1 ms, total 27 ms over 73 (SD card read, part of the AI time)
  AI:     avg 0.5 ms, max 1 ms, total 34 ms over 72 (BLE read of a packet to /IP_INT for the next)
  BLE:    avg 157.1 ms, max 294 ms, total 11468 ms over 73 (/IP_INT to BLE read of the packet)
```

With the fast interval, the `BLE:` average should fall to about 15 to 30 ms.

## Later option

With one packet in flight, the SoftDevice has one notification queued at a time. Once the
interval is short, a larger `hvn_tx_queue_size` would let several packets go in one connection
event, given the 7.5 ms event length (`NRF_SDH_BLE_GAP_EVENT_LENGTH 6`). Using it needs the AI
processor to send larger I2C frames, or to let the BLE processor read ahead. That is a joint
change for later.

## Background

- [`CLAUDE_download_speed_proposal.md`](CLAUDE_download_speed_proposal.md), in this folder:
  the AI processor investigation, the measurements and the changes made there. The timing
  instrumentation was added and the per-packet console output silenced on 7 October 2026.
- [ww-hardware #34](https://github.com/wildlifeai/ww-hardware/issues/34): the BLE processor's
  per-packet console output during downloads, fixed in release 0.30.55.
- [Finding I](../2026-09-03_capture_bench_findings/I_transfer_throughput_console/explanation.md),
  3 to 4 September 2026: the original download and upload measurements.
