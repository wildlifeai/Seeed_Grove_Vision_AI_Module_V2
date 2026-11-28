# Camera errors seen in the logs, and what the firmware now does

#### File: `error_catalogue.md`
#### Path: `_Documentation\development reports\2026-10-12_cameraErrors\error_catalogue.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 14 October 2026

## Background

Victor has several hardware setups where poor connection at the HM0360 connector causes various errors.
Many of these were either error messages from the camera driver (instead of 'frame ready' messages),
or no response at all.

Console output including the errors were captured in log files (before and after software improvements).

Claude and Charles worked through the log files and implemented a variety of changes to better handle errors
when they arose.

## Purpose of this Document

To collate the various types of error that have been seen, and to describe the mitigations that have been added.

_Note that none of this fixes the fundamental problem of poor electrical connectivity!_

A list of each kind of error in the bench logs, with what the AI processor firmware did before
and what it does now. Table 1 is the errors that follow a disturbed HM0360 connection, and is
meant as a basis for a discussion with Himax. Table 2 is everything else that was changed.

"Step" is the step number in
[`CLAUDE_camera_error_handling_proposal.md`](CLAUDE_camera_error_handling_proposal.md), which
has the detail of the software mitigations.

## Where the logs come from

| Date | Logs | Board | Firmware | How the fault was caused |
|---|---|---|---|---|
| 11 Oct | [`2026-10-11_camera_connector_pressure/logs`](../2026-10-11_camera_connector_pressure/logs), 14 files | Board 1 WILD-DKOB, board 2 WILD-C2LD | `dev` of 6 Oct (board 1) and 24 Aug (board 2), HM0360 image | pressure on the HM0360 connector, by hand, holder or tape |
| 12 Oct | console output, quoted in the proposal | Charles's board | this branch, HM0360 image | none: an undisturbed board |
| 13 Oct | [`logs/dkob_*`](logs), 5 files | Board 1 WILD-DKOB | this branch at 187302ce, both images | 30 s presses on the HM0360 connector, and test commands |
| 13 Oct | [`logs/wdt*`, `logs/test*`](logs), 5 files | Charles's board | this branch | the watchdog experiment, no fault |

All the pressure tests used the HM0360 as the main camera.

The messages referred to are values from the `SENSORDPLIB_STATUS_E` list in `sensor_dp_lib.h`.
The hex codes in the logs are our own event numbers for them (`app_msg.h`):

| In the logs | Himax library value |
|---|---|
| `0x011c`, `EDM WDT2 timeout` | `SENSORDPLIB_STATUS_EDM_WDT2_TIMEOUT` (-76) |
| `0x011b`, `EDM WDT3 timeout` | `SENSORDPLIB_STATUS_EDM_WDT3_TIMEOUT` (-77) |
| `0x012a` | `SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL1` (-40) |
| `0x0131` | `SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL1` (-30) |
| `Image Event Frame Ready` | `SENSORDPLIB_STATUS_XDMA_FRAME_READY` (5) |

## Table 1: errors that follow a disturbed HM0360 connection

