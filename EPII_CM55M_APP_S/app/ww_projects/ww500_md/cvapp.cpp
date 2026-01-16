/*
 * cvapp.cpp
 *
 *  Created on: 2022/02/22
 *      Author: 902452
 */

#include <cstdio>
#include <cmath>
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
#include "tensorflow/lite/kernels/internal/reference/softmax.h"
#include "tensorflow/lite/kernels/internal/types.h"

#include "xprintf.h"
#include "ff.h"
#include "fatfs_task.h"
// POSIX string functions (strcasecmp)
#include <strings.h>

// Compare model in flash with file in chunks to avoid large allocations
#define VERIFY_CHUNK_SIZE 512

// Required for the app_get_xxx() functions
// #include "cisdp_cfg.h"
#include "cisdp_sensor.h"

#include "common_config.h"

#include "printf_x.h" // Print colours

#define LOCAL_FRAQ_BITS (8)
#define SC(A, B) ((A << 8) / B)

// TODO can we make these in the model file?
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

// Uncomment this to get information about the model:
#define PRINTMODELFINGERPRINT

extern "C" {
	// These are defined in the linker .ld file, and aligned on 32-byte boundaries
    extern uint8_t __tensor_arena_start__;
    extern uint8_t __tensor_arena_end__;
}

static uint8_t *tensor_arena_buf = &__tensor_arena_start__;
static size_t tensor_arena_size = (size_t)(&__tensor_arena_end__ - &__tensor_arena_start__);


// #define TENSOR_ARENA_BUFSIZE (125 * 1024)
// __attribute__(( section(".bss.NoInit"))) uint8_t tensor_arena_buf[TENSOR_ARENA_BUFSIZE] __ALIGNED(32);

using namespace std;
namespace {

    struct ethosu_driver ethosu_drv; /* Default Ethos-U device driver */
    static tflite::MicroInterpreter *int_ptr = nullptr;
    static tflite::MicroMutableOpResolver<1> *op_resolver_ptr = nullptr;
    TfLiteTensor *input, *output;

    // Labels loaded from SD (one per line) – used for printing class names
    static char g_labels[MAX_CLASSES][MAX_LABEL_LEN];
    static uint8_t g_label_count = 0;
    static bool g_labels_loaded = false;
};

// SPI EEPROM configuration
static bool flash_initialized = false;
static SemaphoreHandle_t xSPIMutex = NULL;

// Add a flag to track if we've already loaded from SD card
static bool model_loaded_from_sd = false;

// Globals to hold the current model identifiers
// TODO should these really be 32-bits?
static int g_project_id = 0;
static int g_deploy_version = 0;

// Global to store the last confidence data for EXIF
static ClassConfidenceData g_last_confidence_data = {0};

// ------------------- Declarations -------------------

// int load_model_cli_command(int model_selection);
bool is_model_in_flash(char *filename);
int load_model_from_sd_to_flash(char *filename);
static const tflite::Model *load_model_from_flash(void);
int erase_model_flash_area(uint32_t model_size);
void compare_sd_spi_xip_chunked(const char *filename);
void debug_sd_model_integrity(const char *filename);
void debug_flash_status(void);
void test_basic_spi_operations();
int cv_deinit(void);
int init_flash();
static inline uint32_t align_down(uint32_t addr, uint32_t align);
static inline uint32_t align_up(uint32_t size, uint32_t align);
int read_persisted_model_info(void);
int write_persisted_model_info(void);
static bool file_exists(const char *path);
static void load_labels_from_manifest(void);
void cv_get_model_info(int *project_id, int *deploy_version);

// --------------------- Utilities / Helpers ---------------------

// Round down address to sector boundary
static inline uint32_t align_down(uint32_t addr, uint32_t align)
{
    return addr & ~(align - 1);
}
// Round up size to align
static inline uint32_t align_up(uint32_t size, uint32_t align)
{
    return (size + align - 1) & ~(align - 1);
}

// Check if a file exists on SD
static bool file_exists(const char *path)
{
    FILINFO finfo;
    FRESULT res = f_stat(path, &finfo);
    return (res == FR_OK);
}

/**
 * Load labels from "/MANIFEST/LABELS.TXT"
 */
static void load_labels_from_manifest(void) {

    g_labels_loaded = false;
    g_label_count = 0;
    //const char *labels_path = "/MANIFEST/LABELS.TXT";

    // Look for "/MANIFEST/LABELS.TXT" - concatenation done by the compiler.
    const char *labels_path = CONFIG_DIR "/LABELS.TXT";

    if (file_exists(labels_path))  {
        uint8_t count = 0;
        if (fatfs_load_labels(labels_path, g_labels, &count, MAX_CLASSES, MAX_LABEL_LEN) == 0) {
            g_label_count = count;
            g_labels_loaded = (g_label_count > 0);
            xprintf("Loaded %d labels from %s\n", g_label_count, labels_path);
            return;
        }
        else  {
            xprintf("Labels load failed from %s\n", labels_path);
        }
    }
    xprintf("No labels found on SD\n");
}

int get_model_id() {
    return g_project_id;
}

// Public getter for other modules to know the current model
void cv_get_model_info(int *project_id, int *deploy_version) {
    *project_id = g_project_id;
    *deploy_version = g_deploy_version;
}

// Public setter for current model info
void cv_set_model_info(int project_id, int deploy_version) {
    // Only log when values actually change
    if (g_project_id != project_id || g_deploy_version != deploy_version)
    {
        xprintf("cv_set_model_info: Project %d -> %d, Version %d -> %d\n",
                g_project_id, project_id, g_deploy_version, deploy_version);
        g_project_id = project_id;
        g_deploy_version = deploy_version;
    }
}

// Public getter for confidence data (for EXIF)
bool cv_get_confidence_data(ClassConfidenceData *data) {
    if (data != nullptr) {
        memcpy(data, &g_last_confidence_data, sizeof(ClassConfidenceData));
        return true;
    }
    return false;
}

/**
 *  Read the persisted model info from flash
 *
 *  Flash layout:
 *  	project_id (4 bytes)
 *  	deploy_version (4 bytes)
 *  	filename (16 bytes)
 *  	model data...
 */
