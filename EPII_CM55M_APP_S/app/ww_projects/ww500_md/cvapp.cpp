/*
 * cvapp.cpp
 *
 *  Created on: 2022/02/22
 *      Author: 902452
 */

#include <cstdio>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "WE2_device.h"
#include "board.h"
#include "cvapp.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "hx_drv_spi.h"
#include "spi_eeprom_comm.h"

#include "WE2_core.h"
#include "WE2_device.h"

#include "ethosu_driver.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"

#include "xprintf.h"
#include "ff.h"
#include "fatfs_task.h"  // For fatfs_load_labels and fatfs_unzip_manifest

// Compare model in flash with file in chunks to avoid large allocations
#define VERIFY_CHUNK_SIZE 512

// Required for the app_get_xxx() functions
//#include "cisdp_cfg.h"
#include "cisdp_sensor.h"

#include "person_detect_model_data_vela.h"
#include "common_config.h"

#include "printf_x.h" // Print colours

#define LOCAL_FRAQ_BITS (8)
#define SC(A, B) ((A << 8) / B)

#define INPUT_SIZE_X 96
#define INPUT_SIZE_Y 96

#ifdef TRUSTZONE_SEC
#define U55_BASE BASE_ADDR_APB_U55_CTRL_ALIAS
#else
#ifndef TRUSTZONE
#define U55_BASE BASE_ADDR_APB_U55_CTRL_ALIAS
#else
#define U55_BASE BASE_ADDR_APB_U55_CTRL
#endif
#endif

extern uint8_t __tensor_arena_start__;
extern uint8_t __tensor_arena_end__;

static uint8_t* tensor_arena_buf = &__tensor_arena_start__;
static size_t tensor_arena_size  = 0;  // Will be calculated at runtime in cv_init()

// #define TENSOR_ARENA_BUFSIZE (125 * 1024)
// __attribute__(( section(".bss.NoInit"))) uint8_t tensor_arena_buf[TENSOR_ARENA_BUFSIZE] __ALIGNED(32);

using namespace std;
namespace
{
    // constexpr int tensor_arena_size = TENSOR_ARENA_BUFSIZE;
    // static uint32_t tensor_arena= (uint32_t)tensor_arena_buf;

	struct ethosu_driver ethosu_drv; /* Default Ethos-U device driver */
	tflite::MicroInterpreter *int_ptr = nullptr;
	tflite::MicroMutableOpResolver<1> *op_resolver_ptr = nullptr;
	TfLiteTensor *input, *output;
    
    // Labels loaded from SD (one per line) – used for printing class names
    static const int MAX_LABELS = 64;           // Adjust if needed
    static const int MAX_LABEL_LEN = 48;        // Adjust if needed
    static char g_labels[MAX_LABELS][MAX_LABEL_LEN];
    static int g_label_count = 0;
    static bool g_labels_loaded = false;
};
// SPI EEPROM configuration
static USE_DW_SPI_MST_E spi_id = USE_DW_SPI_MST_Q;
static bool flash_initialized = false;
static SemaphoreHandle_t xSPIMutex = NULL;

// Global pointer to hold dynamically allocated model
static uint8_t* global_model_buffer = nullptr;

// Add a flag to track if we've already loaded from SD card
static bool model_loaded_from_sd = false;
int model_loaded = 0; // Will be read from flash or default to 1

// ------------------- Declarations -------------------

// int load_model_cli_command(int model_selection);
bool is_model_in_flash(char* filename);
int load_model_from_sd_to_flash(char* filename);
static const tflite::Model* load_model_from_flash(void);
int erase_model_flash_area(uint32_t model_size);
void compare_sd_spi_xip_chunked(const char* filename);
void debug_sd_model_integrity(const char* filename);
void debug_flash_status();
void test_basic_spi_operations();
int cv_deinit();
int init_flash();
static inline uint32_t align_down(uint32_t addr, uint32_t align);
static inline uint32_t align_up(uint32_t size, uint32_t align);
int read_persisted_model_number();
int write_persisted_model_number(int model_num);
static bool file_exists(const char* path);

// --------------------- Utilities / Helpers ---------------------

// Round down address to sector boundary
static inline uint32_t align_down(uint32_t addr, uint32_t align) {
    return addr & ~(align - 1);
}
// Round up size to align
static inline uint32_t align_up(uint32_t size, uint32_t align) {
    return (size + align - 1) & ~(align - 1);
}

// Check if a file exists on SD
static bool file_exists(const char* path) {
    FILINFO finfo;
    FRESULT res = f_stat(path, &finfo);
    return (res == FR_OK);
}

