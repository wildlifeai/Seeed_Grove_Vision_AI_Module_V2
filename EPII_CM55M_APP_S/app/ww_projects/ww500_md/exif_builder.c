/*
 * exif_builder.c
 *
 * Builds EXIF APP1 segments for JPEG files.
 *
 * Extracted from image_task.c — June 2026.
 *
 * This module is decoupled from image-task internals. The caller assembles
 * an ExifInput_t and calls exif_build_segment(); this module knows nothing
 * about sensor types, the CV module, or the FatFS API.
 */

/*************************************** Includes *******************************************/

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "xprintf.h"
#include "exif_builder.h"
#include "exif_gps.h"
#include "xip_manager.h"   /* MAX_CLASSES — for TAG_WW_CONFIDENCE_BASE range check */

/*************************************** Static storage *************************************/

/* Extra 512 bytes absorbs the worst-case JPEG Comment padding needed to reach
 * the next sector boundary (see exif_build_segment). */
static uint8_t exif_buffer[EXIF_MAX_LEN + 512];

/* Cursor to where non-inline IFD data is appended. */
static uint8_t *next_data_ptr;

/* Pointer to start of the TIFF header inside exif_buffer.
 * IFD offsets are relative to this address. */
static uint8_t *tiff_start;

/*************************************** Extern variables ***********************************/

extern GPS_Coordinate exif_gps_deviceLat;
extern GPS_Coordinate exif_gps_deviceLon;
extern GPS_Altitude   exif_gps_deviceAlt;

/*************************************** Static helpers *************************************/

static void write16_le(uint8_t *ptr, uint16_t val) {
    ptr[0] = val & 0xFF;
    ptr[1] = val >> 8;
}

static void write32_le(uint8_t *ptr, uint32_t val) {
    ptr[0] =  val        & 0xFF;
    ptr[1] = (val >>  8) & 0xFF;
    ptr[2] = (val >> 16) & 0xFF;
    ptr[3] = (val >> 24) & 0xFF;
}

static void write16_be(uint8_t *ptr, uint16_t val) {
    ptr[0] = val >> 8;
    ptr[1] = val & 0xFF;
}

/* Add one 12-byte IFD entry at entry_ptr.
 * next_data_ptr is advanced as variable-length data is appended. */
