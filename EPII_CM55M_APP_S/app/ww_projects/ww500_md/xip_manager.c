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
#include "printf_x.h"
#include "xip_manager.h"

/*************************************** Definitions *******************************************/

// Read SD card model file in chunks of this size.
// Larger values reduce per-chunk overhead (f_read calls, xprintf, DMA setup).
// Buffers are heap-allocated so this does not affect stack usage.
#define FILE_CHUNK_SIZE     4096

/*************************************** Local variables *************************************/

// Use this constant since a compiler issue can redefine USE_DW_SPI_MST_Q
// in some translation units (see "Bizarre" comment in original cvapp.cpp).
static USE_DW_SPI_MST_E spi_inst = (USE_DW_SPI_MST_E)0;

static bool flash_initialized = false;
static SemaphoreHandle_t xSPIMutex = NULL;

static uint8_t g_label_count = 0;

/*************************************** Local Function Declarations **************************/

static inline uint32_t align_up(uint32_t size, uint32_t align);
static inline uint32_t virt_to_phys(uint32_t virt);
static inline uint32_t phys_to_virt(uint32_t phys);
static bool file_exists(const char *path);
static uint8_t load_labels_from_manifest(char *filename, char (*labels)[MAX_LABEL_LEN]);

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
int xip_init_flash(void) {
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
bool xip_enable_XIP(bool enable) {
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex in xip_enable_XIP()\n");
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

    if (xip_init_flash() != 0) {
        return -1;
    }

    blocks_needed = align_up(flashSizeRequired, FLASH_BLOCK_SIZE) / FLASH_BLOCK_SIZE;
    block_addr    = virt_to_phys(MODEL_XIP_ADDR);

    xprintf("Erasing %d x 64KB blocks from 0x%08x to cover %lu bytes\n",
            blocks_needed, block_addr, (unsigned long)flashSizeRequired);

    // Disable XIP before erase
    xip_enable_XIP(false);

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

    if (xip_init_flash() != 0) {
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
    xip_enable_XIP(false);

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
    if (xip_enable_XIP(true)) {
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
    char manifest_path[10 + MAX_MODEL_NAME_LEN];
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
    if (!xip_enable_XIP(false)) {
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
    if (xip_init_flash() != 0) {
        xprintf("Flash init failed\n");
        return 0;
    }

    if (!xip_valid_model_in_flash()) {
        xprintf("No valid TFLite model in flash\n");
        return 0;
    }

    if (!xip_enable_XIP(true)) {
        return 0;
    }

    // The model starts on a 16-byte boundary beyond the meta data
    return MODEL_XIP_ADDR + align_up(sizeof(ModelMetaData), 16);
}

/**
 * Write a ModelMetaData structure to the start of the model flash area via SPI.
 */
int32_t xip_write_meta_data_to_flash(ModelMetaData *metaDataRam) {
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
    xip_enable_XIP(false);
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
    uint32_t write_size;
    int32_t result;
    uint32_t flashSizeRequired;

    char manifest_path[10 + MAX_MODEL_NAME_LEN];

    // Build the full path to the model file on the SD card
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", CONFIG_DIR, filename);

    if (xip_init_flash() != 0) {
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

    xip_enable_XIP(false);

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
            break;  // EOF
        }

        // Round up to a number of 32-bit words
        write_size = align_up(bytesRead, 4);

        xprintf("Writing %d bytes (aligned %d) to 0x%08x\n",
                bytesRead, write_size, (unsigned)flash_address);

        result = hx_lib_spi_eeprom_word_write(spi_inst, flash_address,
                                               (uint32_t *)write_buf, write_size);
        if (result != 0) {
            xprintf("Flash write failed at 0x%08x\n", (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return false;
        }

        // Verify the write
        memset(verify_buf, 0, write_size);
        result = hx_lib_spi_eeprom_word_read(spi_inst, flash_address,
                                              (uint32_t *)verify_buf, write_size);
        if (result != 0) {
            xprintf("Flash verify read failed at 0x%08x\n", (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return false;
        }

        if (memcmp(write_buf, verify_buf, write_size) != 0) {
            xprintf("Verify FAIL at 0x%08x\n", (unsigned)flash_address);
            f_close(&file);
            xSemaphoreGive(xSPIMutex);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return false;
        }

        flash_address  += write_size;
        totalBytesRead += bytesRead;
    }

    // Step 5: close the file and report results
    xSemaphoreGive(xSPIMutex);
    f_close(&file);
    vPortFree(write_buf);
    vPortFree(verify_buf);

    if (totalBytesRead == fileSize) {
        xprintf("Model successfully written to 0x%08x (%lu bytes)\n",
                (unsigned)MODEL_FLASH_ADDR, (unsigned long)fileSize);
        xip_enable_XIP(true);
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

    xip_write_meta_data_to_flash(&metaDataRam);

    // Enable XIP so the metaDataFlash pointer is valid for the print below
    if (xip_enable_XIP(true)) {
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

    if (xip_init_flash() != 0) {
        return -1;
    }

    xip_enable_XIP(false);

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

/*************************************** Firmware Slot Management ******************************/

/**
 * Erase one firmware image slot in flash.
 * Not yet implemented — requires further documentation on bootloader layout.
 */
int xip_erase_firmware_slot(uint8_t slot) {
    /* Not yet implemented */
    (void)slot;
    xprintf("xip_erase_firmware_slot: not yet implemented\n");
    return -1;
}

/**
 * Write a firmware image from an SD card file to the specified flash slot.
 * Not yet implemented — requires further documentation on bootloader layout.
 */
int xip_write_firmware_from_sd(uint8_t slot, const char *filename) {
    /* Not yet implemented */
    (void)slot;
    (void)filename;
    xprintf("xip_write_firmware_from_sd: not yet implemented\n");
    return -1;
}

/**
 * Write the slot selector sector to indicate which firmware slot to boot from.
 * Not yet implemented — requires further documentation on bootloader layout.
 */
int xip_write_slot_selector(uint8_t slot) {
    /* Not yet implemented */
    (void)slot;
    xprintf("xip_write_slot_selector: not yet implemented\n");
    return -1;
}