// Read the persisted model number from flash (stored at header - 4 bytes)
// Flash layout: [model_number(4 bytes)][filename(16 bytes)][model data...]
int read_persisted_model_number() {
    if (init_flash() != 0) {
        xprintf("Flash init failed, defaulting to model 1\n");
        return 1; // Default to model 1 if flash not available
    }

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for read_persisted_model_number\n");
        return 1;
    }

    // Disable XIP before SPI read
    hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);

    // Model number is stored 20 bytes before the model data
    // (4 bytes for model number + 16 bytes for filename header)
    uint32_t model_xip_addr = 0x3A200000;
    uint32_t model_num_xip_addr = model_xip_addr - 20;
    uint32_t model_num_phys_addr = virt_to_phys(model_num_xip_addr);

    uint32_t stored_model_num = 0;
    if (hx_lib_spi_eeprom_word_read(spi_id, model_num_phys_addr, &stored_model_num, 4) != 0) {
        xprintf("Failed to read persisted model number, defaulting to 1\n");
        xSemaphoreGive(xSPIMutex);
        return 1;
    }

    xSemaphoreGive(xSPIMutex);

    // Validate the model number (should be 1-9, otherwise flash is uninitialized)
    if (stored_model_num >= 1 && stored_model_num <= 9) {
        xprintf("Persisted model number from flash: %d\n", (int)stored_model_num);
        return (int)stored_model_num;
    } else {
        xprintf("Invalid persisted model number (0x%08lX), defaulting to 1\n", stored_model_num);
        return 1;
    }
}

// Write the model number to flash (stored at header - 4 bytes)
int write_persisted_model_number(int model_num) {
    if (init_flash() != 0) {
        return -1;
    }

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for write_persisted_model_number\n");
        return -1;
    }

    // Disable XIP before write
    hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);

    uint32_t model_xip_addr = 0x3A200000;
    uint32_t model_num_xip_addr = model_xip_addr - 20;
    uint32_t model_num_phys_addr = virt_to_phys(model_num_xip_addr);

    // Note: We don't erase here because this location is part of the same sector
    // that gets erased when we write a new model. We just update the value.
    uint32_t model_num_word = (uint32_t)model_num;
    if (hx_lib_spi_eeprom_word_write(spi_id, model_num_phys_addr, &model_num_word, 4) != 0) {
        xprintf("Failed to write persisted model number\n");
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    // Small delay for write to complete
    for (volatile int d = 0; d < 20000; ++d) { asm volatile("nop"); }

    xSemaphoreGive(xSPIMutex);
    xprintf("Persisted model number %d to flash\n", model_num);
    return 0;
}


void img_rescale(
	const uint8_t *in_image,
	const int32_t width,
	const int32_t height,
	const int32_t nwidth,
	const int32_t nheight,
	int8_t *out_image,
	const int32_t nxfactor,
	const int32_t nyfactor)
{
	int32_t x, y;
	int32_t ceil_x, ceil_y, floor_x, floor_y;

	int32_t fraction_x, fraction_y, one_min_x, one_min_y;
	int32_t pix[4]; // 4 pixels for the bilinear interpolation
	int32_t out_image_fix;

	for (y = 0; y < nheight; y++)
	{ // compute new pixels
		for (x = 0; x < nwidth; x++)
		{
			floor_x = (x * nxfactor) >> LOCAL_FRAQ_BITS; // left pixels of the window
			floor_y = (y * nyfactor) >> LOCAL_FRAQ_BITS; // upper pixels of the window

			ceil_x = floor_x + 1; // right pixels of the window
			if (ceil_x >= width)
				ceil_x = floor_x; // stay in image

			ceil_y = floor_y + 1; // bottom pixels of the window
			if (ceil_y >= height)
				ceil_y = floor_y;

			fraction_x = x * nxfactor - (floor_x << LOCAL_FRAQ_BITS); // strength coefficients
			fraction_y = y * nyfactor - (floor_y << LOCAL_FRAQ_BITS);

			one_min_x = (1 << LOCAL_FRAQ_BITS) - fraction_x;
			one_min_y = (1 << LOCAL_FRAQ_BITS) - fraction_y;

			pix[0] = in_image[floor_y * width + floor_x]; // store window
			pix[1] = in_image[floor_y * width + ceil_x];
			pix[2] = in_image[ceil_y * width + floor_x];
			pix[3] = in_image[ceil_y * width + ceil_x];

			// interpolate new pixel and truncate it's integer part
			out_image_fix = one_min_y * (one_min_x * pix[0] + fraction_x * pix[1]) + fraction_y * (one_min_x * pix[2] + fraction_x * pix[3]);
			out_image_fix = out_image_fix >> (LOCAL_FRAQ_BITS * 2);
			out_image[nwidth * y + x] = out_image_fix - 128;
		}
	}
}

static void _arm_npu_irq_handler(void)
{
	/* Call the default interrupt handler from the NPU driver */
	ethosu_irq_handler(&ethosu_drv);
}

/**
 * @brief  Initialises the NPU IRQ
 **/
static void _arm_npu_irq_init(void)
{
	const IRQn_Type ethosu_irqnum = (IRQn_Type)U55_IRQn;

	/* Register the EthosU IRQ handler in our vector table.
	 * Note, this handler comes from the EthosU driver */
	EPII_NVIC_SetVector(ethosu_irqnum, (uint32_t)_arm_npu_irq_handler);

	/* Enable the IRQ */
	NVIC_EnableIRQ(ethosu_irqnum);
}