static void addIFD(ExifTagID tagID, uint8_t *entry_ptr, void *tagData) {
    switch (tagID) {
    case TAG_X_RESOLUTION:
    case TAG_Y_RESOLUTION:
    {
        uint32_t *rational = (uint32_t *)tagData;
        write16_le(entry_ptr,     tagID);
        write16_le(entry_ptr + 2, TYPE_RATIONAL);
        write32_le(entry_ptr + 4, 1);
        write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
        write32_le(next_data_ptr, rational[0]); next_data_ptr += 4;
        write32_le(next_data_ptr, rational[1]); next_data_ptr += 4;
        break;
    }
    case TAG_RESOLUTION_UNIT:
    {
        uint16_t value = *(uint16_t *)tagData;
        write16_le(entry_ptr,     tagID);
        write16_le(entry_ptr + 2, TYPE_SHORT);
        write32_le(entry_ptr + 4, 1);
        write16_le(entry_ptr + 8, value);
        write16_le(entry_ptr + 10, 0);
        break;
    }
    case TAG_USER_COMMENT:
    {
        /* EXIF 2.3 §4.6.5: UserComment must use TYPE_UNDEFINED (7) with an
         * 8-byte charset identifier prefix ("ASCII\0\0\0" for ASCII text).
         * The total stored length is 8 + strlen(text); no null terminator. */
        const char *text      = (const char *)tagData;
        uint32_t    text_len  = (uint32_t)strlen(text);
        uint32_t    total_len = 8u + text_len;
        write16_le(entry_ptr,     TAG_USER_COMMENT);
        write16_le(entry_ptr + 2, UNDEFINED);
        write32_le(entry_ptr + 4, total_len);
        write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
        if (next_data_ptr + total_len <= exif_buffer + EXIF_MAX_LEN) {
            memcpy(next_data_ptr,     "ASCII\0\0\0", 8);
            memcpy(next_data_ptr + 8, text, text_len);
        }
        next_data_ptr += total_len;  /* advance regardless; overflow caught by caller */
        break;
    }
    case TAG_DATETIME_ORIGINAL:
    case TAG_CREATE_DATE:
    case TAG_MAKE:
    case TAG_MODEL:
    case TAG_DEPLOYMENT_ID:
    case TAG_GPS_LATITUDE_REF:
    case TAG_GPS_LONGITUDE_REF:
    case TAG_MAKER_NOTE:
    {
        char    *ascii  = (char *)tagData;
        uint32_t length = (uint32_t)strlen(ascii) + 1u;  /* include null terminator */
        write16_le(entry_ptr,     tagID);
        write16_le(entry_ptr + 2, TYPE_ASCII);
        write32_le(entry_ptr + 4, length);
        if (length <= 4u) {
            memset(entry_ptr + 8, 0, 4);
            memcpy(entry_ptr + 8, ascii, length);
        } else {
            write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
            if (next_data_ptr + length <= exif_buffer + EXIF_MAX_LEN) {
                memcpy(next_data_ptr, ascii, length);
            }
            next_data_ptr += length;  /* advance regardless; overflow caught by caller */
        }
        break;
    }
    case TAG_GPS_LATITUDE:
    case TAG_GPS_LONGITUDE:
    {
        uint32_t *dms = (uint32_t *)tagData;  /* 3 pairs of (numerator, denominator) */
        write16_le(entry_ptr,     tagID);
        write16_le(entry_ptr + 2, TYPE_RATIONAL);
        write32_le(entry_ptr + 4, 3);
        write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
        for (int i = 0; i < 3; ++i) {
            write32_le(next_data_ptr, dms[i * 2]);     next_data_ptr += 4;
            write32_le(next_data_ptr, dms[i * 2 + 1]); next_data_ptr += 4;
        }
        break;
    }
    case TAG_GPS_ALTITUDE:
    {
        uint32_t *rational = (uint32_t *)tagData;
        write16_le(entry_ptr,     tagID);
        write16_le(entry_ptr + 2, TYPE_RATIONAL);
        write32_le(entry_ptr + 4, 1);
        write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
        write32_le(next_data_ptr, rational[0]); next_data_ptr += 4;
        write32_le(next_data_ptr, rational[1]); next_data_ptr += 4;
        break;
    }
    case TAG_GPS_ALTITUDE_REF:
    {
        uint8_t value = *(uint8_t *)tagData;
        write16_le(entry_ptr,     tagID);
        write16_le(entry_ptr + 2, TYPE_BYTE);
        write32_le(entry_ptr + 4, 1);
        memset(entry_ptr + 8, 0, 4);
        entry_ptr[8] = value;
        break;
    }
    case TAG_NN_DATA:
    {
        uint8_t *bytes  = (uint8_t *)tagData;
        uint32_t length = bytes[0];  /* first byte is count of bytes that follow */
        write16_le(entry_ptr,     tagID);
        write16_le(entry_ptr + 2, TYPE_BYTE);
        write32_le(entry_ptr + 4, length);
        if (length <= 4u) {
            memset(entry_ptr + 8, 0, 4);
            memcpy(entry_ptr + 8, &bytes[1], length);
        } else {
            write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
            memcpy(next_data_ptr, &bytes[1], length);
            next_data_ptr += length;
        }
        break;
    }
    case TAG_GPS_IFD_POINTER:
    {
        uint32_t offset = *(uint32_t *)tagData;
        write16_le(entry_ptr,     tagID);
        write16_le(entry_ptr + 2, TYPE_LONG);
        write32_le(entry_ptr + 4, 1);
        write32_le(entry_ptr + 8, offset);
        break;
    }
    default:
    {
        /* Dynamic confidence tags (private range 0xF300–0xF3FF).
         * Even offsets: confidence percentage (SHORT).
         * Odd offsets:  label string (ASCII). */
        if (tagID >= TAG_WW_CONFIDENCE_BASE &&
            tagID <  (ExifTagID)(TAG_WW_CONFIDENCE_BASE + MAX_CLASSES * 2u))
        {
            if ((tagID - TAG_WW_CONFIDENCE_BASE) % 2u == 0u) {
                uint16_t value = *(uint16_t *)tagData;
                write16_le(entry_ptr,     tagID);
                write16_le(entry_ptr + 2, TYPE_SHORT);
                write32_le(entry_ptr + 4, 1);
                write16_le(entry_ptr + 8,  value);
                write16_le(entry_ptr + 10, 0);
            } else {
                char    *ascii  = (char *)tagData;
                uint32_t length = (uint32_t)strlen(ascii) + 1u;
                write16_le(entry_ptr,     tagID);
                write16_le(entry_ptr + 2, TYPE_ASCII);
                write32_le(entry_ptr + 4, length);
                if (length <= 4u) {
                    memset(entry_ptr + 8, 0, 4);
                    memcpy(entry_ptr + 8, ascii, length);
                } else {
                    write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
                    memcpy(next_data_ptr, ascii, length);
                    next_data_ptr += length;
                }
            }
        }
        break;
    }
    } /* switch */
}

