/*
 * xip_manager.c
 *
 * Manages XIP (eXecute-In-Place) serial EEPROM flash for the HX6538 processor.
 *
 * Responsibilities:
 *  - Initialise the SPI EEPROM and XIP subsystem
 *  - Copy NN models and associated metadata from SD card to flash
 *  - Erase flash sectors to prepare for new model writes
 *  - Validate model presence in flash
 *  - Enable/disable XIP memory-mapped access
 *  - Read the firmware slot selector sector (diagnostic)
 *
 * Thread safety: an internal FreeRTOS mutex (xSPIMutex) serialises all SPI
 * EEPROM accesses.  XIP mode is disabled before any SPI transfer and
 * re-enabled afterwards.
 *
 * Flash memory layout (physical addresses):
 *   0x00000000 - 0x000FFFFF   Firmware Slot A  (1 MB)
 *   0x00100000 - 0x001FFFFF   Firmware Slot B  (1 MB)
 *   0x00200000 - 0x00EFFFFF   NN model area    (13 MB)
 *   0x00F00000 - 0x00FEFFFF   Reserved / unused
 *   0x00FFF000 - 0x00FFFFFF   Slot A/B selector (last 4 KB sector)
 *
 * Model area layout (starting at physical 0x00200000 / virtual MODEL_XIP_ADDR):
 *   offset 0                          ModelMetaData struct (see xip_manager.h)
 *   offset align16(sizeof(MetaData))  Raw TFLite Vela model data
 *
 * See xip_manager.h for constant definitions and the ModelMetaData type.
 */

/*************************************** Includes *******************************************/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "ff.h"

#include "hx_drv_spi.h"
#include "spi_eeprom_comm.h"

#include "xprintf.h"
#include "fatfs_task.h"
#include "directory_manager.h"
#include "image_task.h"
#include "printf_x.h"
#include "xip_manager.h"

/*************************************** Definitions *******************************************/

// IMPORTANT — SDK API convention:
// hx_lib_spi_eeprom_word_read() and hx_lib_spi_eeprom_word_write() take a BYTE count
// (their parameter is named bytes_len), not a word count.  The byte count must be a
// multiple of 4.  Do NOT divide by 4 when passing a length to these functions.

// Read SD card model file in chunks of this size.
// Larger values reduce per-chunk overhead (f_read calls, xprintf, DMA setup).
// Buffers are heap-allocated so this does not affect stack usage.
#define FILE_CHUNK_SIZE     4096

// Slot selector checksums — constant for a given slot, independent of firmware content.
// Captured from: 1st BL Build DATE=Jan 17 2025, Version 2.12.
// TODO: verify these values if the bootloader is ever updated.
#define SLOT_A_SELECTOR_CHECKSUM    0x4D04
#define SLOT_B_SELECTOR_CHECKSUM    0x167C

// Set to 1 to read back the entire firmware slot after writing and compare it
// against the SD card file.  Set to 0 to rely on the per-chunk verification
// that is always performed inside write_firmware_from_sd().
#define XIP_FIRMWARE_VERIFY_AFTER_WRITE    1

// Maximum bare filename length for firmware images (no path, including NUL).
// Firmware files are 8.3 format — same constraint as IMAGEFILENAMELEN in image_task.h.
#define MAX_FIRMWARE_NAME_LEN    IMAGEFILENAMELEN

// Flash physical address layout
#define FLASH_START_SAFE_ADDR   0x00200000          // Physical start of model area (after 2 MB firmware slots)
#define FLASH_MODEL_AREA_SIZE   (13 * 1024 * 1024)  // 13 MB available for models
#define FLASH_SECTOR_SIZE       4096                 // Flash erase sector size (4 KB)
#define MODEL_FLASH_ADDR        FLASH_START_SAFE_ADDR

// Total flash chip size and location of the boot-slot selector sector
#define FLASH_TOTAL_SIZE        (16 * 1024 * 1024)  // 16 MB total flash
#define FLASH_SELECTOR_ADDR     0x00FFF000           // Physical address of the last 4 KB slot selector sector
#define FLASH_BLOCK_SIZE        (64 * 1024)          // 64 KB erase block size (FLASH_64KBLOCK)

// Firmware image slot addresses and size
#define FLASH_SLOT_A_ADDR       0x00000000           // Physical base of firmware Slot A
#define FLASH_SLOT_B_ADDR       0x00100000           // Physical base of firmware Slot B
#define FLASH_SLOT_SIZE         (1024 * 1024)        // 1 MB per slot (16 × 64 KB blocks)

// XIP virtual/physical address mapping
#define FLASH_VIRTUAL_BASE      0x3A000000
#define FLASH_PHYSICAL_BASE     0x00000000

// Magic word for ModelMetaData validation ("LABL")
#define LABEL_MAGIC             0x4C41424C

/*************************************** Type definitions **************************************/

/*
 * Layout of the 20-byte slot selector header at FLASH_SELECTOR_ADDR (physical 0x00FFF000).
 * The bootloader reads this to decide which firmware slot to execute at reset.
 *
 * The checksum depends only on the slot address, not on firmware content.
 * Values captured from: 1st BL Build DATE=Jan 17 2025, Version 2.12
 *   Slot A (flash_offset 0x00000000): checksum 0x4D04
 *   Slot B (flash_offset 0x00100000): checksum 0x167C
 * TODO: verify these values if the bootloader is ever updated.
 */