static int _arm_npu_init(bool security_enable, bool privilege_enable)
{
	int err = 0;

	/* Initialise the IRQ */
	_arm_npu_irq_init();

	/* Initialise Ethos-U55 device */
	const void *ethosu_base_address = (void *)(U55_BASE);

	if (0 != (err = ethosu_init(
				  &ethosu_drv,		   /* Ethos-U driver device pointer */
				  ethosu_base_address, /* Ethos-U NPU's base address. */
				  NULL,				   /* Pointer to fast mem area - NULL for U55. */
				  0,				   /* Fast mem region size. */
				  security_enable,	   /* Security enable. */
				  privilege_enable)))
	{ /* Privilege enable. */
		xprintf("failed to initalise Ethos-U device\n");
		return err;
	}

	xprintf("Ethos-U55 device initialised\n");

	return 0;
}

int get_model_number() {
    return model_loaded;
}

// Optional model number parameter, default to 1
int cv_init(bool security_enable, bool privilege_enable, int model_number)
{
    xprintf("cv_init called with model_number: %d\n", model_number);

    // Calculate tensor arena size at runtime (must be done here, not at static init)
    if (tensor_arena_size == 0)
    {
        tensor_arena_size = (size_t)(&__tensor_arena_end__ - &__tensor_arena_start__);
        xprintf("Calculated tensor arena size: %lu bytes\n", (unsigned long)tensor_arena_size);
    }

    // On first boot (model_loaded == 0), read the persisted model number from flash
    if (model_loaded == 0)
    {
        model_loaded = read_persisted_model_number();
        xprintf("First boot: loaded persisted model number %d from flash\n", model_loaded);
    }
    else if (model_number != model_loaded)
    {
        model_loaded = model_number;
        model_loaded_from_sd = false; // reset flag to allow reloading
        xprintf("------------------------Model number changed to %d, resetting load flag\n", model_number);
    }

    xprintf("cv_init now loading with model number: %d\n", model_loaded);

    if (_arm_npu_init(security_enable, privilege_enable) != 0)
    {
        return -1;
    }

    static const tflite::Model *model = nullptr;

#if (MODEL_LOAD_MODE == MODEL_FROM_C_FILE)
    model = tflite::GetModel((const void *)g_person_detect_model_data_vela);

#elif (MODEL_LOAD_MODE == MODEL_FROM_FLASH)
    model = load_model_from_flash();

#elif (MODEL_LOAD_MODE == MODEL_FROM_SD_CARD)
    // New layout: models are delivered as manifest.zip -> contains ./Manifest/labels.txt and ./Manifest/MOD0000X.tfl
    // We assume the unzip step has extracted to /Manifest on SD. We'll prefer files under /Manifest.
    char filename[16];
    snprintf(filename, sizeof(filename), "MOD0000%01d.tfl", model_loaded);
    char manifest_model_path[64];
    char manifest_labels_path[64];
    snprintf(manifest_model_path, sizeof(manifest_model_path), "/Manifest/%s", filename);
    snprintf(manifest_labels_path, sizeof(manifest_labels_path), "/Manifest/labels.txt");
    snprintf(manifest_model_path, sizeof(filename), filename);
    snprintf(manifest_labels_path, sizeof(manifest_labels_path), "/Manifest/labels.txt");
    
    // If a manifest.zip exists but /Manifest is missing, extract it (STORE method only - no compression)
    if (!file_exists(manifest_model_path) && !file_exists(filename)) {
        if (file_exists("/Manifest.zip")) {
            xprintf("manifest.zip found; extracting required files (uncompressed STORE method only)...\n");
            // fatfs_unzip_manifest();
        }
    }
    
    // Attempt to load labels if present using FatFS task
    if (file_exists(manifest_labels_path)) {
        fatfs_load_labels(manifest_labels_path, g_labels, &g_label_count, MAX_LABELS, MAX_LABEL_LEN);
        g_labels_loaded = (g_label_count > 0);
    }
    xprintf("flash_init status: %s\n", flash_initialized ? "initialized" : "not initialized");
    xprintf("model_loaded_from_sd status: %s\n", model_loaded_from_sd ? "true" : "false");

    // Check if we need to load from SD card
    if (!model_loaded_from_sd)
    {
        xprintf("First boot with SD card mode\n");
    const char* load_name = file_exists(manifest_model_path) ? manifest_model_path : filename;
    xprintf("Target model filename: %s\n", load_name);
    debug_sd_model_integrity(load_name);

        // Check if model exists in flash
        if (!is_model_in_flash(filename))
        {
            xprintf("No model in flash, loading from SD card...\n");
            // We always write only the filename header (16 bytes) before model in flash, so pass the 8.3 filename.
            // Source file can be from /Manifest or SD root.
            const char* src_path = file_exists(manifest_model_path) ? manifest_model_path : filename;
            // if (load_model_from_sd_to_flash((char*)src_path) == 0)
            if (load_model_from_sd_to_flash(filename) == 0)
            {
                model_loaded_from_sd = true;
                xprintf("Model written to flash successfully, continuing with load...\n");
                // No reboot needed - XIP is already re-enabled, just continue
            }
            else
            {
                xprintf("Failed to load model from SD card\n");
                return -1;
            }
        }
        else
        {
            xprintf("Model already exists in flash, skipping SD load\n");
            model_loaded_from_sd = true;
        }
    }
    // Load the model from flash
    model = load_model_from_flash();
    // compare_sd_spi_xip_chunked(filename);
#endif

    if (!model)
    {
        xprintf("Failed to load model\n");
        return -1;
    }

    xprintf("Model loaded successfully\n");

    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        xprintf("[ERROR] Model's schema version %d is not equal to supported version %d\n",
                model->version(), TFLITE_SCHEMA_VERSION);
        return -1;
    }
    else
    {
        xprintf("Model schema version: %d\n", model->version());
        xprintf("Input: %d x %d NN: %d x %d\n",
                app_get_raw_width(), app_get_raw_height(), INPUT_SIZE_X, INPUT_SIZE_X);
    }

    static tflite::MicroErrorReporter micro_error_reporter;

    // Allocate op_resolver on heap so it persists beyond cv_init() scope
    // The interpreter keeps a reference to it, so it must live as long as the interpreter
    if (op_resolver_ptr != nullptr)
    {
        delete op_resolver_ptr;
    }
    op_resolver_ptr = new tflite::MicroMutableOpResolver<1>();

    if (kTfLiteOk != op_resolver_ptr->AddEthosU())
    {
        xprintf("Failed to add Arm NPU support to op resolver.\n");
        return -1;
    }

    xprintf("Tensor arena at %p, size = %lu bytes\n",
            tensor_arena_buf,
            (unsigned long)tensor_arena_size);

    tflite::MicroInterpreter *interpreter = new tflite::MicroInterpreter(
        model,
        *op_resolver_ptr,
        tensor_arena_buf,
        tensor_arena_size,
        &micro_error_reporter);

    if (interpreter->AllocateTensors() != kTfLiteOk)
    {
        xprintf("Failed to allocate tensors\n");
        return -1;
    }
    int_ptr = interpreter;
    input = interpreter->input(0);
    output = interpreter->output(0);
    return 0;

    // static tflite::MicroInterpreter static_interpreter(
    // 	model,
    // 	op_resolver,
    // 	(uint8_t *)tensor_arena,
    // 	tensor_arena_size,
    // 	&micro_error_reporter);

    // if (static_interpreter.AllocateTensors() != kTfLiteOk) {
    // 	xprintf("Failed to allocate tensors\n");
    // 	return -1;
    // }
    // int_ptr = &static_interpreter;
    // input = static_interpreter.input(0);
    // output = static_interpreter.output(0);
    // return 0;
}

