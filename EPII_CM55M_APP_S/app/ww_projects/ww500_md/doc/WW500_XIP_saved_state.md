# WW500 saved state in the XIP flash

#### File: `WW500_XIP_saved_state.md`
#### Author: Claude (Opus 5.5), with Charles Palmer
#### Date: 30 October 2026

How the AI processor keeps a copy of its settings in flash, so a wake can take its first frame
without waiting for the SD card. How the work was done, and the bench results, are in
`_Documentation/development reports/2026-10-27_XIP_save_state/`.

## In short

- At sleep the AI processor writes a **fast-start record** to flash if any setting has changed,
  with or without an SD card.
- At the next wake it reads the record (about 5 ms after the scheduler starts) and the camera
  starts at once. The SD card mounts at the same time, and nothing waits for it until it is
  needed: the file name, the EXIF values, the counters and the photo itself.
- **CONFIG.TXT stays the master.** When it loads, it overrides the record's settings, and the
  console reports any difference. The record is only a cache to save time.
- **With no card,** the record is all there is: a `setop` or BLE change survives sleeps (since
  31 October 2026). When a card goes back in, its CONFIG.TXT wins.
- With no valid record (new board, new record format) the C defaults are used and the camera
  waits for CONFIG.TXT, as before.

## The flash area

| Physical address | Contents |
|---|---|
| 0x00F00000 to 0x00F00FFF | Fast-start records: 16 slots of 256 bytes |
| 0x00F01000 to 0x00F01FFF | Board record: the RTC calibration (see below), 16 slots of 256 bytes |

The full flash map is in `xip_manager.h`. Records are only ever written through
`xip_state_area_read/write/erase()` in `xip_manager.c`, which refuse any address outside this area.

- **Records:** each has a magic number, a format version (`XIP_STATE_FORMAT_VERSION`), a type, a
  sequence number and a CRC. A new record goes into the next free slot; when the sector is full it
  is erased first. The reader takes the valid record with the highest sequence number, so a bad
  CRC or a power cut during a write falls back to the previous record.
- **Wear:** a record is written only when something has changed, so a camera left alone writes
  rarely. One erase per 16 changes.
- **Not touched by** an xmodem load of the bootloader or firmware, a firmware update from the SD
  card, or a model load (the model size check in `xip_copy_model_from_sd_to_flash()` stops a model
  reaching 0x00F00000).

## What the fast-start record holds

- **32 op parameters:** all except the counters (ops 0 to 4, 19, 20), which only CONFIG.TXT holds.
- **The extra camera registers** staged with `camreg`, with the camera image that wrote them. A
  record written by the other camera image keeps its registers but they are not used.
- **State ops 25, 37 and 38** (the HM0360 and RP3 exposure state) are treated differently: during
  a wake they may already have moved from the record's value, so CONFIG.TXT does not override
  them (`xip_state_keepStateOp()`). The RP3 exposure only counts as a change when the total light
  differs by more than 25 %, so it does not cause a write at every sleep.

To change the record layout, raise `XIP_STATE_FORMAT_VERSION`. Older records are then ignored
and the first wake uses the C defaults.

## The board record

