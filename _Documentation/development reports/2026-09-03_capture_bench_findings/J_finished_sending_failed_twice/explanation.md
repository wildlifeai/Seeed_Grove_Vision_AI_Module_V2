# `Finished sending` is logged as failed twice before it goes out, every time

#### File: explanation.md
#### Author: Claude (Fable 5.1), reviewed by Victor Anton
#### 3 September 2026, re-checked against `dev` on 4 September

**Repo:** ww-hardware (nRF52, `MokoTech/Workspace/WildlifeWatcher_1`), line numbers from `dev` at
75406df (fw-v0.30.48, the version on the device).
**Labels:** bug, review-finding. **Severity:** low. The line does arrive, about 300 ms late; the
point is that `Failed to send` is printed for normal back-pressure, so a real failure of that line
would look the same, and a real drop is logged with the same words.

## 1. What is the problem

The Himax's `Finished sending N bytes (P packets)` string reaches the nRF while the last binary
chunk is still queued in the SoftDevice, and the console reports it as a failure twice before it
goes out:

```
[03:11.416] nrf   | BLE out: Failed to send  43 bytes: 'Finished sending 26371 bytes (110 packets)'
[03:11.416] nrf   | BLE out: Failed to send  43 bytes: 'Finished sending 26371 bytes (110 packets)'
[03:11.718] nrf   | BLE out: Sent  43 bytes: 'Finished sending 26371 bytes (110 packets)'
```

Twenty-one transfers on 3 and 4 September, twenty-one times the same three lines. What happens:

1. `ble_actions_sendBLE()` prints `Failed to send` for any non-success return from the
   SoftDevice (`ble_actions.c:1484` to `1499`), with the comment "Don't worry - the code will
   try again when the TX_RDY event happens". The return here is `NRF_ERROR_RESOURCES`, the
   notification queue still holding the last chunk, which `trySend()` handles correctly by
   waiting for TX ready (`bleMsg.c:190` to `196`). The log just calls that a failure.
2. It is printed twice because `trySend` is kicked twice before TX ready. When the string
   arrives, `rxComplete()` schedules its parsing (`aiProcessor.c:313`) and then, because it is
   not a binary message, calls `bleMsg_endBinary()` (`:349`), which kicks `trySend`
   (`bleMsg.c:352`). The parse runs first and the state machine pushes the string
   (`aiStateMachine.c:435`), which kicks again (`bleMsg.c:313`). Both kicks find the queue
   full; the third, from `bleMsg_onTxReady()` (`:326`), sends it.
3. Every further push while the queue is full retries the head of the queue and prints another
   line. In finding H's runs `AI processor not responding` was queued during the stream and
   showed four failures before `Finished sending` showed its two.

A genuine error takes the other branch of the same `if`: not connected or any other code drops
the message (`bleMsg.c:197` to `204`) after printing the identical `Failed to send`.

## 2. How to reproduce

Any `AI txfile`; watch the nRF console at the end. 18 of 18 on 3 September (five bench sessions),
3 of 3 on the 0.30.48 logs of findings H and I.

## 3. Where in the code

- `ble_actions.c:1484` `ble_actions_sendBLE()`, the print at `:1499` and the comment at `:1498`.
- `bleMsg.c:103` `trySend()`, the `NRF_ERROR_RESOURCES` wait at `:190` to `196`, the drop at
  `:198` to `206`; the kicks at `:313` (`bleMsg_push()`), `:326` (`bleMsg_onTxReady()`), `:352`
  (`bleMsg_endBinary()`).
- `aiProcessor.c:293` to `316` the string case of `rxComplete()`, `:348` to `:352` the
  `bleMsg_endBinary()` on the first non-binary message.

## 4. Suggested fix

In `ble_actions_sendBLE()`, print `NRF_ERROR_RESOURCES` as a deferral (`TX queue full, waiting`)
or not at all, and keep `Failed to send` for the branch that drops the message. Optionally skip
the `bleMsg_endBinary()` kick while `binaryPending` is set, which removes the second attempt.
Cosmetic, but it would let `Failed to send` mean what it says.

## Evidence

| File | What it is |
|---|---|
| [`logs/finished_sending_failed_to_send.txt`](logs/finished_sending_failed_to_send.txt) | Every `Finished sending` line of 3 September from the five bench logs: 18 transfers, 36 failures, 18 sends |
| [`../H_command_during_binary_send/logs/repro_H_bench.txt`](../H_command_during_binary_send/logs/repro_H_bench.txt) | `04:06.714` and `05:51.195`: the same pattern, plus `AI processor not responding` failing four times ahead of it |
| [`../I_transfer_throughput_console/logs/finding_I_bench.txt`](../I_transfer_throughput_console/logs/finding_I_bench.txt) | `03:11.416`: the lines quoted above |
