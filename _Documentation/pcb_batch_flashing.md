# Flashing and camera-checking a batch of WW500 boards
#### Claude (Opus 5.5), reviewed by Victor Anton, 25 September 2026

`_Tools/ww500_ship_check.py` puts both Himax images on a board and proves both cameras
take a photo, one board at a time, from a small window with one button. The operator plugs
in a board, presses its RESET button and clicks; the tool does the rest in under two
minutes. First used on 25 September 2026 for ten PCBs on production firmware (main
`9da3da3e`), all of which passed.

It covers the Himax only. The nRF (BLE) firmware is updated separately, through the app.

## What you need

* A USB-serial adapter on the board's **Himax console header**, 921600 baud. The adapter
  powers the board. Find its COM port in Device Manager ("USB Serial Port"); it is the one
  that prints clean text at 921600 when the board resets.
* Python 3.10 or later with `pip install pyserial xmodem Pillow`.
* The two images. Either let the tool download them (`--release`, needs the GitHub CLI
  `gh`, signed in) or pass the `.img` files yourself.
* Close TeraTerm or any other program holding the port: Windows COM ports are exclusive.

## Run it

Ship what production serves (the newest successful release run on `main`):

```
python _Tools\ww500_ship_check.py --port COM6 --release latest
```

Or name a run, or the files:

```
python _Tools\ww500_ship_check.py --port COM6 --release 35817728276
python _Tools\ww500_ship_check.py --port COM6 --hm0360 H6923D42.IMG --rp3 R6923D42.IMG
```

Useful options: `--first 5` starts the labels at PCB-05; `--require-sd` fails a board whose
SD card does not mount; `--ship-op7` and `--ship-op8` change the settings every board is
left with (defaults 0 and 1000, see below).

## Per board

1. Plug the adapter onto the board.
2. Check the **label** in the window matches the board, and correct it if not.
3. Press the board's **RESET** button, then click **I have reset the board - go**. The
   order does not matter: the tool is listening all the time.
4. Watch the line under the button. If it turns red and says **Press the RESET button**,
   press it again (the tool beeps).
5. Green **All checks passed**: glance at the two photos, unplug, next board. The label
   moves on by itself.
6. Red **FAILED**: reseat the camera ribbon(s), press RESET and click again **with the same
   label** (after a failure the label does not move on). A second failure goes on the
   faulty pile: type the next label and carry on.

**One label per physical board.** Every click adds a row to the summary under the label in
the window, so clicking again on the same board under a new label makes it look like two
boards. On 25 September the same SD card's image counter ran on through the runs labelled
PCB-01 to PCB-04, which is how a relabelled rerun shows up afterwards.

## What PASS means

| Check | How |
|---|---|
| Both images written | XMODEM transfer acknowledged, both images boot and print their build |
| Right image in each slot | Each boot reports its camera (`Camera: RP v3 (IMX708)`, `Camera: HM0360`) and labels its own slot |
| Both cameras connected | The RP3 image reports `HM0360 present at 0x24` and `Main camera present at 0x1a` |
| The colour camera takes a photo | A JPEG streamed from the RP3 image decodes, is over 3 KB, has contrast (grey-level spread) of at least 6 and a mean brightness between 8 and 248 |
| The night camera takes a photo | The same, from the HM0360 image |

The limits only catch a dead, disconnected or covered camera. Focus, colour and dirt on the
lens are for the operator's glance at the two thumbnails, which is why the photos are shown.

The summary also records, without failing the board, whether the **SD card** mounted and
the **self-test bits** (`0800` = no SD card, `0200` = HM0360 not answering, `0100` = main
camera missing).

## What the board is left with

* The HM0360 image in one slot and the RP3 image in the other, both labelled, **HM0360
  active** (the default camera).
* op 7 = 0 (timelapse off) and op 8 = 1000 (sleep 1 s after going idle). `setop` saves
  these to the SD card's `CONFIG.TXT`, which both images share, and a board without a card
  runs on the same values as firmware defaults. Change them with `--ship-op7`/`--ship-op8`.
* No photos on the SD card: test photos stream over the cable (`preview 1`).

## Output

Default folder `~/ww500_ship_check/batch_<date>` (change with `--logdir`):

* `summary.csv`: one row per run, with the time, label, result and reason, firmware build,
  both photo measurements, the camera presence lines, SD card state, self-test bits and
  the final `slots` line.
* `<label>.log`: every byte the board printed, with `#####` lines marking each step.
* `<label>_RP3.jpg`, `<label>_HM0360.jpg`: the photos that were judged.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| "Waiting for the USB adapter on COMn" | The port is missing or held by another program. Plug the adapter in, close TeraTerm, check `--port` |
| "Press the RESET button" keeps coming back | The bootloader was never seen: wrong port, adapter on the wrong header, or board unpowered |
| "transfer of ... failed" | Retried automatically once (it asks for RESET). A second failure: reseat the adapter and rerun |
| RP3 or HM0360 "no photo" | Reseat that camera's ribbon and rerun with the same label |
| "camera missing on I2C" | The camera did not answer at all: ribbon unplugged or faulty module |
| Summary shows `sd_card` missing, `selftest` 0800 | No SD card, or one that does not mount. Only a failure with `--require-sd` |

## Firmware behaviour the tool depends on

Each of these cost time to find; keep them in mind before changing the tool.

* **The ROM bootloader listens for 30 ms after a reset** (`Please input any key to enter
  X-Modem mode in 30 ms`). A person cannot hit that; the tool streams `1` continuously while
  it waits. DTR and RTS do not reset these boards. After a burn the bootloader asks
  `Do you want to end file transmission and reboot system? (y)`; answering `y` while still
  streaming catches the next window, so two images need one RESET press.
* **The console takes one character at a time and runs a command on `\n` only.** Type 30 ms
  apart, end with `\r\n` (a bare `\r` is ignored, so nothing happens), and send Ctrl-C first
  to clear stray characters.
* **The RP3 image cannot take a photo straight after a cold boot** on firmware without the
  I2C slave-ID fix (branch `fix/cis-i2c-slave-id-nesting`): `hm0360_md_init()` leaves the
  shared bus pointing at the HM0360, so the IMX708 never starts streaming and every frame
  times out. A deployed unit only meets this on its first photo after power-up. The tool
  therefore sets op 7 to 5 s, sends `dpd`, and takes the photo after the RTC wake, which
  re-initialises the IMX708. With the fix merged this step is harmless.
* **Deliberate reboots happen at the next sleep**, and an inactivity countdown already
  running keeps its old length, so after `switchslot` the tool sends `dpd` rather than
  waiting up to a minute.
* **Preview frames on the HM0360 image have other output printed inside them**: the report
  to the BLE processor lands about 5,000 characters into every frame. The tool rebuilds the
  JPEG from the long base64 runs and accepts it only if it fully decodes.

See also [`live_preview.md`](live_preview.md) for the preview stream,
[`firmware_update_and_recovery.md`](firmware_update_and_recovery.md) for single-board
flashing and recovery, and [`dual_image_build_and_flash.md`](dual_image_build_and_flash.md)
for building your own pair of images.
