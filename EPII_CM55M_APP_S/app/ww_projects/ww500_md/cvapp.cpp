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
#include "hx_drv_spi.h"
#include "spi_eeprom_comm.h"
#include "task.h"

#include "WE2_core.h"
#include "WE2_device.h"

#include "ethosu_driver.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"

#include "xprintf.h"

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

#define TENSOR_ARENA_BUFSIZE (125 * 1024)
__attribute__((section(".bss.NoInit"))) uint8_t tensor_arena_buf[TENSOR_ARENA_BUFSIZE] __ALIGNED(32);

using namespace std;

int load_model_cli_command(int model_selection);
bool is_model_in_flash(char* filename);
int load_model_from_sd_to_flash(char* filename);
static const tflite::Model* load_model_from_flash(void);
int erase_model_flash_area(uint32_t model_size);
void cleanup_model_buffer(void);

// SPI EEPROM configuration
static USE_DW_SPI_MST_E spi_id = USE_DW_SPI_MST_Q;
static bool flash_initialized = false;

// Global pointer to hold dynamically allocated model
static uint8_t* global_model_buffer = nullptr;

// Add a flag to track if we've already loaded from SD card
static bool model_loaded_from_sd = false;
int model_number = 2;

namespace
{

	constexpr int tensor_arena_size = TENSOR_ARENA_BUFSIZE;
	uint32_t tensor_arena = (uint32_t)tensor_arena_buf;

	struct ethosu_driver ethosu_drv; /* Default Ethos-U device driver */
	tflite::MicroInterpreter *int_ptr = nullptr;
	TfLiteTensor *input, *output;
};

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

int cv_init(bool security_enable, bool privilege_enable) {
    
    if (_arm_npu_init(security_enable, privilege_enable) != 0) {
        return -1;
    }
    
    static const tflite::Model *model = nullptr;
    
#if (MODEL_LOAD_MODE == MODEL_FROM_C_FILE)
    model = tflite::GetModel((const void *)g_person_detect_model_data_vela);
    
#elif (MODEL_LOAD_MODE == MODEL_FROM_FLASH)
    model = load_model_from_flash();
    
#elif (MODEL_LOAD_MODE == MODEL_FROM_SD_CARD)
    // Check if we need to load from SD card
    if (!model_loaded_from_sd) {
        char filename[1];
        xprintf("First boot with SD card mode\n");
        
        // Check if model exists in flash
        if (!is_model_in_flash(filename)) {
			xprintf("No model in flash, loading from SD card...\n");
			if (load_model_from_sd_to_flash(filename) == 0) {
				model_loaded_from_sd = true;
				xprintf("Model loaded from SD card, rebooting to reinitialize...\n");
				// Give time for xprintf to complete
				for (volatile int i = 0; i < 1000000; i++);
				NVIC_SystemReset(); // Reboot to reinitialize with new model
			} else {
				xprintf("Failed to load model from SD card\n");
				return -1;
			}
        } else {
            xprintf("Model already exists in flash, skipping SD load\n");
            model_loaded_from_sd = true;
        }
    }
    // Load the model from flash
    model = load_model_from_flash();
#endif
    
    if (!model) {
        xprintf("Failed to load model\n");
        return -1;
    }
    
    xprintf("Model loaded successfully\n");

	if (model->version() != TFLITE_SCHEMA_VERSION) {
		xprintf("[ERROR] Model's schema version %d is not equal to supported version %d\n",
				model->version(), TFLITE_SCHEMA_VERSION);
		return -1;
	} else {
		xprintf("Model schema version: %d\n", model->version());
		xprintf("Input: %d x %d NN: %d x %d\n",
				app_get_raw_width(), app_get_raw_height(), INPUT_SIZE_X, INPUT_SIZE_X);
	}

	static tflite::MicroErrorReporter micro_error_reporter;
	static tflite::MicroMutableOpResolver<1> op_resolver;

	if (kTfLiteOk != op_resolver.AddEthosU()) {
		xprintf("Failed to add Arm NPU support to op resolver.\n");
		return -1;
	}
    
	static tflite::MicroInterpreter static_interpreter(
		model,
		op_resolver,
		(uint8_t *)tensor_arena,
		tensor_arena_size,
		&micro_error_reporter);
        
	if (static_interpreter.AllocateTensors() != kTfLiteOk) {
		xprintf("Failed to allocate tensors\n");
		return -1;
	}
	int_ptr = &static_interpreter;
	input = static_interpreter.input(0);
	output = static_interpreter.output(0);
	return 0;
}

