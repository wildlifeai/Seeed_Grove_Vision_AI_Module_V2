# Image transfer is held near 1 KB/s by the nRF's own console output

#### File: explanation.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026, measured against the quiet upload path on 4 September

**Repo:** ww-hardware (nRF52, `MokoTech/Workspace/WildlifeWatcher_1`), line numbers from `dev` at
75406df (fw-v0.30.48). The device reports `WW500-C02 V 00.30.48 21:58:30 Jul 21 2026`.
**Labels:** enhancement, review-finding. **Severity:** medium. Three quarters of every capture the
app takes is this transfer: 24 s for a 26 KB colour image, 10 s for a 12 KB one.

## 1. What is the problem

For every 241-byte chunk of an `AI txfile` download the nRF, before the BLE send, logs the I2C
receive and hex-dumps the whole buffer (`rxComplete()`, `aiProcessor.c:283` to `288`: about 16
lines, 1,500 characters for 244 bytes of data) and flushes the deferred log (`ble_actions.c:1235`)
right before `bleMsg_sendBinary()` (`:1237`). The log backend is a 115200 baud UART with
`NRF_LOG_DEFERRED` on, so each packet costs about 130 ms of UART time plus the scheduler backlog
of sixteen queued print events.

The nRF already knows this. Since 0.30.47 (18ffc89, 10 July 2026) exactly this logging is
skipped during an upload, gated on `g_fileTxActive` (`fileTx.c:134`, set at `:372`), with the
commit's own comment: the dump "steals main-loop time between relaying a packet and reading
its ack, inflating the per-packet time from ~15ms to ~175ms". The gate is only ever set by the
upload path, so a download still pays the full price.

Measured on 4 September, same device, same nRF, same UART and BLE link, minutes apart:

| Transfer | Chunks of 241 B | Per packet | Rate | nRF console per packet |
|---|---|---|---|---|
| Download, `AI txfile 59200BE0.JPG`, dump on | 110 | 308 ms | 1.05 KB/s | 1,491 chars, 129 ms of UART |
| Upload, `ftx LARGE.BIN`, `g_fileTxActive` set | 2,125 | 45 ms | 5.24 KB/s | 31 chars, 3 ms |

The upload is the harder job: every packet is written to the SD card on the Himax before it is
acknowledged, and the nRF's own per-packet timer on that path (`pkt time` in its log) reads
13 ms median, 19 ms at the 90th percentile. On the download the nRF's own line `DEBUG: we
waited 16ms for BLE transfer` sits beside every packet, so the link is not the limit. The Himax
console is not a factor either: 1.9 s of UART time across the whole download at 921600.

Earlier downloads gave the same figure: 23.8 s, 23.0 s and 24.1 s for the same 26 KB (retest
bench and finding H's two runs, 3 September), 10.7 s for a 12.9 KB image (flash bench).

## 2. How to reproduce

1. Engineer Console: `AI txfile 59200BE0.JPG` (any JPG on the card). Time from the first
   `BLE binary: Sending` on the nRF console to `Finished sending`.
2. Engineer Console, flows, File Transfer Test: **Large Binary (~500KB)**. Time from
   `FILE_START sent` to `ftx done` in the app's log, or read the screen.

[`measure_I.py`](measure_I.py) drives step 1 over adb and computes both from the three-way bench
log: bytes, packets, duration, rate, the gap between packets, the nRF's BLE wait, and the nRF
console characters per packet.

The screen's loopback benchmark (`FILE_LOOPBACK`, packet type 10) would have given the bare link
figure but did not run on 0.30.48: all thirty rounds timed out. The nRF logged
`BLE fileTx LOOPBACK 8 bytes` for each packet and nothing left it. Recorded separately as
finding K.

## 3. Where in the code

- `aiProcessor.c:279` to `289`: the receive log and hex dump, skipped only when `g_fileTxActive`.
  `:316` to `:348`: the binary relay to BLE. `:484`: the `we waited` measurement.
- `ble_actions.c:1235` to `1237`: `NRF_LOG_FLUSH()` then `bleMsg_sendBinary()`, with the comment
  that the flush is there for an unrelated print oddity.
- `fileTx.c:134`, `:372`, `:216`, `:734`, `:790`: the gate, set and cleared by the upload session.
- `bleMsg.c:341` `bleMsg_beginBinary()` / `:348` `bleMsg_endBinary()`: already bracket a download.
- `ww500_c02/s132/config/sdk_config.h:7808` `NRF_LOG_DEFERRED 1`, `:7657` the UART backend at
  115200 (register value 30801920), `:7780` `NRF_LOG_BUFSIZE 1024`, the ring the flush drains.

## 4. Suggested fix

Give the download the gate the upload already has: raise a flag in `bleMsg_beginBinary()` (or on
the first `AI_PROCESSOR_MSG_RX_BINARY` frame) and clear it in `bleMsg_endBinary()`, and skip the
hex dump and the `NRF_LOG_FLUSH()` while either flag is set. The one-line `BLE binary: Sending`
log per packet is fine to keep. Expect the upload path's figure, about 45 ms per packet and
5 KB/s, a four to five times faster picture. The app models the transfer at 1.1 KB/s for its
countdown and measures the real rate after 2 KB, so it needs no change when this lands.

## Evidence

| File | What it is |
|---|---|
| [`logs/finding_I_bench.txt`](logs/finding_I_bench.txt) | Three-way bench log (app, nRF, Himax) of the version query, the download, the upload and the benchmark, hex dumps removed and per-packet chatter kept only around the key events |
| [`logs/download_run1.txt`](logs/download_run1.txt), [`logs/upload_run1.txt`](logs/upload_run1.txt) | The script's summaries |
| [`logs/loopback_screen.txt`](logs/loopback_screen.txt) | The File Transfer Test screen after the upload and the benchmark, transcribed |
| [`measure_I.py`](measure_I.py) | The measurement |
| [`../H_command_during_binary_send/logs/repro_H_bench.txt`](../H_command_during_binary_send/logs/repro_H_bench.txt) | Two more downloads of the same file, 3 September |