int read_persisted_model_info(void) {
	uint32_t model_info_xip_addr;
	uint32_t model_info_phys_addr;
    uint32_t stored_info[2] = {0}; // [0] = project_id, [1] = deploy_version

    if (init_flash() != 0)   {
        xprintf("Flash init failed, using default model info\n");
        return -1;
    }

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for read_persisted_model_info\n");
        return -1;
    }

    // Disable XIP before SPI read
    hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

    // Model info is stored 24 bytes before the model data
    // (4 bytes project_id + 4 bytes version + 16 bytes filename header)

//    uint32_t model_xip_addr = 0x3A200000;
//    uint32_t model_info_xip_addr = model_xip_addr - 24;
//    uint32_t model_info_phys_addr = virt_to_phys(model_info_xip_addr);

    // CGP - TODO -surely we should be placing model_info_xip_addr at the start of a EEPROM sector?
    // TODO - remove magic numbers
    model_info_xip_addr = MODEL_XIP_ADDR - 24;
    model_info_phys_addr = virt_to_phys(model_info_xip_addr);

    if (hx_lib_spi_eeprom_word_read(USE_DW_SPI_MST_Q, model_info_phys_addr, stored_info, 8) != 0)  {
        xprintf("Failed to read persisted model info, using defaults\n");
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    xSemaphoreGive(xSPIMutex);

    // Basic validation: check if values are not 0xFFFFFFFF (uninitialized)
    if ((stored_info[0] != 0xFFFFFFFF) && (stored_info[1] != 0xFFFFFFFF)) {
        g_project_id = stored_info[0];
        g_deploy_version = stored_info[1];
        xprintf("Read persisted model info from flash at 0x%08x: Project=%d, Version=%d\n",
        		model_info_phys_addr, g_project_id, g_deploy_version);
        return 0;
    }
    else  {
        xprintf("Invalid persisted model info (0x%08lX, 0x%08lX), using defaults\n", stored_info[0], stored_info[1]);
        return -1;
    }
}

// Write the model info to flash
int write_persisted_model_info()
{
    if (init_flash() != 0)
    {
        return -1;
    }

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)
    {
        xprintf("Failed to take SPI mutex for write_persisted_model_info\n");
        return -1;
    }

    // Disable XIP before write
    hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

    uint32_t model_xip_addr = MODEL_XIP_ADDR;
    uint32_t model_info_xip_addr = model_xip_addr - 24;
    uint32_t model_info_phys_addr = virt_to_phys(model_info_xip_addr);

    uint32_t info_to_write[2] = {(uint32_t)g_project_id, (uint32_t)g_deploy_version};
    if (hx_lib_spi_eeprom_word_write(USE_DW_SPI_MST_Q, model_info_phys_addr, info_to_write, 8) != 0)
    {
        xprintf("Failed to write persisted model info\n");
        xSemaphoreGive(xSPIMutex);
        return -1;
    }

    // Small delay for write to complete
    for (volatile int d = 0; d < 20000; ++d)
    {
        asm volatile("nop");
    }

    xSemaphoreGive(xSPIMutex);
    xprintf("Persisted model info to flash: Project=%d, Version=%d\n", g_project_id, g_deploy_version);
    return 0;
}


#ifdef PRINTMODELFINGERPRINT
#include "tensorflow/lite/schema/schema_generated.h"
//#include "tensorflow/lite/version.h"

// Forward declaration of your printf
extern "C" void xprintf(const char *fmt, ...);

static const char* TfLiteTypeName(TfLiteType t) {
    switch (t) {
    case kTfLiteFloat32: return "f32";
    case kTfLiteInt32:   return "i32";
    case kTfLiteUInt8:   return "u8";
    case kTfLiteInt8:    return "i8";
    case kTfLiteInt16:   return "i16";
    default:             return "?";
    }
}

static const char* BuiltinOpName(tflite::BuiltinOperator op) {
    switch (op) {
    case tflite::BuiltinOperator_CONV_2D:            return "CONV2D";
    case tflite::BuiltinOperator_DEPTHWISE_CONV_2D: return "DWCONV";
    case tflite::BuiltinOperator_FULLY_CONNECTED:   return "FC";
    case tflite::BuiltinOperator_ADD:               return "ADD";
    case tflite::BuiltinOperator_AVERAGE_POOL_2D:  return "AVGPOOL";
    case tflite::BuiltinOperator_MAX_POOL_2D:      return "MAXPOOL";
    case tflite::BuiltinOperator_SOFTMAX:           return "SOFTMAX";
    case tflite::BuiltinOperator_RESHAPE:           return "RESHAPE";
    default:                                        return "OTHER";
    }
}

// Enhanced Ethos-U55-aware fingerprint
void tflm_print_ethosu_fingerprint(const tflite::Model* model) {
    if (!model) {
        xprintf("TFLM: model = NULL\r\n");
        return;
    }

    xprintf("TFLM Ethos-U55 Model Fingerprint\r\n");
    xprintf("---------------------------------\r\n");
    xprintf("Schema version : %d (expected %d)\r\n",
            model->version(), TFLITE_SCHEMA_VERSION);

    const auto* subgraphs = model->subgraphs();
    xprintf("Subgraphs      : %d\r\n", subgraphs->size());

    if (subgraphs->size() == 0) return;

    const auto* g = subgraphs->Get(0);
    const auto* tensors = g->tensors();
    const auto* ops = g->operators();

    xprintf("Operators      : %d\r\n", ops->size());

    for (uint32_t i = 0; i < ops->size(); i++) {
        const auto* op = ops->Get(i);
        const auto* opcode = model->operator_codes()->Get(op->opcode_index());
        tflite::BuiltinOperator builtin = opcode->builtin_code();
        const char* op_name = BuiltinOpName(builtin);
        const char* custom_name = nullptr;

        // If CUSTOM operator, print the custom code string
        if (builtin == tflite::BuiltinOperator_CUSTOM && opcode->custom_code()) {
        	custom_name = opcode->custom_code()->c_str();
        	op_name = "CUSTOM";
        }

        // Count weight bytes for this operator
        size_t op_weight_bytes = 0;
        for (uint32_t j = 0; j < op->inputs()->size(); j++) {
            int t_idx = op->inputs()->Get(j);
            if (t_idx < 0 || t_idx >= static_cast<int>(tensors->size())) continue;
            const auto* t = tensors->Get(t_idx);
            int buf_idx = t->buffer();
            if (buf_idx < 0 || buf_idx >= static_cast<int>(model->buffers()->size())) continue;
            const auto* buf = model->buffers()->Get(buf_idx);
            if (buf->data()) op_weight_bytes += buf->data()->size();
        }

        // Print operator info
        xprintf("  Op %d: %s", i, op_name);
        if (custom_name) xprintf(" (%s)", custom_name);
        xprintf(", weight bytes: %u\r\n", (unsigned)op_weight_bytes);
    }

    // Input tensor
    if (g->inputs()->size() > 0) {
        const auto* t = tensors->Get(g->inputs()->Get(0));
        xprintf("Input tensor   : %s [",
                TfLiteTypeName(static_cast<TfLiteType>(t->type())));
        for (uint32_t i = 0; i < t->shape()->size(); i++) {
            xprintf("%d", t->shape()->Get(i));
            if (i != t->shape()->size() - 1) xprintf(",");
        }
        xprintf("]\r\n");
    }

    // Output tensor
    if (g->outputs()->size() > 0) {
        const auto* t = tensors->Get(g->outputs()->Get(0));
        xprintf("Output tensor  : %s [",
                TfLiteTypeName(static_cast<TfLiteType>(t->type())));
        for (uint32_t i = 0; i < t->shape()->size(); i++) {
            xprintf("%d", t->shape()->Get(i));
            if (i != t->shape()->size() - 1) xprintf(",");
        }
        xprintf("]\r\n");
    }

    // Total weight bytes (all buffers)
    size_t total_weight_bytes = 0;
    const auto* buffers = model->buffers();
    for (uint32_t i = 0; i < buffers->size(); i++) {
        const auto* b = buffers->Get(i);
        if (b->data()) total_weight_bytes += b->data()->size();
    }

    xprintf("Total weight bytes : %u\r\n", (unsigned)total_weight_bytes);
    xprintf("---------------------------------\r\n");
}

