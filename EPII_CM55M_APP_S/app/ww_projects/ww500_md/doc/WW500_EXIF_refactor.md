# EXIF Builder Refactor: Extracting EXIF Code from image_task.c
#### CGP — June 2026

## Motivation

`image_task.c` is 3034 lines. Approximately 700 lines are devoted to EXIF construction:
constants, enums, static buffers, six helper functions, and `build_exif_segment()` itself.
This code has no logical dependency on the image-capture state machine and should live in
its own translation unit.

Separating the EXIF builder will:
- Reduce `image_task.c` by roughly 700 lines
- Allow the EXIF code to be read and maintained independently
- Give the EXIF module a clean, versioned API that is easy to unit-test
- Place related EXIF code (`exif_builder`, `exif_utc`, `exif_gps`) together as a group

---

## Proposed New Files

| File | Purpose |
|------|---------|
| `exif_builder.h` | Public API: constants, enums, `ExifInput_t`, function declarations |
| `exif_builder.c` | All EXIF construction logic |

These names sit naturally alongside the existing `exif_utc.c`/`.h` (RTC timestamp
formatting) and `exif_gps.c`/`.h` (GPS coordinate conversion), which are kept unchanged.

---

## What Moves to `exif_builder.h`

### Constants

```c
#define EXIF_MAX_LEN              1024
#define EXIF_MAX_DYNAMIC_CLASSES  4
#define EXIF_MAX_LABEL_LEN        20
#define IFD0_ENTRY_COUNT          9
#define GPS_IFD_ENTRY_COUNT       6
#define GPS_IFD_SIZE              (2 + GPS_IFD_ENTRY_COUNT * 12 + 4)
#define EXIF_COMMENT_LENGTH       256
```

### Enums

```c
typedef enum {
    TAG_X_RESOLUTION       = 0x011A,
    TAG_Y_RESOLUTION       = 0x011B,
    TAG_RESOLUTION_UNIT    = 0x0128,
    TAG_DATETIME_ORIGINAL  = 0x9003,
    TAG_CREATE_DATE        = 0x9004,
    TAG_MAKE               = 0x010F,
    TAG_MODEL              = 0x0110,
    TAG_GPS_IFD_POINTER    = 0x8825,
    TAG_GPS_LATITUDE_REF   = 0x0001,
    TAG_GPS_LATITUDE       = 0x0002,
    TAG_GPS_LONGITUDE_REF  = 0x0003,
    TAG_GPS_LONGITUDE      = 0x0004,
    TAG_GPS_ALTITUDE_REF   = 0x0005,
    TAG_GPS_ALTITUDE       = 0x0006,
    TAG_NN_DATA            = 0xC000,
    TAG_USER_COMMENT       = 0x9286,
    TAG_MAKER_NOTE         = 0x927C,
    TAG_DEPLOYMENT_ID      = 0xF200,
    TAG_WW_CONFIDENCE_BASE = 0xF300
} ExifTagID;

typedef enum {
    TYPE_BYTE     = 1,
    TYPE_ASCII    = 2,
    TYPE_SHORT    = 3,
    TYPE_LONG     = 4,
    TYPE_RATIONAL = 5,
    UNDEFINED     = 7,
    SLONG         = 9,
    SRATIONAL     = 10
} ExifDataType;
```

### Input Data Struct

The caller (`prepareJpegFile` in `image_task.c`) populates this struct from its own
context and passes it to `exif_build_segment()`. This keeps `exif_builder.c` decoupled
from image-task internals (HM0360 AE register types, CV module, FatFS task API).

```c
typedef struct {
    uint16_t     width;           /* image width in pixels */
    uint16_t     height;          /* image height in pixels */
    char         timestamp[22];   /* "YYYY:MM:DD HH:MM:SS\0" from exif_utc */
    const char  *deployment_id;   /* UUID string, or NULL if no deployment active */
    const uint8_t *nn_data;       /* length-prefixed byte array: [total][count][score...] */
    const char  *user_comment;    /* NN label summary string, or NULL */
    const char  *maker_note;      /* AE register CSV string, or NULL */
} ExifInput_t;
```

Passing `NULL` for optional fields (`deployment_id`, `user_comment`, `maker_note`) causes
those IFD entries to be skipped. The caller already has all these values assembled before
calling the builder.

### Public Functions

