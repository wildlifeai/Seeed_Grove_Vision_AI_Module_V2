# Pressure on the HM0360 connector stops the boot or kills a running capture

#### File: README.md
#### Author: Claude (Opus 5.5), bench work by Victor Anton
#### 11 October 2026

## Status

Open.

## Outcome

The camera ribbons sit badly in the field enclosures, and the holders and tape that keep them in
place press on the HM0360 connector. On two boards, pressure on that connector stops the AI
processor booting, and sustained pressure (30 s) during a capture left it dead or stuck in 5 of 6
tries. Nothing restarts it: no watchdog runs, the hard fault handler loops forever, and the BLE
processor does not notice. A missing camera on its own, and moving the ribbon or the camera, are
handled well.

## Open items

- [#272](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/272): the AI
  processor does not recover from a camera datapath failure (watchdog, hard fault handler,
  unhandled datapath events).
- [ww-hardware#69](https://github.com/wildlifeai/ww-hardware/issues/69): the BLE processor does
  not notice a dead AI processor.
- [ww-hardware#70](https://github.com/wildlifeai/ww-hardware/issues/70): the holder must not
  press on the connector; PCB revision (camera supplies, HM0360 power).

## Boards

| | Board 1 | Board 2 |
|---|---|---|
| Name | WILD-DKOB | WILD-C2LD |
| AI processor | `dev` 6c8ee1ca (6 Oct 2026), HM0360 image in slot A | `dev` da79fcb9 (24 Aug 2026), HM0360 image in slot A |
| BLE processor | 0.30.52 | 0.30.48 |
| SD card | none (self-test 0800) | none (self-test 0800) |

All tests use the HM0360 image and its connector.

## What happened

| Test | Board 1 | Board 2 |
|---|---|---|
| HM0360 removed with the power off, then reseated | "not present at 0x24", self-test 0b00, sleeps normally; back after reseating | same |
| HM0360 pulled out and pushed back with the power on (board idle) | handled both ways | not tried |
| Camera moved and ribbon flexed, connector not pressed | 8 of 8 fine | not tried |
| Pressure on the connector, then RESET | boot stops in 4 of 5 | 1 of 5 by hand; electrical tape stopped it at once |
| Pressure on the connector during `capture 300 1000`, 5 s | recovers (frame timeout, `retry 1/5`) | not tried |
| Same, 30 s | 2 of 3 dead | 3 of 3 failed |

### Pressure at boot

The console text is corrupted, then the boot stops wherever it is: in the ROM bootloader
(`Versi\xden`, then nothing after `waiting input key`), in the second-stage bootloader (`slot
flash_of\xNN\xNNs...`), or just after our app starts (`FreeRTOS tickless idle is enabled`, then
noise). The BLE processor never gets the first `Wake`. Releasing the pressure and pressing RESET
always recovered it, and once (board 1, 17:09) the boot carried on by itself when the pressure
came off. Logs: `b1_1652`, `b1_1702`, `b1_1704`, `b1_1709`, `b2_1725`.

At 16:12 on board 1, before we knew the cause, a ribbon moved with the power on gave a double
reset, a stopped second-stage bootloader, and a minute later a bus fault (`CFSR 0x00008200`, `BFAR
0`). RESET did not recover it; a power cycle did (`b1_1612`).

### The HM0360 can stay latched until a power cycle

On board 2 the tape gave a garbled console (17:29). With the tape off, three RESETs each booted
the AI processor fully, but every boot reported `HM0360 not present at 0x24`. A power cycle,
without touching the ribbon, brought the camera back (`b2_1729`). The HM0360 is powered from the
always-on 1V8 and 2V8 and has no reset line, so RESET never resets it.

### Sustained pressure during a capture

Three different endings, none of which recover without RESET or sleep:

1. **Silent freeze** (board 1 17:16, board 2 17:34 and 17:40): the output stops mid-capture, no
   frame timeout, no fault print, and `selftest` gets no echo 1 and 2 minutes later.
2. **Hard fault** (board 1 17:12): frame timeout and `retry 1/5`, then timeout events every 2 ms,
   then 70 lines of `No Register EDM Timing Callback Fun 0x20000` (printed by the prebuilt
   `libdriver.a`), then `Entering HardFault interrupt!` (`CFSR 0x00020000`). The handler loops
   forever.
3. **Stuck in Capturing** (board 2 17:35): silent for 44 s, then `IMAGE task unhandled event
   0x012a in 'Capturing'` and `0x0131` (XDMA WDMA2 and WDMA3 "frame end count not reached"). The
   console answers but no frames come and no timeout fires. It slept at the second inactivity
   event, 2.5 minutes later, with nothing reported.

In every case the BLE processor logged nothing about the AI processor.

## Where to look in the logs

Start with the capture failures: they are what #272 fixes. Line numbers are in the files in
`logs/`.

| Log | Lines | What it shows |
|---|---|---|
| `b1_1712_capture_30s_press_hardfault` | 120-123 | First corrupted text, frame timeout, `retry 1/5` |
| | 124-214 | Timeout events every 2 ms, then about 70 `No Register EDM Timing Callback Fun` |
| | 215-234 | Hard fault (`CFSR 0x00020000`), then silent: the handler loops |
| `b2_1735_capture_30s_press_stuck` | 432-434 | Unhandled 0x012a and 0x0131 in 'Capturing' |
| | 448-524 | Answers `selftest` but never captures again, sleeps at the second inactivity |
| `b1_1716_capture_30s_press_freeze` | 240-314 | Last frame, then silent, no echo to `selftest` |
| `b2_1734_capture_30s_press_freeze` | 303-375 | Same |
| `b2_1740_capture_30s_press_freeze` | 18-115 | Same, twice |
| `b1_1711_capture_5s_press` | 268-280 | What recovery should look like: timeout, `retry 1/5`, next frame |
| `*_pressure_at_boot` | top of each | Corrupted text, then the boot stops (ww-hardware#70) |
| `b2_1729_tape_hm0360_latched` | 30, 163, 303, 460 | Not present after three RESETs, present after a power cycle |

The `nrf |` lines in the capture logs show the BLE processor never reacting (ww-hardware#69).

## Where in the code

AI processor (`dev` 22531acf):
- The watchdog only runs to force a reset before sleep:
  [`image_task.c:2794-2818`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/22531acfa426bc4976cce693e571773925ed3ef5/EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c#L2794-L2818).
  Nothing catches a freeze.
- The hard fault handler prints and loops; the reset is in an `#if 0`:
  [`hardfault_handler.c:93-104`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/22531acfa426bc4976cce693e571773925ed3ef5/EPII_CM55M_APP_S/app/ww_projects/ww500_md/hardfault_handler.c#L93-L104).
- In Capturing, only the EDM WDT timeouts restart the sensor; other datapath errors fall to the
  unhandled-event print:
  [`image_task.c:1145-1160`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/22531acfa426bc4976cce693e571773925ed3ef5/EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c#L1145-L1160),
  events in
  [`app_msg.h:78-86`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/22531acfa426bc4976cce693e571773925ed3ef5/EPII_CM55M_APP_S/app/ww_projects/ww500_md/app_msg.h#L78-L86).
- The EDM timing interrupt has no callback; the library's switch is
  `sensordplib_set_edm_timing_inlib()` in `library/sensordp/inc/sensor_dp_lib.h`.

BLE processor (ww-hardware `dev` cf24e9e): while the AI processor sleeps, a message pulses WAKE
with no timeout
([`aiStateMachine.c:280-291`](https://github.com/wildlifeai/ww-hardware/blob/cf24e9e866ebf3f43cc8f893ecd214171c2499b7/MokoTech/Workspace/WildlifeWatcher_1/aiStateMachine.c#L280-L291));
a response timeout only tells the app
([`:623-632`](https://github.com/wildlifeai/ww-hardware/blob/cf24e9e866ebf3f43cc8f893ecd214171c2499b7/MokoTech/Workspace/WildlifeWatcher_1/aiStateMachine.c#L623-L632)).
It can pull the AI processor's reset line (P0.06) but cannot switch its power.

## How to reproduce

Needs the HM0360 image and the console adapters on the AI processor (921600) and the BLE
processor (115200). Windows COM ports are exclusive: one tool at a time on the AI processor's.

**Camera check and pressure at boot**, with the photo app (`_Tools/ww500_ship_check.py`, runbook
`_Documentation/pcb_batch_flashing.md`, "Taking a photo"):

1. Run `python _Tools\ww500_ship_check.py --port COM6`. Without image files only the photo
   buttons work.
2. Click **Take a black and white photo**. Each click restarts the AI processor (it asks for
   RESET if the board sleeps), reads `selftest`, and either takes a photo ("No issues") or says
   the camera is disconnected.
3. Hold firm pressure on the HM0360 connector and click again. A failed boot is named in the
   window ("console output is garbled", "did not finish booting", "did not start"). Release,
   press RESET and click: it recovers.
4. If RESET keeps giving "disconnected" afterwards, the HM0360 is latched: unplug and replug the
   board without touching the ribbon, then click.

Each click goes to `~/ww500_ship_check/batch_<date>/<label>.log`.

**Pressure during a capture**, with the logger in this folder:

1. Close the photo app and run `py ribbon_bench.py --dir <folder> --himax COM6 --nrf COM5`. It
   logs both consoles with a time on every line, types each line appended to
   `<folder>/inbox_himax.txt`, and reconnects when the board is unplugged.
2. Press RESET, then append `capture 300 1000` to the inbox (one frame a second for 5 minutes).
3. After about 10 s, press firmly on the HM0360 connector for 30 s, then release.
4. Watch `<folder>/events.log`: the frames stop. Two minutes later append `selftest`. A healthy
   board answers; a failed one stays silent, or answers but never captures again.

## Files

- `ribbon_bench.py`: the logger used for the capture tests.
- `logs/`: one excerpt per event, named `b<board>_<time>_<what>.log`. The `*_pressure_at_boot`,
  `b1_1612` and `b2_1729` excerpts come from the photo app and carry only its `#####` times; the
  logger's have a time on every line. Full logs on request.
