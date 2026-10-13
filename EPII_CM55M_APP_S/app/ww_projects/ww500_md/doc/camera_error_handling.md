# Camera error handling on the AI processor
#### Claude, for Charles Palmer - 13 October 2026

How the `ww500_md` firmware behaves now when a capture fails or the processor stops. How it got
this way is in `_Documentation/development reports/2026-10-12_cameraErrors/`.

## When a frame does not arrive

The image task waits for each frame in its Capturing state. A **frame fault** is anything that
arrives in place of the frame:

- a datapath event: an EDM watchdog timeout (`0x011b` to `0x011e`), a CDM FIFO error, a WDMA or
  RDMA error (`0x0121` to `0x013e`), or the parser error (`0x0151`)
- the firmware's own frame timer (`Image Event Frame Timeout`, `0x0a0f`), which expires at the
  capture interval plus 7 s if nothing at all has arrived

The EDM watchdogs are part of the Himax datapath library and fire after 5 s. They usually come
as a pair, `0x011c` then `0x011b`. Events within 500 ms of the first are treated as part of the
same fault and ignored.

What happens next depends on the camera:

| Build | On a frame fault |
|---|---|
| HM0360 | The sensor is restarted once (stop, 100 ms, run). If the frame then arrives, the capture carries on. If not, the capture fails |
| RP3 | The capture fails at once. Stopping an RP camera powers it down, so a restart can't work |

When a capture fails, the AI processor:

1. sends `Capture failed after N of M images: <reason>` to the console and the app (N images
   were taken and saved), and does not send `Captured`
2. stops the sensor, saves its state and sleeps

It does not try again. The app can send the command again.

The code is `frameFault()`, `captureFailed()` and `frameTimerStart()` in `image_task.c`.
Datapath events are named on the console by `dpEventString()`.

## Captures after a cold boot

`capture` and `light` are refused between a cold boot and the first sleep, with
`Error: no captures until after the first sleep.` A cold boot follows power-on, RESET, `reset`,
`switchslot`, a firmware update, and a reset by a fault or the watchdog. Captures the firmware
starts itself (motion and timer wakes) are not affected. The code is `captureRefused()` in
`CLI-commands.c`.

## Inactivity

The inactivity detector (`inactivity.c`) sends its event when no task has run for the period in
op 8. It fires again if the board is still awake, but not more often than once a second. The
FreeRTOS timer task does not count as activity. The image task ignores the event in states that
can't act on it.

Still open: the IF task sends `Sleep` to the app when the detector fires, even while a capture
is running.

## Faults

Every fault handler (`hardfault_handler.c`) prints a report, including the stacked `PC` and
`LR`, then `>>> Reset after fault`. The processor resets at once (a system reset request) and
cold-boots. A watchdog started by the handler is the backup, 2 s later. The report is on the
console only.

A system reset request does not stop the hardware watchdog, so `watchdog_init()` stops it and
clears its flag at the start of every boot.

A failed `configASSERT()` is handled in the same way (`vAssertCalled()`, same file). It prints
`ASSERT failed: <file>:<line> in task '<name>'` and resets. With a debugger attached it stops
at a breakpoint instead. This is switched on by `WW_ASSERT_RESETS` in `ww500_md.mk`. Other apps
in the SDK keep the old behaviour, which is to loop for ever.

## The awake watchdog

`watchdog.c` runs the hardware watchdog while the processor is awake. A FreeRTOS timer feeds it
every 4 s. If the feeds stop for 20 s the processor resets and cold-boots.

- The hardware resets at the second timeout, so the 10 s period gives a 20 s reset.
- It is stopped on the way into DPD, and suspended while the flash is being written
  (`disable_xip()` to `enable_xip()` in `xip_manager.c`).
- It catches a processor that has stopped, or a loop in the CLI or IF task. It does not catch a
  loop in the image task, which has a lower priority than the timer task.
- A debugger that halts the processor will be reset after 20 s. Type `wdt 0` first.
- `#define AWAKE_WATCHDOG` in `watchdog.h` turns it off at build time.
- DPD current is unchanged by it (measured 13 October 2026).

## Console commands for testing

| Command | What it does |
|---|---|
| `hardfault` | Forces a hard fault. Expect the fault report, then a reset |
| `assert` | Forces a failed `configASSERT()`. Expect `ASSERT failed`, then a reset |
| `freeze` | Masks interrupts and loops for ever. Expect a watchdog reset about 20 s later |
| `wdt 0`, `wdt 1` | Stops or starts the awake watchdog, until the next boot |
