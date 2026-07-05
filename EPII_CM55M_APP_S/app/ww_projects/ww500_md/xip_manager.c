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

// Firmware slot layout (dpd layout — we2_image_gen_local_dpd):
//
//   0x00000  hx_memory_descriptor      (ROM reads from Slot A only)
//   0x01000  2nd_bootloader            (ROM RSA-verifies from Slot A only)
//   0x13000  bootloader                (always from Slot A)
//   0x27000  hx_mem_descriptor_ota     (2nd_bootloader reads from SELECTED slot)
//   0x28000  cm55m_application         (from selected slot)
//
// FLASH_OTA_OFFSET (0x27000) — correct lower bound for Slot A OTA erase/write/verify.
//   The descriptor at 0x27000 contains a 2-byte CRC of the application; it changes
//   with every build.  Writing only from 0x28000 leaves a stale CRC → boot failure.
//   The boot chain at 0x00000–0x26FFF is preserved — erasing it while the board
//   runs from Slot B bricks the board (no software recovery path).
//
// FLASH_APP_OFFSET (0x28000) — application start; kept for size-validation checks.
//
// Slot B: erase/write/verify from 0 (full 1 MB).  Slot B is never the ROM's boot
//   source so it can be fully erased.  A complete image is required because the
//   2nd_bootloader reads Slot B's hx_mem_descriptor_ota from Slot B base + 0x27000.
#define FLASH_OTA_OFFSET        0x27000              // hx_mem_descriptor_ota start (dpd layout)
#define FLASH_APP_OFFSET        0x28000              // cm55m_application start (dpd layout)

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

/*
 * Wildlife Watcher slot metadata, stored in the spare (0xFF) bytes of the slot
 * selector sector after the bootloader's 20-byte header. The bootloader only
 * reads its own header, so this record is invisible to it.
 *
 * Records which camera variant (XIP_SLOT_VARIANT_x) each firmware slot holds,
 * so the day/night automatic camera switch can verify that the other slot
 * really contains the wanted variant before flipping the selector.
 */
#define SLOT_META_OFFSET    32          // byte offset within the selector sector (word-aligned)
#define SLOT_META_MAGIC     "WWSM"

// aligned(4): instances are cast to (uint32_t *) for the word-based SPI EEPROM
// API, but the members alone would only guarantee byte alignment
typedef struct {
    char    magic[4];       // "WWSM"
    uint8_t variant[2];     // XIP_SLOT_VARIANT_x for Slot A ([0]) and Slot B ([1])
    uint8_t reserved[2];    // keeps the record a whole number of words
} __attribute__((aligned(4))) SlotMetaRecord;

/*
 * First word of a programmed firmware slot: the secure-boot container magic
 * ("ckBS" bytes = 0x53426B63 read as a little-endian word). An erased slot
 * reads 0xFFFFFFFF.
 */
#define SB_CONTAINER_MAGIC  0x53426B63u

/*************************************** Local variables *************************************/

// Use this constant since a compiler issue can redefine USE_DW_SPI_MST_Q
// in some translation units (see "Bizarre" comment in original cvapp.cpp).
static USE_DW_SPI_MST_E spi_inst = (USE_DW_SPI_MST_E)0;

static bool flash_initialized = false;

// xSPIMutex serialises all SPI flash accesses and spans the XIP-disabled window.
// disable_xip() takes it and holds it; enable_xip() releases it.  The mutex must
// remain held for the entire disable → operate → enable sequence so no other task
// can call disable_xip() or enable_xip() in the middle of a write or erase.
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
static bool disable_xip(void);
static bool enable_xip(void);
static int32_t write_metadata_to_flash(ModelMetaData *metaDataRam);
static int erase_firmware_slot(uint8_t slot);
static int write_firmware_from_sd(uint8_t slot, const char *filepath);
static int write_slot_selector(uint8_t slot);
static int read_slot_meta(SlotMetaRecord *meta);
static int write_selector_sector(const SlotSelectorHeader *hdr, const SlotMetaRecord *meta);

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
 * Create the SPI mutex, open the SPI peripheral, and confirm the flash chip
 * is present by reading its ID.  Returns with XIP enabled.
 * Safe to call multiple times — initialises only on the first call.
 *
 * hx_lib_spi_eeprom_open() reconfigures the SPI controller and takes the chip
 * out of XIP continuous-read mode, so hx_lib_spi_eeprom_read_ID() can follow
 * without an explicit mode switch.  XIP is re-enabled before returning so the
 * invariant (XIP always on except during a disable_xip/enable_xip window) holds
 * from the very first call.
 *
 * No mutex is taken here: init_flash() is called before any competing SPI
 * access can occur, so serialisation is not needed at this point.
 */