// Initialize the flash memory (SPI EEPROM)
int init_flash(void) {
    if (!flash_initialized) {
        xprintf("Init EEPROM...\r\n");

        if (hx_lib_spi_eeprom_open(spi_id) != 0) {
            xprintf("Failed to open SPI EEPROM\n");
            return -1;
        }

        uint8_t id_info = 0;
        if (hx_lib_spi_eeprom_read_ID(spi_id, &id_info) != 0) {
            xprintf("Failed to read flash ID\n");
            return -1;
        }
        xprintf("Flash ID: 0x%02X\n", id_info);
        flash_initialized = true;
    }
    return 0;
}

// Calculate number of sectors needed for given size
uint32_t calculate_sectors_needed(uint32_t data_size) {
    return ((data_size + sizeof(uint32_t) + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE);
}

// Improved model validation with detailed debugging
bool is_model_in_flash(char* filename) {
    if (init_flash() != 0) {
        return false;
    }

    uint8_t header[16] = {0};
    if (hx_lib_spi_eeprom_word_read(spi_id, MODEL_FLASH_ADDR, (uint32_t*)header, sizeof(header)) != 0) {
        xprintf("Failed to read model header from flash\n");
        return false;
    }

    xprintf("Flash header bytes: ");
    for (int i = 0; i < 16; i++) {
        xprintf("%02X ", header[i]);
    }
    xprintf("\n");

    // Check for "TFL3"
    xprintf("Checking bytes 4-7: %02X %02X %02X %02X (should be 54 46 4C 33)\n",
           header[4], header[5], header[6], header[7]);

    return (header[4] == 0x54 && header[5] == 0x46 &&
            header[6] == 0x4C && header[7] == 0x33);
}

// Erase flash sectors for model storage
int erase_model_flash_area(uint32_t model_size) {
    cleanup_model_buffer();
    if (init_flash() != 0) {
        return -1;
    }

    uint32_t total_size = model_size; 
    uint32_t sectors_needed = calculate_sectors_needed(total_size);

    xprintf("Erasing %lu sectors for %lu bytes of model...\n", sectors_needed, total_size);

    for (uint32_t i = 0; i < sectors_needed; i++) {
        uint32_t sector_addr = MODEL_FLASH_ADDR + (i * FLASH_SECTOR_SIZE);
        if (hx_lib_spi_eeprom_erase_sector(spi_id, sector_addr, FLASH_SECTOR) != 0) {
            xprintf("Failed to erase sector at address 0x%08lX\n", sector_addr);
            return -1;
        }
    }

    return 0;
}

// Load model from SD to flash at MODEL_FLASH_ADDR
int load_model_from_sd_to_flash(char *filename) {
    FRESULT res;
    FIL file;
    UINT bytesRead;
    uint8_t buffer[512] __attribute__((aligned(4)));

    if (init_flash() != 0) {
        return -1;
    }

    res = f_open(&file, filename, FA_READ);
    if (res != FR_OK) {
        xprintf("Failed to open model file %s (error: %d)\n", filename, res);
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

    uint32_t flash_address = MODEL_FLASH_ADDR;
    uint32_t virt_address  = phys_to_virt(MODEL_FLASH_ADDR);

    uint32_t totalBytesRead = 0;
    while (totalBytesRead < fileSize) {
        memset(buffer, 0x00, sizeof(buffer));

        res = f_read(&file, buffer, sizeof(buffer), &bytesRead);
        if (res != FR_OK || bytesRead == 0) {
            break;
        }

        uint32_t write_size = ((bytesRead + 3) / 4) * 4;

        xprintf("Writing %lu bytes (aligned to %lu) at flash addr 0x%08lX\n",
               bytesRead, write_size, flash_address);

        if (hx_lib_spi_eeprom_word_write(spi_id, flash_address, (uint32_t*)buffer, write_size) != 0) {
            xprintf("Flash write failed at 0x%08lX\n", flash_address);
            f_close(&file);
            return -1;
        }

        // Verify via virtual mapping
        volatile uint32_t *verify_virt = (volatile uint32_t*)(virt_address + totalBytesRead);
        volatile uint32_t *buffer_check = (volatile uint32_t*)buffer;
        if (verify_virt[0] != buffer_check[0]) {
            xprintf("WARNING: Verification mismatch at 0x%08lX (expected 0x%08lX, got 0x%08lX)\n",
                   virt_address + totalBytesRead, buffer_check[0], verify_virt[0]);
        }

        flash_address += write_size;
        totalBytesRead += bytesRead;
    }

    f_close(&file);

    if (totalBytesRead == fileSize) {
        xprintf("Model successfully written to physical 0x%08lX (virtual 0x%08lX)\n",
               MODEL_FLASH_ADDR, virt_address);

        volatile uint8_t *final_check = (volatile uint8_t*)virt_address;
        xprintf("Final check at 0x%08lX: ", virt_address);
        for (int i = 0; i < 16; i++) xprintf("%02X ", final_check[i]);
        xprintf("\n");

        return 0;
    }

    xprintf("Incomplete write: %lu/%lu bytes\n", totalBytesRead, fileSize);
    return -1;
}

// CLAUDYCLAUD
// Modified load function - test BOTH SPI and XIP access
static const tflite::Model *load_model_from_flash(void)
{
    if (init_flash() != 0) {
        xprintf("Flash init failed\n");
        return nullptr;
    }

    const uint32_t phys = MODEL_FLASH_ADDR;     // 0x00200000
    const uint32_t virt = phys_to_virt(phys);   // 0x3A200000

    // Step 1: Read via SPI to confirm data exists
    uint8_t spi_hdr[16] = {0};
    if (hx_lib_spi_eeprom_word_read(spi_id, phys, (uint32_t*)spi_hdr, sizeof(spi_hdr)) != 0) {
        xprintf("SPI read failed at 0x%08lX\n", phys);
        return nullptr;
    }

    xprintf("SPI@0x%08lX: ", phys);
    for (int i = 0; i < 16; i++) xprintf("%02X ", spi_hdr[i]);
    xprintf("\n");

    // Check if we have valid TFLite header via SPI
    if (spi_hdr[4] != 0x54 || spi_hdr[5] != 0x46 || 
        spi_hdr[6] != 0x4C || spi_hdr[7] != 0x33) {
        xprintf("No valid TFLite model in flash (SPI check)\n");
        return nullptr;
    }

    // Step 2: Try XIP access (may not work without proper config)
    volatile uint8_t *xip_hdr = (volatile uint8_t*)virt;
    xprintf("XIP@0x%08lX: ", virt);
    for (int i = 0; i < 16; i++) xprintf("%02X ", xip_hdr[i]);
    xprintf("\n");

    // Step 3: Check if XIP matches SPI data
    bool xip_matches = (memcmp((const void*)spi_hdr, (const void*)xip_hdr, 16) == 0);
    
    if (xip_matches) {
        xprintf("XIP mapping works! Using virtual address\n");
        static const tflite::Model* model = tflite::GetModel((const void *)virt);
        return model;
    } else {
        xprintf("XIP mapping broken - trying to enable XIP mode...\n");
        
        // Try to enable basic XIP mode
        if (hx_lib_spi_eeprom_enable_XIP(spi_id, true, FLASH_SINGLE, true) == 0) {
            xprintf("XIP enabled, checking again...\n");
            
            // Re-check XIP access
            xprintf("XIP retry@0x%08lX: ", virt);
            for (int i = 0; i < 16; i++) xprintf("%02X ", xip_hdr[i]);
            xprintf("\n");
            
            if (memcmp((const void*)spi_hdr, (const void*)xip_hdr, 16) == 0) {
                xprintf("XIP now works! Loading model\n");
                static const tflite::Model* model = tflite::GetModel((const void *)virt);
                return model;
            }
        }
        
        xprintf("XIP still not working - this hardware may not support memory-mapped flash\n");
        return nullptr;
    }
}

// Clean up function (call when switching models)
void cleanup_model_buffer(void) {
    if (global_model_buffer) {
        vPortFree(global_model_buffer); 
        global_model_buffer = nullptr;
        model_loaded_from_sd = false;
        xprintf("Model buffer cleaned up\n");
    }
}

int load_model_cli_command(int model_selection){
    model_number = model_selection;
    static const tflite::Model* model;
    char filename[16];
    snprintf(filename, sizeof(filename), "MOD0000%01d.tfl", model_number);
    xprintf("filename: %s\n", filename);

    if (model_number > 0){
        // Check if model exists in flash
        if (!is_model_in_flash(filename)) {
            xprintf("No model in flash, loading from SD card...\n");
            if (load_model_from_sd_to_flash(filename) == 0) {
                model_loaded_from_sd = true;
                xprintf("Model loaded from SD card, rebooting to reinitialize...\n");
                // Give time for xprintf to complete
                for (volatile int i = 0; i < 1000000; i++);
                NVIC_SystemReset(); // Reboot to reinitialize with new model
            } else {
                xprintf("Failed to load model from SD card\n");
                return -1;
            }
        } else { 
            xprintf("Model already exists in flash, skipping SD load\n");
            model_loaded_from_sd = true;
        }
    }
    // Load the model from flash
    model = load_model_from_flash();
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

	TfLiteStatus invoke_status = int_ptr->Invoke();

	if (invoke_status != kTfLiteOk) {
		xprintf("	TensorLite invoke fail\n");
		return invoke_status;
	}
//	else {
//		xprintf("	TensorLite invoke pass\n");
//	}
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

int cv_deinit()
{
	// TODO: add more deinit items here if need.
	return 0;
}