Values that belong to this board, with their own format version (`XIP_STATE_BOARD_VERSION`), so a
change to the fast-start record never loses them. It holds the **RTC calibration** (`rtc_cal.c`,
#245): the RC32K oscillator's error, which is 2 to 5 % fast and differs from board to board.
- Before sleep, on a cold boot, when there is no saved value, and once a day of RTC time, the board
  times one RTC second between two RTC alarm interrupts against the crystal (core busy 2 to 3 s) and
  saves the result in ppm. About one write a day.
- Each DPD alarm is then set to more RTC seconds, so the sleep lasts the time asked for. The console
  prints `RTC correction N ppm` and `Sleep Ns = M RTC s` before each sleep.
- Only the sleep is corrected: the RTC's time of day still runs fast between `setutc`s.

## The order of a wake

1. `app_main()` creates the tasks. The image task runs above the FatFS task (priorities 2 and 1),
   so the camera is not held up by the card's start-up.
2. The FatFS task reads the record (`xip_state_loadFastStart()`) and releases the image task
   (`fatfs_waitForSettings()`), then mounts the card and loads CONFIG.TXT.
3. The image task initialises the sensor and starts the capture. On a timer or motion wake the NN
   starts just after the capture has started, when the model is already in flash.
4. When the frame is ready the NN runs and its result is sent to the BLE processor at once.
5. The image task then waits for the card (`fatfs_waitForSdReady()`), adds the NN result to the
   counters and saves the photo. If CONFIG.TXT says the camera is off (op 10 = 0), the frame is
   not saved.
6. At sleep the FatFS task saves CONFIG.TXT (if there is a card) and writes a new record if anything
   has changed (card or not).

Each wake prints its timings once the first photo is queued, in ms since `app_main()`:

```
Boot timing (timer wake), ms since app_main(): cameras checked 7, scheduler started 11, flash record read 16,
SD mounted 273, CONFIG.TXT loaded 278, image dir ready 282, SD ready 284, camera ready 31, capture started 42,
frame ready 76, photo queued 286,
```

Typical `frame ready` with a model in flash (30 October 2026): RP3 timer wake 115 to 119 ms,
HM0360 timer wake 75 ms, HM0360 motion wake 92 ms, RP3 motion wake about 120 ms. In good light:
the RP3's capture takes longer in dim light (24 ms at 273 to 588 lines of exposure, 54 ms at
2548).

On a motion wake `app_main()` first reads the HM0360's motion grid (`hm0360_md_saveWakeGrid()`):
it must, since the HM0360 keeps taking motion-detection frames while the AI processor is awake
and the next one overwrites the grid. The read is one I2C transaction, about 4 ms.

On the HM0360 image the first frame after a motion wake comes about 92 ms after `app_main()`
however early the capture starts, and whatever op 11 is. The sensor, not the firmware, sets that
time. The cause is not known.

## Console messages

| Message | Meaning |
|---|---|
| `Fast-start record: seq N loaded (32 ops, R extra camera registers)` | Record read at boot |
| `Fast-start record: none, using the C defaults` | No valid record: this wake waits for the card |
| `Fast-start record: seq N matches CONFIG.TXT` | Checked after CONFIG.TXT loaded |
| `Fast-start record: seq N differs from CONFIG.TXT:` then `op X: record A, CONFIG.TXT B` | CONFIG.TXT was edited since the last sleep (on a PC, for example). Its values are used |
| `Fast-start record: written, seq N slot S` / `unchanged (seq N)` | At sleep |

## Commands

- `xipstate`: lists every slot of both sectors and the newest record's values.
- `rtccal`: shows the RTC calibration. `rtccal measure` measures and saves it now; `rtccal off`
  stops the correction and the measuring (to time the uncorrected sleep), `rtccal on` restores them.
- `xipstate test`: the bench test of the record module. It fills the fast-start sector twice
  (33 writes, 2 erases), checks a bad CRC is skipped, and ends by writing back the record that was
  there. Needed only after a change to `xip_state.c`.

All are refused while the camera is busy, since flash access switches XIP off.

## Code

| File | What |
|---|---|
| `xip_state.c/.h` | Records: read, write, the fast-start payload, the comparison with CONFIG.TXT |
| `xip_manager.c/.h` | Flash access to the area, and the flash map |
| `fatfs_task.c` | Reads the record at start-up, compares after CONFIG.TXT, writes at sleep; `fatfs_waitForSettings()`, `fatfs_waitForSdReady()` |
| `image_task.c` | Waits for the card only where it needs it; `initNN()`, `countNNResult()` |
| `boot_timing.c` | The `Boot timing` line |
| `rtc_cal.c/.h` | The RTC calibration: measurement, when it is due, the alarm correction |
| `sleep_mode.c` | Applies the alarm correction at DPD entry |