/* Returns the byte count of a complete GPS IFD block. */
static size_t get_gps_ifd_size(void) {
    return GPS_IFD_SIZE;
}

/* Write a GPS sub-IFD at gps_ifd_start using the device GPS coordinates
 * stored in exif_gps_deviceLat/Lon/Alt. */
static void create_gps_ifd(uint8_t *gps_ifd_start) {
    uint8_t *p   = gps_ifd_start;
    uint8_t *ifd;

    write16_le(p, GPS_IFD_ENTRY_COUNT);
    p += 2;

    ifd = p;
    p += GPS_IFD_ENTRY_COUNT * 12;
    write32_le(p, 0);  /* no next IFD */
    p += 4;

//#define EMULATED_GPS
#ifdef EMULATED_GPS
    char     latRef[] = "N";
    char     lonRef[] = "E";
    uint32_t lat[6]   = {37, 1, 48, 1, 3000, 100};
    uint32_t lon[6]   = {122, 1, 25, 1, 1500, 100};
    uint8_t  altRef   = 0;
    uint32_t alt[2]   = {5000, 100};
    addIFD(TAG_GPS_LATITUDE_REF,  ifd + 0 * 12, latRef);
    addIFD(TAG_GPS_LATITUDE,      ifd + 1 * 12, lat);
    addIFD(TAG_GPS_LONGITUDE_REF, ifd + 2 * 12, lonRef);
    addIFD(TAG_GPS_LONGITUDE,     ifd + 3 * 12, lon);
    addIFD(TAG_GPS_ALTITUDE_REF,  ifd + 4 * 12, &altRef);
    addIFD(TAG_GPS_ALTITUDE,      ifd + 5 * 12, alt);
#else
    uint8_t lat_buf[26];
    uint8_t lon_buf[26];
    uint8_t alt_buf[9];

    exif_gps_generate_byte_array(&exif_gps_deviceLat, lat_buf);
    exif_gps_generate_byte_array(&exif_gps_deviceLon, lon_buf);
    exif_gps_generate_altitude_byte_array(&exif_gps_deviceAlt, alt_buf);

    char    *latRef = (char *)&lat_buf[0];
    char    *lonRef = (char *)&lon_buf[0];
    uint8_t  altRef = alt_buf[0];

    uint32_t lat[6], lon[6], alt[2];
    exif_gps_extract_rationals(lat_buf,     lat);
    exif_gps_extract_rationals(lon_buf,     lon);
    exif_gps_extract_alt_rationals(alt_buf, alt);

    addIFD(TAG_GPS_LATITUDE_REF,  ifd + 0 * 12, latRef);
    addIFD(TAG_GPS_LATITUDE,      ifd + 1 * 12, lat);
    addIFD(TAG_GPS_LONGITUDE_REF, ifd + 2 * 12, lonRef);
    addIFD(TAG_GPS_LONGITUDE,     ifd + 3 * 12, lon);
    addIFD(TAG_GPS_ALTITUDE_REF,  ifd + 4 * 12, &altRef);
    addIFD(TAG_GPS_ALTITUDE,      ifd + 5 * 12, alt);
#endif
}