```c
/*
 * Build a complete EXIF APP1 segment into the module-internal buffer,
 * including SOI marker and sector-alignment COM padding.
 * Returns total byte count (SOI + EXIF + padding), or 0 on error.
 * The returned buffer is valid until the next call to exif_build_segment().
 */
uint16_t exif_build_segment(const ExifInput_t *input);

/*
 * Returns a pointer to the internal EXIF buffer.
 * Only valid after a successful call to exif_build_segment().
 */
const uint8_t *exif_get_buffer(void);
```

---

## What Moves to `exif_builder.c`

All of the following, currently in `image_task.c`:

| Symbol | Current location | Action |
|--------|-----------------|--------|
| `exif_buffer[]` static array | `image_task.c` line 391 | Move; stays `static` |
| `next_data_ptr` static pointer | `image_task.c` line 394 | Move; stays `static` |
| `tiff_start` static pointer | `image_task.c` line 396 | Move; stays `static` |
| `write16_le()`, `write32_le()`, `write16_be()` | lines 2216–2232 | Move; stays `static` |
| `addIFD()` | lines 2244–2349 | Move; stays `static` |
| `create_gps_ifd()` | lines 2352–2409 | Move; stays `static` |
| `get_gps_ifd_size()` | lines 2412–2420 | Move; stays `static` |
| `build_exif_segment()` | lines 2521–2787 | Move; **rename** to `exif_build_segment()`, change signature |

The sector-alignment COM padding currently in `prepareJpegFile` (lines 2035–2049) also
moves into `exif_build_segment()` — it operates on `exif_buffer` and belongs logically
inside the EXIF module.

### Headers that `exif_builder.c` will include

```c
#include "exif_builder.h"
#include "exif_gps.h"      // exif_gps_deviceLat/Lon/Alt, GPS helper functions
```

`exif_utc.h` is **not** needed in `exif_builder.c` because the caller pre-formats the
timestamp into `ExifInput_t.timestamp`. This follows the preferred decoupling pattern
(see *What Stays in image_task.c* below).

---

## What Stays in `image_task.c`

### Abbreviated `prepareJpegFile()`

After the refactor, `prepareJpegFile` assembles `ExifInput_t` from local state and calls
`exif_build_segment()`. The sector-alignment padding and the EXIF buffer declaration are
gone from this function.

```c
static void prepareJpegFile(int8_t *outCategories, uint8_t classCount,
                             fileBufferInfo_t *extraBlock)
{
    ExifInput_t exif_input;
    char maker_note[MAKERDATALEN];

    /* populate ExifInput_t from local state */
    exif_input.width  = (uint16_t)app_get_raw_width();
    exif_input.height = (uint16_t)app_get_raw_height();
    exif_utc_get_rtc_as_exif_string(exif_input.timestamp, sizeof(exif_input.timestamp));

    /* NN data — raw score array */
    /* ... assemble nnData as before ... */
    exif_input.nn_data = nnData;

    /* UserComment */
    /* ... build user_comment string as before ... */
    exif_input.user_comment = (user_comment[0] != '\0') ? user_comment : NULL;

    /* MakerNote — format the AE CSV string here, not inside exif_builder */
#ifdef EXIF_MAKER_NOTES
    snprintf(maker_note, sizeof(maker_note), "%d, %d, %d, %d, %c",
             gain.integration, gain.analogGain, gain.digitalGain, gain.aeMean,
             (gain.aeConverged == 1) ? 'Y' : 'N');
    exif_input.maker_note = maker_note;
#else
    exif_input.maker_note = NULL;
#endif

    /* Deployment ID */
    fatfs_getDeploymentId(deployment_id, sizeof(deployment_id));
    exif_input.deployment_id =
        (strcmp(deployment_id, DEPLOYMENT_ID_ZERO_UUID) != 0) ? deployment_id : NULL;

    uint32_t exifLength = exif_build_segment(&exif_input);

    fileOp.buffer = (uint8_t *)exif_get_buffer();
    fileOp.length = exifLength;

    /* ... rest of prepareJpegFile unchanged ... */
}
```

Note that `HM0360_GAIN_T gain` and the `#ifdef USE_HM0360` AE register read remain
entirely in `image_task.c`. `exif_builder.c` never sees the HM0360 type.

---

## Note on `write16_le` / `write32_le`

These helpers are also used in `bmp_create_gray8_header()` (inside `#ifdef INVESTIGATE_BMP`
in `image_task.c`). If BMP support is retained they can be duplicated in both files, or
extracted to a small `jpeg_utils.h` header if preferred. Given the BMP path is
experimental, duplication is acceptable for now.

---

