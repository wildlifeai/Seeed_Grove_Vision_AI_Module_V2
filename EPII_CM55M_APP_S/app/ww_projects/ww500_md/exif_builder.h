/*
 * exif_builder.h
 *
 * Public API for building EXIF APP1 segments in JPEG files.
 *
 * Extracted from image_task.c — June 2026.
 *
 * Related modules (unchanged):
 *   exif_utc.c / exif_utc.h  — RTC timestamp formatting
 *   exif_gps.c / exif_gps.h  — GPS coordinate conversion
 */

#ifndef EXIF_BUILDER_H_
#define EXIF_BUILDER_H_

#include <stdint.h>
#include <stddef.h>

/*************************************** Constants *******************************************/

/* Maximum bytes the EXIF builder may write into its internal buffer.
 * Must be a multiple of 512 so that a full buffer never needs COM padding. */
#define EXIF_MAX_LEN              1024
/* Maximum dynamic class entries written to EXIF to bound buffer growth. */
#define EXIF_MAX_DYNAMIC_CLASSES  4
/* Maximum label length copied into EXIF. */
#define EXIF_MAX_LABEL_LEN        20
/* Fixed IFD0 mandatory tag count. */
#define IFD0_ENTRY_COUNT          9
/* GPS sub-IFD entry count. */
#define GPS_IFD_ENTRY_COUNT       6
/* Complete GPS IFD block size: 2-byte count + entries + 4-byte next-IFD offset. */
#define GPS_IFD_SIZE              (2 + GPS_IFD_ENTRY_COUNT * 12 + 4)
/* Buffer length for EXIF UserComment text. */
#define EXIF_COMMENT_LENGTH       256

/*************************************** Enums *******************************************/

/* EXIF tag identifiers used in IFD entries. */
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

/* EXIF data type codes (TIFF baseline). */
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

/*************************************** Types *******************************************/

/*
 * Input to exif_build_segment().
 *
 * The caller fills this struct from its own context and passes it in.
 * exif_builder.c is fully decoupled from image-task internals (sensor types,
 * CV module, FatFS API).
 *
 * Pass NULL for any optional pointer field to omit that IFD entry entirely.
 */
typedef struct {
    uint16_t       width;           /* image width in pixels */
    uint16_t       height;          /* image height in pixels */
    char           timestamp[20];   /* "YYYY:MM:DD HH:MM:SS\0" — 19 chars + NUL */
    const char    *deployment_id;   /* UUID string, or NULL if no deployment active */
    const uint8_t *nn_data;         /* [total_bytes][count][score...], or NULL */
    const char    *user_comment;    /* NN label summary string, or NULL */
    const char    *maker_note;      /* AE register CSV string, or NULL */
} ExifInput_t;

/*************************************** Public Functions *******************************************/

/*
 * Build a complete EXIF APP1 segment into the internal buffer, including
 * the SOI marker (0xFFD8) and sector-alignment COM padding.
 *
 * Returns the total byte count (SOI + APP1 + padding), or 0 on error.
 * The returned buffer is valid until the next call to exif_build_segment().
 */
uint16_t exif_build_segment(const ExifInput_t *input);

/*
 * Returns a read-only pointer to the internal EXIF buffer.
 * Only valid after a successful call to exif_build_segment().
 */
const uint8_t *exif_get_buffer(void);

#endif /* EXIF_BUILDER_H_ */