#endif	// PRINTMODELFINGERPRINT

/**
 * Bilinear interpolation.
 *
 * Image is squashed but not cropped.
 *
 * Array is converted
 * Many ML models (TFLite / Edge Impulse) expect zero-centered input\
 * So out_image_fix - 128  converts unsigned grayscale → signed
 */
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

static int _arm_npu_init(bool security_enable, bool privilege_enable) {
    int err = 0;

    /* Initialise the IRQ */
    _arm_npu_irq_init();

    /* Initialise Ethos-U55 device */
    const void *ethosu_base_address = (void *)(U55_BASE);

    if (0 != (err = ethosu_init(
                  &ethosu_drv,         /* Ethos-U driver device pointer */
                  ethosu_base_address, /* Ethos-U NPU's base address. */
                  NULL,                /* Pointer to fast mem area - NULL for U55. */
                  0,                   /* Fast mem region size. */
                  security_enable,     /* Security enable. */
                  privilege_enable)))
    { /* Privilege enable. */
        xprintf("failed to initalise Ethos-U device\n");
        return err;
    }

    xprintf("Ethos-U55 device initialised\n");

    return 0;
}

// Optional model number parameter, default to 1
/**
 *
 * 	@param security_enable
 *	@param privilege_enable
 *	@param project_id
 *	@param deploy_version
 *	@return 0 for OK
 */