// Initialize the flash memory (SPI EEPROM)
int init_flash(void) {
    // Create mutex on first call
    if (xSPIMutex == NULL) {
        xSPIMutex = xSemaphoreCreateMutex();
        if (xSPIMutex == NULL) {
            xprintf("Failed to create SPI mutex\n");
            return -1;
        }
    }

    // Take mutex
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex\n");
        return -1;
    }

    if (!flash_initialized) {
        xprintf("Init EEPROM...\r\n");

        // CRITICAL: Disable XIP mode before SPI operations
        hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay for mode switch

        if (hx_lib_spi_eeprom_open(spi_id) != 0) {
            xprintf("Failed to open SPI EEPROM\n");
            flash_initialized = false;
            xSemaphoreGive(xSPIMutex);
            return -1;
        }

        uint8_t id_info = 0;
        if (hx_lib_spi_eeprom_read_ID(spi_id, &id_info) != 0) {
            xprintf("Failed to read flash ID\n");
            xSemaphoreGive(xSPIMutex);
            return -1;
        }
        xprintf("Flash ID: 0x%02X\n", id_info);
        flash_initialized = true;
    }

    xSemaphoreGive(xSPIMutex);
    return 0;
}

// Erase the flash region covering the 16-byte header PLUS the model data.
int erase_model_flash_area(uint32_t model_size) {
    if (init_flash() != 0) {
        return -1;
    }

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for erase\n");
        return -1;
    }

    // Disable XIP before erase
    hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);

    // Addresses (XIP virtual used as reference)
    const uint32_t model_xip_addr = 0x3A200000;
    const uint32_t header_xip_addr = model_xip_addr - 16;
    const uint32_t model_num_xip_addr = model_xip_addr - 20; // 4 bytes for model number before filename header

    // Convert to physical addresses
    const uint32_t model_phys_addr = virt_to_phys(model_xip_addr);   // e.g. 0x00200000
    const uint32_t header_phys_addr = virt_to_phys(header_xip_addr); // e.g. 0x001FFFF0
    const uint32_t model_num_phys_addr = virt_to_phys(model_num_xip_addr); // e.g. 0x001FFFEC

    // Compute erase region such that it covers model_number + header + model_size
    uint32_t erase_start = align_down(model_num_phys_addr, FLASH_SECTOR_SIZE);
    uint32_t total_length = (model_phys_addr + model_size) - erase_start + 20;
    uint32_t sectors_needed = align_up(total_length, FLASH_SECTOR_SIZE) / FLASH_SECTOR_SIZE;

    xprintf("Erasing %lu sectors from 0x%08lX to cover header+model (%lu bytes)...\n",
           (unsigned long)sectors_needed, erase_start, (unsigned long)total_length);

    int ret = 0;
    for (uint32_t i = 0; i < sectors_needed; i++) {
        uint32_t sector_addr = erase_start + (i * FLASH_SECTOR_SIZE);
        int er = hx_lib_spi_eeprom_erase_sector(spi_id, sector_addr, FLASH_SECTOR);
        xprintf("  Erase sector %lu addr 0x%08lX -> result %d\n",
               (unsigned long)i, sector_addr, er);
        if (er != 0) {
            xprintf("Failed to erase sector at address 0x%08lX\n", sector_addr);
            ret = -1;
            break;
        }
    }

    xSemaphoreGive(xSPIMutex);
    return ret;
}


