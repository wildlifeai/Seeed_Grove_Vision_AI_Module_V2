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

**Decided 27 Oct:** this thread does problem 1 now. For problem 2 the flash layout makes room for
the board's clock adjustment, but the clock work itself waits until the save-state work is done.
The decisions are listed at the end.

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

Two 4 KB sectors at 0x00F00000, one for each kind of record:

| Sector | Record | Contents | Written when |
|---|---|---|---|
| 0x00F00000 | **Fast start** | The settings the first photo needs, and the last light state | At DPD entry, only if something has changed |
| 0x00F01000 | **Board** | The clock trim, plus any fine correction (see section 4) | Only when it is calibrated at production |

They are kept apart so that the frequent erases of the fast-start sector can never touch the
board's calibration. Both belong to the board, not to a firmware image, so both camera images use
them.

**For now, only the fast-start record is built.** The board sector is reserved, so the clock work
can add it later without changing the layout.

**How records are written.** Each record has a magic number, a format version, a sequence number,
the data and a CRC. A new record goes into the next blank slot in its sector. When the sector is
full, it is erased and the new record goes in the first slot.

**How a record is read, and the fall-back.** The newest record that passes every check (magic,
format version, CRC) is used. If none does, the C defaults in `fatfs_task.c` are used, and the next
sleep writes a fresh record. That one rule covers every unlikely case without special handling: a
blank sector (a new board), new firmware with a new format, a record with a bad CRC (a write
interrupted part way, which falls back to the record before it), or a power cut during an erase.
For the fast-start record the cost is small, since CONFIG.TXT is read a moment later anyway.

### 2. Fast-start settings

**A copy of CONFIG.TXT, not a replacement.** CONFIG.TXT stays the master copy that the app, the
console and people editing the card use. The record's only job is to shorten the time to the first
photo. There is one record, shared by both camera images. The RP3 exposure (ops 37 and 38) is only
used by the RP3 image, and the HM0360 image leaves it alone.

- **Stored:** the settings that affect the first photo: ops 5, 6, 9, 10, 12, 13, 14, 15, 29, 30,
  34, 35, 36, maybe 11 and 17. Plus state that changes with the light, not at every wake: op 25 (the
  last flash decision) and the RP3's last settled exposure and gain (#158; proposed as ops 37 and
  38, see section 5).
- **Not stored:** the counters (ops 0 to 4, 19, 20). They change at every wake and are not needed
  for the first photo, so they stay on the card. GPS and the deployment ID stay on the card too,
  since they are only needed when the photo is saved.