## TAG_USER_COMMENT Fix

This refactor is the right moment to correct a minor standards violation identified
during the Apple JPEG investigation (see `doc/WW500_Apple_JPEG_fix.md`).

`TAG_USER_COMMENT` (0x9286) is currently stored with `TYPE_ASCII` (2). EXIF 2.3 §4.6.5
requires it to use `TYPE_UNDEFINED` (7) with a mandatory 8-byte character-set identifier
prefix (`"ASCII\0\0\0"` for plain ASCII text). This was not the cause of the Apple
rejection but is a standards violation that strict EXIF parsers can reject.

### Current code in `addIFD()` (non-compliant)

`TAG_USER_COMMENT` falls through into the generic ASCII string handler:

```c
case TAG_USER_COMMENT:
    /* ... falls into the common ASCII handler ... */
    write16_le(entry_ptr + 2, TYPE_ASCII);   /* wrong type */
```

### Replacement in `exif_builder.c`

Give `TAG_USER_COMMENT` its own case before the generic ASCII cases:

```c
case TAG_USER_COMMENT:
{
    const char *text   = (const char *)tagData;
    uint32_t text_len  = strlen(text);
    uint32_t total_len = 8u + text_len;   /* 8-byte charset prefix + comment text */
    write16_le(entry_ptr,     TAG_USER_COMMENT);
    write16_le(entry_ptr + 2, UNDEFINED);             /* TYPE_UNDEFINED = 7 per EXIF 2.3 */
    write32_le(entry_ptr + 4, total_len);
    write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
    memcpy(next_data_ptr,     "ASCII\0\0\0", 8);      /* charset identifier */
    memcpy(next_data_ptr + 8, text, text_len);        /* no null terminator — UNDEFINED field */
    next_data_ptr += total_len;
    break;
}
```

`total_len` does **not** include a null terminator. EXIF 2.3 does not require one for
`TYPE_UNDEFINED` fields, and omitting it keeps the size predictable. The `EXIF_MAX_LEN`
budget is sufficient; UserComment strings in practice are well under 100 bytes.

---

## Build System

Add `exif_builder.c` to `ww500_md.mk` alongside `image_task.c`. No include-path changes
are needed as the new files live in the same directory.

```makefile
APP_SRC += $(APP_PATH)/exif_builder.c
```

---

## Expected Line Count Impact

| File | Before | After (approx.) |
|------|--------|-----------------|
| `image_task.c` | 3034 | ~2330 |
| `exif_builder.c` | (new) | ~620 |
| `exif_builder.h` | (new) | ~80 |

`image_task.c` loses approximately the EXIF declarations (~120 lines, 176–295), the
three static EXIF globals (~6 lines, 391–396), the seven EXIF functions (~575 lines,
2213–2787), and the sector-alignment block (~20 lines, 2035–2049). The call site
(`prepareJpegFile`) shrinks by the same padding block and gains a small `ExifInput_t`
assembly section.

---

## Migration Steps

1. Create `exif_builder.h` with constants, enums, `ExifInput_t`, and the two public
   function declarations.
2. Create `exif_builder.c`; copy and adapt the seven functions listed above. Apply the
   `TAG_USER_COMMENT` fix in `addIFD()`.
3. Move the sector-alignment COM padding logic from `prepareJpegFile` into
   `exif_build_segment()` (end of function, before returning `exif_len`).
4. Update `prepareJpegFile()` in `image_task.c`: populate `ExifInput_t`, call
   `exif_build_segment()`, retrieve buffer via `exif_get_buffer()`.
5. Remove all moved declarations, definitions, and static variables from `image_task.c`.
6. Add `#include "exif_builder.h"` to `image_task.c`.
7. Add `exif_builder.c` to `ww500_md.mk`.
8. Build and verify with the Python tools (`jpeg_segments.py`, `jpegdump_exif.py`)
   against a fresh sample JPEG.

---

## Files Referenced

| File | Role |
|------|------|
| `image_task.c` | Source of EXIF code to be extracted |
| `exif_builder.h` | New — public API |
| `exif_builder.c` | New — EXIF construction logic |
| `exif_utc.h` / `exif_utc.c` | Existing — RTC timestamp formatting (unchanged) |
| `exif_gps.h` / `exif_gps.c` | Existing — GPS coordinate helpers (unchanged) |
| `ww500_md.mk` | Build file — add `exif_builder.c` |
| `doc/WW500_Apple_JPEG_fix.md` | Background on APP1 length bug and UserComment issue |