int cv_init(bool security_enable, bool privilege_enable, int project_id, int deploy_version) {
	DIR dir;
	FILINFO fno;
	bool any_model_found = false;
	char filename[IMAGEFILENAMELEN];	// for 8.3 this is 13, including the training \0
	char manifest_path[10 + IMAGEFILENAMELEN];			// let's restrict this to "/12345678/" then the filename, so 10 + IMAGEFILENAMELEN
	static const tflite::Model *model = nullptr;

	// TODO - consider an arrangement for multiple models in flash
	xprintf("cv_init called with ProjectID: %d, Version: %d\n", project_id, deploy_version);

	// On first boot (g_project_id == 0), read the persisted model info from flash
	if (g_project_id == 0)  {
		if (read_persisted_model_info() != 0)  {
			// If flash is empty or invalid, use the values passed to init
			g_project_id = project_id;
			g_deploy_version = deploy_version;
			xprintf("First boot: No valid info in flash, using provided: Project=%d, Version=%d\n",
					g_project_id, g_deploy_version);
		}
	}
	else if ((project_id != g_project_id) || (deploy_version != g_deploy_version))  {
		g_project_id = project_id;
		g_deploy_version = deploy_version;
		model_loaded_from_sd = false; // reset flag to allow reloading
		xprintf("------------------------Model changed to Project=%d, Version=%d. Resetting load flag.\n",
				g_project_id, g_deploy_version);
	}

	xprintf("cv_init now loading with Project: %d, Version: %d\n", g_project_id, g_deploy_version);

	// Initialise Ethos-U55 device
	if (_arm_npu_init(security_enable, privilege_enable) != 0) {
		return -1;
	}

	// TBP - I've removed the static model data inclusion here and all refernces to the embedded model within the app
	// #if (MODEL_LOAD_MODE == MODEL_FROM_C_FILE)
	//     model = tflite::GetModel((const void *)g_person_detect_model_data_vela);

#if (MODEL_LOAD_MODE == MODEL_FROM_FLASH)
	model = load_model_from_flash();

#elif (MODEL_LOAD_MODE == MODEL_FROM_SD_CARD)

	const tflite::Model *loaded = nullptr;

	// Manifest unzip is handled by directory_manager during SD init.
	// (We expect either /Manifest/model.tfl exists, or the model may be in root.)

	// Build the expected filename and path
	// TODO put file and path names using #define
	snprintf(filename, sizeof(filename), "%dV%d.TFL",
			(unsigned int)(g_project_id % 10000), (unsigned int)g_deploy_version);

	snprintf(manifest_path, sizeof(manifest_path), "%s/%s", CONFIG_DIR, filename);

	xprintf("DEBUG: Looking for %s\n", manifest_path);
	// Check if any .tfl model exists in /Manifest

	// Searches for the first .TFL file on the SD card and exits if found
	if (fatfs_mounted()) {
		if (f_opendir(&dir, CONFIG_DIR) == FR_OK) {
			while ((f_readdir(&dir, &fno) == FR_OK) && (fno.fname[0] != '\0')) {
				const char *ext = strrchr(fno.fname, '.');
				// The strcasecmp function is case-insensitive: matches .tfl and .TFL
				if (ext && strcasecmp(ext, ".TFL") == 0) {
					// Use this specific model filename
					snprintf(filename, sizeof(filename), "%s", fno.fname);
					// Update manifest_path to point to the discovered model
					snprintf(manifest_path, sizeof(manifest_path), "%s/%s", CONFIG_DIR, filename);
					any_model_found = true;
					xprintf("----------------- Found model file '%s' on SD card\n", filename);
					break;
				}
			}
			f_closedir(&dir);
		}
	}
	else  {
		// Don't attempt disk access if filesystem not mounted
		xprintf("FatFS not mounted, skipping model search\n");
	}

	if (!any_model_found) {
		// TODO - This implies that the SD card must contain a .tfl file for the NN to work - even if there is a model in flash...
		xprintf("No .TFL model found in MANIFEST folder. Skipping neural network initialiSation.\n");
		model = nullptr;
		return 1;
	}
	else  {
		// Check if the model from manifest matches what's in flash
		if (is_model_in_flash(filename))  {
			xprintf("Flash already contains '%s'; loading from flash.\n", filename);
			loaded = load_model_from_flash();
		}
		else  {
			xprintf("Flash model differs or missing; copying '%s' to flash...\n", filename);

//			// Try manifest path first, then fallback to root
//			// TODO surely we should restrict to a single location?
//			const char *src_path = file_exists(manifest_path) ? manifest_path : filename;
//			xprintf("Source chosen: %s\n", src_path);
//
//			if (load_model_from_sd_to_flash((char *)src_path) == 0) {
//				xprintf("Copied %s to flash OK; loading from flash...\n", src_path);
//				loaded = load_model_from_flash();
//			}
//			else {
//				xprintf("SD->flash copy failed for %s\n", src_path);
//			}

			if (load_model_from_sd_to_flash(filename) == 0) {
				xprintf("Copied %s to flash OK\n", filename);
				loaded = load_model_from_flash();
			}
			else {
				xprintf("SD->flash copy failed for %s\n", filename);
			}
		}

		model = loaded;
	}

#endif // MODEL_LOAD_MODE

	if (!model)  {
		xprintf("Failed to load model\n");
		return -1;
	}

	xprintf("Model loaded successfully\n");

	// Attempt to load labels so results print friendly names
	load_labels_from_manifest();

	if (model->version() != TFLITE_SCHEMA_VERSION)  {
		xprintf("[ERROR] Model's schema version %d is not equal to supported version %d\n",
				model->version(), TFLITE_SCHEMA_VERSION);
		return -1;
	}

#ifdef PRINTMODELFINGERPRINT
	// Print information about the model
	tflm_print_ethosu_fingerprint(model);
#else
	xprintf("Model schema version: %d\n", model->version());
#endif // PRINTMODELFINGERPRINT

	static tflite::MicroErrorReporter micro_error_reporter;

	// Allocate op_resolver on heap so it persists beyond cv_init() scope
	// The interpreter keeps a reference to it, so it must live as long as the interpreter
	if (op_resolver_ptr != nullptr) {
		delete op_resolver_ptr;
	}
	op_resolver_ptr = new tflite::MicroMutableOpResolver<1>();
	xprintf("DEBUG: 0\n");

	if (kTfLiteOk != op_resolver_ptr->AddEthosU())  {
		xprintf("Failed to add Arm NPU support to op resolver.\n");
		return -1;
	}

	xprintf("DEBUG: 1\n");
	static tflite::MicroInterpreter *static_interpreter = new tflite::MicroInterpreter(
			model,
			*op_resolver_ptr,
			tensor_arena_buf,
			tensor_arena_size,
			&micro_error_reporter);
	xprintf("DEBUG: 2\n");

	// TODO fix crach that happens here when running loadmodel 2 3
	// See https://chatgpt.com/s/t_696ab268fa708191abc79f8768b3dffe
	// later: https://chatgpt.com/s/t_696ab3bc127881918e7ca6e01cc911a7
	if (static_interpreter->AllocateTensors() != kTfLiteOk) {
		xprintf("Failed to allocate tensors\n");
		return -1;
	}
	else {
#if 0
		xprintf("Used %lu/%lu bytes for tensors. Tensor arena is at 0x%08x\n",
				static_interpreter->arena_used_bytes(), tensor_arena_size, tensor_arena_buf);
#else
		xprintf("DEBUG 3\n");
#endif // 0
	}

	int_ptr = static_interpreter;
	input = static_interpreter->input(0);
	output = static_interpreter->output(0);
	xprintf("DEBUG: 4\n");

	return 0;
}

/**
 * Initialize the flash memory (SPI EEPROM)
 *
 * TODO - this is called many times - should only be called once.
 */
int init_flash(void) {
	uint8_t id_info;

	if (flash_initialized) {
		return 0;
	}

	// Create mutex on first call
	if (xSPIMutex == NULL)  {
		xSPIMutex = xSemaphoreCreateMutex();
		if (xSPIMutex == NULL)
		{
			xprintf("Failed to create SPI mutex\n");
			return -1;
		}
	}

	// Take mutex
	if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
		xprintf("Failed to take SPI mutex\n");
		return -1;
	}

	xprintf("Init EEPROM...\r\n");

//	hx_lib_spi_eeprom_open(USE_DW_SPI_MST_Q);
//    //hx_lib_spi_eeprom_open_speed(USE_DW_SPI_MST_Q, 6000000);
//
//	hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, true, FLASH_QUAD, true);

	if (hx_lib_spi_eeprom_open(USE_DW_SPI_MST_Q) != 0) {
		xprintf("Failed to open SPI EEPROM\n");
		flash_initialized = false;
		xSemaphoreGive(xSPIMutex);
		return -1;
	}

	// CRITICAL: Disable XIP mode before SPI operations
	hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

	// Small delay for mode switch
	// TODO - why?
	vTaskDelay(pdMS_TO_TICKS(10));

	if (hx_lib_spi_eeprom_read_ID(USE_DW_SPI_MST_Q, &id_info) != 0) {
		xprintf("Failed to read flash ID\n");
		xSemaphoreGive(xSPIMutex);
		return -1;
	}

	xprintf("Flash ID: 0x%02X\n", id_info);
	flash_initialized = true;

	xSemaphoreGive(xSPIMutex);

	return 0;
}

