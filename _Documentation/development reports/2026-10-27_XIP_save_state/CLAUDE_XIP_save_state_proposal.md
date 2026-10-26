# Proposal: save settings and the clock adjustment in the XIP flash

#### File: `CLAUDE_XIP_save_state_proposal.md`
#### Path: `_Documentation\development reports\2026-10-27_XIP_save_state\CLAUDE_XIP_save_state_proposal.md`
#### Author: Claude (Opus 5.5), for review by Charles Palmer
#### Date: 27 October 2026

## What this is for

Two problems, one solution:

1. **The first photo waits for the SD card.** After a motion wake the camera cannot start until
   CONFIG.TXT has been read, and the SD card's own start-up takes 150 to 190 ms of the 315 to 346 ms
   from `app_main()` to the first frame (#251). The flash on the board can be read within a few ms.
2. **The AI processor's clock runs about 4 to 5 % fast** (#245). The trim register can correct it,
   but the right value probably differs from board to board, and the register does not survive DPD
   or a power cycle. So the value has to be kept somewhere and applied at every boot.

The XIP flash keeps its contents through DPD and power cycles, so both can be kept there.

## What is already known

From `2026-09-30_App_Comms_Bugs/CLAUDE_Time_to_first_photo.md` and `ww500_minimal/doc/README.md`:

- **Where the time goes on a motion wake:** SD mount about 170 ms (mostly the card itself),
  CONFIG.TXT and the image folder about 15 ms, RP3 start-up about 60 ms, the NN start-up about
  25 ms, the frame 50 ms. If the camera did not wait for the card, the first frame might arrive
  about 130 ms after `app_main()` (an estimate).
- **The SD driver's waits are busy-waits** in a higher-priority task, so even with the settings in
  hand the image task cannot run during the card start-up until those become `vTaskDelay()`.
- **Free flash:** 0x00F00000 to 0x00FEFFFF (960 KB) is reserved and unused. It is clear of both
  firmware slots, the model area and the slot selector.
- **Writing the flash:** erase is per 4 KB sector, and programming can only turn 1s into 0s. Writes
  already go through `disable_xip()` / `enable_xip()` in `xip_manager.c`, which hold the SPI mutex.
- **The trim** (`hx_drv_scu_set_RC32K1K_trim()`, measured on one board, 23 Sep): 31 (default)
  +4.4 %, 33 about 0 %, 34 -2.2 %. So each step is about 2 %, which is coarse. It reads back at 31
  after DPD or a power cycle. `ww500_md` never sets it.

## Proposal

### 1. One small area of flash, two kinds of record

At 0x00F00000, two 4 KB sectors used in turn (so a power cut during an erase never loses
everything). Each record has a magic number, a type, a format version, a sequence number, the data
and a CRC. New records go into the next blank slot, and the newest valid one wins. A sector is
erased only when it fills up.

| Record | Contents | Written when |
|---|---|---|
| **Board** | The clock trim, plus any fine correction (see 3) | Only when it is calibrated, perhaps a few times in the board's life |
| **Fast start** | The settings the first photo needs, and the last light state | At DPD entry, only if something has changed |

The board record belongs to the board, not to a firmware image, so both camera images use it.

### 2. Fast-start settings

**A copy of CONFIG.TXT, not a replacement.** CONFIG.TXT stays the master copy that the app, the
console and people editing the card use.

- **Stored:** the settings that affect the first photo: ops 5, 6, 9, 10, 12, 13, 14, 15, 29, 30,
  34, 35, 36, maybe 11 and 17. Plus state that changes with the light, not at every wake: op 25 (the
  last flash decision) and the RP3's last settled exposure and gain (#158; proposed as ops 37 and
  38, see section 4).
- **Not stored:** the counters (ops 0 to 4, 19, 20). They change at every wake and are not needed
  for the first photo, so they stay on the card. GPS and the deployment ID stay on the card too,
  since they are only needed when the photo is saved.
- **At a warm boot:** read the record before the card. If it is valid and matches this camera image,
  start the camera and take the first photo with it. The frame waits in RAM until the card is ready.
  If there is no valid record (a cold boot, new firmware, a corrupt record), wait for the card, as
  today.
- **When CONFIG.TXT has been read:** it wins for settings (for state, see section 4). If it differs (the card was edited or swapped), use the
  card's values and write a new record at the next sleep. A stale record only affects that one photo
  (for example flash or no flash). The exception is op 10: if the card says the camera is off,
  throw the photo away.
- **Wear:** writing only when something has changed means a few writes a day. Even one at every
  wake would erase each sector only once every 32 or so writes.

### 3. Clock adjustment

**First, a bench test that decides the design.** Does the trim hold while the chip is in DPD, or does
the oscillator go back to its default as soon as it sleeps? Set trim 33, sleep for 120 s on a timer,
and time the sleep on the PC. `ww500_minimal` already has the `rc32ktrim` command, so no new code is
needed.

