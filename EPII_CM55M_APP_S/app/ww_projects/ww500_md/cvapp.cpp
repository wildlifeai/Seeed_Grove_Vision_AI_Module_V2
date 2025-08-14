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
// #include "../../scenario_app/edge_impulse_firmware/firmware-sdk/ei_device_info_lib.h"

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


static const tflite::Model *GetModelFromSdCard(void);
uint32_t read_data(uint8_t *data, uint32_t address, uint32_t num_bytes);
uint32_t erase_data(uint32_t address, uint32_t num_bytes);
uint32_t write_data_local(const uint8_t *data, uint32_t address, uint32_t num_bytes);

bool is_model_in_flash(void);
int load_model_from_sd_to_flash(void);
const tflite::Model* load_model_from_flash(void);

// SPI EEPROM configuration
static USE_DW_SPI_MST_E spi_id = USE_DW_SPI_MST_Q;
static bool flash_initialized = false;

uint32_t memory_size;
const int BLOCK_SIZE = 1024;
const int MEMORY_BLOCKS = 50;
static uint8_t ram_memory[MEMORY_BLOCKS * BLOCK_SIZE];
// Add a flag to track if we've already loaded from SD card
static bool model_loaded_from_sd = false;

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
    int ercode = 0;
    
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
        printf("First boot with SD card mode\n");
        
        // Check if model exists in flash
        if (!is_model_in_flash()) {
			printf("No model in flash, loading from SD card...\n");
			if (load_model_from_sd_to_flash() == 0) {
				model_loaded_from_sd = true;
				printf("Model loaded from SD card, rebooting to reinitialize...\n");
				// Give time for printf to complete
				for (volatile int i = 0; i < 1000000; i++);
				NVIC_SystemReset(); // Reboot to reinitialize with new model
			} else {
				printf("Failed to load model from SD card\n");
				return -1;
			}
        } else {
            printf("Model already exists in flash, skipping SD load\n");
            model_loaded_from_sd = true;
        }
    }
    
    // Load the model from flash
    model = load_model_from_flash();
#endif
    
    if (!model) {
        printf("Failed to load model\n");
        return -1;
    }
    
    printf("Model loaded successfully\n");

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

// Initialize flash if not already done
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
        
        flash_initialized = true;
    }
    return 0;
}