| ID | What the log shows | Seen | Firmware before | Firmware now | Step |
|---|---|---|---|---|---|
| H1 | No frame ready message. `EDM WDT2 timeout` (`0x011c`) then `EDM WDT3 timeout` (`0x011b`), 5 s after the frame was asked for | 10 times on 11 Oct, 27 on 13 Oct | Restarted the sensor, up to 5 times | Restarts the sensor once. If that fails, reports `Capture failed` and sleeps | 1, 2, 3 |
| H2 | The same pair with no pressure at all, on the first frame after a cold boot | Occasionally (12 Oct) | As H1 | As H1. Captures before the first sleep are now refused as well | 2, 9 |
| H3 | Straight after a sensor restart, three more `0x011c`/`0x011b` pairs in 12 ms | Once (`b1_1712`) | Ignored by a timing guard | Ignored: events within 500 ms of a fault belong to it | 2 |
| H4 | `No Register EDM Timing Callback Fun 0x20000`, 69 times in 60 ms, then a hard fault | Once (`b1_1712`) | Processor dead until RESET | The fault handler reports and resets. The burst itself is not handled | 7 |
| H5 | `0x012a` and `0x0131` (WDMA2 and WDMA3 "frame end count not reached") | Once (`b2_1735`) | Unhandled. Stuck waiting for a frame for 2.5 minutes | Treated as H1 | 2 |
| H6 | No frame and no event of any kind. The EDM watchdog never fires | Once, 44 s (`b2_1735`) | Waited indefinitely | Our own frame timer acts after the capture interval plus 7 s | 5 |
| H7 | The processor stops mid-capture. No output, no reply to commands | 3 times (`b1_1716`, `b2_1734`, `b2_1740`) | Dead until RESET | A watchdog resets it after 20 s. Not yet seen on a real freeze | 8 |
| H8 | The boot stops, with a corrupted console, in the ROM bootloader, the second bootloader or as the application starts | 5 logs on 11 Oct | Stuck until the pressure is off and RESET is pressed | No change. This is before, or just as, our code starts | none |
| H9 | A bus fault a minute after a ribbon was moved with the power on. RESET did not recover it | Once (`b1_1612`) | Printed the fault and stopped | The fault handler reports and resets. A reset may not be enough here | 7 |
| H10 | `HM0360 not present at 0x24` after RESET, until a power cycle | Once, over 3 RESETs (`b2_1729`) | Sets a self-test bit, disables the camera, sleeps normally | No change needed | none |
| H11 | Stray bytes on the console (`\x00` and others) while the connector is pressed | Most pressure logs | Not applicable | Not applicable | none |

### Notes on Table 1

**H1, the frame timeout.** This is the common one.
- 11 Oct: 10 timeouts (`b1_1711` 2, `b1_1712` 1, `b1_1715` 7). The restart recovered 9. The
  tenth is H3 and H4.
- 13 Oct: 27 timeouts (`dkob_1452` 4, `dkob_1506` 2, `dkob_1508` 21). 24 of them led to a
  restart, and 21 of those restarts gave the frame. The other 3 timed out a second time and
  ended in `Capture failed`, a normal sleep, and a new capture at the next wake.
- Under steady pressure each restart gives exactly one frame, and the next frame times out
  5 s later (`b1_1715`, `dkob_1508`). So the restart recovers a frame but not the fault.
- `0x011b` always follows `0x011c`, and never comes first or alone.

**H2, with no pressure.** `capture 3 1000` after a cold boot, HM0360, 12 October: the first frame
timed out, one restart gave it, and frames 2 and 3 were normal. Charles has seen this more than
once, but usually the first frame arrives. So H1 is not only a connector fault.

**H4, the callback burst and hard fault** (`b1_1712`, in order):
- 17:12:55.717: one `... EDM Timing Callback Fun 0x3ffff` (every timing error bit set), then
  `0x011c` and the sensor restart
- three more `0x011c`/`0x011b` pairs in 12 ms (H3), then nothing for 5 s
- 17:13:01.130 to 01.188: 69 lines of `No Register EDM Timing Callback Fun 0x20000`
- 17:13:01.369: hard fault, `CFSR 0x00020000` (invalid state), `HFSR 0x40000000` (escalated)

The lines are printed by the prebuilt Himax library from inside its interrupt handler. `0x20000`
is bit 17, "con_de_more" in the EDM timing error list. Our code registers no callback for these.
This has not happened again in 17 presses on 13 October.

**H5 and H6** were one event on board 2: nothing for 44 s, then the two WDMA errors, then no
frames. That board ran August firmware. On current code both parts are covered, but neither has
happened again, so the new handling is not yet shown on a real fault. H6 has been shown on a
different fault (see O1 in Table 2).

**H7, the freezes.** The console stops between two frames with no timeout and no fault report.
`selftest` typed 1 and 2 minutes later gets no echo. The watchdog has been tested with a console
command that masks interrupts and loops, on both images. No real freeze has happened since it
went in, so it is not yet known whether it rescues one. The freezes came with console
corruption, which suggests the pressure disturbs the clock or supply as well as the camera.