// Erase the flash region covering the model info, header, AND the model data.
int erase_model_flash_area(uint32_t model_size) {
	uint32_t model_xip_addr;
	uint32_t model_info_xip_addr; // 8 bytes for model info before filename header

	// Convert to physical addresses
	uint32_t model_phys_addr;      // e.g. 0x00200000
	uint32_t model_info_phys_addr; // e.g. 0x001FFFE8

	uint32_t erase_start;
	uint32_t total_length;
	uint32_t sectors_needed;
	uint32_t sector_addr;

    int ret = 0;

	if (init_flash() != 0) {
		return -1;
	}

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
        xprintf("Failed to take SPI mutex for erase\n");
        return -1;
    }

    // Disable XIP before erase
    hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

    // Addresses (XIP virtual used as reference)
    model_xip_addr = MODEL_XIP_ADDR;
    model_info_xip_addr = model_xip_addr - 24; // 8 bytes for model info before filename header

    // Convert to physical addresses
    model_phys_addr = virt_to_phys(model_xip_addr);           // e.g. 0x00200000
    model_info_phys_addr = virt_to_phys(model_info_xip_addr); // e.g. 0x001FFFE8

    // Compute erase region such that it covers model_info + header + model_size
    erase_start = align_down(model_info_phys_addr, FLASH_SECTOR_SIZE);
    total_length = (model_phys_addr + model_size) - erase_start;
    sectors_needed = align_up(total_length, FLASH_SECTOR_SIZE) / FLASH_SECTOR_SIZE;

    xprintf("Erasing %lu sectors from 0x%08lX to cover info+header+model (%lu bytes)...\n",
            (unsigned long)sectors_needed, erase_start, (unsigned long)total_length);

    for (uint32_t i = 0; i < sectors_needed; i++) {
        sector_addr = erase_start + (i * FLASH_SECTOR_SIZE);
        ret = hx_lib_spi_eeprom_erase_sector(USE_DW_SPI_MST_Q, sector_addr, FLASH_SECTOR);
        xprintf("  Erase sector %lu addr 0x%08lX -> result %d\n",
                (unsigned long)i, sector_addr, ret);
        if (ret != 0) {
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
    DWORD fileSize;
    uint32_t model_xip_addr;
    uint32_t header_xip_addr;
    uint32_t model_phys_addr;
    uint32_t header_phys_addr;
    uint32_t flash_address;
    uint32_t totalBytesRead;
    uint32_t write_size;


    union {
        uint8_t bytes[MODEL_XIP_INFO_SIZE];
        uint32_t words[MODEL_XIP_INFO_SIZE / 4];
    } fname_header, fname_verify;

    // aligned buffers
    uint8_t write_buf[256] __attribute__((aligned(4)));
    uint8_t verify_buf[256] __attribute__((aligned(4)));

    // If 'filename' includes a path, use it to open the file but only store the basename in flash header
    const char *src_path = filename; // may include /Manifest/
    const char *base = filename;     // basename part only
    for (const char *p = filename; *p; ++p)  {
        if (*p == '/' || *p == '\\')
            base = p + 1; // advance past last separator
    }
    xprintf("load_model_from_sd_to_flash: src_path='%s' base='%s'\n", src_path, base);

    if (init_flash() != 0)  {
        return -1;
    }

    res = f_open(&file, src_path, FA_READ);
    if (res != FR_OK)  {
        xprintf("Failed to open model file %s (error: %d)\n", src_path, res);
        return -1;
    }

    fileSize = f_size(&file);
    if (fileSize == 0)  {
        xprintf("Model file is empty\n");
        f_close(&file);
        return -1;
    }
    xprintf("Model file size: %lu bytes\n", fileSize);

    if (erase_model_flash_area(fileSize) != 0)  {
        xprintf("Failed to erase flash for model\n");
        f_close(&file);
        return -1;
    }

    //
    model_xip_addr = MODEL_XIP_ADDR;
    header_xip_addr = model_xip_addr - MODEL_XIP_INFO_SIZE;
    model_phys_addr = virt_to_phys(model_xip_addr);
    header_phys_addr = virt_to_phys(header_xip_addr);

    // Take semaphore for header write
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
        xprintf("Failed to take SPI mutex for header write\n");
        f_close(&file);
        return -1;
    }

    // Ensure XIP is disabled
    hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

    memset(fname_header.bytes, 0, sizeof(fname_header.bytes));
    strncpy((char *)fname_header.bytes, base, sizeof(fname_header.bytes) - 1);
    xprintf("Writing filename header: %s\n", fname_header.bytes);

    // write header (filename) via SPI (4 words)
    int wr = hx_lib_spi_eeprom_word_write(USE_DW_SPI_MST_Q, header_phys_addr, fname_header.words, MODEL_XIP_INFO_SIZE);
    if (wr != 0)  {
        xprintf("Failed to write filename header to flash (err %d)\n", wr);
        xSemaphoreGive(xSPIMutex);
        f_close(&file);
        return -1;
    }

    // small delay to let write finish
    // TODO -why?
    for (volatile int d = 0; d < 20000; ++d)  {
        asm volatile("nop");
    }

    // verify header by reading back via SPI
    memset(fname_verify.bytes, 0, sizeof(fname_verify.bytes));
    if (hx_lib_spi_eeprom_word_read(USE_DW_SPI_MST_Q, header_phys_addr, fname_verify.words, MODEL_XIP_INFO_SIZE) != 0) {
        xprintf("Failed to verify filename header in flash (read error)\n");
        xSemaphoreGive(xSPIMutex);
        f_close(&file);
        return -1;
    }

    if (memcmp(fname_header.bytes, fname_verify.bytes, MODEL_XIP_INFO_SIZE) != 0)  {
        xprintf("Filename header verify FAIL (SPI)\n");
        xSemaphoreGive(xSPIMutex);
        f_close(&file);
        return -1;
    }

    xSemaphoreGive(xSPIMutex);

    // Write model data at model_phys_addr
    flash_address = model_phys_addr;
    totalBytesRead = 0;

    while (totalBytesRead < fileSize) {
        memset(write_buf, 0, sizeof(write_buf));
        res = f_read(&file, write_buf, sizeof(write_buf), &bytesRead);
        if (res != FR_OK || bytesRead == 0)  {
            break;
        }
        write_size = (bytesRead + 3) & ~3U;
        if (write_size > bytesRead)  {
            memset(write_buf + bytesRead, 0, write_size - bytesRead);
        }

        // Take semaphore for each chunk write
        if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
            xprintf("Failed to take SPI mutex for chunk write\n");
            f_close(&file);
            return -1;
        }

        // Ensure XIP is disabled
        hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

        xprintf("Writing %u bytes (aligned %u) to 0x%08lX\n",
                (unsigned)bytesRead, (unsigned)write_size, (unsigned long)flash_address);
        if (hx_lib_spi_eeprom_word_write(USE_DW_SPI_MST_Q,
                                         flash_address,
                                         (uint32_t *)write_buf,
                                         write_size) != 0) {
            xprintf("Flash write failed at 0x%08lX\n", flash_address);
            xSemaphoreGive(xSPIMutex);
            f_close(&file);
            return -1;
        }

        // TODO - Why a delay? In any event, use FreeRTOS delay
        for (volatile int d = 0; d < 10000; ++d)  {
            asm volatile("nop");
        }

        memset(verify_buf, 0, write_size);
        if (hx_lib_spi_eeprom_word_read(USE_DW_SPI_MST_Q,
                                        flash_address,
                                        (uint32_t *)verify_buf,
                                        write_size) != 0)  {
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

        flash_address += write_size;
        totalBytesRead += bytesRead;
    }
    f_close(&file);

    if (totalBytesRead == fileSize) {
        xprintf("Model successfully written to physical 0x%08lX (%lu bytes)\n",
                (unsigned long)MODEL_FLASH_ADDR, (unsigned long)fileSize);

        // Persist the model info to flash so it survives reboots
        write_persisted_model_info();

        // Re-enable XIP mode after all writes complete
        if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) == pdTRUE)  {
            if (hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, true, FLASH_QUAD, true) == 0) {
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
bool is_model_in_flash(char *filename) {
    char fname_param[MODEL_XIP_INFO_SIZE] = {0};

    if (init_flash() != 0)  {
        return false;
    }

    debug_flash_status();	// print debug info

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for is_model_in_flash\n");
        return false;
    }

    // Ensure XIP is disabled
    hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

    // Read filename header (first 16 bytes) BEFORE model address
    uint32_t model_xip_addr = MODEL_XIP_ADDR;
    uint32_t header_xip_addr = model_xip_addr - MODEL_XIP_INFO_SIZE;
    uint32_t fname_addr = virt_to_phys(header_xip_addr);

    union {
        uint8_t bytes[MODEL_XIP_INFO_SIZE];
        uint32_t words[MODEL_XIP_INFO_SIZE/4];
    } fname_header;

    memset(fname_header.bytes, 0, sizeof(fname_header.bytes));
    if (hx_lib_spi_eeprom_word_read(USE_DW_SPI_MST_Q, fname_addr, fname_header.words, MODEL_XIP_INFO_SIZE) != 0)  {
        xprintf("Failed to read filename header from flash\n");
        xSemaphoreGive(xSPIMutex);
        return false;
    }

    xprintf("Flash filename header: ");
    for (int i = 0; i < MODEL_XIP_INFO_SIZE; i++) {
        xprintf("%02X ", fname_header.bytes[i]);
    }
    xprintf("\n");

    xSemaphoreGive(xSPIMutex);

    // Compare filename
    strncpy(fname_param, filename, sizeof(fname_param) - 1);
    xprintf("Filename parameter: %s\n", fname_param);

    // Use the filename length of 13 bytes - IMAGEFILENAMELEN
    if (strncmp(filename, (const char *)fname_header.bytes, IMAGEFILENAMELEN) == 0) {
    	xprintf("Model filename in flash is '%s'\n", filename);
    	return true;
    }
    else {
    	xprintf("Model filename in flash is '%s', not '%s'\n", fname_header.bytes, filename);
    	return false;
    }
}