int load_model_from_sd_to_flash(char *filename) {
    FRESULT res;
    FIL file;
    UINT bytesRead;    
    union {
        uint8_t bytes[16];
        uint32_t words[4];
    } fname_header, fname_verify;
    // aligned buffers
    uint8_t write_buf[256] __attribute__((aligned(4)));
    uint8_t verify_buf[256] __attribute__((aligned(4)));

    // If 'filename' includes a path, use it to open the file but only store the 8.3 basename in flash header
    const char* src_path = filename;
    const char* base = filename;
    for (const char* p = filename; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    
    if (init_flash() != 0) {
        return -1;
    }
    xprintf("Yep Passed init\n");
    res = f_open(&file, src_path, FA_READ);
    if (res != FR_OK) {
        xprintf("Failed to open model file %s (error: %d)\n", src_path, res);
        return -1;
    }

    DWORD fileSize = f_size(&file);
    if (fileSize == 0) {
        xprintf("Model file is empty\n");
        f_close(&file);
        return -1;
    }
    xprintf("Model file size: %lu bytes\n", fileSize);

    if (erase_model_flash_area(fileSize) != 0) {
        xprintf("Failed to erase flash for model\n");
        f_close(&file);
        return -1;
    }

    uint32_t model_xip_addr = 0x3A200000;
    uint32_t header_xip_addr = model_xip_addr - 16;
    uint32_t model_phys_addr = virt_to_phys(model_xip_addr);
    uint32_t header_phys_addr = virt_to_phys(header_xip_addr);

    // Take semaphore for header write
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for header write\n");
        f_close(&file);
        return -1;
    }

    // Ensure XIP is disabled
    hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);

    memset(fname_header.bytes, 0, sizeof(fname_header.bytes));
    strncpy((char*)fname_header.bytes, base, sizeof(fname_header.bytes) - 1);
    xprintf("Writing filename header: %s\n", fname_header.bytes);

    // write header via SPI (4 words)
    int wr = hx_lib_spi_eeprom_word_write(spi_id, header_phys_addr, fname_header.words, 16);
    if (wr != 0) {
        xprintf("Failed to write filename header to flash (err %d)\n", wr);
        xSemaphoreGive(xSPIMutex);
        f_close(&file);
        return -1;
    }

    // small delay to let write finish
    for (volatile int d = 0; d < 20000; ++d) { asm volatile("nop"); }
    
    // verify header by reading back via SPI
    memset(fname_verify.bytes, 0, sizeof(fname_verify.bytes));
    if (hx_lib_spi_eeprom_word_read(spi_id, header_phys_addr, fname_verify.words, 16) != 0) {
        xprintf("Failed to verify filename header in flash (read error)\n");
        xSemaphoreGive(xSPIMutex);
        f_close(&file);
        return -1;
    }

    if (memcmp(fname_header.bytes, fname_verify.bytes, 16) != 0) {
        xprintf("Filename header verify FAIL (SPI)\n");
        xSemaphoreGive(xSPIMutex);
        f_close(&file);
        return -1;
    }

    xSemaphoreGive(xSPIMutex);

    // Write model data at model_phys_addr
    uint32_t flash_address = model_phys_addr;
    uint32_t totalBytesRead = 0;

    while (totalBytesRead < fileSize) {
        memset(write_buf, 0, sizeof(write_buf));
        res = f_read(&file, write_buf, sizeof(write_buf), &bytesRead);
        if (res != FR_OK || bytesRead == 0) {
            break;
        }
        uint32_t write_size = (bytesRead + 3) & ~3U;
        if (write_size > bytesRead) {
            memset(write_buf + bytesRead, 0, write_size - bytesRead);
        }

        // Take semaphore for each chunk write
        if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
            xprintf("Failed to take SPI mutex for chunk write\n");
            f_close(&file);
            return -1;
        }

        // Ensure XIP is disabled
        hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);

        xprintf("Writing %u bytes (aligned %u) to 0x%08lX\n",
                (unsigned)bytesRead, (unsigned)write_size, (unsigned long)flash_address);
        if (hx_lib_spi_eeprom_word_write(spi_id,
                                         flash_address,
                                         (uint32_t*)write_buf,
                                         write_size) != 0) {
            xprintf("Flash write failed at 0x%08lX\n", flash_address);
            xSemaphoreGive(xSPIMutex);
            f_close(&file);
            return -1;
        }
        for (volatile int d = 0; d < 10000; ++d) { asm volatile("nop"); }
        
        memset(verify_buf, 0, write_size);
        if (hx_lib_spi_eeprom_word_read(spi_id,
                                        flash_address,
                                        (uint32_t*)verify_buf,
                                        write_size) != 0) {
            xprintf("Flash verify read failed at 0x%08lX\n", flash_address);
            xSemaphoreGive(xSPIMutex);
            f_close(&file);
            return -1;
        }
        if (memcmp(write_buf, verify_buf, write_size) != 0) {
            xprintf("Verify FAIL at 0x%08lX\n", flash_address);
            xSemaphoreGive(xSPIMutex);
            f_close(&file);
            return -1;
        }

        xSemaphoreGive(xSPIMutex);

        flash_address   += write_size;
        totalBytesRead  += bytesRead;
    }
    f_close(&file);
    
    if (totalBytesRead == fileSize) {
        xprintf("Model successfully written to physical 0x%08lX (%lu bytes)\n",
                (unsigned long)MODEL_FLASH_ADDR, (unsigned long)fileSize);
        
        // Persist the model number to flash so it survives reboots
        write_persisted_model_number(model_loaded);
        
        // Re-enable XIP mode after all writes complete
        if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) == pdTRUE) {
            if (hx_lib_spi_eeprom_enable_XIP(spi_id, true, FLASH_SINGLE, true) == 0) {
                xprintf("XIP mode re-enabled successfully\n");
            }
            xSemaphoreGive(xSPIMutex);
        }
        return 0;
    }
    xprintf("Incomplete write: %lu/%lu bytes\n",
            (unsigned long)totalBytesRead, (unsigned long)fileSize);
    return -1;
}

