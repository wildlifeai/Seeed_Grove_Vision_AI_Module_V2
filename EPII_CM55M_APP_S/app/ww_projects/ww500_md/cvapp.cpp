/*
 * cvapp.cpp
 *
 *  Created on: 2022/02/22
 *      Author: 902452
 */

/*************************************** Includes *******************************************/

#include <cstdio>
#include <cmath>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// POSIX string functions (strcasecmp)
// TODO try to omit this
#include <strings.h>

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "WE2_device.h"
#include "board.h"
#include "cvapp.h"
#include "ff.h"

#include "WE2_core.h"

#include "ethosu_driver.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/kernels/internal/reference/softmax.h"
#include "tensorflow/lite/kernels/internal/types.h"

#include "xprintf.h"
#include "fatfs_task.h"

// Required for the app_get_xxx() functions
// #include "cisdp_cfg.h"
#include "cisdp_sensor.h"
#include "common_config.h"
#include "printf_x.h" // Print colours
#include "ww500_md.h"
#include "app_msg.h"
#include "xip_manager.h"

/*************************************** Definitions *******************************************/

// Experiment to add multiple resolvers to NN
#define EXTRARESOLVERS

#define LOCAL_FRAQ_BITS (8)
#define SC(A, B) ((A << 8) / B)

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

// #define OLD

#ifdef TFLM_2412
//	Later library - defined in the makefile
#pragma message "Using tflmtag2412_u55tag2411"
#else
#pragma message "Using tflmtag2209_u55tag2205"
#endif // TFLM_2412

/*************************************** External variables *******************************************/

extern "C" {
	// These are defined in the linker .ld file, and aligned on 32-byte boundaries
    extern uint8_t __tensor_arena_start__;
    extern uint8_t __tensor_arena_end__;
}

static uint8_t *tensor_arena_buf = &__tensor_arena_start__;
static size_t tensor_arena_size = (size_t)(&__tensor_arena_end__ - &__tensor_arena_start__);

// These are the handles for the input queues of tasks. So we can send them messages
extern QueueHandle_t xImageTaskQueue;

/*************************************** C++ Namespace *******************************************/

// see https://chatgpt.com/share/696ae627-6f60-8005-92cb-6030e56990cc
// for fix of dynamic model

using namespace std;
namespace {

    struct ethosu_driver ethosu_drv; /* Default Ethos-U device driver */
    static const tflite::Model *modelUsed = nullptr;
    static tflite::MicroInterpreter *interpreter = nullptr;
#ifdef EXTRARESOLVERS
    static tflite::MicroMutableOpResolver<4> *op_resolver_ptr = nullptr;
#else
    static tflite::MicroMutableOpResolver<1> *op_resolver_ptr = nullptr;
#endif //

    static tflite::MicroErrorReporter micro_error_reporter;
    TfLiteTensor *input;
    TfLiteTensor *output;

    static uint8_t g_class_count = 0;
};

/*************************************** Local variables *************************************/

// Globals to hold the current model identifiers
// TODO should these really be 32-bits?
static uint16_t g_project_id = 0;
static uint16_t g_deploy_version = 0;

#ifdef USE_PERCENTAGE
// Global to store the last confidence data for EXIF
static ClassConfidenceData g_last_confidence_data = {0};
#endif // USE_PERCENTAGE

static bool coldBoot;

/*************************************** Local Function Declarations *****************************/

static const tflite::Model *load_model_from_sd(char *filename);
static const tflite::Model *load_model_from_flash(void);

#ifdef USE_PERCENTAGE
static void outputAsPercentage(TfLiteTensor *output);
#endif // USE_PERCENTAGE

static void tflm_print_ethosu_fingerprint(const tflite::Model* model);

/*************************************** Local Function Definitions  *************************************/