/*************************************** Public Functions ***********************************/

/*
 * Build a complete EXIF APP1 segment into the internal buffer, including
 * the SOI marker (0xFFD8) and sector-alignment COM padding.
 *
 * Returns the total byte count (SOI + APP1 + padding), or 0 on error.
 * The returned buffer is valid until the next call to exif_build_segment().
 */

uint16_t exif_build_segment(const ExifInput_t *input) {
    uint16_t  exif_len;
    uint8_t  *p;
    uint8_t  *len_ptr;
    uint8_t  *ifd_start;
    uint8_t  *gps_ifd_start;
    uint32_t  gps_ifd_offset;
    size_t    gps_size;

    if (input == NULL) {
        return 0;
    }

    /* Count IFD0 entries: start with the fixed mandatory set. */
    uint16_t dynamic_ifd_count = IFD0_ENTRY_COUNT;
    if (input->user_comment  != NULL) dynamic_ifd_count++;
    if (input->maker_note    != NULL) dynamic_ifd_count++;
    if (input->deployment_id != NULL) dynamic_ifd_count++;

    uint32_t xres_rational[2] = {input->width,  1u};
    uint32_t yres_rational[2] = {input->height, 1u};
    uint16_t res_unit = 2u;

    p = exif_buffer;

    /* SOI marker 0xFFD8 */
    *p++ = 0xFF;
    *p++ = 0xD8;

    /* APP1 marker 0xFFE1 */
    *p++ = 0xFF;
    *p++ = 0xE1;

    /* Placeholder for APP1 segment length (filled in below). */
    len_ptr = p;
    p += 2;

    /* EXIF identifier. */
    memcpy(p, "Exif\0\0", 6);
    p += 6;

    /* TIFF header: little-endian ("II"), magic 0x002A, IFD0 at offset 8. */
    tiff_start = p;
    memcpy(p, "II", 2);
    p += 2;
    write16_le(p, 0x002A);
    p += 2;
    write32_le(p, 0x00000008u);
    p += 4;

    /* IFD0 entry count — must not change after this point. */
    write16_le(p, dynamic_ifd_count);
    p += 2;

    ifd_start = p;
    p += dynamic_ifd_count * 12;

    /* Next IFD offset = 0 (no IFD1). */
    write32_le(p, 0);
    p += 4;

    /* next_data_ptr starts immediately after the IFD block. */
    next_data_ptr = p;

    /* Reserve space for the GPS IFD before writing any variable-length data.
     * Failing silently would leave TAG_GPS_IFD_POINTER pointing at garbage;
     * abort the whole segment if it won't fit. */
    gps_ifd_start  = next_data_ptr;
    gps_ifd_offset = (uint32_t)(gps_ifd_start - tiff_start);
    gps_size = get_gps_ifd_size();
    if (next_data_ptr + gps_size > exif_buffer + EXIF_MAX_LEN) {
        return 0;
    }
    next_data_ptr += gps_size;

    /* Write mandatory IFD0 entries. */
    uint8_t entry = 0;
    addIFD(TAG_MAKE,             ifd_start + (entry++ * 12), "Wildlife.ai");
    addIFD(TAG_MODEL,            ifd_start + (entry++ * 12), "WW500");
    addIFD(TAG_RESOLUTION_UNIT,  ifd_start + (entry++ * 12), &res_unit);
    addIFD(TAG_X_RESOLUTION,     ifd_start + (entry++ * 12), xres_rational);
    addIFD(TAG_Y_RESOLUTION,     ifd_start + (entry++ * 12), yres_rational);
    addIFD(TAG_DATETIME_ORIGINAL, ifd_start + (entry++ * 12), (void *)input->timestamp);
    addIFD(TAG_CREATE_DATE,      ifd_start + (entry++ * 12), (void *)input->timestamp);
    addIFD(TAG_NN_DATA, ifd_start + (entry++ * 12), (void *)input->nn_data);

    /* Optional IFD0 entries — included only when the caller provides data. */
    if (input->maker_note != NULL) {
        addIFD(TAG_MAKER_NOTE, ifd_start + (entry++ * 12), (void *)input->maker_note);
    }
    if (input->user_comment != NULL) {
        addIFD(TAG_USER_COMMENT, ifd_start + (entry++ * 12), (void *)input->user_comment);
    }
    if (input->deployment_id != NULL) {
        xprintf("EXIF: Adding DeploymentID: %s\n", input->deployment_id);
        addIFD(TAG_DEPLOYMENT_ID, ifd_start + (entry++ * 12), (void *)input->deployment_id);
    }

    /* GPS IFD pointer (always present). */
    addIFD(TAG_GPS_IFD_POINTER, ifd_start + (entry++ * 12), &gps_ifd_offset);

    if (gps_size > 0) {
        create_gps_ifd(gps_ifd_start);
    }

    /* If any variable-length write exceeded EXIF_MAX_LEN, the EXIF structure
     * is incomplete (writes were skipped by addIFD bounds guards above).
     * Return 0 so the caller discards this frame rather than saving corrupt data. */
    if (next_data_ptr > exif_buffer + EXIF_MAX_LEN) {
        return 0;
    }

    /* APP1 length field: bytes from len_ptr to end of payload (excludes SOI + marker). */
    write16_be(len_ptr, (uint16_t)(next_data_ptr - len_ptr));

    exif_len = (uint16_t)(next_data_ptr - exif_buffer);

    xprintf("Added %d EXIF tags\n", entry);

    /* Sector-alignment COM padding.
     *
     * Pad exif_buffer to the next 512-byte boundary using a JPEG Comment segment
     * (marker 0xFF 0xFE).  Without this, FatFS splits the write: one CMD24 flush
     * for the partial first sector, then a CMD25 batch for the JPEG body.  With
     * the pad, fp->fptr lands on a sector boundary, so the JPEG body is written
     * as a single CMD25 transaction.
     *
     * EXIF_MAX_LEN is a multiple of 512, so a full buffer never needs padding.
     * exif_buffer is declared EXIF_MAX_LEN + 512 bytes to absorb the worst case.
     * JPEG Comment segments are valid anywhere between markers (ISO 10918-1 B.2.4.5). */
    if (exif_len > 0u && (exif_len % 512u) != 0u) {
        uint32_t pad = 512u - (exif_len % 512u);
        if (pad < 4u) {
            /* A Comment segment needs at least 4 bytes (marker + 2-byte length). */
            pad += 512u;
        }
        uint8_t *pad_p = exif_buffer + exif_len;
        *pad_p++ = 0xFF;
        *pad_p++ = 0xFE;
        uint16_t data_len = (uint16_t)(pad - 4u);
        *pad_p++ = (uint8_t)((data_len + 2u) >> 8);
        *pad_p++ = (uint8_t)((data_len + 2u) & 0xFF);
        memset(pad_p, 0, data_len);
        exif_len += (uint16_t)pad;
    }

    return exif_len;
}

/*
 * Returns a read-only pointer to the internal EXIF buffer.
 * Only valid after a successful call to exif_build_segment().
 */

const uint8_t *exif_get_buffer(void) {
    return exif_buffer;
}