// Improved model validation with detailed debugging
bool is_model_in_flash(char* filename) {
    if (init_flash() != 0) {
        return false;
    }
    
    debug_flash_status();

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for is_model_in_flash\n");
        return false;
    }

    // Ensure XIP is disabled
    hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);

    // Read filename header (first 16 bytes) BEFORE model address
    uint32_t model_xip_addr = 0x3A200000;
    uint32_t header_xip_addr = model_xip_addr - 16;
    uint32_t fname_addr = virt_to_phys(header_xip_addr);
    
    union {
        uint8_t bytes[16];
        uint32_t words[4];
    } fname_header;
    
    memset(fname_header.bytes, 0, sizeof(fname_header.bytes));
    if (hx_lib_spi_eeprom_word_read(spi_id, fname_addr, fname_header.words, 16) != 0) {
        xprintf("Failed to read filename header from flash\n");
        xSemaphoreGive(xSPIMutex);
        return false;
    }
    
    xprintf("Flash filename header: ");
    for (int i = 0; i < 16; i++) {
        xprintf("%02X ", fname_header.bytes[i]);
    }
    xprintf("\n");
    
    xSemaphoreGive(xSPIMutex);

    // Compare filename
    char fname_param[16] = {0};
    strncpy(fname_param, filename, sizeof(fname_param) - 1);
    xprintf("filename parameter: %s\n", fname_param);
    
    if (memcmp(fname_header.bytes, fname_param, 16) == 0) {
        xprintf("Model filename in flash matches parameter\n");
        return true;
    } else {
        xprintf("Model filename in flash does NOT match parameter\n");
        return false;
    }
}

// Modified load function - test BOTH SPI and XIP access
static const tflite::Model *load_model_from_flash(void)
{
    if (init_flash() != 0) {
        xprintf("Flash init failed\n");
        return nullptr;
    }

    const uint32_t phys = MODEL_FLASH_ADDR;     // 0x00200000
    const uint32_t virt = phys_to_virt(phys);   // 0x3A200000

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for load_model_from_flash\n");
        return nullptr;
    }

    // Disable XIP for SPI read
    hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);

    // Step 1: Read via SPI to confirm data exists
    uint8_t spi_hdr[32] __attribute__((aligned(4))) = {0};
    if (hx_lib_spi_eeprom_word_read(spi_id, phys, (uint32_t*)spi_hdr, sizeof(spi_hdr)) != 0) {
        xprintf("Failed to read SPI header\n");
        xSemaphoreGive(xSPIMutex);
        return nullptr;
    }

    xprintf("SPI@: ");
    for (size_t i = 0; i < sizeof(spi_hdr); i++) xprintf("%02X ", spi_hdr[i]);
    xprintf("\n");

    // Check if we have valid TFLite header via SPI
    if (spi_hdr[4] != 0x54 || spi_hdr[5] != 0x46 || 
        spi_hdr[6] != 0x4C || spi_hdr[7] != 0x33) {
        xprintf("No valid TFLite model in flash (SPI check)\n");
        xSemaphoreGive(xSPIMutex);
        return nullptr;
    }

    // Step 2: Enable XIP mode for memory-mapped access
    if (hx_lib_spi_eeprom_enable_XIP(spi_id, true, FLASH_SINGLE, true) != 0) {
        xprintf("Failed to enable XIP mode\n");
        xSemaphoreGive(xSPIMutex);
        return nullptr;
    }

    xSemaphoreGive(xSPIMutex);

    // Step 3: Verify XIP mapping works
    volatile uint8_t *xip_hdr = (volatile uint8_t*)virt;
    xprintf("XIP@0x%08lX: ", virt);
    for (int i = 0; i < 32; i++) xprintf("%02X ", xip_hdr[i]);
    xprintf("\n");

    // Step 4: Check if XIP matches SPI data
    bool xip_matches = (memcmp((const void*)spi_hdr, (const void*)xip_hdr, 32) == 0);
    
    if (xip_matches) {
        xprintf("XIP mapping works! Using virtual address\n");
        return tflite::GetModel((const void *)virt);
    } else {
        xprintf("XIP mapping mismatch!\n");
        return nullptr;
    }
}