- **One path at every boot** (revised 27 Oct after Charles's review). Early in `app_main()`, before
  the tasks start, fill `op_parameter[]` from the record. If there is no valid record, use the
  defaults in `fatfs_task.c` (which match the shipped CONFIG.TXT). Then everything runs the same
  way, whichever source was used. The record is missing only before the first sleep after a cold boot
  (a new board, or new firmware with a new record format), and no photo is taken then, since
  captures are refused until the first sleep (`captureRefused()`, `CLI-commands.c`) and a cold boot
  is never a motion or timer wake. So by the first photo there is always a record.
- **When CONFIG.TXT has been read:** it wins for settings (for state, see section 5). If it differs (the card was edited or swapped), use the
  card's values and write a new record at the next sleep. A stale record only affects that one photo
  (for example flash or no flash). The exception is op 10: if the card says the camera is off,
  throw the photo away.
- **Wear:** writing only when something has changed means a few writes a day. Even one at every
  wake would erase each sector only once every 32 or so writes.

### 3. What changes in the tasks and their state machines

(Added 27 Oct after Charles's review.) Today the image task blocks at start-up on
`xSDInitDoneSemaphore` (`image_task.c:1997`) until the FatFS task has mounted the card, read
CONFIG.TXT, set up the image folder and loaded the staged camera registers (`fatfs_task.c:1759`).
So nothing in the image task's state machine ever runs before the card is ready. Starting the camera
earlier moves that wait to the first point that really needs the card.

**What the first photo needs from the card, and when:**

| What | Needed at | From the card today |
|---|---|---|
| Op parameters | Camera start | CONFIG.TXT. From the flash record instead |
| Staged camera registers (`camreg`) | RP3 initialisation, before the first frame | `cis_file_process(CAMERA_EXTRA_FILE)`, `image_task.c:2362` |
| NN model | NN start-up | Usually the flash already. The card only if the model is not in flash |
| File name, image folder, sequence number (ops 0, 19, 20) | `prepareJpegFile()`, after the frame and the NN | The directory manager, set up from the card |
| Deployment ID, GPS | `prepareJpegFile()` (EXIF) | CONFIG.TXT lines `I` and `G` |
| Writing the file | `APP_MSG_FATFSTASK_WRITE_IMAGE` | Already a message, which queues until the FatFS task is ready |

**The proposed change, in messages and states:**

1. **The image task no longer waits at start-up.** It starts with `op_parameter[]` already filled
   from the record and goes through INIT and CAPTURING as today.
2. **A new message, `APP_MSG_IMAGETASK_SD_READY`,** replaces the semaphore. The FatFS task sends it
   at the point where it now gives the semaphore. The image task records it in a flag, in whatever
   state it is in.
3. **A new image task state, `WAIT_FOR_SD`.** When a frame (and its NN result) is ready to be
   turned into a file, the image task checks the flag. If the card is not ready yet, it enters
   `WAIT_FOR_SD` and holds the frame in RAM. It goes on to `prepareJpegFile()` and the write when
   `SD_READY` arrives. In this state it ignores frame and data path events, and treats an inactivity
   event the way CAPTURING does.
4. **CONFIG.TXT differences are applied by message order.** The FatFS task updates
   `op_parameter[]` before it sends `SD_READY`, so the image task sees the card's values from that
   message onwards. If the card says op 10 = 0 (camera off), the held frame is discarded in
   `WAIT_FOR_SD`.
5. **The FatFS task itself does not change shape.** It still does its start-up work before entering
   its message loop, and messages sent to it meanwhile wait in its queue.

**Open points:**

- **The staged camera registers** are applied at sensor initialisation, before the first frame, and
  are read from the card. Reading the card from the image task while the FatFS task is mounting it
  is not allowed, since FatFS is not re-entrant. Decided: they go into the flash record as a
  separate, removable block (section 6).
- **The NN start-up** has to stay off the card when the model is in flash. That is the normal case
  at a warm boot, but needs checking in `cv_init()`.
- **The barriers:** `startupBarrier` and `shutdownBarrier` need checking. The image task now reaches
  its states in a different order relative to the FatFS task, and #208 already shows the shutdown
  barrier counts calls rather than tasks.

### 4. Clock adjustment (deferred until the save-state work is done)

The flash layout reserves the board record for this (section 1). Nothing else is built now. What
follows is kept as the plan for later.

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

**How each board gets its value: decided, at production.** The ship-check or batch-flashing tool
times the clock against the PC, and a console command writes the value to the board record.

(Not chosen: working the error out automatically from the BLE processor's time updates. It could be
added later if production calibration turns out not to be enough.)

### 5. The RP3 exposure (#158): an op parameter, the flash, or both

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

**Decided: the third column, in two stages.** First the two new ops, as a small change with no
flash work (step 2 below). Then the flash record carries ops 37 and 38 like any other op it copies.

**Two things this changes in section 2:**

- **For state, the flash wins over the card** (revised 28 Oct: cards do not move between cameras).
  The record and CONFIG.TXT are written at the same sleep, so they normally agree. Once the image
  task no longer waits for the card (step 5), ops 25, 37 and 38 may change in RAM before CONFIG.TXT
  loads, so the card's values are older: for state, the RAM value (started from the record) is kept.
  For settings the card wins, as in section 2. With no record at boot, the card's values are used.
- **Wear:** the exposure changes with the light, so write a new record only when it has moved by
  more than a threshold. On the bench (28 Oct) the settled exposure wandered about 15 % in a steady
  scene, so 10 % is too tight: set it in step 4 from a count of record writes (25 % is a likely start).

### 6. The extra camera registers: kept, but easy to remove

**What they are.** After each sensor initialisation, `cis_file_process(CAMERA_EXTRA_FILE)`
(`image_task.c:2362`) applies a short list of extra register writes from a file on the card:
`RPV3_EX.BIN` in the RP3 image and `HM0360EX.BIN` in the HM0360 image. The file mechanism is
Charles's (`cis_file.c`, March 2025). A later Claude session added the `camreg` console command,
which edits the list (at most 24 entries) and saves it back to the card. It was used in the RP3
image-quality work, for example to override the RP3's starting exposure as a stopgap for #158. If
the file is missing, nothing is applied.

**Decided 27 Oct:** keep them, copied into the flash record, but designed so they can be cut down or
removed later without touching the rest. That means:

- **A separate block in the record,** with its own count (0 means none). Nothing else in the record
  depends on it.
- **One function applies them from a table in RAM,** wherever the table came from. The RP3 and
  HM0360 initialisation never read the card for them. At boot the table is filled from the record.
  When the card has been read, the FatFS task loads the file as it does now. If the file differs from
  the record, the card's version is used from the next sensor initialisation and copied into the
  record at the next sleep, the same rule as for settings.
- **Removing them later** means deleting the block, its one call at sensor initialisation and the
  `camreg` command, and raising the record's format version. Nothing else changes.

Once ops 37 and 38 keep the RP3's exposure (#158), the exposure stopgap is no longer needed. That may
be the point to decide whether the extra registers are still wanted at all (see also #170, which asks
what is bench-only code).

## Left for later

- **The clock adjustment** (#245, section 4): after the save-state work, in this thread.
- **#56** (after a motion wake the clock carries on from when the chip went to sleep): that is a
  stale first read after DPD (`ww500_md.c:689`), not drift, so a correct rate does not help it. It
  needs the slower synced read, kept out of the path to the first photo, or the clock rebuilt from
  the alarm time on a timer wake. To come back to after the save-state work.
- **#152** (a restart sets the clock to 2024): could use the same flash area, by saving the time
  just before a deliberate restart (`reset`, `switchslot`, a firmware update). First check whether
  the clock keeps running through a watchdog reset, in which case no flash is needed. To come back
  to after the save-state work.
- **#217** (removing comments from CONFIG.TXT): not part of this thread. It should get a
  development thread of its own.

## Proposed steps

Each step is a separate change, tested on its own.

1. **Checks, no code:** the flash chip's part number (for endurance and sector-erase time), and, by
   reading the code, that loading a very large model can never erase past 0x00EFFFFF into the new
   area.
2. **RP3 exposure as ops 37 and 38** (#158, section 5), saved on the card. A small change on its own.
   It needs matching changes to the BLE processor's op list, the app and `config_file.md`.
3. **The flash record module:** read, append, erase and CRC, plus a console command to dump the
   records. The format includes the board record type, but only the fast-start record is written.
   No change to how the camera behaves. **Tested with a console command** (for example
   `xipstate test`), run by hand on the bench, perhaps only once per change to the module:
   - It writes enough records to fill the sector, erase it and start again at the first slot, and
     does that twice. With records of about 128 bytes that is about 65 writes and 2 erases, a
     negligible part of the flash's life.
   - After every write it reads the newest record back and checks it matches what was written, and
     it checks that each erase happens at the expected count.
   - It also writes one record with a bad CRC, and checks that the reader skips it and returns the
     record before it. Then it erases the sector and checks that the reader falls back to the C
     defaults.
   - It ends by writing the real record again, so the camera's state is as it was before the test.
   - It reports one line per erase and a final pass or fail, and it is refused while a
     capture is running.
4. **Fast-start settings:** write the record at DPD entry, and read it at every boot (or use the C
   defaults). The extra camera registers go in as their own block (section 6). To begin with, still wait for the card, so the record can be checked against
   CONFIG.TXT on every wake without changing any behaviour.
5. **Stop waiting for the card:** the `SD_READY` message and the `WAIT_FOR_SD` state (section 3),
   and change the SD driver's waits so the image task can run. Measure the time to the first frame
   with `Boot timing`.
6. **Optional extras** (revised 29 Oct, after step 5 showed the photo now waits for the card, not
   the camera): shorten the card's path. Remove the FatFS task's fixed 10 ms "settling" wait and
   cut `init_flash()`'s fixed 10 ms wait to 1 ms; later perhaps move the motion-wake HM0360 grid
   read out of `app_main()`. Moving the NN start-up after the capture is dropped: it would make the
   frame earlier but not the photo.
7. **Docs:** the flash map in `xip_manager.h` and `slot_selector.md`, and a short durable doc for the
   records.

Then the items in "Left for later", starting with the clock adjustment.

## Decisions (Charles, 27 October 2026)

1. **Clock calibration:** at production (A).
2. **Fast-start records:** one, shared by both camera images.
3. **Cache or master:** a cache. Its only purpose is to reduce the time to the first photo.
   CONFIG.TXT stays the master.
4. **Cold boots:** keep it simple. One boot path, with the C defaults when there is no record
   (section 2).
5. **#217:** kept separate, for a development thread of its own.
6. **#158:** two new op parameters (37, 38). Victor is expected to agree.
7. **#56 and #152:** left until the main save-state code is done.
8. **The clock adjustment:** room is made in the flash layout for it, but the work waits until the
   save-state work is done.
9. **The extra camera registers:** copied into the record as a separate block, designed to be easy
   to remove later (section 6).