// Modified load function - test BOTH SPI and XIP access
/**
 *
 */
static const tflite::Model *load_model_from_flash(void) {
    if (init_flash() != 0) {
        xprintf("Flash init failed\n");
        return nullptr;
    }

    const uint32_t phys = MODEL_FLASH_ADDR;   // 0x00200000
    const uint32_t virt = phys_to_virt(phys); // 0x3A200000= MODEL_XIP_ADDR

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
        xprintf("Failed to take SPI mutex for load_model_from_flash\n");
        return nullptr;
    }

    // Disable XIP for SPI read
    hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

    // Step 1: Read via SPI to confirm data exists

    uint8_t spi_hdr[32] __attribute__((aligned(4))) = {0};
    if (hx_lib_spi_eeprom_word_read(USE_DW_SPI_MST_Q, phys, (uint32_t *)spi_hdr, sizeof(spi_hdr)) != 0) {
        xprintf("Failed to read SPI header\n");
        xSemaphoreGive(xSPIMutex);
        return nullptr;
    }

#if 0
    // This can be considered code to test the XIP mapping
    xprintf("SPI@: ");
    for (size_t i = 0; i < sizeof(spi_hdr); i++) {
        xprintf("%02X ", spi_hdr[i]);
    }
    xprintf("\n");

    // Check if we have valid TFLite header via SPI
    if (spi_hdr[4] != 'T' || spi_hdr[5] != 'F' ||
        spi_hdr[6] != 'L' || spi_hdr[7] != '3')  {
        xprintf("No valid TFLite model in flash (SPI check)\n");
        xSemaphoreGive(xSPIMutex);
        return nullptr;
    }

    // Step 2: Enable XIP mode for memory-mapped access

    if (hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, true, FLASH_QUAD, true) != 0) {
        xprintf("Failed to enable XIP mode\n");
        xSemaphoreGive(xSPIMutex);
        return nullptr;
    }

    xSemaphoreGive(xSPIMutex);

    // Step 3: Verify XIP mapping works
    volatile uint8_t *xip_hdr = (volatile uint8_t *)virt;
    xprintf("XIP@0x%08lX: ", virt);
    for (int i = 0; i < 32; i++) {
        xprintf("%02X ", xip_hdr[i]);
    }
    xprintf("\n");

    // Step 4: Check if XIP matches SPI data
    bool xip_matches = (memcmp((const void *)spi_hdr, (const void *)xip_hdr, 32) == 0);

    if (xip_matches)  {
        xprintf("XIP mapping works! Using virtual address\n");
        return tflite::GetModel((const void *) virt);
    }
    else  {
        xprintf("XIP mapping mismatch!\n");
        return nullptr;
    }