/**
 * This runs the neural network processing.
 *
 * The function gets the address and dimesnions of the image from
 * app_get_raw_addr(), app_get_raw_width(), app_get_raw_height()
 *
 * It rescales the image to INPUT_SIZE_X, INPUT_SIZE_Y
 * then runs the NN.
 *
 * I have modified the code so it returns the result of the calculation
 *
 * @param outCategories = pointer to an array containing the processing results
 * @param categoriesCount = size of the array
 * @return error code
 */
TfLiteStatus cv_run(int8_t * outCategories, uint16_t categoriesCount) {

	// give image to input tensor
	/*
	void img_rescale(
			const uint8_t*in_image,
			const int32_t width,
			const int32_t height,
			const int32_t nwidth,		96
			const int32_t nheight,		96
			int8_t*out_image,
			const int32_t nxfactor,
			const int32_t nyfactor)

	 */
	img_rescale((uint8_t *)app_get_raw_addr(),
				app_get_raw_width(),
				app_get_raw_height(),
				INPUT_SIZE_X,
				INPUT_SIZE_Y,
				input->data.int8,
				SC(app_get_raw_width(), INPUT_SIZE_X),
				SC(app_get_raw_height(), INPUT_SIZE_Y));
    xprintf("Image rescaled and loaded into input tensor.\n");
	TfLiteStatus invoke_status = int_ptr->Invoke();
    xprintf("Model invoked.\n");
	if (invoke_status != kTfLiteOk) {
		xprintf("	TensorLite invoke fail\n");
		return invoke_status;
	}

    //print generic results
    // For int8 output tensor
    int8_t* results = output->data.int8;
    int classes = (output->dims->size >= 2) ? output->dims->data[1] : output->dims->data[0];
    for (int i = 0; i < classes; ++i) {
        const char* label = (g_labels_loaded && i < g_label_count) ? g_labels[i] : nullptr;
        if (results[i] > 0) {
            XP_LT_GREEN;
            if (label) {
                xprintf("POSITIVE: %s (%d)\n", label, results[i]);
            } else {
                xprintf("POSITIVE: class %d (%d)\n", i, results[i]);
            }
        } else {
            XP_LT_RED;
            if (label) {
                xprintf("NEGATIVE: %s (%d)\n", label, results[i]);
            } else {
                xprintf("NEGATIVE: class %d (%d)\n", i, results[i]);
            }
        }
    }
    XP_WHITE;
    
#if ORIGINAL
	// retrieve output data
	int8_t model_score = output->data.int8[1];
	// CGP not used int8_t no_model_score = output->data.int8[0];

	// CGP add some colour to highlight this message
	if (model_score > 0) {
		XP_LT_GREEN;
		xprintf("TARGET ANIMAL DETECTED!\n\n");
	}
	else {
		XP_LT_RED;
		xprintf("No target animal detected.\n\n");
	}
	XP_WHITE;

	xprintf("model_score: %d\n", model_score);

	// error_reporter not declared...
	//	error_reporter->Report(
	//		   "   model score: %d, no model score: %d\n", model_score, no_model_score);
#else
	if (categoriesCount != 2) {
		return kTfLiteError;	// error
	}

	for (uint8_t i=0; i < categoriesCount; i++) {
		outCategories[i] = output->data.int8[i];
	}
#endif

	return invoke_status;
}


// Robust deinit: safely release interpreter and tensor resources before model reloads
int cv_deinit()
{
    // Disable interrupts or enter critical section if needed (to prevent concurrent access)
    taskENTER_CRITICAL();

    // Free the TFLite interpreter if it exists
    if (int_ptr != nullptr) {
        delete int_ptr;
        int_ptr = nullptr;
    }
    
    // Free the op_resolver if it exists
    if (op_resolver_ptr != nullptr) {
        delete op_resolver_ptr;
        op_resolver_ptr = nullptr;
    }
    
    // Reset tensor pointers
    input = nullptr;
    output = nullptr;
    
    // Reset model loaded flag
    model_loaded_from_sd = false;
    
    // NOTE: We do NOT reset flash_initialized here!
    // The flash hardware remains initialized, we just need to manage XIP mode properly

    // Leave critical section
    taskEXIT_CRITICAL();
    return 0;
}

/* ------------debuggers------------- */

void debug_sd_model_integrity(const char* filename) {
    xprintf("=== SD MODEL INTEGRITY CHECK ===\n");
    
    FIL file;
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK) {
        xprintf("FAIL: Cannot open SD file %s (error: %d)\n", filename, res);
        return;
    }
    
    DWORD fileSize = f_size(&file);
    xprintf("SD Model size: %lu bytes\n", fileSize);

    // Read and print first 16 bytes
    uint8_t header[16];
    UINT bytesRead;
    res = f_read(&file, header, sizeof(header), &bytesRead);
    if (res == FR_OK && bytesRead == sizeof(header)) {
        xprintf("SD TFLite header (first 16 bytes): ");
        for (int i = 0; i < 16; ++i) xprintf("%02X ", header[i]);
        xprintf("\n");
        // Check for 'T','F','L','3' at header[4..7]
        if (header[4] == 'T' && header[5] == 'F' && header[6] == 'L' && header[7] == '3') {
            xprintf("Header magic OK: TFL3\n");
        } else {
            xprintf("Header magic NOT OK: expected TFL3 at bytes 4-7\n");
        }
    } else {
        xprintf("Failed to read SD header for integrity check\n");
    }
    f_close(&file);
}