#ifdef PRINTMODELFINGERPRINT
#include "tensorflow/lite/schema/schema_generated.h"

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
static void tflm_print_ethosu_fingerprint(const tflite::Model* model) {
    if (!model) {
        xprintf("TFLM: model = NULL\r\n");
        return;
    }

    xprintf("TFLM Ethos-U55 Model Fingerprint\r\n");
    xprintf("---------------------------------\r\n");
    xprintf("Schema version : %d (expected %d)\r\n",
            model->version(), TFLITE_SCHEMA_VERSION);

    const auto* subgraphs = model->subgraphs();
    if (!subgraphs) {
        xprintf("Subgraphs      : <none>\r\n");
        return;
    }

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
            if (i != t->shape()->size() - 1) {
            	xprintf(",");
            }
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
    void *ethosu_base_address = (void *)(U55_BASE);

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


/**
 * Loads model from the SD card to flash.
 *
 * Model is expected to be in /MANIFEST folder.
 *
 * Model and metadata are copied to flash, then a pointer to the model
 * is returned via xip_load_model_from_flash().
 */
static const tflite::Model *load_model_from_sd(char *filename) {

	if (xip_copy_model_from_sd_to_flash(filename)) {
		xprintf("Copied %s to flash OK\n", filename);
	}
	else {
		xprintf("SD model->flash copy failed for %s\n", filename);
		return nullptr;
	}

	//	Now try to copy labels from ssVvv.TXT to the meta data area of the XIP flash
	if (xip_copy_metadata_to_flash(filename)) {
		xprintf("Copied labels to flash for %s\n", filename);
	}
	else {
		xprintf("SD labels->flash copy failed for %s\n", filename);
	}

	return load_model_from_flash();
}

/**
 * Validate the model in flash and return a TFLite model pointer.
 *
 * Thin C++ wrapper around xip_get_model_xip_address() — obtains the
 * validated virtual address then calls tflite::GetModel().
 */
static const tflite::Model *load_model_from_flash(void) {
	uint32_t addr = xip_get_model_xip_address();
	return addr ? tflite::GetModel((const void *)addr) : nullptr;
}

/********************************** Public Functions  *************************************/

/**
 *	Initialise TFLM model
 *
 *	The project_id and deploy_version parameters are used to determine the TFLM file name & model name.
 *	These are loaded from CONFIG.TXT or default values (0, 0) are used. Models names are like "123V4.TFL"
 *	and class labels are stored on SD card as "123V4.TXT"
 *
 *	Then:
 *
 *	Option 1: use named model already in flash
 *	Option 2: or if named model is on SD card, erase the flash and program the new model
 *		(but don't look for a model if the project_id is 0).
 *	Option 3: or use any existing model is in flash
 *	Option 4: or if no model is available then don't use NN
 *
 *	When the model is saved to flash, meta data is also saved to flash (just before the model).
 *	The meta data includes the class label names, and these are the values (read from flash)
 *	that are reported on the console and used in the EXIF data.
 *
 * 	@param security_enable
 *	@param privilege_enable
 *	@param project_id - from Operational Parameter OP_PARAMETER_MODEL_PROJECT
 *	@param deploy_version - from Operational Parameter OP_PARAMETER_MODEL_VERSION
 *	@param woken - used to print info only on cold boots
 *	@return 0 for OK, -1 if no NN is used
 */
int cv_init(bool security_enable, bool privilege_enable, uint16_t project_id, uint16_t deploy_version, APP_WAKE_REASON_E woken) {
	char filename[MAX_MODEL_NAME_LEN];	// for 8.3 this is 13, including the trailing \0

	// Enforce clean state
	cv_deinit();

	XP_GREEN;
	if (project_id == 0) {
		xprintf("\nNot initialising NN (project ID is 0)\n");
		XP_WHITE;
		return -1;
	}
#ifdef TFLM_2412
    xprintf("Initialising NN with 2412/ETHOS-U 2411 library. Arena size %u\n", tensor_arena_size);
#else
    xprintf("Initialising NN 2209/ETHOS-U 2205 library. Arena size %u\n", tensor_arena_size);
#endif // TFLM_2412

	XP_WHITE;

	// Allow for printing only on cold boots
	coldBoot = (woken == APP_WAKE_REASON_COLD);

	if (_arm_npu_init(security_enable, privilege_enable) != 0) {
		return -1;
	}

	// Create a file name based on the project_id and deploy_version parameters
	snprintf(filename, sizeof(filename), "%dV%d.TFL", (int) project_id, (int) deploy_version);

	xprintf("Looking for model '%s' in flash or SD card\n", filename);

	// Option 1: named model is in flash
	if (xip_is_model_in_flash(filename, coldBoot)) {
		xprintf("Flash already contains model '%s'; loading from flash.\n", filename);
		modelUsed = load_model_from_flash();
	}
	// Option 2: named model is on SD card
	else if (xip_is_file_in_sd(filename)) {
		modelUsed = load_model_from_sd(filename);
	}
	// Option 3: any model is in flash (not the named one, but something usable)
	else if (xip_valid_model_in_flash()) {
		xprintf("Found another valid model\n");
		modelUsed = load_model_from_flash();
		if (!modelUsed) {
			xprintf("Error loading model from flash\n");
			return -1;
		}
	}
	// Option 4: no model is available
	else {
		xprintf("Failed to load model\n");
		return -1;
	}


#ifdef PRINTMODELFINGERPRINT
	if (coldBoot) {
		// Print information about the model
		tflm_print_ethosu_fingerprint(modelUsed);
	}
#else
	xprintf("Model schema version: %d\n", modelUsed->version());
#endif // PRINTMODELFINGERPRINT

	// NOTE: some models might need more than  1 of these.
	/* The allon_sensor_tflm_cmsis_nn loads all of these:
    op_resolver.AddDepthwiseConv2D();
	op_resolver.AddRelu6();
	op_resolver.AddConv2D();
	op_resolver.AddAveragePool2D();
	op_resolver.AddReshape();
	op_resolver.AddSoftmax();
	*/
#ifdef EXTRARESOLVERS
	// experimental
	op_resolver_ptr = new tflite::MicroMutableOpResolver<4>();
	if (!op_resolver_ptr) {
		return -1;
	}

	if (op_resolver_ptr->AddEthosU() != kTfLiteOk) {
		xprintf("Failed to add Arm NPU support to op resolver.\n");
		return -1;
	}
	if (op_resolver_ptr->AddPad() != kTfLiteOk) {
		xprintf("Failed to add Padding ");
		return -1;
	}

	if (op_resolver_ptr->AddTranspose() != kTfLiteOk) {
		xprintf("Failed to add Transpose\n");
		return -1;
	}
#ifdef TFLM_2412
	// Only present in the later library
	if (op_resolver_ptr->AddBatchMatMul() != kTfLiteOk) {
		xprintf("Failed to add MatMul\n");
		return -1;
	}
#endif // TFLM_2412

#else
	op_resolver_ptr = new tflite::MicroMutableOpResolver<1>();
	if (!op_resolver_ptr) {
		return -1;
	}

	if (op_resolver_ptr->AddEthosU() != kTfLiteOk) {
		xprintf("Failed to add Arm NPU support to op resolver.\n");
		return -1;
	}
#endif // EXTRARESOLVERS

#ifdef TFLM_2412
//    // New API: different signature

    interpreter = new tflite::MicroInterpreter(
			modelUsed,
			*op_resolver_ptr,
			tensor_arena_buf,
			tensor_arena_size);
#else
    // Old API:
	interpreter = new tflite::MicroInterpreter(
			modelUsed,
			*op_resolver_ptr,
			tensor_arena_buf,
			tensor_arena_size,
			&micro_error_reporter);
#endif // TFLM_2412

	if (!interpreter) {
		return -1;
	}

	if (interpreter->AllocateTensors() != kTfLiteOk) {
		return -1;
	}

	input  = interpreter->input(0);
	output = interpreter->output(0);

    const TfLiteIntArray* dims = output->dims;

    // Common cases:
    // [classes]
    // [1, classes]
    if (dims->size == 1) {
    	g_class_count = dims->data[0];
    }
    else if (dims->size >= 2) {
    	g_class_count = dims->data[dims->size - 1];
    }
    else {
    	// error
    	g_class_count = 0;
    }

    xprintf("There are %d classes (%d)\n", g_class_count, metaDataFlash->class_count);
    // Warning: the number of classes in the metadata has been set by the number of labels in the label text file. This must be the same!
    if (g_class_count != metaDataFlash->class_count) {
    	XP_RED;
    	xprintf("WARNING: Number of classes and labels unmatched\n");
    	XP_WHITE;
    }

	return 0;
}

/*
 * Erase just enough flash to remove the metadata and the 'TFTl3' in the model header
 */
void cv_eraseModel(void) {
	APP_MSG_T send_msg;

	cv_deinit();

	// We will erase 4k so any number is OK here
	xip_erase_model_flash_area(8);

	// Update the numbers in the Operational Parameter array. PROJECT_ID 0 means don't look for a model file
	fatfs_setOperationalParameter(OP_PARAMETER_MODEL_PROJECT, PROJECT_ID);
	fatfs_setOperationalParameter(OP_PARAMETER_MODEL_VERSION, PROJECT_VER);

	// Now send a message to Image Task Queue so it knows the job is done.
	send_msg.msg_event = APP_MSG_IMAGETASK_NN_MODEL_ERASED;

	if (xQueueSend(xImageTaskQueue, (void *)&send_msg, __QueueSendTicksToWait) != pdTRUE) {
		xprintf("Failed to send message 0x%04x\n", send_msg.msg_event);
	}
}


/*
 * Erase just enough flash to remove the metadata and the 'TFTl3' in the model header
 */
void cv_newModel(uint16_t project_id, uint16_t deploy_version) {
	APP_MSG_T send_msg;
	int cv_init_result;

	cv_init_result = cv_init(true, true, project_id, deploy_version, APP_WAKE_REASON_COLD);

	if (cv_init_result < 0) {
		xprintf("Nwe model init failed.\n");
		// TODO - do we do this?
		//selfTest_setErrorBits(1 << SELF_TEST_AI_NN_ERROR);

		XP_RED;
		xprintf("----------- MODEL UPDATE FAILED \n");
		XP_WHITE;
		// send an error code?
		send_msg.msg_data = 1;
	}
	else {
		xprintf("Initialised neural network.\n");
		XP_GREEN;
		xprintf("-------- MODEL UPDATE SUCCESS\n");
		XP_WHITE;

		// Update the numbers in the Operational Parameter array otherwise the previous model will be reloaded
		fatfs_setOperationalParameter(OP_PARAMETER_MODEL_PROJECT, project_id);
		fatfs_setOperationalParameter(OP_PARAMETER_MODEL_VERSION, deploy_version);
		// send an error code?
		send_msg.msg_data = 0;
	}

	// Now send a message to Image Task Queue so it knows the job is done.
	send_msg.msg_event = APP_MSG_IMAGETASK_NN_MODEL_UPDATED;

	if (xQueueSend(xImageTaskQueue, (void *)&send_msg, __QueueSendTicksToWait) != pdTRUE) {
		xprintf("Failed to send message 0x%04x\n", send_msg.msg_event);
	}
}

// Robust deinit: safely release interpreter and tensor resources before model reloads
int cv_deinit(void) {

    if (interpreter) {
    	delete interpreter;
    	interpreter = nullptr;
    }

    if (op_resolver_ptr) {
    	delete op_resolver_ptr;
    	op_resolver_ptr = nullptr;
    }

    modelUsed = nullptr;

    // IMPORTANT: clear tensor arena
    memset(tensor_arena_buf, 0, tensor_arena_size);

    // Reset tensor pointers
    input = nullptr;
    output = nullptr;

    // Optional: shut down Ethos-U if required
    //_arm_npu_deinit();
    return 0;
}


/**
 * This runs the neural network processing.
 *
 * The function gets the address and dimensions of the image from
 * app_get_raw_addr(), app_get_raw_width(), app_get_raw_height()
 *
 * It rescales the image to match what the model requires
 * then runs the NN.
 *
 * I have modified the code so it returns the result of the calculation
 *
 * @param outCategories = pointer to an array containing the processing results
 * @param categoriesCount = size of the array
 * @return error code
 */
TfLiteStatus cv_run(int8_t *outCategories, uint8_t *categoriesCount) {

	uint16_t input_height = 0;
	uint16_t input_width = 0;
	uint8_t input_channels = 0;

	if (modelUsed == nullptr) {
		// can't run!
		return kTfLiteError;
	}

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

	// TODO - consider hx_lib_image_resize_helium() etc - could be faster.
    img_rescale((uint8_t *)app_get_raw_addr(),
                app_get_raw_width(),
                app_get_raw_height(),
				input_width,
				input_height,
                input->data.int8,
                SC(app_get_raw_width(), input_width),
                SC(app_get_raw_height(), input_height));

    TfLiteStatus invoke_status = interpreter->Invoke();
    xprintf("Model invoked.\n");

    if (coldBoot){
    	XP_LT_GREY;
    	xprintf("DEBUG: meta data now\n");
    	printf_x_printBuffer((const uint8_t *)metaDataFlash, sizeof(ModelMetaData));

    	// Small delay for buffer to print
    	vTaskDelay(pdMS_TO_TICKS(10));

    	XP_WHITE;
    	printf("Magic: 0x%08x Model '%s' has %d labels of %d bytes:\n",
    			(int) metaDataFlash->magic, metaDataFlash->modelName,
				metaDataFlash->class_count, metaDataFlash->label_len);

    	for (uint8_t i=0; i < metaDataFlash->class_count; i++) {
    		xprintf("%d = '%s'\n", i, metaDataFlash->labels[i]);
    	}
    }

    if (invoke_status != kTfLiteOk)   {
        xprintf("	TensorLite invoke fail\n");
        return invoke_status;
    }

    // See here for how TFLM can process outputs:
    // https://chatgpt.com/share/69670b6b-2034-8005-a63b-7c09e3f76cf1

#ifdef USE_PERCENTAGE
    // Moved all of the percentage processing to its own function
    outputAsPercentage(output);
#endif // USE_PERCENTAGE

    // Write the class count to the caller
    * categoriesCount = g_class_count;

    for (uint8_t i = 0; i < g_class_count; i++)  {
        outCategories[i] = output->data.int8[i];
    }

    return invoke_status;
}

#ifdef USE_PERCENTAGE
/**
 * Converts the output tensor to confidence levels expressed as percentages.
 *
 * Output are written to the global g_last_confidence_data
 */
static void outputAsPercentage(TfLiteTensor *output) {
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

        const char * label = metaDataFlash->labels[i];

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
}

#endif // USE_PERCENTAGE

/**
 * Checks if a model is loaded
 * @return // True if a model is ready to be used
 */
bool cv_modelLoaded(void) {
	return (modelUsed != nullptr);
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

#ifdef USE_PERCENTAGE
// Public getter for confidence data (for EXIF)
bool cv_get_confidence_data(ClassConfidenceData *data) {
    if (data != nullptr) {
        memcpy(data, &g_last_confidence_data, sizeof(ClassConfidenceData));
        return true;
    }
    return false;
}
#endif // USE_PERCENTAGE

/**
 * Return pointer to the class label
 *
 * Used by the detection path (processNNOutput) regardless of percentage mode,
 * so it is defined unconditionally.
 *
 * @param index index to the labels
 * @return pointer to the string
 */
const char * cv_getLabel(uint8_t index) {
	if (index < MAX_CLASSES) {
		return metaDataFlash->labels[index];
	}
	else {
		return "";
	}
}