// Calculate number of sectors needed for given size
uint32_t calculate_sectors_needed(uint32_t data_size) {
    return ((data_size + sizeof(uint32_t) + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE);
}

// Check if there's a valid model in flash memory
bool is_model_in_flash(void) {
    if (init_flash() != 0) {
        return false;
    }
    
    // Read model header (magic + model_number + size)
    uint32_t header[3] = {0}; // [magic, model_number, size]
    
    if (hx_lib_spi_eeprom_word_read(spi_id, MODEL_FLASH_ADDR, header, sizeof(header)) != 0) {
        printf("Failed to read model header from flash\n");
        return false;
    }
    
    uint32_t magic = header[0];
    uint32_t model_number = header[1];
    uint32_t modelSize = header[2];
    
    // Check magic number
    if (magic != MODEL_MAGIC_HEADER) {
        printf("No valid model found in flash (invalid magic: 0x%08lX)\n", magic);
        return false;
    }
    
    // Check model number matches
    if (model_number != MODEL_NUMBER) {
        printf("Model in flash has wrong number: %05lu (expected: %05d)\n", model_number, MODEL_NUMBER);
        return false;
    }
    
    // Basic size validation
    if (modelSize > 0 && modelSize != 0xFFFFFFFF && modelSize < FLASH_MODEL_AREA_SIZE) {
        printf("Found correct model in flash: MOD%05lu.tfl (%lu bytes)\n", model_number, modelSize);
        return true;
    }
    
    printf("Model in flash has invalid size: %lu bytes\n", modelSize);
    return false;
}

// Erase flash sectors for model storage
int erase_model_flash_area(uint32_t model_size) {
    if (init_flash() != 0) {
        return -1;
    }
    
    uint32_t total_size = sizeof(uint32_t) + model_size; // Size header + model data
    uint32_t sectors_needed = calculate_sectors_needed(total_size);
    
    xprintf("Erasing %lu sectors for %lu bytes of data...\n", sectors_needed, total_size);
    
    for (uint32_t i = 0; i < sectors_needed; i++) {
        uint32_t sector_addr = MODEL_FLASH_ADDR + (i * FLASH_SECTOR_SIZE);
        
        if (hx_lib_spi_eeprom_erase_sector(spi_id, sector_addr, FLASH_SECTOR) != 0) {
            xprintf("Failed to erase sector at address 0x%08lX\n", sector_addr);
            return -1;
        }
    }
    
    return 0;
}

// Load model from SD card to flash memory (returns success/failure)
int load_model_from_sd_to_flash(void) {
    FRESULT res;
    FIL file;
    UINT bytesRead;
    uint8_t buffer[512];
    char filename[16];
    
    // Generate filename based on MODEL_NUMBER: MOD00001.tfl
    snprintf(filename, sizeof(filename), "MOD%05d.tfl", MODEL_NUMBER);
    
    printf("Loading model %s from SD card to flash...\n", filename);
    
    if (init_flash() != 0) {
        return -1;
    }
    
    res = f_open(&file, filename, FA_READ);
    if (res != FR_OK) {
        printf("Failed to open model file %s (error: %d)\n", filename, res);
        return -1;
    }
    
    printf("Successfully opened model file %s\n", filename);
    
    // Get file size
    DWORD fileSize = f_size(&file);
    if (fileSize == 0) {
        printf("Model file is empty\n");
        f_close(&file);
        return -1;
    }
    
    printf("Model file size: %lu bytes\n", fileSize);
    
    // Erase flash sectors before writing (account for header + model data)
    uint32_t total_data_size = sizeof(uint32_t) * 3 + fileSize; // header + model
    if (erase_model_flash_area(total_data_size) != 0) {
        printf("Failed to erase flash for model\n");
        f_close(&file);
        return -1;
    }
    
    // Write the model header: [magic, model_number, size]
    uint32_t header[3] = {MODEL_MAGIC_HEADER, MODEL_NUMBER, (uint32_t)fileSize};
    if (hx_lib_spi_eeprom_word_write(spi_id, MODEL_FLASH_ADDR, header, sizeof(header)) != 0) {
        printf("Failed to write model header\n");
        f_close(&file);
        return -1;
    }
    
    printf("Written model header: magic=0x%08lX, number=%05lu, size=%lu bytes\n", 
           header[0], header[1], header[2]);
    
    // Write the model data after the header
    uint32_t flash_address = MODEL_FLASH_ADDR + sizeof(header);
    uint32_t totalBytesWritten = 0;
    
    while (totalBytesWritten < fileSize) {
        res = f_read(&file, buffer, sizeof(buffer), &bytesRead);
        if (res != FR_OK || bytesRead == 0) {
            if (res != FR_OK) {
                printf("File read error: %d\n", res);
            }
            break;
        }
        
        // Write to flash - ensure we write in word-aligned chunks
        uint32_t write_size = ((bytesRead + 3) / 4) * 4; // Round up to next 4-byte boundary
        
        // Pad buffer if necessary
        if (write_size > bytesRead) {
            memset(&buffer[bytesRead], 0xFF, write_size - bytesRead);
        }
        
        if (hx_lib_spi_eeprom_word_write(spi_id, flash_address, (uint32_t*)buffer, write_size) != 0) {
            printf("Flash write failed at address 0x%08lX\n", flash_address);
            f_close(&file);
            return -1;
        }
        
        flash_address += write_size;
        totalBytesWritten += bytesRead; // Only count actual file bytes, not padding
    }
    
    f_close(&file);
    
    if (totalBytesWritten == fileSize) {
        printf("Model successfully written to flash at 0x%08lX (%lu bytes)\n", MODEL_FLASH_ADDR, fileSize);
        return 0;
    } else {
        printf("Incomplete write: %lu/%lu bytes\n", totalBytesWritten, fileSize);
        return -1;
    }
}


// Load model from flash memory and return TFLite model pointer
const tflite::Model* load_model_from_flash(void) {
    if (init_flash() != 0) {
        return nullptr;
    }
    
    // Read model header (magic + model_number + size)
    uint32_t header[3] = {0}; // [magic, model_number, size]
    
    if (hx_lib_spi_eeprom_word_read(spi_id, MODEL_FLASH_ADDR, header, sizeof(header)) != 0) {
        printf("Failed to read model header from flash\n");
        return nullptr;
    }
    
    uint32_t magic = header[0];
    uint32_t model_number = header[1];
    uint32_t modelSize = header[2];
    
    // Validate header
    if (magic != MODEL_MAGIC_HEADER || model_number != MODEL_NUMBER) {
        printf("Invalid model in flash (magic: 0x%08lX, number: %05lu)\n", magic, model_number);
        return nullptr;
    }
    
    if (modelSize == 0 || modelSize == 0xFFFFFFFF) {
        printf("No valid model found in flash\n");
        return nullptr;
    }
    
    printf("Loading model MOD%05lu.tfl from flash (%lu bytes) using XIP mode\n", model_number, modelSize);
    
    // Calculate physical address where model data starts (after header)
    uint32_t model_data_phys_addr = MODEL_FLASH_ADDR + sizeof(header);
    
    // Convert physical address to virtual address for XIP access
    uint32_t model_data_virt_addr = FLASH_PHYS_TO_VIRT(model_data_phys_addr);
    
    printf("model_data_phys_addr: 0x%08lX\n", model_data_phys_addr);
    printf("model_data_virt_addr: 0x%08lX\n", model_data_virt_addr);
    
    // Cast virtual address to void pointer for XIP access
    const void* model_data = (const void*)model_data_virt_addr;
    
    // Create TFLite model directly from flash memory (XIP)
    const tflite::Model* model = tflite::GetModel(model_data);
    if (!model) {
        printf("Failed to parse TFLite model from flash XIP data\n");
        
        // Fallback: try reading into RAM if XIP doesn't work
        printf("Attempting fallback: loading model into RAM\n");
        
        // Allocate memory for model data
        static uint8_t* model_buffer = nullptr;
        if (model_buffer) {
            free(model_buffer);
        }
        
        // Allocate word-aligned buffer
        uint32_t aligned_size = ((modelSize + 3) / 4) * 4;
        model_buffer = (uint8_t*)malloc(aligned_size);
        if (!model_buffer) {
            printf("Failed to allocate memory for model (%lu bytes)\n", aligned_size);
            return nullptr;
        }
        
        // Read model data from flash into RAM using physical address
        if (hx_lib_spi_eeprom_word_read(spi_id, model_data_phys_addr, (uint32_t*)model_buffer, aligned_size) != 0) {
            printf("Failed to read model data from flash\n");
            free(model_buffer);
            model_buffer = nullptr;
            return nullptr;
        }
        
        // Try creating model from RAM buffer
        model = tflite::GetModel(model_buffer);
        if (!model) {
            printf("Failed to parse TFLite model from RAM buffer\n");
            free(model_buffer);
            model_buffer = nullptr;
            return nullptr;
        }
        
        printf("Model successfully loaded from flash via RAM fallback\n");
    } else {
        printf("Model successfully loaded from flash using XIP mode: MOD%05lu.tfl\n", model_number);
    }
    
    return model;
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