**H8** is outside what the application can handle. See
[ww-hardware#70](https://github.com/wildlifeai/ww-hardware/issues/70) for the holder, and
[ww-hardware#69](https://github.com/wildlifeai/ww-hardware/issues/69) for the BLE processor
noticing a dead AI processor.

**H10.** The HM0360 runs from the always-on 1V8 and 2V8 and has no reset line, so RESET does
not reset it.

### Questions for Himax

1. **H1, H2.** What makes WDT2 and WDT3 time out while WDT1 never does? Is a missing first frame
   after the sensor is started a known behaviour of the HM0360 or of the datapath?
2. **H1.** Under a marginal connection, why does each sensor restart give exactly one frame?
3. **H4.** What does the EDM timing error `0x20000` mean, and `0x3ffff`? Should the application
   register a callback, or turn the detection off with `sensordplib_set_edm_timing_inlib(0)`?
4. **H4.** Can 69 of these interrupts in 60 ms, each printing from the interrupt handler, lead to
   the hard fault that followed?
5. **H6.** Why did the EDM watchdog not fire for 44 s when no frame arrived?
6. **H5.** What do `XDMA_WDMA2STATUS_ERR_FE_COUNT_NOT_REACH` and the WDMA3 equivalent tell us
   about the state of the datapath, and what is the right recovery?
7. **H7.** Is there a known way for a disturbed sensor interface to stop the processor
   completely, with no fault and no watchdog event?
8. **General.** Is the recommended recovery from a datapath error a sensor restart, a full
   re-initialisation of the datapath, or something else?

## Table 2: other faults addressed in this work

| ID | What was wrong | Seen | Firmware now | Step |
|---|---|---|---|---|
| O1 | An RP3 that acknowledges every I2C write but never sends a frame | One faulty board, every capture (13 Oct) | `EDM WDT2 timeout` at 5 s, then `Capture failed` and sleep | 2, 3, 5 |
| O2 | The sensor restart could never work on the RP cameras: stopping the sensor powers it down, and the restart does not bring it back | Found from O1 | No restart on RP builds. The capture fails at the first fault | 2 |
| O3 | Datapath events printed as `'Unrecognised' (0x011c)` | Every log with a fault | Printed with a name, such as `'EDM WDT2 timeout'` | 1 |
| O4 | A failed capture told the app nothing. The camera then woke 1 s later and took pictures the app had not asked for | By design, since February | `Capture failed after N of M images: <reason>` to the app. No automatic retry | 3 |
| O5 | `Inactive for 1ms` 24 times, and `unhandled event` sent to the app 22 times | `b2_1729`, and after every failed capture | At most one inactivity event a second. The image task ignores those it can't act on | 6 |
| O6 | `Sleep` sent to the app while a capture is still running | `b2_1735`, and Charles's console output of 13 Oct | Not changed. Still open | none |
| O7 | Every fault handler printed and looped for ever. A usage fault was reported as "secure bus error" | `b1_1712`, `b1_1612` | Prints the status registers and the stacked `PC` and `LR`, then resets | 7 |
| O8 | A failed `configASSERT()` looped for ever with nothing on the console | 13 Oct, during a firmware update | Prints `ASSERT failed: <file>:<line> in task '<name>'`, then resets | added after step 9 |
| O9 | A capture could be asked for between a cold boot and the first sleep, which was never intended | The bench scripts and the ship-check tool | `capture` and `light` are refused until after the first sleep | 9 |
| O10 | Nothing reset a processor that had stopped | The freezes in Table 1 | A watchdog runs while awake and resets after 20 s | 8 |

### Faults introduced by this work, and fixed

| ID | What went wrong | Found | Fix |
|---|---|---|---|
| X1 | A firmware update ended in a hang. Suspending the watchdog sent FreeRTOS timer commands, the timer queue (5 entries) filled, and the IF task's next timer call failed into a `configASSERT(0)` | 13 Oct, Charles | The watchdog no longer sends timer commands. The queue is now 10, and that timer call waits 100 ms for space |
| X2 | A fault was followed by a second reset (RP3), or by a watchdog period of 10 s where 20 s was expected (HM0360). A system reset request does not stop the hardware watchdog or clear its first-timeout flag | 13 Oct, Victor (`dkob_1446`) | The watchdog is stopped and its flag cleared at the start of every boot |

### Not errors

- `Image not written. Error: 13` in the 11 October logs: Victor's boards had no SD card.
- The hardware watchdog resets at its second timeout, so a period of 10 s gives a reset at 20 s.
  This is how it works, not a fault.
- `BLE out: Failed to send` in the 13 October logs: no phone was connected.

## What is still unproven

- That the watchdog rescues a real freeze (H7).
- The handling of `0x012a` and `0x0131` (H5) on a real fault.
- Whether a reset is enough after the bus fault in H9.