typedef struct {
    char     magic[8];       // "HIMAXWE2" (ASCII, 8 bytes, not NUL-terminated in flash)
    uint32_t flash_offset;   // Slot base: FLASH_SLOT_A_ADDR or FLASH_SLOT_B_ADDR
    uint32_t constant_02;    // Always 0x00000002
    uint16_t hx_dsp_flag;    // Always 0x0001
    uint16_t checksum;       // 0x4D04 (Slot A) or 0x167C (Slot B)
} SlotSelectorHeader;

/*************************************** Local variables *************************************/

// Use this constant since a compiler issue can redefine USE_DW_SPI_MST_Q
// in some translation units (see "Bizarre" comment in original cvapp.cpp).
static USE_DW_SPI_MST_E spi_inst = (USE_DW_SPI_MST_E)0;

static bool flash_initialized = false;

// TODO - work out if we even need this mutex - I don't think it serves any purpose.
static SemaphoreHandle_t xSPIMutex = NULL;

static uint8_t g_label_count = 0;

/*************************************** Local Function Declarations **************************/

static inline uint32_t align_up(uint32_t size, uint32_t align);
static inline uint32_t virt_to_phys(uint32_t virt);
static inline uint32_t phys_to_virt(uint32_t phys);
static bool file_exists(const char *path);
static uint8_t load_labels_from_manifest(char *filename, char (*labels)[MAX_LABEL_LEN]);
static int read_slot_selector(SlotSelectorHeader *hdr);
static int get_active_slot(void);
#if XIP_FIRMWARE_VERIFY_AFTER_WRITE
static int verify_firmware_slot(uint8_t slot, const char *filepath);
#endif
static int init_flash(void);
static bool enable_xip(bool enable);
static int32_t write_metadata_to_flash(ModelMetaData *metaDataRam);
static int erase_firmware_slot(uint8_t slot);
static int write_firmware_from_sd(uint8_t slot, const char *filepath);
static int write_slot_selector(uint8_t slot);

/*************************************** Local Function Definitions ***************************/

/**
 * Rounds up 'size' to be an integral multiple of 'align' bytes.
 *
 * Examples if 'align' is 4:
 *   size=0: 0+3=3, 3&~3=0
 *   size=1: 1+3=4, 4&~3=4
 *   size=4: 4+3=7, 7&~3=4
 *   size=5: 5+3=8, 8&~3=8
 */
static inline uint32_t align_up(uint32_t size, uint32_t align) {
    return (size + align - 1) & ~(align - 1);
}

static inline uint32_t virt_to_phys(uint32_t virt) {
    return virt - FLASH_VIRTUAL_BASE + FLASH_PHYSICAL_BASE;
}

static inline uint32_t phys_to_virt(uint32_t phys) {
    return phys - FLASH_PHYSICAL_BASE + FLASH_VIRTUAL_BASE;
}

/**
 * Return true if a file exists at the given FATFS path.
 */
static bool file_exists(const char *path) {
    FILINFO finfo;
    FRESULT res = f_stat(path, &finfo);
    return (res == FR_OK);
}

/**
 * Load class labels from "/MANIFEST/<basename>.TXT" on the SD card.
 *
 * The filename parameter is expected to end with ".TFL".  The labels are
 * read from a file with the same base name but a ".TXT" extension.
 * The label count is stored in the module-level g_label_count.
 *
 * @param filename  model filename, e.g. "1V2.TFL"
 * @param labels    destination array to receive label strings
 * @return number of labels loaded (0 if no file or on error)
 */