static int init_flash(void) {
    uint8_t id_info;

    if (flash_initialized) {
        return 0;
    }

    if (xSPIMutex == NULL) {
        xSPIMutex = xSemaphoreCreateMutex();
        if (xSPIMutex == NULL) {
            xprintf("init_flash: failed to create SPI mutex\n");
            return -1;
        }
    }

    if (hx_lib_spi_eeprom_open(spi_inst) != 0) {
        xprintf("init_flash: failed to open SPI EEPROM\n");
        return -1;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    if (hx_lib_spi_eeprom_read_ID(spi_inst, &id_info) != 0) {
        xprintf("init_flash: failed to read flash ID\n");
        return -1;
    }

    // Restore XIP: open() left the chip in SPI command mode.
    if (hx_lib_spi_eeprom_enable_XIP(spi_inst, true, FLASH_QUAD, true) != 0) {
        xprintf("init_flash: failed to enable XIP\n");
        return -1;
    }

    xprintf("Flash ID 0x%02X initialised\n", id_info);
    flash_initialized = true;
    return 0;
}

/**
 * Switch the serial flash chip out of XIP (memory-mapped) mode and back into
 * normal SPI command mode.  Must be called before any erase or write operation.
 *
 * Background — what XIP mode is:
 *   The HX6538 connects its 16 MB serial flash over a QSPI bus.  In XIP mode
 *   the flash controller places the chip into a continuous-read state and maps
 *   the entire 16 MB into the CPU virtual address space starting at
 *   FLASH_VIRTUAL_BASE (0x3A000000):
 *
 *     0x3A000000–0x3A0FFFFF  Firmware Slot A
 *     0x3A100000–0x3A1FFFFF  Firmware Slot B
 *     0x3A200000–0x3AEFFFFF  NN model area
 *     0x3AFFF000–0x3AFFFFFF  Boot slot selector
 *
 *   While XIP is active, the CPU (and the D-cache/I-cache) can read any of
 *   these addresses as ordinary memory.  The TFLite model is executed directly
 *   from the flash chip via this mechanism.
 *
 * Why XIP must be disabled before a write or erase:
 *   In XIP mode the flash chip treats every bus cycle as a read request.  An
 *   SPI program or erase command sent to a chip in XIP mode is undefined — the
 *   chip may misinterpret the command bytes as address or data, corrupt an
 *   unintended location, or lock up.  XIP must therefore be disabled (returning
 *   the chip to normal SPI command mode) before issuing any write or erase.
 *
 * Returns true on success with xSPIMutex still held — the caller must call
 * enable_xip() on every exit path to release it.
 * Returns false if the mutex could not be taken or the mode switch failed;
 * in that case the mutex is NOT held on return.
 */
static bool disable_xip(void) {
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("disable_xip: failed to take SPI mutex\n");
        return false;
    }

    if (hx_lib_spi_eeprom_enable_XIP(spi_inst, false, FLASH_QUAD, true) != 0) {
        xprintf("disable_xip: mode switch failed\n");
        xSemaphoreGive(xSPIMutex);
        return false;
    }

    // xSPIMutex remains held — caller must call enable_xip() to release it.
    return true;
}

/**
 * Switch the serial flash chip back into XIP (memory-mapped) mode after a
 * write or erase operation is complete.
 *
 * See disable_xip() for a full explanation of XIP mode.  Once this function
 * returns successfully the flash virtual address window (0x3A000000+) is live
 * again and the TFLite model can be accessed normally.
 *
 * Assumes xSPIMutex is already held by the calling task (taken by disable_xip).
 * Always releases xSPIMutex before returning, whether or not the mode switch
 * succeeded.
 */