void debug_flash_status() {
    xprintf("=== FLASH DEBUG STATUS ===\n");
    xprintf("Flash initialized: %s\n", flash_initialized ? "YES" : "NO");
    xprintf("SPI ID: %d\n", (int)spi_id);
    
    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for debug_flash_status\n");
        return;
    }

    // CRITICAL: Disable XIP before reading ID
    hx_lib_spi_eeprom_enable_XIP(spi_id, false, FLASH_SINGLE, false);
    
    uint8_t id_info = 0;
    int result = hx_lib_spi_eeprom_read_ID(spi_id, &id_info);
    xprintf("Flash ID read result: %d, ID: 0x%02X\n", result, id_info);
    
    xSemaphoreGive(xSPIMutex);
}

void test_basic_spi_operations() {
    xprintf("=== TESTING BASIC SPI OPS ===\n");

    uint32_t test_addr = (MODEL_FLASH_ADDR + 4*FLASH_SECTOR_SIZE) & ~(FLASH_SECTOR_SIZE - 1);

    int er = hx_lib_spi_eeprom_erase_sector(spi_id, test_addr, FLASH_SECTOR);
    xprintf("Erase result: %d at 0x%08lX\n", er, test_addr);

    uint32_t test_data[4] = {0xDEADBEEF, 0x12345678, 0xABCDEF00, 0x11223344};

    // Test writing one word at a time
    xprintf("Writing one word at a time:\n");
    for (int i = 0; i < 4; i++) {
        int wr = hx_lib_spi_eeprom_word_write(spi_id, test_addr + (i * 4), &test_data[i], 1);
        xprintf("  Write word %d (0x%08lX) to 0x%08lX: result %d\n", 
                i, test_data[i], test_addr + (i * 4), wr);
    }

    // Test reading one word at a time
    xprintf("Reading one word at a time:\n");
    for (int i = 0; i < 4; i++) {
        uint32_t read_data = 0;
        int rr = hx_lib_spi_eeprom_word_read(spi_id, test_addr + (i * 4), &read_data, 1);
        xprintf("  Read word %d from 0x%08lX: result %d, data 0x%08lX %s\n", 
                i, test_addr + (i * 4), rr, read_data,
                (read_data == test_data[i]) ? "OK" : "FAIL");
    }
}

void compare_sd_spi_xip_chunked(const char *filename)
{
    FIL file;
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK) {
        xprintf("Failed to open model file %s (error: %d)\n", filename, res);
        return;
    }

    DWORD fileSize = f_size(&file);
    xprintf("Model file size: %lu bytes\n", fileSize);

    UINT bytesRead = 0;
    uint8_t sd_buf[VERIFY_CHUNK_SIZE] = {0};
    uint8_t spi_buf[VERIFY_CHUNK_SIZE] __attribute__((aligned(4))) = {0};
    uint8_t xip_buf[VERIFY_CHUNK_SIZE] = {0};

    DWORD offset = 0;
    int any_mismatch = 0;
    while (offset < fileSize) {
        UINT to_read = (fileSize - offset > VERIFY_CHUNK_SIZE) ? VERIFY_CHUNK_SIZE : (fileSize - offset);
        // Read SD
        f_lseek(&file, offset);
        res = f_read(&file, sd_buf, to_read, &bytesRead);
        if (res != FR_OK || bytesRead != to_read) {
            xprintf("Failed to read model file at offset %lu\n", offset);
            break;
        }
        // // Read SPI
        if (hx_lib_spi_eeprom_word_read(spi_id, MODEL_FLASH_ADDR + offset, (uint32_t*)spi_buf, to_read) != 0) {
        // if (hx_lib_spi_eeprom_word_read(spi_id, MODEL_FLASH_ADDR + offset, (uint32_t*)spi_buf, (to_read + 3) / 4) != 0) {
            xprintf("Failed to read SPI at offset %lu\n", offset);
            break;
        }
        // Read XIP
        uint32_t virt_address = phys_to_virt(MODEL_FLASH_ADDR + offset);
        memcpy(xip_buf, (void*)virt_address, to_read);

        // Print mismatches for this chunk
        for (UINT i = 0; i < to_read; i++) {
            if (sd_buf[i] != spi_buf[i] || sd_buf[i] != xip_buf[i] || spi_buf[i] != xip_buf[i]) {
                xprintf("Mismatch at offset %lu (+%u): SD=0x%02X SPI=0x%02X XIP=0x%02X\n",
                        offset, i, sd_buf[i], spi_buf[i], xip_buf[i]);
                any_mismatch = 1;
            }
        }
        offset += to_read;
    }
    if (!any_mismatch) {
        xprintf("Model in flash (SPI/XIP) matches SD file!\n");
    }
    f_close(&file);
}