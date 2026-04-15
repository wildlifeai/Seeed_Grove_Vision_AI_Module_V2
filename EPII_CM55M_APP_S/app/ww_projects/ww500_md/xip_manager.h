/*
 * xip_manager.h
 *
 * Manages the XIP (eXecute-In-Place) serial EEPROM flash on the HX6538 processor.
 *
 * The 16 MB external flash chip is mapped as follows (physical addresses):
 *
 *   0x00000000 - 0x000FFFFF   Firmware Image Slot A  (1 MB)
 *   0x00100000 - 0x001FFFFF   Firmware Image Slot B  (1 MB)
 *   0x00200000 - 0x00EFFFFF   NN model space        (13 MB)
 *   0x00FF0000 - 0x00FFFFFF   Slot A/B selector      (last sector)
 *
 * The NN model area starts at physical 0x00200000, which maps to virtual
 * address 0x3A200000 when XIP mode is enabled.  A ModelMetaData structure
 * is stored at the start of this area, followed (on a 16-byte boundary)
 * by the raw TFLite Vela model data.
 *
 * Callers that need a tflite::Model pointer should call
 * xip_get_model_xip_address() to obtain the validated virtual address,
 * then wrap it with tflite::GetModel().
 */

#ifndef XIP_MANAGER_H_
#define XIP_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*************************************** Definitions *******************************************/

// Flash physical address layout
#define FLASH_START_SAFE_ADDR   0x00200000          // Physical start of model area (after 2 MB firmware slots)
#define FLASH_MODEL_AREA_SIZE   (14 * 1024 * 1024)  // 14 MB available for models
#define FLASH_SECTOR_SIZE       4096                 // Flash erase sector size
#define MODEL_FLASH_ADDR        FLASH_START_SAFE_ADDR

// XIP virtual/physical address mapping.
// The SPI EEPROM appears at virtual base 0x3A000000 when XIP mode is active.
// Physical address 0x00000000 maps to virtual 0x3A000000.
#define FLASH_VIRTUAL_BASE      0x3A000000
#define FLASH_PHYSICAL_BASE     0x00000000

// Virtual address at which model metadata begins (corresponds to MODEL_FLASH_ADDR)
#define MODEL_XIP_ADDR          0x3A200000

// Size of the legacy filename header stored immediately before MODEL_XIP_ADDR
#define MODEL_XIP_INFO_SIZE     16          // Must be divisible by 4

// Model metadata limits
#define MAX_CLASSES             16          // Maximum number of NN output classes
#define MAX_LABEL_LEN           20          // Maximum bytes per class label string (including NUL)
#define LABEL_MAGIC             0x4C41424C  // "LABL" — magic word for metadata validation
#define MAX_MODEL_NAME_LEN      13          // 8.3 format filename + NUL (e.g. "1V2.TFL\0")

/*************************************** Type definitions **************************************/

/**
 * Metadata record stored in XIP flash at MODEL_XIP_ADDR, immediately before
 * the model data.  Written whenever a model is loaded from the SD card.
 * The model data begins on a 16-byte boundary after this structure.
 */
typedef struct {
    uint32_t magic;                          // Must equal LABEL_MAGIC
    uint16_t class_count;                    // Number of output classes
    uint16_t label_len;                      // Bytes per label (MAX_LABEL_LEN)
    char modelName[MAX_MODEL_NAME_LEN];      // Model filename, e.g. "1V2.TFL"
    char labels[MAX_CLASSES][MAX_LABEL_LEN]; // Class label strings
    uint32_t crc;                            // Reserved — set to 0 for now
} ModelMetaData;

// Read-only pointer to the metadata as it appears in XIP-mapped flash.
// Valid only while XIP mode is enabled.
#define metaDataFlash ((const ModelMetaData *)MODEL_XIP_ADDR)

/*************************************** Public Function Declarations **************************/

/**
 * Initialise the SPI EEPROM and create the SPI mutex.
 * Safe to call multiple times — initialises only on the first call.
 *
 * @return 0 on success, -1 on failure
 */
int xip_init_flash(void);

/**
 * Enable or disable XIP memory-mapped access.
 *
 * XIP must be enabled before reading model data via virtual addresses.
 * XIP must be disabled before issuing SPI read/write/erase commands.
 *
 * @param enable  true to enable, false to disable
 * @return true on success
 */
bool xip_enable_XIP(bool enable);

/**
 * Sector-erase enough flash to hold flashSizeRequired bytes, starting at
 * MODEL_XIP_ADDR.
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
 * Write a ModelMetaData structure to the start of the model flash area
 * via SPI (bypasses XIP mapping).
 *
 * @param metaDataRam  pointer to the structure to write
 * @return 0 on success, non-zero on failure
 */
int32_t xip_write_meta_data_to_flash(ModelMetaData *metaDataRam);

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
 * Copy a model file (given as a full path) from the SD card to flash, writing
 * a legacy 16-byte filename header immediately before MODEL_XIP_ADDR.
 *
 * NOTE: This is an older code path kept for compatibility.
 * Prefer xip_copy_model_from_sd_to_flash() + xip_copy_metadata_to_flash().
 *
 * @param filename  full path including directory, e.g. "/MANIFEST/1V2.TFL"
 * @return 0 on success, -1 on failure
 */
int xip_load_model_from_sd_to_flash(char *filename);

#ifdef __cplusplus
}
#endif

#endif /* XIP_MANAGER_H_ */