static bool enable_xip(void) {
    if (hx_lib_spi_eeprom_enable_XIP(spi_inst, true, FLASH_QUAD, true) != 0) {
        xprintf("enable_xip: mode switch failed\n");
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

    if (!disable_xip()) {
        return -1;
    }

    for (uint32_t i = 0; i < blocks_needed; i++) {
        ret = hx_lib_spi_eeprom_erase_sector(spi_inst, block_addr, FLASH_64KBLOCK);
        xprintf("  Erase block %d addr 0x%08x -> result %d\n", i, block_addr, ret);
        if (ret != 0) {
            xprintf("Failed to erase block at address 0x%08x\n", block_addr);
            enable_xip();
            return -1;
        }
        block_addr += FLASH_BLOCK_SIZE;

        // Force a task switch to prevent the inactivity timeout firing.
        // Will cause a call to vApplicationTaskSwitchedIn()
        vTaskDelay(1);
    }

    enable_xip();
    return ret;
}

/**
 * Check whether the named model is stored in flash by reading and validating
 * the ModelMetaData header (read via SPI command mode).
 *
 * Simplification opportunity: this is a read-only operation.  With XIP enabled
 * the metadata is accessible directly at MODEL_XIP_ADDR, so disable_xip /
 * enable_xip could be avoided by casting MODEL_XIP_ADDR to a ModelMetaData
 * pointer instead.  If the metadata was written in the current boot cycle,
 * SCB_InvalidateDCache_by_Addr() would need to be called first.
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

    if (!disable_xip()) {
        return false;
    }

    ret = hx_lib_spi_eeprom_word_read(spi_inst, meta_physical_addr,
                                       (uint32_t *)&metaDataRam, meta_length);
    if (ret != 0) {
        enable_xip();
        return false;
    }

    // Check the magic word
    if (metaDataRam.magic != LABEL_MAGIC) {
        xprintf("Missing signature 0x%08x\n", LABEL_MAGIC);
        enable_xip();
        return false;
    }

    if (!enable_xip()) {
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
 * offset in flash (read via SPI command mode).
 *
 * Simplification opportunity: this is a read-only check.  With XIP enabled the
 * same bytes are accessible at MODEL_XIP_ADDR + align_up(sizeof(ModelMetaData), 16)
 * + 4, so disable_xip/enable_xip could be avoided entirely.  If the model area
 * was written in the current boot cycle, SCB_InvalidateDCache_by_Addr() would
 * need to be called first to discard any stale cache lines.
 */
bool xip_valid_model_in_flash(void) {
    uint32_t model_physical_addr;
    // The "TFL3" string should be at spi_hdr[4] for 4 bytes
    uint8_t spi_hdr[8] __attribute__((aligned(4))) = {0};

    if (!disable_xip()) {
        xprintf("Failed to disable XIP\n");
        return false;
    }

    // The model starts on a 16-byte boundary beyond the meta data
    model_physical_addr = virt_to_phys(MODEL_XIP_ADDR)
                          + align_up(sizeof(ModelMetaData), 16);

    if (hx_lib_spi_eeprom_word_read(spi_inst, model_physical_addr,
                                     (uint32_t *)spi_hdr, sizeof(spi_hdr)) != 0) {
        xprintf("Failed to read SPI header\n");
        enable_xip();
        return false;
    }

    if (memcmp(&spi_hdr[4], "TFL3", 4) != 0) {
        xprintf("No valid TFLite model in flash\n");
        enable_xip();
        return false;
    }

    enable_xip();
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

    // xip_valid_model_in_flash() re-enabled XIP on all return paths.
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

    if (!disable_xip()) {
        return -3;
    }

    res = hx_lib_spi_eeprom_word_write(spi_inst, meta_physical_addr,
                                        (uint32_t *)metaDataRam, numBytes);
    enable_xip();

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

    if (!disable_xip()) {
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }

    while (1) {
        memset(write_buf, 0, FILE_CHUNK_SIZE);

        res = f_read(&file, write_buf, FILE_CHUNK_SIZE, &bytesRead);
        if (res != FR_OK) {
            enable_xip();
            f_close(&file);
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
            enable_xip();
            f_close(&file);
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
            enable_xip();
            f_close(&file);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return false;
        }

        if (memcmp(write_buf, verify_buf, write_size_bytes) != 0) {
            xprintf("Verify FAIL at 0x%08x\n", (unsigned)flash_address);
            enable_xip();
            f_close(&file);
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
    enable_xip();
    f_close(&file);
    vPortFree(write_buf);
    vPortFree(verify_buf);

    if (totalBytesRead == fileSize) {
        xprintf("Model successfully written to 0x%08x (%lu bytes)\n",
                (unsigned)MODEL_FLASH_ADDR, (unsigned long)fileSize);
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

    // XIP re-enabled by write_metadata_to_flash(); metaDataFlash pointer is valid.
    XP_LT_GREY;
    xprintf("Meta data now in flash:\n");
    printf_x_printBuffer((const uint8_t *)metaDataFlash, sizeof(ModelMetaData));
    XP_WHITE;

    return true;
}

/**
 * Read the first 32 bytes of the slot selector sector and print them to
 * the console via printf_x_printBuffer() (read via SPI command mode).
 *
 * Simplification opportunity: the selector sector is mapped at virtual address
 * phys_to_virt(FLASH_SELECTOR_ADDR) = 0x3AFFF000 while XIP is active, so this
 * function could read from that address directly without toggling XIP.
 */
int xip_dump_slot_selector(void) {
    uint8_t buf[32] __attribute__((aligned(4)));

    if (init_flash() != 0) {
        return -1;
    }

    if (!disable_xip()) {
        return -1;
    }

    if (hx_lib_spi_eeprom_word_read(spi_inst, FLASH_SELECTOR_ADDR,
                                     (uint32_t *)buf, sizeof(buf)) != 0) {
        xprintf("xip_dump_slot_selector: SPI read failed\n");
        enable_xip();
        return -1;
    }

    enable_xip();

    xprintf("Slot selector (0x%08x, first %u bytes):\n",
            FLASH_SELECTOR_ADDR, (unsigned)sizeof(buf));
    printf_x_printBuffer(buf, sizeof(buf));

    return 0;
}

/*************************************** Firmware Slot Management — Static Helpers ***************/

/**
 * Read and validate the slot selector sector header (read via SPI command mode).
 * Caller must have called init_flash() first.
 *
 * Simplification opportunity: the selector sector is mapped at virtual address
 * phys_to_virt(FLASH_SELECTOR_ADDR) = 0x3AFFF000 while XIP is active, so this
 * function could memcpy from that address without toggling XIP.
 *
 * @param hdr  output; filled on success
 * @return 0 on success, -1 on error or invalid magic
 */
static int read_slot_selector(SlotSelectorHeader *hdr) {
    if (!disable_xip()) {
        return -1;
    }

    int ret = hx_lib_spi_eeprom_word_read(spi_inst, FLASH_SELECTOR_ADDR,
                                           (uint32_t *)hdr, sizeof(SlotSelectorHeader));
    enable_xip();

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
    uint32_t slot_base      = (slot == 0) ? FLASH_SLOT_A_ADDR : FLASH_SLOT_B_ADDR;
    uint32_t file_offset    = (slot == 0) ? FLASH_OTA_OFFSET : 0;
    uint32_t flash_address  = slot_base + file_offset;
    uint32_t start_address  = flash_address;
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

    if (file_offset > 0) {
        res = f_lseek(&file, file_offset);
        if (res != FR_OK) {
            f_close(&file);
            vPortFree(file_buf);
            vPortFree(flash_buf);
            xprintf("verify_firmware_slot: seek to 0x%08x failed (%d)\n",
                    (unsigned)file_offset, res);
            return -1;
        }
    }

    xprintf("verify_firmware_slot: slot %d — verifying flash 0x%08x onwards\n",
            slot, (unsigned)flash_address);

    if (!disable_xip()) {
        f_close(&file);
        vPortFree(file_buf);
        vPortFree(flash_buf);
        xprintf("verify_firmware_slot: failed to disable XIP\n");
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
            break;  // EOF — all bytes matched
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

    enable_xip();
    f_close(&file);
    vPortFree(file_buf);
    vPortFree(flash_buf);

    if (ret == 0) {
        xprintf("verify_firmware_slot: slot %d verify OK"
                "  0x%08x–0x%08x  (%lu bytes)\n",
                slot, (unsigned)start_address,
                (unsigned)(start_address + total_verified - 1),
                (unsigned long)total_verified);
    }
    return ret;
}
#endif /* XIP_FIRMWARE_VERIFY_AFTER_WRITE */

/*************************************** Firmware Slot Management ********************************/

/**
 * Erase the writable portion of a firmware slot.
 *
 * Slot A: erases from FLASH_OTA_OFFSET (0x27000) to end-of-slot.
 *   The boot chain at 0x00000–0x26FFF is preserved — erasing it while the board
 *   runs from Slot B bricks the board with no software recovery path.
 *   Phase 0: 1 × 4 KB sector at FLASH_OTA_OFFSET (0x27000) — the descriptor sector.
 *   FLASH_APP_OFFSET (0x28000) is not 64 KB-aligned, so two further phases:
 *     Phase 1: 8 × 4 KB sector erases  (0x28000–0x2FFFF)
 *     Phase 2: 13 × 64 KB block erases (0x30000–0xFFFFF)
 *
 * Slot B: erases the full 1 MB (16 × 64 KB blocks from slot base).
 *   Slot B is never the ROM's source for the boot chain, so it can be fully
 *   erased at any time.  A complete image (including boot chain descriptors)
 *   must be written so the 2nd_bootloader can locate Slot B's application.
 */
static int erase_firmware_slot(uint8_t slot) {
    uint32_t slot_addr;

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

    uint32_t slot_end = slot_addr + FLASH_SLOT_SIZE;

    if (!disable_xip()) {
        return -1;
    }

    if (slot == 0) {
        // Slot A: preserve boot chain — erase descriptor + application area.
        // Phase 0: descriptor sector at FLASH_OTA_OFFSET (one 4 KB sector).
        // Phases 1+2: application area from FLASH_APP_OFFSET to end of slot.
        uint32_t desc_start     = slot_addr + FLASH_OTA_OFFSET;
        uint32_t app_start      = slot_addr + FLASH_APP_OFFSET;
        uint32_t block_boundary = ((app_start + FLASH_BLOCK_SIZE - 1) / FLASH_BLOCK_SIZE) * FLASH_BLOCK_SIZE;
        uint32_t n_sectors      = (block_boundary - app_start) / FLASH_SECTOR_SIZE;   // 8
        uint32_t n_blocks       = (slot_end - block_boundary)  / FLASH_BLOCK_SIZE;    // 13

        xprintf("erase_firmware_slot: slot 0 — boot chain preserved 0x%08x–0x%08x\n",
                (unsigned)slot_addr, (unsigned)(desc_start - 1));
        xprintf("erase_firmware_slot: phase 0: 1 x 4KB sector   0x%08x  (descriptor)\n",
                (unsigned)desc_start);
        xprintf("erase_firmware_slot: phase 1: %lu x 4KB sectors  0x%08x–0x%08x\n",
                (unsigned long)n_sectors, (unsigned)app_start, (unsigned)(block_boundary - 1));
        xprintf("erase_firmware_slot: phase 2: %lu x 64KB blocks  0x%08x–0x%08x\n",
                (unsigned long)n_blocks, (unsigned)block_boundary, (unsigned)(slot_end - 1));

        // Phase 0: descriptor sector
        if (hx_lib_spi_eeprom_erase_sector(spi_inst, desc_start, FLASH_SECTOR) != 0) {
            xprintf("erase_firmware_slot: 4KB erase failed at 0x%08x\n", (unsigned)desc_start);
            enable_xip();
            return -1;
        }
        vTaskDelay(1);

        // Phase 1: 4 KB sectors up to next 64 KB boundary
        uint32_t addr = app_start;
        for (uint32_t i = 0; i < n_sectors; i++) {
            int ret = hx_lib_spi_eeprom_erase_sector(spi_inst, addr, FLASH_SECTOR);
            if (ret != 0) {
                xprintf("erase_firmware_slot: 4KB erase failed at 0x%08x\n", (unsigned)addr);
                enable_xip();
                return -1;
            }
            addr += FLASH_SECTOR_SIZE;
            vTaskDelay(1);
        }

        // Phase 2: 64 KB blocks to end of slot
        addr = block_boundary;
        for (uint32_t i = 0; i < n_blocks; i++) {
            int ret = hx_lib_spi_eeprom_erase_sector(spi_inst, addr, FLASH_64KBLOCK);
            if (ret != 0) {
                xprintf("erase_firmware_slot: 64KB erase failed at 0x%08x\n", (unsigned)addr);
                enable_xip();
                return -1;
            }
            addr += FLASH_BLOCK_SIZE;
            vTaskDelay(1);
        }

        xprintf("erase_firmware_slot: slot 0 erased OK  0x%08x–0x%08x\n",
                (unsigned)desc_start, (unsigned)(slot_end - 1));

    } else {
        // Slot B: full 1 MB erase — safe, and required so the complete image is written
        const uint32_t n_blocks = FLASH_SLOT_SIZE / FLASH_BLOCK_SIZE;   // 16

        xprintf("erase_firmware_slot: slot 1 — full erase  %lu x 64KB blocks  0x%08x–0x%08x\n",
                (unsigned long)n_blocks, (unsigned)slot_addr, (unsigned)(slot_end - 1));

        uint32_t addr = slot_addr;
        for (uint32_t i = 0; i < n_blocks; i++) {
            int ret = hx_lib_spi_eeprom_erase_sector(spi_inst, addr, FLASH_64KBLOCK);
            if (ret != 0) {
                xprintf("erase_firmware_slot: 64KB erase failed at 0x%08x\n", (unsigned)addr);
                enable_xip();
                return -1;
            }
            addr += FLASH_BLOCK_SIZE;
            vTaskDelay(1);
        }

        xprintf("erase_firmware_slot: slot 1 erased OK  0x%08x–0x%08x\n",
                (unsigned)slot_addr, (unsigned)(slot_end - 1));
    }

    enable_xip();
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
    uint32_t slot_base;

    if (slot == 0) {
        slot_base = FLASH_SLOT_A_ADDR;
    } else if (slot == 1) {
        slot_base = FLASH_SLOT_B_ADDR;
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

    // Slot A: write from FLASH_OTA_OFFSET (0x27000) — includes descriptor + application.
    //   Boot chain (0x00000–0x26FFF) preserved in flash.
    // Slot B: write full image from byte 0 — complete image required so the
    //         2nd_bootloader (loaded from Slot A) can locate Slot B's application.
    uint32_t file_offset;
    uint32_t app_bytes;

    if (slot == 0) {
        if (file_size <= FLASH_APP_OFFSET) {
            f_close(&file);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            xprintf("write_firmware_from_sd: file too small — no application data beyond 0x%08x\n",
                    (unsigned)FLASH_APP_OFFSET);
            return -1;
        }
        file_offset = FLASH_OTA_OFFSET;
        app_bytes   = (uint32_t)file_size - FLASH_OTA_OFFSET;

        res = f_lseek(&file, FLASH_OTA_OFFSET);
        if (res != FR_OK) {
            f_close(&file);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            xprintf("write_firmware_from_sd: seek to 0x%08x failed (%d)\n",
                    (unsigned)FLASH_OTA_OFFSET, res);
            return -1;
        }
        xprintf("write_firmware_from_sd: slot 0 — descriptor + application\n");
        xprintf("  file  0x%08x–0x%08x  →  flash 0x%08x–0x%08x  (%lu bytes)\n",
                (unsigned)file_offset, (unsigned)(file_size - 1),
                (unsigned)(slot_base + file_offset),
                (unsigned)(slot_base + (uint32_t)file_size - 1),
                (unsigned long)app_bytes);
        xprintf("  boot chain (0x%08x–0x%08x) preserved in flash\n",
                (unsigned)slot_base, (unsigned)(slot_base + FLASH_OTA_OFFSET - 1));
    } else {
        file_offset = 0;
        app_bytes   = (uint32_t)file_size;
        xprintf("write_firmware_from_sd: slot 1 — full image\n");
        xprintf("  file  0x00000000–0x%08x  →  flash 0x%08x–0x%08x  (%lu bytes)\n",
                (unsigned)(file_size - 1),
                (unsigned)slot_base, (unsigned)(slot_base + (uint32_t)file_size - 1),
                (unsigned long)app_bytes);
    }

    flash_address = slot_base + file_offset;

    if (!disable_xip()) {
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        xprintf("write_firmware_from_sd: failed to disable XIP\n");
        return -1;
    }

    while (1) {
        memset(write_buf, 0xFF, FILE_CHUNK_SIZE);   // pre-fill with erased state

        res = f_read(&file, write_buf, FILE_CHUNK_SIZE, &bytes_read);
        if (res != FR_OK) {
            xprintf("write_firmware_from_sd: file read error %d\n", res);
            enable_xip();
            f_close(&file);
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
            enable_xip();
            f_close(&file);
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
            enable_xip();
            f_close(&file);
            vPortFree(write_buf);
            vPortFree(verify_buf);
            return -1;
        }

        if (memcmp(write_buf, verify_buf, write_size) != 0) {
            xprintf("write_firmware_from_sd: verify mismatch at 0x%08x\n",
                    (unsigned)flash_address);
            enable_xip();
            f_close(&file);
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

    enable_xip();
    f_close(&file);
    vPortFree(write_buf);
    vPortFree(verify_buf);

    if (total_written != app_bytes) {
        xprintf("write_firmware_from_sd: incomplete write %lu/%lu bytes\n",
                (unsigned long)total_written, (unsigned long)app_bytes);
        return -1;
    }

    xprintf("write_firmware_from_sd: slot %d written and chunk-verified OK\n"
            "  flash 0x%08x–0x%08x  (%lu bytes)\n",
            slot, (unsigned)(slot_base + file_offset),
            (unsigned)(slot_base + file_offset + total_written - 1),
            (unsigned long)total_written);
    return 0;
}

/**
 * Erase the slot selector sector and write a fresh 20-byte header pointing
 * to the specified firmware slot.  The remainder of the sector is left in
 * the erased (0xFF) state.
 */
static int write_slot_selector(uint8_t slot) {
    SlotSelectorHeader hdr;
    SlotMetaRecord meta;

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

    // Preserve the camera variant labels across the sector rewrite
    if (read_slot_meta(&meta) != 0) {
        return -1;
    }

    if (write_selector_sector(&hdr, &meta) != 0) {
        return -1;
    }

    xprintf("write_slot_selector: slot %d selector written OK\n", slot);

    return 0;
}

/**
 * Read the Wildlife Watcher slot metadata record from the selector sector.
 *
 * A missing/blank record (fresh device, or selector written by older firmware)
 * is not an error: the record is returned initialised with both slots
 * XIP_SLOT_VARIANT_UNKNOWN.
 */
static int read_slot_meta(SlotMetaRecord *meta) {
    if (!disable_xip()) {
        return -1;
    }

    int ret = hx_lib_spi_eeprom_word_read(spi_inst, FLASH_SELECTOR_ADDR + SLOT_META_OFFSET,
                                          (uint32_t *)meta, sizeof(SlotMetaRecord));
    enable_xip();

    if (ret != 0) {
        xprintf("read_slot_meta: SPI read failed\n");
        return -1;
    }

    if (memcmp(meta->magic, SLOT_META_MAGIC, 4) != 0) {
        // Record never written - report both slots unknown
        memcpy(meta->magic, SLOT_META_MAGIC, 4);
        meta->variant[0]  = XIP_SLOT_VARIANT_UNKNOWN;
        meta->variant[1]  = XIP_SLOT_VARIANT_UNKNOWN;
        meta->reserved[0] = 0xFF;
        meta->reserved[1] = 0xFF;
    }

    return 0;
}

/**
 * Erase the selector sector then write the bootloader header and the Wildlife
 * Watcher metadata record. The caller must have called init_flash().
 */
static int write_selector_sector(const SlotSelectorHeader *hdr, const SlotMetaRecord *meta) {
    // Local copies: the SPI write API takes non-const pointers
    SlotSelectorHeader h = *hdr;
    SlotMetaRecord     m = *meta;

    if (!disable_xip()) {
        return -1;
    }

    // Erase the 4 KB selector sector
    if (hx_lib_spi_eeprom_erase_sector(spi_inst, FLASH_SELECTOR_ADDR, FLASH_SECTOR) != 0) {
        xprintf("write_selector_sector: sector erase failed\n");
        enable_xip();
        return -1;
    }

    // Write the 20-byte bootloader header
    if (hx_lib_spi_eeprom_word_write(spi_inst, FLASH_SELECTOR_ADDR,
                                      (uint32_t *)&h, sizeof(SlotSelectorHeader)) != 0) {
        xprintf("write_selector_sector: header write failed\n");
        enable_xip();
        return -1;
    }

    // Write the metadata record; the rest of the sector stays 0xFF (erased)
    if (hx_lib_spi_eeprom_word_write(spi_inst, FLASH_SELECTOR_ADDR + SLOT_META_OFFSET,
                                      (uint32_t *)&m, sizeof(SlotMetaRecord)) != 0) {
        xprintf("write_selector_sector: metadata write failed\n");
        enable_xip();
        return -1;
    }

    enable_xip();

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

    // Step 6: the new image's camera variant is unknown until it boots and
    // labels itself (cameraSwitch_labelBootSlot()), so clear any stale label.
    // A failure is not fatal to the update, but a stale label could mislead
    // the camera switching logic - make it visible.
    if (xip_set_slot_variant((uint8_t)target_slot, XIP_SLOT_VARIANT_UNKNOWN) != 0) {
        xprintf("firmware: warning: failed to clear the slot %d variant label\n", target_slot);
    }

    xprintf("firmware: slot %d updated OK. Type 'reset' to boot the new image.\n",
            target_slot);
    return 0;
}

/**
 * Report which firmware slot the bootloader will execute.
 *
 * @return 0 (Slot A), 1 (Slot B), or -1 on failure
 */
int xip_get_active_slot(void) {
    return get_active_slot();
}

/**
 * Read the camera variant label recorded for a slot.
 *
 * @param slot  0 or 1
 * @return XIP_SLOT_VARIANT_x, or -1 on failure
 */
int xip_get_slot_variant(uint8_t slot) {
    SlotMetaRecord meta;

    if (slot > 1) {
        return -1;
    }
    if (init_flash() != 0) {
        return -1;
    }
    if (read_slot_meta(&meta) != 0) {
        return -1;
    }

    return meta.variant[slot];
}

/**
 * Record the camera variant label for a slot. Only rewrites the selector
 * sector if the label actually changes.
 *
 * @param slot     0 or 1
 * @param variant  XIP_SLOT_VARIANT_x
 * @return 0 on success (including no-change), negative on failure
 */
int xip_set_slot_variant(uint8_t slot, uint8_t variant) {
    SlotSelectorHeader hdr;
    SlotMetaRecord meta;

    if (slot > 1) {
        return -1;
    }
    if (init_flash() != 0) {
        return -1;
    }

    // Need the current header so the sector rewrite keeps the bootloader's choice
    if (read_slot_selector(&hdr) != 0) {
        return -1;
    }
    if (read_slot_meta(&meta) != 0) {
        return -1;
    }

    if (meta.variant[slot] == variant) {
        return 0;   // no change - avoid a pointless sector erase
    }

    meta.variant[slot] = variant;

    if (write_selector_sector(&hdr, &meta) != 0) {
        return -2;
    }

    xprintf("Slot %c labelled variant %d\n", (slot == 0) ? 'A' : 'B', variant);

    return 0;
}

/**
 * Point the bootloader at the other firmware slot, WITHOUT writing any image.
 * Used for day/night camera switching when both slots already hold images.
 *
 * Refuses to switch if the target slot does not start with the secure-boot
 * container magic (i.e. it is erased or corrupt), so the device cannot be
 * pointed at an unbootable slot.
 *
 * The caller is responsible for scheduling a reset (e.g. app_setResetRequest()).
 *
 * @return the new active slot (0 or 1) on success;
 *         -1 selector read/flash failure, -2 target slot has no image,
 *         -3 selector write failure
 */
int xip_switch_slot(void) {
    int active_slot;
    int target_slot;
    uint32_t slot_base;
    uint32_t magic = 0;

    // get_active_slot() also initialises the flash, but be explicit so this
    // function does not depend on that internal detail
    if (init_flash() != 0) {
        return -1;
    }

    active_slot = get_active_slot();
    if (active_slot < 0) {
        return -1;
    }

    target_slot = (active_slot == 0) ? 1 : 0;
    slot_base   = (target_slot == 0) ? FLASH_SLOT_A_ADDR : FLASH_SLOT_B_ADDR;

    // Safety: the target slot must contain a programmed application.
    // Check at FLASH_APP_OFFSET, not the slot base: Slot A's base always holds
    // the boot chain, so only the application partition indicates whether an
    // app image was ever written to that slot.
    if (!disable_xip()) {
        return -1;
    }
    int ret = hx_lib_spi_eeprom_word_read(spi_inst, slot_base + FLASH_APP_OFFSET,
                                          &magic, sizeof(magic));
    enable_xip();

    if (ret != 0) {
        xprintf("switch_slot: SPI read failed\n");
        return -1;
    }
    if (magic != SB_CONTAINER_MAGIC) {
        xprintf("switch_slot: slot %d has no image (first word 0x%08x) - not switching\n",
                target_slot, (unsigned)magic);
        return -2;
    }

    if (write_slot_selector((uint8_t)target_slot) != 0) {
        return -3;
    }

    xprintf("switch_slot: selector now points at slot %d\n", target_slot);

    return target_slot;
}