- **If it holds:** apply the stored trim at every boot, as early as possible. This fixes the sleep
  timer (#245) and the clock's rate.
- **If it does not:** the trim only helps while awake, which is a small part of the time. The
  correction then has to be done in software: shorten each timer alarm by the measured error, and
  add the error to the clock after each sleep. The board record would hold an error figure (for
  example in parts per million) instead of a trim.

Either way, the steps of about 2 % leave up to about 1 % of error after trimming. A software
correction on top could get closer.

**How each board gets its value** (a question for you, below):

- **A. At production:** the ship-check or batch-flashing tool times the clock against the PC, and a
  console command writes the value to the board record.
- **B. Automatically:** each time the BLE processor sends the time, the AI processor compares it
  with its own clock. It knows how long it has been since the last time update, so it can work out
  the error and save it. That works on every board, in the field, with no extra step. It adds no
  extra cost, since the time update already happens. It depends on the BLE processor's own clock
  being accurate.

A could be first, with B added later.

### 4. The RP3 exposure (#158): an op parameter, the flash, or both

**The problem.** The RP3 forgets its exposure at every sleep. Each wake starts from the table value
(2368 lines, gain 1x), and auto-exposure may only take 3 steps per wake, so in bright light the first
photos come out white.

**The op parameter fix.** Two new ops (37 exposure, 38 gain; one op is 16 bits, and the values go up
to 5000 and 960). `ae.c` stores the settled values before sleep, and they are saved in CONFIG.TXT
with the rest of the state, as op 25 already is. At a wake, after the RP3 is initialised, the stored
values are written to the sensor's exposure and gain registers before the first frame. Setting
`ae.c`'s own variables is not enough, since `ae_notifySensorInit()` resets them because the sensor
has just gone back to its init table. They are state, not settings: the default is the table value,
they are ignored when op 29 = 0, the HM0360 image leaves them alone, and the app should not show
them as controls.

**Does the RP3 then have to wait for the SD card?** Yes, if the values are only on the card. But the
RP3 waits for the card today anyway, for all its other settings. So the op fix makes the first photo
**better** (#158) without making it **slower**, and the flash record makes it **faster** (#251). The
two do not compete. This thread is mostly for the RP3: the first photo needs both its settings and
its exposure, and only the flash can provide them before the card is ready.

| | Op parameters only (SD card) | Flash record only | Op parameters, copied into the flash record |
|---|---|---|---|
| Fixes the white first photos (#158) | Yes | Yes | Yes |
| First photo without waiting for the card (#251) | No | Yes | Yes |
| Works before the flash work is done | Yes, a small change on its own | No | Yes, the ops can come first |
| Readable with `getop`, kept with the card's state | Yes | No | Yes |
| Cross-repo cost | Two new op numbers (BLE processor, app, `config_file.md`) | None | Two new op numbers |
| Flash wear | None | A write whenever the exposure changes | The same as the flash record only |

**Suggestion: the third column, in two stages.** First the two ops, as a small change with no flash
work (it can go before step 2 below). Then the flash record carries ops 37 and 38 like any other op it
copies.

**Two things this changes in section 2:**

- **For state, the flash wins over the card.** The record and CONFIG.TXT are written at the same
  sleep, so they normally agree. When they do not, the card has been edited or swapped, and a swapped
  card may come from another camera and another scene. So for settings the card wins, as in
  section 2, but for state (ops 25, 37, 38) the flash record wins, since it belongs to this board.
- **Wear:** the exposure changes with the light, so writing a new record only when it has moved by
  more than, say, 10 % keeps flash writes to a few a day.

## What this does not fix

- **#56** (after a motion wake the clock carries on from when the chip went to sleep): that is a
  stale first read after DPD (`ww500_md.c:689`), not drift. A correct rate does not help it. It
  needs the slower synced read, kept out of the path to the first photo, or the clock rebuilt from
  the alarm time on a timer wake. Worth doing in the same thread, but separately.
- **#152** (a restart sets the clock to 2024): could use the same flash area, by saving the time
  just before a deliberate restart (`reset`, `switchslot`, a firmware update). First check whether
  the clock keeps running through a watchdog reset, in which case no flash is needed.

## Proposed steps

Each step is a separate change, tested on its own.

1. **Checks, no code:** the trim-in-DPD test above; the flash chip's part number (for endurance and
   sector-erase time); and, by reading the code, that loading a very large model can never erase past
   0x00EFFFFF into the new area.
2. **The flash record module:** read, append, erase and CRC, plus a console command to dump the
   records. No change to how the camera behaves.
3. **Clock adjustment:** an `rc32ktrim` command in `ww500_md` that can save to the board record, and
   the stored value applied at boot. Measure the sleep timer before and after (#245).
4. **Fast-start settings:** write the record at DPD entry, read it at a warm boot, and compare with
   CONFIG.TXT. To begin with, still wait for the card, so the record can be checked against
   CONFIG.TXT on every wake without changing any behaviour.
5. **Stop waiting for the card:** start the camera from the record, and change the SD driver's waits
   so the image task can run. Measure the time to the first frame with `Boot timing`.
6. **RP3 exposure** (#158): see section 4. Ops 37 and 38 can be done at any point, even before
   step 2. Once step 4 is in, the record carries them too.
7. **Optional extras:** move the NN start-up after the capture start (about 25 ms), and remove the
   two fixed 10 ms waits in the SD start-up.
8. **Docs:** the flash map in `xip_manager.h` and `slot_selector.md`, and a short durable doc for the
   records.

## Questions for you

1. **Calibration:** A (at production), B (automatic, from the BLE processor's time updates), or A
   first and then B? Does the BLE processor keep time from a crystal?
2. **One fast-start record or two?** Both camera images share the same CONFIG.TXT today, so one
   record would do for the settings. The RP3 exposure only means something to the RP3 image. My
   suggestion: one shared record, with the camera's own state tagged by the image it came from.
3. **Is a cache enough?** Or would you rather the flash become the master copy of the settings, with
   CONFIG.TXT only read when the card has changed? That would save more time but is a bigger change,
   since today the card can be edited on a PC.
4. **Cold boots:** keep waiting for the card as today (my suggestion, since a cold boot is when the
   card or firmware is most likely to be new)?
5. **#217 (removing comments from CONFIG.TXT):** fold it into this work, since the save path is
   being touched anyway, or keep it separate?
6. **#158:** two new ops (37, 38) first, on their own, then copied into the flash record (section
   4)? Is Victor happy with two new op numbers?
7. **#56 and #152:** in this thread (as separate steps after step 3), or a thread of their own?
