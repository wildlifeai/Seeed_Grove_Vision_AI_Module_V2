# #295: leave the EXIF date out when the photo time is not plausible

#### File: `CLAUDE_295_no_exif_date.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/CLAUDE_295_no_exif_date.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 27 November 2026

## In short

- **The issue:** [#295](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/295),
  raised by Victor. Thread [README](README.md).
- **The fault.** After a reset, until the BLE processor has the time, photos are stamped
  `2024-01-01` plus the time since the reset. A wrong date goes into the data unnoticed.
- **Proposal, three parts:**
  1. **No date unless the photo time is plausible.** If the photo time is before 2026, whatever it
     came from (the RTC, or a `setutc` from the BLE processor, whose own clock could be wrong),
     leave out `DateTimeOriginal` and `CreateDate`, and say so on the console.
  2. **No overwrite.** Those 2024 times can repeat after another reset, and a photo with the
     same name overwrote the old one. Image files are now created only if the name is free; if not,
     the name's last digit is stepped.
  3. **Tidy-up with it.** Move the WW500 photo EXIF assembly (about 150 lines) out of
     `image_task.c` into a new `exif_photo.c`. Same EXIF content, apart from part 1.
- **Agreed already:** Victor, in #295 (the website stores a photo with no date). The rule:
  Charles, 25 November, revised 27 November (below).
- **State: fixed.** Benched on both camera images 28 November 2026, committed `da5f974b`, #295
  closed.
  `image_task.c` went from 3,439 to 3,309 lines.

## 1. No date unless the photo time is plausible

**The rule** (Charles, 27 November, replacing the 25 November version in the README): **leave the
date out if the photo time is before 2026, whatever its source.** The photo time comes from
`exif_utc_get_time_at_tick()`: the `setutc` time if one arrived this wake, otherwise the RTC.
Neither is trusted on its own:

- the RTC restarts at 2024-01-01 after a reset and runs about 5 % fast (#299);
- a `setutc` carries the BLE processor's clock, which could itself be wrong.

So the test is on the final photo time, the same value used for the file name: **year before
`EXIF_PHOTO_FIRST_VALID_YEAR` (2026, `exif_photo.h`)**. No flag about where the time came from.

`RTC_FIRST_VALID_YEAR` (2025, `exif_utc.c`) is unchanged: it decides something else, whether the
RTC has ever been set and needs writing at sleep. A later threshold for photos is safer, and costs
nothing, as no photo is taken before 2026.

**Changes:**

| File | Change |
|---|---|
| `exif_builder.h` | `ExifInput_t` gets `bool timestamp_known` |
| `exif_builder.c` | If false, `DateTimeOriginal` and `CreateDate` are not written: the IFD0 count drops by 2 |
| `exif_photo.c/.h` (new, part 3) | `EXIF_PHOTO_FIRST_VALID_YEAR` 2026. Sets `timestamp_known` from the photo year. Console: `Photo time 2024-01-01 00:03:05 is before 2026: no EXIF date` |

**Unchanged:** the file name (8 hex digits from the photo time, as #295 asks), the FAT file date,
and every other EXIF field.

## 2. No overwrite (from the #295 comment)

Image file names are the photo time in seconds, times 16, plus a digit for photos in the same
second. After a reset with no time, the RTC restarts at 2024-01-01 00:00:00, so a photo taken the
same number of seconds after a later reset, in the same folder, gets the same name.
`fileWriteImage()` opens with `FA_CREATE_ALWAYS`, so the older photo is lost.

**Change** (`fatfs_task.c`, `fileWriteImage()` only, the image writes):

- Open with `FA_CREATE_NEW`. If the name exists (`FR_EXIST`), step the last hex digit of the name
  (`...0.JPG` to `...1.JPG` and on, up to `F`) and try again. Console:
  `'B08C7110.JPG' exists: saved as 'B08C7111.JPG'`.
- If all 16 exist, the write fails with an error (no overwrite).
- `image_task.c` copies the name into `lastImageFileName` (used by `txfile`) when the write has
  completed, not before, so it holds the name actually used.

Other file writes (CONFIG.TXT, file transfers) keep their behaviour.

## 3. Tidy-up: `exif_photo.c`

`image_task.c` is 3,439 lines. `prepareJpegFile()` has about 150 lines that decide what a WW500
photo's EXIF says: camera model, software string, NN data, the UserComment labels, the MakerNote
(HM0360 gain and white balance), the deployment ID, the timestamp. `exif_builder.c` is kept
generic (it lays out whatever `ExifInput_t` holds, and knows no camera types).

**Move** that assembly to a new `exif_photo.c/.h`:

```c
// Builds the EXIF for one photo into exif_builder.c's buffer (exif_get_buffer()). Returns its length, 0 on failure
uint32_t exif_photo_build(uint16_t width, uint16_t height, const rtc_time *photoTime, uint8_t flashFired,
		const int8_t *outCategories, uint8_t classCount, const HM0360_GAIN_T *gain);
```

The width and height are passed in too, so `exif_photo.c` needs no camera header. `gain` may be
NULL (no MakerNote), for a build without the HM0360.

- `prepareJpegFile()` keeps the JPEG buffer handling, the photo time, the file name and the file
  operation, and calls `exif_photo_build()`. It shrinks from about 210 to about 60 lines.
- `ENABLE_EXIF_CONFIDENCE` and `EXIF_MAKER_NOTES` move with the code. The note in `cvapp.h` that
  refers to `image_task.c` is updated.
- `gain` and the flash state are passed in, so `exif_photo.c` reads no image-task variables.
- New file in the layout of `_Documentation/c_file_format.md`.

**A pure move:** apart from part 1, the EXIF bytes are unchanged. Checked on the bench by comparing
a photo's tags with exiftool before and after.

**Not proposed here:** the bench test code in `image_task.c` (`INVESTIGATE_BMP`,
`INVESTIGATE_FLASH_BRIGHTNESS`, tone mapping: `prepareBmpFile()`, `incrementBrightness()`,
`incrementToneMapping()`, about 200 lines) could move to an `image_test.c` or be deleted. That is
Charles's decision, as a separate change.

## 4. Docs

Done with the code:
- `WW500_EXIF_refactor.md` (`ww500_md/doc/`): an update note on `exif_photo.c` and `timestamp_known`.
- `WW500_Neural_Network_Operation.md`: `prepareJpegFile()` now calls `exif_photo_build()`.
- `cross-repo-contracts.md`: the date tags can be absent.
- `hardware-traps.md`: no EXIF date before 2026; image files never overwritten.
- `cvapp.h`: its note about `ENABLE_EXIF_CONFIDENCE` now points at `exif_photo.c`.

## 6. Bench results

**RP3 image, built 07:48 on 28 November** ([`logs/ai18.txt`](logs/ai18.txt)). Charles checked normal
photos with exiftool first: tags as before, with dates. Then, with 16 dummy files
`7DD2BBC0.JPG` to `7DD2BCB0.JPG` (5 bytes each) in `/MEDIA/4f2a9c1e/IMAGES.018`, `hack 6` so the BLE
processor had no time, and `utctests` to set the RTC to 2025-03-21 09:05:00:

| Capture | Console |
|---|---|
| 1 | `No setutc after 300ms: photo time from the AI processor's RTC`; `Photo time 2025:03:21 09:05:08 is before 2026: no EXIF date`; `'7DD2BC40.JPG' exists: saved as '7DD2BC41.JPG'` |
| 2, 3 | Photo times 09:05:18 and 09:05:27: no EXIF date; names `7DD2BCE0.JPG`, `7DD2BD70.JPG`, past the dummies, so no clash |

A cheaper test than a reset: `utctests` puts the RTC at a known time, so the names are known to the
second, and it checks the rule on a 2025 time as well as a 2024 one.

exiftool on `7DD2BC41.JPG` (Charles, 28 November): no `DateTimeOriginal` or `CreateDate`; `Model`,
`Software`, the deployment ID (`0xf200`), GPS, `UserComment` and `MakerNote` all present. The dummy
`7DD2BC40.JPG` kept.

**HM0360 image, 28 November:** a normal photo with dates, and one after `utctests` without: both as
expected (Charles; no log kept).

## 5. Bench test

1. **A normal photo, before and after the change:** on a wake with `setutc`, `AI capture 1 1`.
   exiftool shows the same tags as before (part 3 is a pure move), including both dates.
2. **RTC time before 2026:** `AI reset`, then `hack 6` so the BLE processor has no time either.
   After the first sleep, `AI capture 1 1`. Expect the console line, and no
   `DateTimeOriginal`/`CreateDate` in exiftool. The file is named from the 2024 time.
3. **An old `setutc`:** from the app, give the BLE processor an old time (`setutc 2025-06-01T00:00:00Z`,
   no `AI ` prefix). At the next wake it passes that to the AI processor. `AI capture 1 1`: expect the
   console line and no date, though a `setutc` arrived. Then give it the right time again.
4. **No overwrite:** copy a photo on the PC to a name the next photo will take (from the 2024
   case: repeat step 2 and take the photo at the same time after the reset), or simply create a
   file with the expected name. Expect `exists: saved as ...` and both files kept.
5. Both camera images.