static uint8_t load_labels_from_manifest(char *filename, char (*labels)[MAX_LABEL_LEN]) {
    char labels_path[MAX_MODEL_NAME_LEN * 2];   // plenty for "/MANIFEST/xxxxxxxx.TXT"
    char base[MAX_MODEL_NAME_LEN];

    strncpy(base, filename, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    char *dot = strrchr(base, '.');
    if (dot != NULL) {
        strcpy(dot, ".TXT");
    }
    else {
        return 0;
    }

    snprintf(labels_path, sizeof(labels_path), "%s/%s", CONFIG_DIR, base);
    xprintf("Looking for labels in %s\n", labels_path);

    if (file_exists(labels_path)) {
        uint8_t count = 0;
        if (fatfs_load_labels(labels_path, labels, &count, MAX_CLASSES, MAX_LABEL_LEN) == 0) {
            g_label_count = count;
            xprintf("Loaded %d labels from %s\n", g_label_count, labels_path);
            return g_label_count;
        }
        else {
            xprintf("Labels load failed from %s\n", labels_path);
            return 0;
        }
    }
    else {
        xprintf("File %s not found\n", labels_path);
        return 0;
    }
}

/*************************************** Global Function Definitions **************************/

/**
 * Initialise the SPI EEPROM and create the SPI mutex.
 * Safe to call multiple times — initialises only on the first call.
 */
static int init_flash(void) {
    uint8_t id_info;

    if (flash_initialized) {
        return 0;
    }

    // Create mutex on first call.
    // In FreeRTOS, a mutex is created in the unlocked state.
    if (xSPIMutex == NULL) {
        xSPIMutex = xSemaphoreCreateMutex();
        if (xSPIMutex == NULL) {
            xprintf("Failed to create SPI mutex\n");
            return -1;
        }
    }

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex\n");
        return -1;
    }

    if (hx_lib_spi_eeprom_open(spi_inst) != 0) {
        xprintf("Failed to open SPI EEPROM\n");
        flash_initialized = false;
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    // Small delay for mode switch
    vTaskDelay(pdMS_TO_TICKS(10));

    if (hx_lib_spi_eeprom_read_ID(spi_inst, &id_info) != 0) {
        xprintf("Failed to read flash ID\n");
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    // Disable XIP mode so that SPI accesses are possible
    if (hx_lib_spi_eeprom_enable_XIP(spi_inst, false, FLASH_QUAD, true) != 0) {
        xprintf("Failed to disable XIP mode\n");
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    xSemaphoreGive(xSPIMutex);

    xprintf("Flash ID 0x%02X Initialised\n", id_info);
    flash_initialized = true;

    return 0;
}

/**
 * Enable or disable XIP memory-mapped access.
 */
static bool enable_xip(bool enable) {
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex in enable_xip()\n");
        return false;
    }

    if (hx_lib_spi_eeprom_enable_XIP(spi_inst, enable, FLASH_QUAD, true) != 0) {
        xprintf("Failed to enable XIP mode\n");
        xSemaphoreGive(xSPIMutex);
        return false;
    }

    xSemaphoreGive(xSPIMutex);
    return true;
}

/**
 * Block-erase enough flash to hold flashSizeRequired bytes, starting at
 * MODEL_XIP_ADDR.  Flash is erased in 64 KB blocks (FLASH_64KBLOCK).
 *
 * MODEL_FLASH_ADDR (0x00200000) is 64 KB aligned, so no alignment padding
 * is needed at the start of the erase.
 */
int xip_erase_model_flash_area(uint32_t flashSizeRequired) {
    int32_t ret = 0;
    uint32_t blocks_needed;
    uint32_t block_addr;

    if (init_flash() != 0) {
        return -1;
    }

    blocks_needed = align_up(flashSizeRequired, FLASH_BLOCK_SIZE) / FLASH_BLOCK_SIZE;
    block_addr    = virt_to_phys(MODEL_XIP_ADDR);

    xprintf("Erasing %d x 64KB blocks from 0x%08x to cover %lu bytes\n",
            blocks_needed, block_addr, (unsigned long)flashSizeRequired);

    // Disable XIP before erase
    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex in xip_erase_model_flash_area()\n");
        return -1;
    }

    for (uint32_t i = 0; i < blocks_needed; i++) {
        ret = hx_lib_spi_eeprom_erase_sector(spi_inst, block_addr, FLASH_64KBLOCK);
        xprintf("  Erase block %d addr 0x%08x -> result %d\n", i, block_addr, ret);
        if (ret != 0) {
            xprintf("Failed to erase block at address 0x%08x\n", block_addr);
            ret = -1;
            break;
        }
        block_addr += FLASH_BLOCK_SIZE;

        // Force a task switch to prevent the inactivity timeout firing.
        // Will cause a call to vApplicationTaskSwitchedIn()
        vTaskDelay(1);
    }

    xSemaphoreGive(xSPIMutex);
    return ret;
}

/**
 * Check whether the named model is stored in flash by reading and validating
 * the ModelMetaData header.
 */
bool xip_is_model_in_flash(char *filename, bool cold_boot) {
    int ret = 0;
    ModelMetaData metaDataRam;
    uint32_t meta_physical_addr;
    uint8_t maxLabels;

    if (init_flash() != 0) {
        return false;
    }

    // Round up meta size to be word-aligned
    uint32_t meta_length = align_up(sizeof(ModelMetaData), 4);

    if ((meta_length & 0x3) != 0) {
        return false;   // should never happen
    }

    // The meta data is written at the start of the XIP model area
    meta_physical_addr = virt_to_phys(MODEL_XIP_ADDR);

    // Ensure XIP is disabled before SPI access
    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for xip_is_model_in_flash\n");
        return false;
    }

    ret = hx_lib_spi_eeprom_word_read(spi_inst, meta_physical_addr,
                                       (uint32_t *)&metaDataRam, meta_length);
    xSemaphoreGive(xSPIMutex);

    if (ret != 0) {
        return false;
    }

    // Check the magic word
    if (metaDataRam.magic != LABEL_MAGIC) {
        xprintf("Missing signature 0x%08x\n", LABEL_MAGIC);
        return false;
    }

    // Enable XIP mode to permit memory access to the XIP flash
    if (enable_xip(true)) {
        xprintf("XIP mode re-enabled\n");
    }
    else {
        return false;
    }

    // Print verbose diagnostics only on cold boot
    if (cold_boot) {
        XP_LT_GREY;
        xprintf("Meta data found in flash\n");
        printf_x_printBuffer((const uint8_t *)&metaDataRam, sizeof(ModelMetaData));

        // Small delay for buffer to print
        vTaskDelay(pdMS_TO_TICKS(10));

        XP_WHITE;

        xprintf("Magic: 0x%08x Model '%s' has %d labels of %d bytes:\n",
                (int)metaDataRam.magic, metaDataRam.modelName,
                metaDataRam.class_count, metaDataRam.label_len);

        maxLabels = metaDataRam.class_count;
        if (maxLabels > MAX_CLASSES) {
            maxLabels = MAX_CLASSES;
        }
        for (uint8_t i = 0; i < maxLabels; i++) {
            xprintf("%d = '%s'\n", i, metaDataRam.labels[i]);
        }
    }

    // Compare filename against the name stored in the metadata
    if (strncmp(filename, metaDataRam.modelName, MAX_MODEL_NAME_LEN) == 0) {
        xprintf("Model file name in flash is '%s'\n", filename);
        return true;
    }
    else {
        char flashName[MAX_MODEL_NAME_LEN];
        strncpy(flashName, metaDataRam.modelName, MAX_MODEL_NAME_LEN - 1);
        flashName[MAX_MODEL_NAME_LEN - 1] = '\0';
        xprintf("Model file name in flash is '%s', not '%s'\n", flashName, filename);
        return false;
    }
}

