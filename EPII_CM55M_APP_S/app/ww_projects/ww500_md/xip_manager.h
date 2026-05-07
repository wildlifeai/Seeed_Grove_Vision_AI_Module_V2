/*
 * xip_manager.h
 *
 * Public interface for the XIP (eXecute-In-Place) flash manager on the HX6538 processor.
 *
 * The 16 MB external flash chip is mapped as follows (physical addresses):
 *
 *   0x00000000 - 0x000FFFFF   Firmware Image Slot A  (1 MB)
 *   0x00100000 - 0x001FFFFF   Firmware Image Slot B  (1 MB)
 *   0x00200000 - 0x00EFFFFF   NN model area          (13 MB)
 *   0x00F00000 - 0x00FEFFFF   Reserved / unused
 *   0x00FFF000 - 0x00FFFFFF   Slot A/B selector      (last 4 KB sector)
 *
 * The NN model area starts at physical 0x00200000, which maps to virtual
 * address 0x3A200000 when XIP mode is enabled.  A ModelMetaData structure
 * is stored at the start of this area, followed (on a 16-byte boundary)
 * by the raw TFLite Vela model data.
 *
 * ModelMetaData layout (stored at physical 0x00200000 / virtual MODEL_XIP_ADDR):
 *   magic       (uint32_t)      — must equal 0x4C41424C ("LABL")
 *   class_count (uint16_t)      — number of NN output classes
 *   label_len   (uint16_t)      — bytes allocated per label entry (MAX_LABEL_LEN)
 *   modelName   (char[13])      — 8.3 format model filename, e.g. "1V2.TFL"
 *   labels      (char[16][20])  — class label strings, NUL-padded
 *   crc         (uint32_t)      — reserved for future integrity check, written as 0
 *
 * The model data immediately follows the metadata on a 16-byte boundary.
 * Use xip_get_model_xip_address() to obtain the validated virtual address
 * suitable for passing to tflite::GetModel().
 */

#ifndef XIP_MANAGER_H_
#define XIP_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#include "image_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*************************************** Definitions *******************************************/

// Virtual address at which model metadata begins (physical 0x00200000)
#define MODEL_XIP_ADDR          0x3A200000

// Model metadata limits
#define MAX_CLASSES             16          // Maximum number of NN output classes
#define MAX_LABEL_LEN           20          // Maximum bytes per class label string (including NUL)
#define MAX_MODEL_NAME_LEN      IMAGEFILENAMELEN          // 8.3 format filename + NUL (e.g. "1V2.TFL\0")


/*************************************** Type definitions **************************************/

/**
 * Metadata record stored in XIP flash at MODEL_XIP_ADDR (physical 0x00200000),
 * immediately before the model data.  Written whenever a model is loaded from
 * the SD card.  The model data begins on a 16-byte boundary after this structure.
 *
 * Fields:
 *   magic       — must equal 0x4C41424C ("LABL") before the record is trusted
 *   class_count — number of output classes; must match the model's output tensor size
 *   label_len   — bytes allocated per label entry (MAX_LABEL_LEN)
 *   modelName   — 8.3 format filename used to identify the stored model, e.g. "1V2.TFL"
 *   labels      — NUL-terminated class name strings, one per output class
 *   crc         — reserved for future integrity checking; currently written as 0
 *
 *   Arranged for 4-byte alignment
 */
typedef struct {
    uint32_t magic;                          // Must equal 0x4C41424C ("LABL")
    uint32_t crc;                            // Reserved — set to 0 for now

    uint16_t class_count;                    // Number of output classes
    uint16_t label_len;                      // Bytes per label (MAX_LABEL_LEN)

    char labels[MAX_CLASSES][MAX_LABEL_LEN]; // Class label strings
    char modelName[MAX_MODEL_NAME_LEN];      // Model filename, e.g. "1V2.TFL"
    uint8_t reserved[3];                     // Padding to 4-byte boundary
} ModelMetaData;

// Read-only pointer to the metadata as it appears in XIP-mapped flash.
// Valid only while XIP mode is enabled.
#define metaDataFlash ((const ModelMetaData *)MODEL_XIP_ADDR)

/*************************************** Public Function Declarations **************************/

/**
 * Sector-erase enough flash to hold flashSizeRequired bytes, starting at
 * the model area.
 *
 * @param flashSizeRequired  total bytes to erase (metadata + model data)
 * @return 0 on success, -1 on failure
 */
int xip_erase_model_flash_area(uint32_t flashSizeRequired);

/**
 * Check whether the named model is stored in flash, by reading and
 * validating the ModelMetaData header and comparing modelName.
 *
 * @param filename   model filename, e.g. "1V2.TFL"
 * @param cold_boot  if true, print verbose metadata diagnostics
 * @return true if the named model is present
 */
bool xip_is_model_in_flash(char *filename, bool cold_boot);

/**
 * Check whether the named file exists in the /MANIFEST folder on the SD card.
 *
 * @param filename  filename to search for, e.g. "1V2.TFL"
 * @return true if found
 */
bool xip_is_file_in_sd(char *filename);

/**
 * Check that a plausible TFLite model is present in flash by inspecting
 * the "TFL3" magic bytes at the expected model offset.
 *
 * @return true if a valid-looking model is present
 */
bool xip_valid_model_in_flash(void);

/**
 * Validate the model header in flash, enable XIP, and return the virtual
 * address at which the model data begins.
 *
 * The returned address is suitable for passing directly to tflite::GetModel().
 *
 * @return virtual address of the model data, or 0 on failure
 */
uint32_t xip_get_model_xip_address(void);

/**
 * Copy a model file from /MANIFEST/<filename> on the SD card to the XIP
 * flash model area.  Erases the required flash sectors first.
 * Does not write metadata — call xip_copy_metadata_to_flash() separately.
 *
 * @param filename  model filename only (no path), e.g. "1V2.TFL"
 * @return true on success
 */
bool xip_copy_model_from_sd_to_flash(char *filename);

/**
 * Build a ModelMetaData record from the model name and the corresponding
 * label file on the SD card, then write it to the start of the model flash area.
 *
 * @param modelName  model filename only (no path), e.g. "1V2.TFL"
 * @return true on success
 */
bool xip_copy_metadata_to_flash(char *modelName);

/**
 * Read the first 32 bytes of the slot selector sector and print them to
 * the console — diagnostic function to inspect bootloader slot selection.
 *
 * @return 0 on success, -1 on failure
 */
int xip_dump_slot_selector(void);

/**
 * Update the inactive firmware slot from a file on the SD card, then switch to it.
 *
 * Sequence: read slot selector → find active slot → erase inactive slot →
 * write image → verify write → update slot selector.
 *
 * On any failure before the slot selector is written, the selector is left
 * unchanged so the device continues to boot the existing firmware.
 *
 * @param filename  bare filename in /MANIFEST, e.g. "output.img"
 * @return 0 on success, negative error code on failure
 */
int xip_update_firmware_from_sd(const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* XIP_MANAGER_H_ */
