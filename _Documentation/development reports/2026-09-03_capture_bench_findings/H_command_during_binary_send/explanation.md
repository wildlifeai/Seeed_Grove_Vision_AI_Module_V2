# A command received while a file is streaming is forwarded at once and restarts the packet counter

#### File: explanation.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026, reproduced on demand the same evening

**Repo:** ww-hardware (nRF52, `MokoTech/Workspace/WildlifeWatcher_1`), line numbers from `dev` at
75406df (fw-v0.30.48), the code whose console strings match the device; `main` at f3ea286 has the
same forwarding and counter code a few lines earlier and no response-timeout path. The Himax side
quoted from `ae_review` at e8b7feb5.
**Labels:** bug, review-finding. **Severity:** medium. The phone cannot tell a restarted counter
from lost packets, the command's reply arrives 15 to 19 s late, and the nRF tells the phone the
Himax is not responding in between.

## 1. What is the problem

`aiProcessorTxString()` forwards every `AI ...` command to the Himax the moment it arrives over
BLE and zeroes `binaryPacketNum` on the way (`aiProcessor.c:650`, "in anticipation of receiving
one or more binary packets"). If a `txfile` stream is in progress, the chunks already queued go
out with their old numbers and the next one is packet 1 again (`aiProcessor.c:333` pre-increments).

The Himax cannot act on the command until the file is done. Its IF task defers a message that
arrives while it is in I2C TX (`if_task.c:1112` to `1126`, `savedMessage`) and re-issues it each
time it returns to Idle, but the next `Binary Continues` for the file is already queued ahead of
it, so the command is deferred again on every packet:

```
[05:42.280] himax | IF Task state changed from 'I2C TX State' (3) to 'Idle' (1)
[05:42.280] himax | Issuing deferred event 0x0700 'I2C Rx'
[05:42.280] himax | IF Task received event 'Binary Continues' (0x0706). Rx data = 0x3000ea30
[05:42.280] himax | IF Task state changed from 'Idle' (1) to 'I2C TX State' (3)
[05:42.280] himax | IF Task received event 'I2C Rx' (0x0700). Rx data = 0x00000000
[05:42.280] himax | Deferring event 0x0700
```

Meanwhile the nRF's response timer runs out (`AI_RESPONSE_TIMEOUT_MS`, 10 s, `aiStateMachine.c:73`,
started at `:850`), it recovers its state machine to IDLE (`:628`), and `AI processor not
responding` is queued for the phone.
The command is finally processed right after `Finished sending`, and its reply follows.

## 2. How to reproduce

From the app's Engineer Console, which writes straight to the characteristic (the app's own
command queue would otherwise hold the second command until the stream ends):

1. `AI txfile 59200BE0.JPG`, any JPG on the card. This one is 26 KB, 110 packets, about 23 s.
2. While the packets are flowing, `AI slots`.

[`repro_H.py`](repro_H.py) does both from the PC over adb and reads the outcome from the
three-way bench log: it waits for a chosen packet number, sends the probe, and reports where
the counter restarted, when the reply came and whether `not responding` was seen.

Two of two on 3 September 2026, probe at packet 20 and at packet 40. Run 1:

```
[03:42.114] nrf   | BLE in: Received  22 bytes 'AI txfile 59200BE0.JPG'
[03:47.331] nrf   | BLE binary: Sending 241 byte binary payload (packet type 6, packet num 22):
[03:47.645] nrf   | BLE in: Received   8 bytes 'AI slots'
[03:47.645] nrf   | Sending 'slots' to AI processor (4 ctrl bytes, 5 payload, 2 CRC)
[03:47.645] nrf   | BLE binary: Sending 241 byte binary payload (packet type 6, packet num 1):
[03:48.101] app   | ImageReassembler: Sequence gap — expected 23, got 1 (234 total gaps)
[03:57.509] nrf   | <info> app: AI processor response timeout - recovering to IDLE
[04:06.089] himax | Finished sending 26371 bytes (110 packets)
[04:06.398] himax | MKL62BA command received: 'slots'
[04:06.664] app   | ImageReassembler: Transfer complete! Received 26371/26371 (234 gaps)
[04:06.683] app   | ImageReassembler: Image has 234 sequence gaps — may be corrupt
[04:06.714] nrf   | BLE out: Sent  28 bytes: 'AI processor not responding'
[04:06.714] nrf   | BLE out: Sent  43 bytes: 'Finished sending 26371 bytes (110 packets)'
[04:06.714] nrf   | BLE out: Sent 116 bytes: 'Active slot 1 running 'RP3 (day/colour)'. ...
```

Run 2 is the same shape: packets 41 to 43 went out with their old numbers, then 1 to 67; the
app reported `expected 44, got 1 (213 total gaps)`; the timeout fired 9.8 s after the forward;
the reply landed 15.6 s after the probe, in the same burst as `Finished sending`.

In both runs all 26,371 bytes arrived in order and the Himax's own count was 110 packets, so
the data was intact; the last packet the phone saw was numbered 88 (run 1) and 67 (run 2).

## 3. Where in the code

- `aiProcessor.c:596` `aiProcessorTxString()`, the reset at `:650`; the pre-increment at `:333`;
  the end of a binary sequence at `:351` (`bleMsg_endBinary()` when a non-binary message arrives).
- `aiStateMachine.c:73` `AI_RESPONSE_TIMEOUT_MS`, the timer at `:850`, the recovery at `:628`.
- `bleMsg.c:341` `bleMsg_beginBinary()` / `:348` `bleMsg_endBinary()`: `currentMode` already says
  whether a binary sequence is in progress.
- Himax, for context only: `if_task.c:1112` to `1126` defer the incoming message during I2C TX;
  `:1572` to `1592` replay it on return to Idle.

## 4. Suggested fix

While `currentMode == BLEMSG_MODE_BINARY`, do not forward a BLE-in `AI` command: either queue it
and send it from `bleMsg_endBinary()`, or reply `busy: file transfer in progress` so the sender
can retry. Either way, leave `binaryPacketNum` alone while a sequence is open. The app now holds
its own queue for the length of a stream, so this only bites a second client or a typed command,
but the counter reset is wrong on its own.

## Evidence

| File | What it is |
|---|---|
| [`logs/repro_H_bench.txt`](logs/repro_H_bench.txt) | Three-way bench log (app, nRF, Himax) of both runs, hex dumps removed and the per-packet chatter kept only around the key events |
| [`logs/repro_H_run1.txt`](logs/repro_H_run1.txt), [`logs/repro_H_run2.txt`](logs/repro_H_run2.txt) | The script's timeline and summary for each run |
| [`repro_H.py`](repro_H.py) | The reproduction |
| [`../A_stuck_awake_inactivity_in_i2c_tx/logs/capture_retest_bench.txt`](../A_stuck_awake_inactivity_in_i2c_tx/logs/capture_retest_bench.txt) | The original sighting, `08:12.422`: the app's re-entered `slots` landing on a capture download |