/**
 * Check whether the named file exists in /MANIFEST on the SD card.
 */
bool xip_is_file_in_sd(char *filename) {
    // sizeof(CONFIG_DIR) includes its NUL; MAX_MODEL_NAME_LEN includes its NUL;
    // the extra byte accounts for the '/' separator, giving a small margin.
    char manifest_path[sizeof(CONFIG_DIR) + MAX_MODEL_NAME_LEN + 1];
    FILINFO fno;
    FRESULT res;

    if (fatfs_mounted()) {
        snprintf(manifest_path, sizeof(manifest_path), "%s/%s", CONFIG_DIR, filename);
        xprintf("Looking for '%s'\n", manifest_path);
        res = f_stat(manifest_path, &fno);
        return (res == FR_OK);
    }
    else {
        return false;
    }
}

/**
 * Check that a TFLite "TFL3" magic marker is present at the expected model
 * offset in flash (read via SPI, not XIP).
 */
bool xip_valid_model_in_flash(void) {
    uint32_t model_physical_addr;
    // The "TFL3" string should be at spi_hdr[4] for 4 bytes
    uint8_t spi_hdr[8] __attribute__((aligned(4))) = {0};

    // Ensure XIP is disabled before SPI access
    if (!enable_xip(false)) {
        xprintf("Failed to disable XIP\n");
        return false;
    }

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for xip_valid_model_in_flash()\n");
        return false;
    }

    // The model starts on a 16-byte boundary beyond the meta data
    model_physical_addr = virt_to_phys(MODEL_XIP_ADDR)
                          + align_up(sizeof(ModelMetaData), 16);

    if (hx_lib_spi_eeprom_word_read(spi_inst, model_physical_addr,
                                     (uint32_t *)spi_hdr, sizeof(spi_hdr)) != 0) {
        xprintf("Failed to read SPI header\n");
        xSemaphoreGive(xSPIMutex);
        return false;
    }

    if (memcmp(&spi_hdr[4], "TFL3", 4) != 0) {
        xprintf("No valid TFLite model in flash\n");
        xSemaphoreGive(xSPIMutex);
        return false;
    }

    xSemaphoreGive(xSPIMutex);
    return true;
}

/**
 * Validate the model header in flash, enable XIP, and return the virtual
 * address at which the model data begins.
 *
 * @return virtual address of the model, or 0 on failure
 */
uint32_t xip_get_model_xip_address(void) {
    if (init_flash() != 0) {
        xprintf("Flash init failed\n");
        return 0;
    }

    if (!xip_valid_model_in_flash()) {
        xprintf("No valid TFLite model in flash\n");
        return 0;
    }

    if (!enable_xip(true)) {
        return 0;
    }

    // The model starts on a 16-byte boundary beyond the meta data
    return MODEL_XIP_ADDR + align_up(sizeof(ModelMetaData), 16);
}

/**
 * Write a ModelMetaData structure to the start of the model flash area via SPI.
 */
static int32_t write_metadata_to_flash(ModelMetaData *metaDataRam) {
    uint32_t meta_physical_addr;
    int32_t res;
    uint32_t numBytes = sizeof(ModelMetaData);

    /* Sanity checks */
    if ((numBytes & 0x3) != 0) {
        return -1;  // size not word-aligned (should never happen)
    }

    meta_physical_addr = virt_to_phys(MODEL_XIP_ADDR);

    if ((meta_physical_addr & 0x3) != 0) {
        return -2;  // address not word-aligned
    }

    // Disable XIP before SPI write
    enable_xip(false);
    res = hx_lib_spi_eeprom_word_write(spi_inst, meta_physical_addr,
                                        (uint32_t *)metaDataRam, numBytes);
    if (res != 0) {
        xprintf("Failed to write meta data %d\n", res);
    }
    return res;
}

/**
 * Copy a model file from /MANIFEST/<filename> on the SD card to the XIP
 * flash model area.  Erases required sectors first.  Metadata is NOT
 * written here — call xip_copy_metadata_to_flash() separately.
 */
