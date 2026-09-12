# `FILE_LOOPBACK` packets are never echoed, so the app's BLE benchmark always times out

#### File: K_loopback_never_echoes.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 4 September 2026

**Repo:** ww-hardware (nRF52, `MokoTech/Workspace/WildlifeWatcher_1`), line numbers from `dev` at
75406df (fw-v0.30.48, the version on the device).
**Labels:** bug, review-finding. **Severity:** low. A test feature only, but it is the one
measurement of the bare BLE link the app offers, and it reads as a dead link.

## 1. What is the problem

The nRF receives each `FILE_LOOPBACK` packet (type 10) and schedules the echo:

```
[09:02.201] nrf   | BLE in: File Tx Packet type 10, 8 bytes
[09:02.201] nrf   | <info> app:   BLE fileTx LOOPBACK 8 bytes
```

Nothing leaves. `fileTx_processScheduled()` echoes with `bleMsg_sendBinary()` (`fileTx.c:518`),
but that function returns `false` without doing anything unless a binary sequence is open
(`bleMsg.c:363`, `currentMode != BLEMSG_MODE_BINARY`), and the only caller of
`bleMsg_beginBinary()` is the Himax binary relay (`aiProcessor.c:318`). Outside an `AI txfile`
stream the nRF is in string mode, so the echo is dropped and the return value is ignored. The
app waits 5 s per round and reports `Loopback timeout (5000ms)`.

## 2. How to reproduce

Engineer Console, flows, File Transfer Test, **Run Benchmark**. On 4 September 2026 at 08:41,
0 of 10 rounds succeeded at each of the three payload sizes, thirty timeouts in a row, every
packet acknowledged on the nRF console as above and no `BLE out` after any of them. Evidence:
[`I_transfer_throughput_console/logs/finding_I_bench.txt`](I_transfer_throughput_console/logs/finding_I_bench.txt)
from `09:02.201`, and
[`I_transfer_throughput_console/logs/loopback_screen.txt`](I_transfer_throughput_console/logs/loopback_screen.txt).

## 3. Where in the code

- `fileTx.c:510` to `520`: the loopback branch of `fileTx_processScheduled()`.
- `bleMsg.c:362` to `380`: `bleMsg_sendBinary()`, the mode check and the ignored `false`.
- `bleMsg.c:341` / `:348`: `bleMsg_beginBinary()` / `bleMsg_endBinary()`.

## 4. Suggested fix

Bracket the echo: `bleMsg_beginBinary()`, `bleMsg_sendBinary()`, and `bleMsg_endBinary()` once
it has gone out (or a one-shot send that ignores the mode). Log when `bleMsg_sendBinary()`
returns `false` so the next silent drop is visible.