#else
    // Use this once confident:

    // Check if we have valid TFLite header via SPI

    if (memcmp(&spi_hdr[4], "TFL3", 4) != 0) {
        // no match
        xprintf("No valid TFLite model in flash (SPI check)\n");
        xSemaphoreGive(xSPIMutex);
        return nullptr;
    }

    // Step 2: Enable XIP mode for memory-mapped access

    if (hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, true, FLASH_QUAD, true) != 0) {
        xprintf("Failed to enable XIP mode\n");
        xSemaphoreGive(xSPIMutex);
        return nullptr;
    }

    xSemaphoreGive(xSPIMutex);

    // Step 4: Check if XIP matches SPI data
    bool xip_matches = (memcmp((const void *)spi_hdr, (const void *)virt, 32) == 0);

    if (xip_matches)  {
        return tflite::GetModel((const void *) virt);
    }
    else  {
        xprintf("XIP mapping mismatch!\n");
        return nullptr;
    }
#endif // 0
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
TfLiteStatus cv_run(int8_t *outCategories, uint16_t categoriesCount) {

	uint16_t input_height = 0;
	uint16_t input_width = 0;
	uint8_t input_channels = 0;

	// Some debug info here:
	// Expect dimensions = 4, with batch, height, width, channels
	uint16_t dims = input->dims->size;   // number of dimensions
	if ((dims == 4) && (input->dims->data[0] == 1)) {
		input_height = input->dims->data[1];
		input_width = input->dims->data[2];
		input_channels = input->dims->data[3];
	}
	else if (dims == 3) {
		input_height = input->dims->data[0];
		input_width = input->dims->data[1];
		input_channels = input->dims->data[2];
	}

	if (input_channels == 0) {
		// invalid data
		return kTfLiteError;
	}

	// debug figure out raw data type by its size: RP3 camera seems to produce 1.5 bytes per pixel -> YUV420
	xprintf("Input image is %d x %d (%d bytes)\n",
			app_get_raw_width(), app_get_raw_height(), app_get_raw_sz());

	xprintf("Input tensor is %d x %d (%d channels)\n", input_height, input_width, input_channels);

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

//    img_rescale((uint8_t *)app_get_raw_addr(),
//                app_get_raw_width(),
//                app_get_raw_height(),
//                INPUT_SIZE_X,
//                INPUT_SIZE_Y,
//                input->data.int8,
//                SC(app_get_raw_width(), INPUT_SIZE_X),
//                SC(app_get_raw_height(), INPUT_SIZE_Y));

	// TODO - consider hx_lib_image_resize_helium() etc - could be faster.
    img_rescale((uint8_t *)app_get_raw_addr(),
                app_get_raw_width(),
                app_get_raw_height(),
				input_width,
				input_height,
                input->data.int8,
                SC(app_get_raw_width(), input_width),
                SC(app_get_raw_height(), input_height));

    //xprintf("Image rescaled and loaded into input tensor.\n");

    TfLiteStatus invoke_status = int_ptr->Invoke();
    xprintf("Model invoked.\n");

    if (invoke_status != kTfLiteOk)   {
        xprintf("	TensorLite invoke fail\n");
        return invoke_status;
    }

    // See here for how TFLM can process outputs:
    // https://chatgpt.com/share/69670b6b-2034-8005-a63b-7c09e3f76cf1

    //  For int8 output tensor
    int8_t *results = output->data.int8;
    // 2D data use data[1] 1D data use data[0]
    int num_classes = (output->dims->size >= 2) ? output->dims->data[1] : output->dims->data[0];

    // Use TFLite's reference softmax implementation
    // Convert int8 logits to float for processing
    float logits_float[num_classes];
    float probabilities[num_classes];

    // Dequantize int8 logits to float
    // Typical quantization for logits: scale = 0.00390625 (1/256), zero_point = 0
    const float scale = output->params.scale;
    const int32_t zero_point = output->params.zero_point;

    // Raw int8 values are in [-128, 127]
    // To get actual logits in float, you reverse the quantization:
    // logits_float[i]=(int8_value[i]−zero_point)×scale

    for (int i = 0; i < num_classes; ++i) {
        logits_float[i] = (static_cast<float>(results[i]) - static_cast<float>(zero_point)) * scale;
    }

    // Apply softmax using TFLite's reference implementation
    tflite::SoftmaxParams softmax_params;
    softmax_params.beta = 1.0f; // Standard softmax temperature

    tflite::RuntimeShape shape({1, num_classes});
    // Softmax() converts the dequantized logits into probabilities (0-1, total = 1)
    tflite::reference_ops::Softmax(softmax_params, shape, logits_float, shape, probabilities);

    // Store results and display
    g_last_confidence_data.class_count = (num_classes > MAX_CLASSES) ? MAX_CLASSES : num_classes;

    for (int i = 0; i < num_classes; ++i) {
        float confidence = probabilities[i] * 100.0f;                                     // Convert to percentage
        int confidence_int = (int)(confidence + 0.5f);                                    // Round to nearest integer
        int confidence_frac = (int)((confidence - (float)confidence_int) * 10.0f + 0.5f); // Get decimal part
        if (confidence_frac < 0) {
            confidence_frac = 0; // Handle rounding edge cases
        }

        const char *label = (g_labels_loaded && i < g_label_count) ? g_labels[i] : nullptr;

        // Store confidence data for EXIF (if within bounds)
        if (i < MAX_CLASSES)  {
            g_last_confidence_data.confidence_percent[i] = confidence_int;
            g_last_confidence_data.labels[i] = label;
        }

        // Color code based on confidence level
        if (confidence >= 50.0f)  {
            XP_LT_GREEN; // High confidence
        }
        else if (confidence >= 20.0f) {
            XP_YELLOW; // Medium confidence
        }
        else {
            XP_LT_RED; // Low confidence
        }

        if (label)  {
            xprintf("Class '%s': %d.%d%% (raw: %d)\n", label, confidence_int, confidence_frac, results[i]);
        }
        else  {
            xprintf("Class %d: %d.%d%% (raw: %d)\n", i, confidence_int, confidence_frac, results[i]);
        }
    }
    XP_WHITE;

#if ORIGINAL
    // retrieve output data
    int8_t model_score = output->data.int8[1];
    // CGP not used int8_t no_model_score = output->data.int8[0];

    // CGP add some colour to highlight this message
    if (model_score > 0)
    {
        XP_LT_GREEN;
        xprintf("TARGET OBJECT DETECTED!\n\n");
    }
    else
    {
        XP_LT_RED;
        xprintf("No target object detected.\n\n");
    }
    XP_WHITE;

    xprintf("model_score: %d\n", model_score);

    // error_reporter not declared...
    //	error_reporter->Report(
    //		   "   model score: %d, no model score: %d\n", model_score, no_model_score);
#else
    // TODO - must process > 2 categories!
    if (categoriesCount != CATEGORIESCOUNT)   {
        return kTfLiteError; // error
    }

    for (uint8_t i = 0; i < categoriesCount; i++)  {
        outCategories[i] = output->data.int8[i];
    }
#endif

    return invoke_status;
}

