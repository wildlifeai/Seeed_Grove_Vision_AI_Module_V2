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

// Uncomment this to get information about the model:
//#define PRINTMODELFINGERPRINT

#define TENSOR_ARENA_BUFSIZE (125 * 1024)
__attribute__((section(".bss.NoInit"))) uint8_t tensor_arena_buf[TENSOR_ARENA_BUFSIZE] __ALIGNED(32);

using namespace std;

namespace
{

	constexpr int tensor_arena_size = TENSOR_ARENA_BUFSIZE;
	uint32_t tensor_arena = (uint32_t)tensor_arena_buf;

	struct ethosu_driver ethosu_drv; /* Default Ethos-U device driver */
	tflite::MicroInterpreter *int_ptr = nullptr;
	TfLiteTensor *input, *output;
};

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

	if (_arm_npu_init(security_enable, privilege_enable) != 0)
		return -1;

#if (FLASH_XIP_MODEL == 1)
	static const tflite::Model *model = tflite::GetModel((const void *)0x3A180000);
#else
	static const tflite::Model *model = tflite::GetModel((const void *)g_person_detect_model_data_vela);
#endif

	if (model->version() != TFLITE_SCHEMA_VERSION)
	{
		xprintf(
			"[ERROR] model's schema version %d is not equal "
			"to supported version %d\n",
			model->version(), TFLITE_SCHEMA_VERSION);
		return -1;
	}
	else
	{
		xprintf("model's schema version %d\n", model->version());
		xprintf("Input: %d x %d NN: %d x %d\n",
				app_get_raw_width(), app_get_raw_height(), INPUT_SIZE_X, INPUT_SIZE_X);
	}

#ifdef PRINTMODELFINGERPRINT
	// Print information about the model
	tflm_print_ethosu_fingerprint(model);
#endif // PRINTMODELFINGERPRINT

	static tflite::MicroErrorReporter micro_error_reporter;
	static tflite::MicroMutableOpResolver<1> op_resolver;

	if (kTfLiteOk != op_resolver.AddEthosU())
	{
		xprintf("Failed to add Arm NPU support to op resolver.");
		return false;
	}

	static tflite::MicroInterpreter static_interpreter(model, op_resolver, (uint8_t *)tensor_arena, tensor_arena_size, &micro_error_reporter);

	if (static_interpreter.AllocateTensors() != kTfLiteOk)
	{
		return false;
	}
	int_ptr = &static_interpreter;
	input = static_interpreter.input(0);
	output = static_interpreter.output(0);

	//xprintf("cv_init() done\n");

	return ercode;
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
//	img_rescale((uint8_t *)app_get_raw_addr(),
//				app_get_raw_width(),
//				app_get_raw_height(),
//				INPUT_SIZE_X,
//				INPUT_SIZE_Y,
//				input->data.int8,
//				SC(app_get_raw_width(), INPUT_SIZE_X),
//				SC(app_get_raw_height(), INPUT_SIZE_Y));

	// TODO - consider hx_lib_image_resize_helium() etc - could be faster.
    img_rescale((uint8_t *)app_get_raw_addr(),
                app_get_raw_width(),
                app_get_raw_height(),
				input_width,
				input_height,
                input->data.int8,
                SC(app_get_raw_width(), input_width),
                SC(app_get_raw_height(), input_height));

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
	int8_t person_score = output->data.int8[1];
	// CGP not used int8_t no_person_score = output->data.int8[0];

	// CGP add some colour to highlight this message
	if (person_score > 0) {
		XP_LT_GREEN;
		xprintf("PERSON DETECTED!\n\n");
	}
	else {
		XP_LT_RED;
		xprintf("No person detected.\n\n");
	}
	XP_WHITE;

	xprintf("Person_score: %d\n", person_score);

	// error_reporter not declared...
	//	error_reporter->Report(
	//		   "   person score: %d, no person score: %d\n", person_score, no_person_score);
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