bool xip_copy_model_from_sd_to_flash(char *filename) {
    FRESULT res;
    FIL file;
    UINT bytesRead;
    DWORD fileSize;

    uint32_t flash_address;
    uint32_t totalBytesRead;
    uint32_t write_size_bytes;
    int32_t result;
    uint32_t flashSizeRequired;

    // sizeof(CONFIG_DIR) includes its NUL; MAX_MODEL_NAME_LEN includes its NUL;
    // the extra byte accounts for the '/' separator, giving a small margin.
    char manifest_path[sizeof(CONFIG_DIR) + MAX_MODEL_NAME_LEN + 1];

    // Build the full path to the model file on the SD card
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", CONFIG_DIR, filename);

    if (init_flash() != 0) {
        return false;
    }

    // Step 1: allocate write and verify buffers from the heap to avoid stack overflow

    uint8_t *write_buf  = (uint8_t *)pvPortMalloc(FILE_CHUNK_SIZE);
    uint8_t *verify_buf = (uint8_t *)pvPortMalloc(FILE_CHUNK_SIZE);

    if (!write_buf || !verify_buf) {
        vPortFree(write_buf);
        vPortFree(verify_buf);
        xprintf("Failed to allocate write_buf[], verify_buf[]\n");
        return false;
    }

    // Step 2: open the model file and determine its size

    res = f_open(&file, manifest_path, FA_READ);
    if (res != FR_OK) {
        xprintf("Failed to open model file %s (error: %d)\n", manifest_path, res);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }

    fileSize = f_size(&file);
    if (fileSize == 0) {
        xprintf("Model file is empty\n");
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }

    xprintf("Model file size: %lu bytes\n", fileSize);

    // Step 3: erase flash to make space for the model and the meta data.
    // The model data must be pushed out to align on a 16-byte boundary.
    flashSizeRequired = align_up(sizeof(ModelMetaData), 16) + fileSize;

    if (xip_erase_model_flash_area(flashSizeRequired) != 0) {
        xprintf("Failed to erase flash for model\n");
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }

    // Step 4: write the model itself to the flash.
    // The meta data occupies the start of the XIP model area;
    // the model starts on a 16-byte boundary beyond it.
    flash_address  = virt_to_phys(MODEL_XIP_ADDR) + align_up(sizeof(ModelMetaData), 16);
    totalBytesRead = 0;

    xprintf("Writing model to 0x%08x\n", flash_address);

    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for model write\n");
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }

    while (1) {
        memset(write_buf, 0, FILE_CHUNK_SIZE);

        res = f_read(&file, write_buf, FILE_CHUNK_SIZE, &bytesRead);
        if (res != FR_OK) {
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return false;
        }
        if (bytesRead == 0) {
            break;  // EOF - this is the exit from the while() loop
        }

        // Round up to a 4-byte boundary (bytes_len must be a multiple of 4)
        write_size_bytes = align_up(bytesRead, 4);

        xprintf("Writing %d bytes (aligned %d) to 0x%08x\n",
                bytesRead, write_size_bytes, (unsigned)flash_address);

        result = hx_lib_spi_eeprom_word_write(spi_inst, flash_address,
                                               (uint32_t *)write_buf, write_size_bytes);
        if (result != 0) {
            xprintf("Flash write failed at 0x%08x\n", (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return false;
        }

        // Verify the write
        memset(verify_buf, 0, write_size_bytes);
        result = hx_lib_spi_eeprom_word_read(spi_inst, flash_address,
                                              (uint32_t *)verify_buf, write_size_bytes);
        if (result != 0) {
            xprintf("Flash verify read failed at 0x%08x\n", (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return false;
        }

        if (memcmp(write_buf, verify_buf, write_size_bytes) != 0) {
            xprintf("Verify FAIL at 0x%08x\n", (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return false;
        }

        flash_address  += write_size_bytes;
        totalBytesRead += bytesRead;

        // Force a task switch to prevent the inactivity timeout firing.
        // Will cause a call to vApplicationTaskSwitchedIn()
        vTaskDelay(1);

    } // while(1)

    // Step 5: close the file and report results
    xSemaphoreGive(xSPIMutex);
    f_close(&file);
    vPortFree(write_buf);
    vPortFree(verify_buf);

    if (totalBytesRead == fileSize) {
        xprintf("Model successfully written to 0x%08x (%lu bytes)\n",
                (unsigned)MODEL_FLASH_ADDR, (unsigned long)fileSize);
        enable_xip(true);
        return true;
    }

    xprintf("Incomplete write: %lu/%lu bytes\n",
            (unsigned long)totalBytesRead, (unsigned long)fileSize);
    return false;
}

/**
 * Build a ModelMetaData record from the model name and the label file on
 * the SD card, then write it to the start of the model flash area.
 */
bool xip_copy_metadata_to_flash(char *modelName) {
    ModelMetaData metaDataRam;
    uint8_t numLabels;

    memset(&metaDataRam, 0, sizeof(ModelMetaData));

    metaDataRam.magic     = LABEL_MAGIC;
    metaDataRam.label_len = MAX_LABEL_LEN;

    // Copy model name e.g. "1V2.TFL" — 8.3 format
    strncpy(metaDataRam.modelName, modelName, MAX_MODEL_NAME_LEN - 1);

    // Load class labels from the corresponding .TXT file on the SD card
    numLabels = load_labels_from_manifest(modelName, metaDataRam.labels);
    metaDataRam.class_count = numLabels;

    XP_LT_GREY;
    xprintf("Built this metadata:\n");
    printf_x_printBuffer((uint8_t *)&metaDataRam, sizeof(ModelMetaData));
    XP_WHITE;

    if (write_metadata_to_flash(&metaDataRam) != 0) {
        xprintf("Failed to write metadata to flash\n");
        return false;
    }

    // Enable XIP so the metaDataFlash pointer is valid for the print below
    if (enable_xip(true)) {
        xprintf("XIP mode re-enabled\n");
    }

    XP_LT_GREY;
    xprintf("Meta data now in flash:\n");
    printf_x_printBuffer((const uint8_t *)metaDataFlash, sizeof(ModelMetaData));
    XP_WHITE;

    return true;
}

/**
 * Read the first 32 bytes of the slot selector sector and print them to
 * the console via printf_x_printBuffer().
 */
int xip_dump_slot_selector(void) {
    uint8_t buf[32] __attribute__((aligned(4)));

    if (init_flash() != 0) {
        return -1;
    }

    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("xip_dump_slot_selector: mutex take failed\n");
        return -1;
    }

    if (hx_lib_spi_eeprom_word_read(spi_inst, FLASH_SELECTOR_ADDR,
                                     (uint32_t *)buf, sizeof(buf)) != 0) {
        xprintf("xip_dump_slot_selector: SPI read failed\n");
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    xSemaphoreGive(xSPIMutex);

    xprintf("Slot selector (0x%08x, first %u bytes):\n",
            FLASH_SELECTOR_ADDR, (unsigned)sizeof(buf));
    printf_x_printBuffer(buf, sizeof(buf));

    return 0;
}

/*************************************** Firmware Slot Management — Static Helpers ***************/

/**
 * Read and validate the slot selector sector header.
 * Caller must have called init_flash() first.
 *
 * @param hdr  output; filled on success
 * @return 0 on success, -1 on error or invalid magic
 */
static int read_slot_selector(SlotSelectorHeader *hdr) {
    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("read_slot_selector: mutex take failed\n");
        return -1;
    }

    int ret = hx_lib_spi_eeprom_word_read(spi_inst, FLASH_SELECTOR_ADDR,
                                           (uint32_t *)hdr, sizeof(SlotSelectorHeader));
    xSemaphoreGive(xSPIMutex);

    if (ret != 0) {
        xprintf("read_slot_selector: SPI read failed\n");
        return -1;
    }

    if (memcmp(hdr->magic, "HIMAXWE2", 8) != 0) {
        xprintf("read_slot_selector: invalid magic\n");
        return -1;
    }

    return 0;
}

/**
 * Return the index of the currently active firmware slot.
 *
 * @return 0 (Slot A), 1 (Slot B), or -1 on error
 */
static int get_active_slot(void) {
    SlotSelectorHeader hdr;

    if (init_flash() != 0) {
        return -1;
    }

    if (read_slot_selector(&hdr) != 0) {
        return -1;
    }

    if (hdr.flash_offset == FLASH_SLOT_A_ADDR) {
        return 0;
    }
    if (hdr.flash_offset == FLASH_SLOT_B_ADDR) {
        return 1;
    }

    xprintf("get_active_slot: unknown flash_offset 0x%08x\n",
            (unsigned)hdr.flash_offset);
    return -1;
}

#if XIP_FIRMWARE_VERIFY_AFTER_WRITE
/**
 * Full-pass read-back verification: re-open the SD card file and compare
 * every byte against what was written to the flash slot.
 *
 * @param slot     0 for Slot A, 1 for Slot B
 * @param filepath full path to the firmware image on the SD card
 * @return 0 if flash contents match the file, -1 on mismatch or error
 */
static int verify_firmware_slot(uint8_t slot, const char *filepath) {
    FRESULT res;
    FIL file;
    UINT bytes_read;
    uint32_t flash_address = (slot == 0) ? FLASH_SLOT_A_ADDR : FLASH_SLOT_B_ADDR;
    uint32_t total_verified = 0;
    int ret = 0;

    uint8_t *file_buf  = (uint8_t *)pvPortMalloc(FILE_CHUNK_SIZE);
    uint8_t *flash_buf = (uint8_t *)pvPortMalloc(FILE_CHUNK_SIZE);

    if (!file_buf || !flash_buf) {
        vPortFree(file_buf);
        vPortFree(flash_buf);
        xprintf("verify_firmware_slot: malloc failed\n");
        return -1;
    }

    res = f_open(&file, filepath, FA_READ);
    if (res != FR_OK) {
        vPortFree(file_buf);
        vPortFree(flash_buf);
        xprintf("verify_firmware_slot: cannot open %s (%d)\n", filepath, res);
        return -1;
    }

    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        f_close(&file);
        vPortFree(file_buf);
        vPortFree(flash_buf);
        xprintf("verify_firmware_slot: mutex take failed\n");
        return -1;
    }

    while (1) {
        res = f_read(&file, file_buf, FILE_CHUNK_SIZE, &bytes_read);
        if (res != FR_OK) {
            xprintf("verify_firmware_slot: file read error %d\n", res);
            ret = -1;
            break;
        }
        if (bytes_read == 0) {
            break;  // EOF — all bytes matched - this is the way out of the while() loop
        }

        uint32_t read_size = align_up(bytes_read, 4);

        if (hx_lib_spi_eeprom_word_read(spi_inst, flash_address,
                                         (uint32_t *)flash_buf, read_size) != 0) {
            xprintf("verify_firmware_slot: flash read failed at 0x%08x\n",
                    (unsigned)flash_address);
            ret = -1;
            break;
        }

        if (memcmp(file_buf, flash_buf, bytes_read) != 0) {
            xprintf("verify_firmware_slot: mismatch at 0x%08x\n",
                    (unsigned)flash_address);
            ret = -1;
            break;
        }

        flash_address  += read_size;
        total_verified += bytes_read;

        // Force a task switch to prevent the inactivity timeout firing.
        // Will cause a call to vApplicationTaskSwitchedIn()
        vTaskDelay(1);
    } // while (1)

    xSemaphoreGive(xSPIMutex);
    f_close(&file);
    vPortFree(file_buf);
    vPortFree(flash_buf);

    if (ret == 0) {
        xprintf("verify_firmware_slot: slot %d full verify OK (%lu bytes)\n",
                slot, (unsigned long)total_verified);
    }
    return ret;
}
#endif /* XIP_FIRMWARE_VERIFY_AFTER_WRITE */

/*************************************** Firmware Slot Management ********************************/

/**
 * Erase one firmware image slot (16 × 64 KB blocks).
 */
static int erase_firmware_slot(uint8_t slot) {
    uint32_t slot_addr;
    const uint32_t blocks_needed = FLASH_SLOT_SIZE / FLASH_BLOCK_SIZE;  // 16

    if (slot == 0) {
        slot_addr = FLASH_SLOT_A_ADDR;
    } else if (slot == 1) {
        slot_addr = FLASH_SLOT_B_ADDR;
    } else {
        xprintf("erase_firmware_slot: invalid slot %d\n", slot);
        return -1;
    }

    if (init_flash() != 0) {
        return -1;
    }

    xprintf("Erasing firmware slot %d (%lu x 64KB blocks from 0x%08x)\n",
            slot, (unsigned long)blocks_needed, (unsigned)slot_addr);

    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("erase_firmware_slot: mutex take failed\n");
        return -1;
    }

    uint32_t addr = slot_addr;
    for (uint32_t i = 0; i < blocks_needed; i++) {
        int ret = hx_lib_spi_eeprom_erase_sector(spi_inst, addr, FLASH_64KBLOCK);
        if (ret != 0) {
            xprintf("erase_firmware_slot: erase failed at 0x%08x\n", (unsigned)addr);
            xSemaphoreGive(xSPIMutex);
            return -1;
        }
        addr += FLASH_BLOCK_SIZE;

        // Force a task switch to prevent the inactivity timeout firing.
        // Will cause a call to vApplicationTaskSwitchedIn()
        vTaskDelay(1);
    }

    xSemaphoreGive(xSPIMutex);
    xprintf("Firmware slot %d erased OK\n", slot);
    return 0;
}

/**
 * Write a firmware image from an SD card file to the specified flash slot,
 * with per-chunk read-back verification.
 *
 * The slot must already be erased before calling this function.
 *
 * @param slot     0 for Slot A, 1 for Slot B
 * @param filepath full path to the firmware image on the SD card
 * @return 0 on success, -1 on failure
 */
static int write_firmware_from_sd(uint8_t slot, const char *filepath) {
    FRESULT res;
    FIL file;
    UINT bytes_read;
    uint32_t flash_address;
    uint32_t total_written = 0;
    uint32_t write_size;
    int32_t result;
    DWORD file_size;

    if (slot == 0) {
        flash_address = FLASH_SLOT_A_ADDR;
    } else if (slot == 1) {
        flash_address = FLASH_SLOT_B_ADDR;
    } else {
        xprintf("write_firmware_from_sd: invalid slot %d\n", slot);
        return -1;
    }

    if (init_flash() != 0) {
        return -1;
    }

    uint8_t *write_buf  = (uint8_t *)pvPortMalloc(FILE_CHUNK_SIZE);
    uint8_t *verify_buf = (uint8_t *)pvPortMalloc(FILE_CHUNK_SIZE);

    if (!write_buf || !verify_buf) {
        vPortFree(write_buf);
        vPortFree(verify_buf);
        xprintf("write_firmware_from_sd: malloc failed\n");
        return -1;
    }

    res = f_open(&file, filepath, FA_READ);
    if (res != FR_OK) {
        vPortFree(write_buf);
        vPortFree(verify_buf);
        xprintf("write_firmware_from_sd: cannot open %s (%d)\n", filepath, res);
        return -1;
    }

    file_size = f_size(&file);
    if (file_size == 0) {
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        xprintf("write_firmware_from_sd: file is empty\n");
        return -1;
    }

    if (file_size > FLASH_SLOT_SIZE) {
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        xprintf("write_firmware_from_sd: file too large (%lu > %lu bytes)\n",
                (unsigned long)file_size, (unsigned long)FLASH_SLOT_SIZE);
        return -1;
    }

    xprintf("Writing %lu bytes to firmware slot %d (0x%08x)\n",
            (unsigned long)file_size, slot, (unsigned)flash_address);

    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        xprintf("write_firmware_from_sd: mutex take failed\n");
        return -1;
    }

    while (1) {
        memset(write_buf, 0xFF, FILE_CHUNK_SIZE);   // pre-fill with erased state

        res = f_read(&file, write_buf, FILE_CHUNK_SIZE, &bytes_read);
        if (res != FR_OK) {
            xprintf("write_firmware_from_sd: file read error %d\n", res);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return -1;
        }
        if (bytes_read == 0) {
            break;  // EOF
        }

        // Round up to a 4-byte boundary (bytes_len must be a multiple of 4)
        write_size = align_up(bytes_read, 4);

        result = hx_lib_spi_eeprom_word_write(spi_inst, flash_address,
                                               (uint32_t *)write_buf, write_size);
        if (result != 0) {
            xprintf("write_firmware_from_sd: write failed at 0x%08x\n",
                    (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return -1;
        }

        // Per-chunk read-back verify
        memset(verify_buf, 0, write_size);
        result = hx_lib_spi_eeprom_word_read(spi_inst, flash_address,
                                              (uint32_t *)verify_buf, write_size);
        if (result != 0) {
            xprintf("write_firmware_from_sd: verify read failed at 0x%08x\n",
                    (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return -1;
        }

        if (memcmp(write_buf, verify_buf, write_size) != 0) {
            xprintf("write_firmware_from_sd: verify mismatch at 0x%08x\n",
                    (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return -1;
        }

        flash_address  += write_size;
        total_written  += bytes_read;


        // Force a task switch to prevent the inactivity timeout firing.
        // Will cause a call to vApplicationTaskSwitchedIn()
        vTaskDelay(1);
    } // while(1)

    xSemaphoreGive(xSPIMutex);
    f_close(&file);
    vPortFree(write_buf);
    vPortFree(verify_buf);

    if (total_written != (uint32_t)file_size) {
        xprintf("write_firmware_from_sd: incomplete write %lu/%lu bytes\n",
                (unsigned long)total_written, (unsigned long)file_size);
        return -1;
    }

    xprintf("Firmware slot %d written and chunk-verified OK (%lu bytes)\n",
            slot, (unsigned long)total_written);
    return 0;
}

/**
 * Erase the slot selector sector and write a fresh 20-byte header pointing
 * to the specified firmware slot.  The remainder of the sector is left in
 * the erased (0xFF) state.
 */
static int write_slot_selector(uint8_t slot) {
    SlotSelectorHeader hdr;

    if (slot == 0) {
        hdr.flash_offset = FLASH_SLOT_A_ADDR;
        hdr.checksum     = SLOT_A_SELECTOR_CHECKSUM;
    } else if (slot == 1) {
        hdr.flash_offset = FLASH_SLOT_B_ADDR;
        hdr.checksum     = SLOT_B_SELECTOR_CHECKSUM;
    } else {
        xprintf("write_slot_selector: invalid slot %d\n", slot);
        return -1;
    }

    memcpy(hdr.magic, "HIMAXWE2", 8);
    hdr.constant_02 = 0x00000002;
    hdr.hx_dsp_flag = 0x0001;

    if (init_flash() != 0) {
        return -1;
    }

    enable_xip(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("write_slot_selector: mutex take failed\n");
        return -1;
    }

    // Erase the 4 KB selector sector
    if (hx_lib_spi_eeprom_erase_sector(spi_inst, FLASH_SELECTOR_ADDR, FLASH_SECTOR) != 0) {
        xprintf("write_slot_selector: sector erase failed\n");
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    // Write the 20-byte header; remainder stays 0xFF (erased)
    if (hx_lib_spi_eeprom_word_write(spi_inst, FLASH_SELECTOR_ADDR,
                                      (uint32_t *)&hdr, sizeof(SlotSelectorHeader)) != 0) {
        xprintf("write_slot_selector: header write failed\n");
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    xSemaphoreGive(xSPIMutex);

    enable_xip(true);

    xprintf("write_slot_selector: slot %d selector written OK\n", slot);

    return 0;
}

/**
 * Top-level firmware update: erase inactive slot, write and verify image,
 * then update the slot selector.
 */
int xip_update_firmware_from_sd(const char *filename) {
    // sizeof(CONFIG_DIR) includes its NUL; +1 for the '/' separator
    char filepath[sizeof(CONFIG_DIR) + MAX_FIRMWARE_NAME_LEN + 1];
    int active_slot;
    int target_slot;

    snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, filename);

    // Step 0: verify the image file exists before touching flash
    if (!file_exists(filepath)) {
        xprintf("firmware: %s not found on SD card\n", filepath);
        return -1;
    }

    // Step 1: find the active slot
    active_slot = get_active_slot();
    if (active_slot < 0) {
        xprintf("firmware: cannot read slot selector\n");
        return -1;
    }
    xprintf("firmware: current active slot is %d\n", active_slot);

    // Step 2: target is the other slot
    target_slot = (active_slot == 0) ? 1 : 0;
    xprintf("firmware: programming slot %d from %s\n", target_slot, filepath);

    // Step 3: erase target slot
    if (erase_firmware_slot(target_slot) != 0) {
        xprintf("firmware: slot erase failed\n");
        return -2;
    }

    // Step 4: write firmware image with per-chunk verification
    if (write_firmware_from_sd(target_slot, filepath) != 0) {
        xprintf("firmware: write failed — slot selector NOT updated\n");
        return -3;
    }

#if XIP_FIRMWARE_VERIFY_AFTER_WRITE
    // Step 4b: full-pass read-back verification
    if (verify_firmware_slot(target_slot, filepath) != 0) {
        xprintf("firmware: full verify failed — slot selector NOT updated\n");
        return -4;
    }
#endif

    // Step 5: update slot selector to point to the new image
    if (write_slot_selector(target_slot) != 0) {
        xprintf("firmware: slot selector update failed\n");
        return -5;
    }

    xprintf("firmware: slot %d updated OK. Type 'reset' to boot the new image.\n",
            target_slot);
    return 0;
}