// Robust deinit: safely release interpreter and tensor resources before model reloads
int cv_deinit(void) {
    // Disable interrupts or enter critical section if needed (to prevent concurrent access)
    taskENTER_CRITICAL();

    // Free the TFLite interpreter if it exists
    if (int_ptr != nullptr)
    {
        delete int_ptr;
        int_ptr = nullptr;
    }

    // Free the op_resolver if it exists
    if (op_resolver_ptr != nullptr)
    {
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

void debug_sd_model_integrity(const char *filename)
{
    xprintf("=== SD MODEL INTEGRITY CHECK ===\n");

    FIL file;
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK)
    {
        xprintf("FAIL: Cannot open SD file %s (error: %d)\n", filename, res);
        return;
    }

    DWORD fileSize = f_size(&file);
    xprintf("SD Model size: %lu bytes\n", fileSize);

    // Read and print first 16 bytes
    uint8_t header[16];
    UINT bytesRead;
    res = f_read(&file, header, sizeof(header), &bytesRead);
    if (res == FR_OK && bytesRead == sizeof(header))
    {
        xprintf("SD TFLite header (first 16 bytes): ");
        for (int i = 0; i < MODEL_XIP_INFO_SIZE; ++i) {
            xprintf("%02X ", header[i]);
        }
        xprintf("\n");

        // Check for 'T','F','L','3' at header[4..7]
        if (header[4] == 'T' && header[5] == 'F' && header[6] == 'L' && header[7] == '3')
        {
            xprintf("Header magic OK: TFL3\n");
        }
        else
        {
            xprintf("Header magic NOT OK: expected TFL3 at bytes 4-7\n");
        }
    }
    else
    {
        xprintf("Failed to read SD header for integrity check\n");
    }
    f_close(&file);
}

/**
 * Debug code for SPI EEPROM:
 *
 * Prints Flash init status, SPI ID, EEPROM ID byte
 */
void debug_flash_status(void) {
    xprintf("=== FLASH DEBUG STATUS ===\n");
    xprintf("Flash initialized: %s\n", flash_initialized ? "YES" : "NO");
    xprintf("SPI ID: %d\n", (int)USE_DW_SPI_MST_Q);

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)
    {
        xprintf("Failed to take SPI mutex for debug_flash_status\n");
        return;
    }

    // CRITICAL: Disable XIP before reading ID
    hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, false);

    uint8_t id_info = 0;
    int result = hx_lib_spi_eeprom_read_ID(USE_DW_SPI_MST_Q, &id_info);
    xprintf("Flash ID read result: %d, ID: 0x%02X\n", result, id_info);

    xSemaphoreGive(xSPIMutex);
}

void test_basic_spi_operations()
{
    xprintf("=== TESTING BASIC SPI OPS ===\n");

    uint32_t test_addr = (MODEL_FLASH_ADDR + 4 * FLASH_SECTOR_SIZE) & ~(FLASH_SECTOR_SIZE - 1);

    int er = hx_lib_spi_eeprom_erase_sector(USE_DW_SPI_MST_Q, test_addr, FLASH_SECTOR);
    xprintf("Erase result: %d at 0x%08lX\n", er, test_addr);

    uint32_t test_data[4] = {0xDEADBEEF, 0x12345678, 0xABCDEF00, 0x11223344};

    // Test writing one word at a time
    xprintf("Writing one word at a time:\n");
    for (int i = 0; i < 4; i++)
    {
        int wr = hx_lib_spi_eeprom_word_write(USE_DW_SPI_MST_Q, test_addr + (i * 4), &test_data[i], 1);
        xprintf("  Write word %d (0x%08lX) to 0x%08lX: result %d\n",
                i, test_data[i], test_addr + (i * 4), wr);
    }

    // Test reading one word at a time
    xprintf("Reading one word at a time:\n");
    for (int i = 0; i < 4; i++)
    {
        uint32_t read_data = 0;
        int rr = hx_lib_spi_eeprom_word_read(USE_DW_SPI_MST_Q, test_addr + (i * 4), &read_data, 1);
        xprintf("  Read word %d from 0x%08lX: result %d, data 0x%08lX %s\n",
                i, test_addr + (i * 4), rr, read_data,
                (read_data == test_data[i]) ? "OK" : "FAIL");
    }
}

void compare_sd_spi_xip_chunked(const char *filename)
{
    FIL file;
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK)
    {
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
    while (offset < fileSize)
    {
        UINT to_read = (fileSize - offset > VERIFY_CHUNK_SIZE) ? VERIFY_CHUNK_SIZE : (fileSize - offset);
        // Read SD
        f_lseek(&file, offset);
        res = f_read(&file, sd_buf, to_read, &bytesRead);
        if (res != FR_OK || bytesRead != to_read)
        {
            xprintf("Failed to read model file at offset %lu\n", offset);
            break;
        }
        // // Read SPI
        if (hx_lib_spi_eeprom_word_read(USE_DW_SPI_MST_Q, MODEL_FLASH_ADDR + offset, (uint32_t *)spi_buf, to_read) != 0)
        {
            // if (hx_lib_spi_eeprom_word_read(USE_DW_SPI_MST_Q, MODEL_FLASH_ADDR + offset, (uint32_t*)spi_buf, (to_read + 3) / 4) != 0) {
            xprintf("Failed to read SPI at offset %lu\n", offset);
            break;
        }
        // Read XIP
        uint32_t virt_address = phys_to_virt(MODEL_FLASH_ADDR + offset);
        memcpy(xip_buf, (void *)virt_address, to_read);

        // Print mismatches for this chunk
        for (UINT i = 0; i < to_read; i++)
        {
            if (sd_buf[i] != spi_buf[i] || sd_buf[i] != xip_buf[i] || spi_buf[i] != xip_buf[i])
            {
                xprintf("Mismatch at offset %lu (+%u): SD=0x%02X SPI=0x%02X XIP=0x%02X\n",
                        offset, i, sd_buf[i], spi_buf[i], xip_buf[i]);
                any_mismatch = 1;
            }
        }
        offset += to_read;
    }
    if (!any_mismatch)
    {
        xprintf("Model in flash (SPI/XIP) matches SD file!\n");
    }
    f_close(&file);
}
