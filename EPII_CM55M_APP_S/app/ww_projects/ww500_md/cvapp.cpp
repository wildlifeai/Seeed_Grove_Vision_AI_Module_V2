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

#include "hx_drv_spi.h"
#include "spi_eeprom_comm.h"

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

/*************************************** Definitions *******************************************/

// Read file contents in chunks this size
#define FILE_CHUNK_SIZE 512

// #include "dev_common.h"

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

// oRIGINALLY, SOME META DATA WAS STORED THIS NUMBER OF BYTES BEFORE THE MODEL
#define MODEL_OLD_META_SIZE	24

typedef struct {
    uint32_t magic;
    uint16_t class_count;
    uint16_t label_len;
    char modelName[MAX_MODEL_NAME_LEN];
    char labels[MAX_CLASSES][MAX_LABEL_LEN];
    uint32_t crc;   // optional but highly recommended
} ModelMetaData;

// This version becomes available if a valid structure is written to flash - only at the virtual address 0x3A200000
#define metaDataFlash ((const ModelMetaData *)MODEL_XIP_ADDR)


/*************************************** External variables *******************************************/

extern "C" {
	// These are defined in the linker .ld file, and aligned on 32-byte boundaries
    extern uint8_t __tensor_arena_start__;
    extern uint8_t __tensor_arena_end__;
}

static uint8_t *tensor_arena_buf = &__tensor_arena_start__;
static size_t tensor_arena_size = (size_t)(&__tensor_arena_end__ - &__tensor_arena_start__);

/*************************************** C++ Namespace *******************************************/

// #define OLD
// see https://chatgpt.com/share/696ae627-6f60-8005-92cb-6030e56990cc
// for fix of dynamic model

using namespace std;
namespace {

#ifdef OLD
    struct ethosu_driver ethosu_drv; /* Default Ethos-U device driver */
    static tflite::MicroInterpreter *interpreter = nullptr;
    static tflite::MicroMutableOpResolver<1> *op_resolver_ptr = nullptr;
    TfLiteTensor *input, *output;
#else
    struct ethosu_driver ethosu_drv; /* Default Ethos-U device driver */
    static const tflite::Model *model = nullptr;
    static tflite::MicroInterpreter *interpreter = nullptr;
    static tflite::MicroMutableOpResolver<1> *op_resolver_ptr = nullptr;
    static tflite::MicroErrorReporter micro_error_reporter;
    TfLiteTensor *input;
    TfLiteTensor *output;
#endif // OLD

    // Labels loaded from SD (one per line) – used for printing class names
    static char g_labels[MAX_CLASSES][MAX_LABEL_LEN];
    static uint8_t g_label_count = 0;
//    static bool g_labels_loaded = false;
};

/*************************************** Local variables *******************************************/

// Use this constants since a bizarre compiler issue is redefining USE_DW_SPI_MST_Q as 1860 in some places
// See the "Bizarre" comment in is_model_in_flash()
USE_DW_SPI_MST_E spi_inst = (USE_DW_SPI_MST_E)0;

// SPI EEPROM configuration
static bool flash_initialized = false;
static SemaphoreHandle_t xSPIMutex = NULL;

// Globals to hold the current model identifiers
// TODO should these really be 32-bits?
static int g_project_id = 0;
static int g_deploy_version = 0;

// Global to store the last confidence data for EXIF
static ClassConfidenceData g_last_confidence_data = {0};

static bool coldBoot;

/*************************************** Local Function Declarations *****************************/

static bool is_model_in_flash(char *filename);
static bool is_model_in_sd(char *filename);
static const tflite::Model *load_model_from_sd(char * filename);

int load_model_from_sd_to_flash(char *filename);
static const tflite::Model *load_model_from_flash(void);

int erase_model_flash_area(uint32_t flashSizeRequired);
void compare_sd_spi_xip_chunked(const char *filename);
void debug_sd_model_integrity(const char *filename);
void debug_flash_status(void);
void test_basic_spi_operations();
int init_flash(void);
static inline uint32_t align_down(uint32_t addr, uint32_t align);
static inline uint32_t align_up(uint32_t size, uint32_t align);
static bool file_exists(const char *path);
static void load_labels_from_manifest(void);

#ifdef OLD
int read_persisted_model_info(void);
int write_persisted_model_info(void);
#endif // OLD

void build_meta_data(ModelMetaData * metaDataRam, const char *model_name,
                        const char src_labels[MAX_CLASSES][MAX_LABEL_LEN],
                        uint16_t classCount);

int32_t write_meta_data_to_flash(ModelMetaData * metaDataRam);

static bool copy_model_from_sd_to_flash(char * filename);
static bool copy_labels_from_sd_to_flash(char * modelName);

static bool valid_model_in_flash(void);

static bool enableXIP(bool enable);

static inline uint32_t virt_to_phys(uint32_t virt)  {
	return virt - FLASH_VIRTUAL_BASE + FLASH_PHYSICAL_BASE;
}

static inline uint32_t phys_to_virt(uint32_t phys) {
	return phys - FLASH_PHYSICAL_BASE + FLASH_VIRTUAL_BASE;
}


/*************************************** Local Function Definitions  *************************************/

// Round down address to sector boundary
static inline uint32_t align_down(uint32_t addr, uint32_t align)
{
    return addr & ~(align - 1);
}

/**
 * Rounds up 'size' to be an integral number of 'align' bytes
 *
 * Examples if 'align' is 4:
 *
 * size = 0: 0+4-1 = 3  then 0011b & 0011b = 0
 * size = 1: 1+4-1 = 4  then 0100b & 0011b = 0100b = 4
 * size = 2: 2+4-1 = 5  then 0101b & 0011b = 0100b = 4
 * size = 3: 3+4-1 = 6  then 0110b & 0011b = 0100b = 4
 * size = 4: 4+4-1 = 7  then 0111b & 0011b = 0100b = 4
 * size = 5: 5+4-1 = 8  then 1000b & 0011b = 1000b = 8
 *
 * @param size - number being examined
 * @param align - number of bytes in a chunk
 * @return - a number which is greater or equal to 'size' and
 */
static inline uint32_t align_up(uint32_t size, uint32_t align) {
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
 *
 * If the file exists then the labels are loaded into g_labels[][],
 * the number of labels is in g_label_count
 * and g_labels_loaded is set true
 *
 */
static void load_labels_from_manifest(void) {

    // Look for "/MANIFEST/LABELS.TXT" - concatenation done by the compiler.
    const char *labels_path = CONFIG_DIR "/LABELS.TXT";

    if (file_exists(labels_path))  {
        uint8_t count = 0;
        // TODO - labels to flash
        // https://chatgpt.com/share/696d4dca-4900-8005-a0ad-9a211e8a1d9a
        if (fatfs_load_labels(labels_path, g_labels, &count, MAX_CLASSES, MAX_LABEL_LEN) == 0) {
            g_label_count = count;
            //g_labels_loaded = (label_count > 0);
            xprintf("Loaded %d labels from %s\n", g_label_count, labels_path);
            return;
        }
        else  {
            xprintf("Labels load failed from %s\n", labels_path);
        }
    }
    xprintf("No labels found on SD\n");
}

#ifdef OLD
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
    hx_lib_spi_eeprom_enable_XIP(spi_inst, false, FLASH_QUAD, false);

    // Model info is stored 24 bytes before the model data
    // (4 bytes project_id + 4 bytes version + 16 bytes filename header)

//    uint32_t model_xip_addr = 0x3A200000;
//    uint32_t model_info_xip_addr = model_xip_addr - 24;
//    uint32_t model_info_phys_addr = virt_to_phys(model_info_xip_addr);

    // CGP - TODO -surely we should be placing model_info_xip_addr at the start of a EEPROM sector?
    // TODO - remove magic numbers
    model_info_xip_addr = MODEL_XIP_ADDR - MODEL_OLD_META_SIZE;
    model_info_phys_addr = virt_to_phys(model_info_xip_addr);

    if (hx_lib_spi_eeprom_word_read(spi_inst, model_info_phys_addr, stored_info, 8) != 0)  {
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
int write_persisted_model_info() {
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
    hx_lib_spi_eeprom_enable_XIP(spi_inst, false, FLASH_QUAD, false);

    uint32_t model_xip_addr = MODEL_XIP_ADDR;
    uint32_t model_info_xip_addr = model_xip_addr - MODEL_OLD_META_SIZE;
    uint32_t model_info_phys_addr = virt_to_phys(model_info_xip_addr);

    uint32_t info_to_write[2] = {(uint32_t)g_project_id, (uint32_t)g_deploy_version};
    if (hx_lib_spi_eeprom_word_write(spi_inst, model_info_phys_addr, info_to_write, 8) != 0)
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
#endif //OLD

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
        xprintf("SD TFLite header (first 1 bytes): ");
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
    uint8_t id_info = 0;
    int result;

    xprintf("=== FLASH DEBUG STATUS ===\n");
    xprintf("Flash initialized: %s\n", flash_initialized ? "YES" : "NO");

    // Disable XIP before reading ID
    enableXIP(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
        xprintf("Failed to take SPI mutex in debug_flash_status()\n");
        return;
    }

    result = hx_lib_spi_eeprom_read_ID(spi_inst, &id_info);

    xSemaphoreGive(xSPIMutex);

    if (result == 0) {
        xprintf("SPI ID: 0x%02x, Flash ID: 0x%02X\n", spi_inst,  id_info);
    }
    else {
        xprintf("SPI ID: 0x%02x, error %d:\n", spi_inst, result);
    }
}

void test_basic_spi_operations()
{
    xprintf("=== TESTING BASIC SPI OPS ===\n");

    uint32_t test_addr = (MODEL_FLASH_ADDR + 4 * FLASH_SECTOR_SIZE) & ~(FLASH_SECTOR_SIZE - 1);

    int er = hx_lib_spi_eeprom_erase_sector(spi_inst, test_addr, FLASH_SECTOR);
    xprintf("Erase result: %d at 0x%08lX\n", er, test_addr);

    uint32_t test_data[4] = {0xDEADBEEF, 0x12345678, 0xABCDEF00, 0x11223344};

    // Test writing one word at a time
    xprintf("Writing one word at a time:\n");
    for (int i = 0; i < 4; i++)
    {
        int wr = hx_lib_spi_eeprom_word_write(spi_inst, test_addr + (i * 4), &test_data[i], 1);
        xprintf("  Write word %d (0x%08lX) to 0x%08lX: result %d\n",
                i, test_data[i], test_addr + (i * 4), wr);
    }

    // Test reading one word at a time
    xprintf("Reading one word at a time:\n");
    for (int i = 0; i < 4; i++)
    {
        uint32_t read_data = 0;
        int rr = hx_lib_spi_eeprom_word_read(spi_inst, test_addr + (i * 4), &read_data, 1);
        xprintf("  Read word %d from 0x%08lX: result %d, data 0x%08lX %s\n",
                i, test_addr + (i * 4), rr, read_data,
                (read_data == test_data[i]) ? "OK" : "FAIL");
    }
}

void compare_sd_spi_xip_chunked(const char *filename) {
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
    // TODO warning 1.5k taken from task stack for this:
    uint8_t sd_buf[FILE_CHUNK_SIZE] = {0};
    uint8_t spi_buf[FILE_CHUNK_SIZE] __attribute__((aligned(4))) = {0};
    uint8_t xip_buf[FILE_CHUNK_SIZE] = {0};

    DWORD offset = 0;
    int any_mismatch = 0;
    while (offset < fileSize)  {
        UINT to_read = (fileSize - offset > FILE_CHUNK_SIZE) ? FILE_CHUNK_SIZE : (fileSize - offset);
        // Read SD
        f_lseek(&file, offset);
        res = f_read(&file, sd_buf, to_read, &bytesRead);
        if (res != FR_OK || bytesRead != to_read)
        {
            xprintf("Failed to read model file at offset %lu\n", offset);
            break;
        }
        // // Read SPI
        if (hx_lib_spi_eeprom_word_read(spi_inst, MODEL_FLASH_ADDR + offset, (uint32_t *)spi_buf, to_read) != 0)
        {
            // if (hx_lib_spi_eeprom_word_read(spi_inst, MODEL_FLASH_ADDR + offset, (uint32_t*)spi_buf, (to_read + 3) / 4) != 0) {
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

/**
 * Loads model from the SD card to flash.
 *
 * Model is expected to be in /MANIFEST folder.
 *
 * Model is copied to flash and a pointer to the model is returned.
 */
static const tflite::Model *load_model_from_sd(char * filename) {

	if (copy_model_from_sd_to_flash(filename)) {
		xprintf("Copied %s to flash OK\n", filename);
	}
	else {
		xprintf("SD model->flash copy failed for %s\n", filename);
		return nullptr;
	}

	//	Now try to copy LABELS.TXT to the meta data area of teh XIP flash
	if (!copy_labels_from_sd_to_flash(filename)) {
		xprintf("SD labels->flash copy failed for %s\n", filename);
	}
	else {
		xprintf("Copied labels to flash for %s\n", filename);
	}

	return load_model_from_flash();

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

	xprintf("Init EEPROM...\r\n");

	// Create mutex on first call
	// TODO for consistenacy this should be in image_createTask()
	// semaphore vs mutex: https://chatgpt.com/share/69706528-d250-8005-973b-6ab43c1b4629
	// In FreeRTOS, a mutex is created in the given (unlocked) state.
	if (xSPIMutex == NULL)  {
		xSPIMutex = xSemaphoreCreateMutex();
		if (xSPIMutex == NULL) {
			xprintf("Failed to create SPI mutex\n");
			return -1;
		}
	}

	//	hx_lib_spi_eeprom_open(spi_inst);
	//    //hx_lib_spi_eeprom_open_speed(spi_inst, 6000000);
	//
	//	hx_lib_spi_eeprom_enable_XIP(spi_inst, true, FLASH_QUAD, true);

	// Take mutex
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
	// TODO - why?
	vTaskDelay(pdMS_TO_TICKS(10));

	if (hx_lib_spi_eeprom_read_ID(spi_inst, &id_info) != 0) {
		xprintf("Failed to read flash ID\n");
		xSemaphoreGive(xSPIMutex);
		return -1;
	}

	// Disable XIP mode, so that SPI access are possible
	if (hx_lib_spi_eeprom_enable_XIP(spi_inst, false, FLASH_QUAD, true) != 0) {
		xprintf("Failed to disable XIP mode\n");
		xSemaphoreGive(xSPIMutex);
		return -1;
	}

	xSemaphoreGive(xSPIMutex);

	xprintf("Flash ID: 0x%02X\n", id_info);
	flash_initialized = true;

	return 0;
}

// Erase the flash region covering the model info, header, AND the model data.
/**
 * Erase an area of flash for the model file plus meta information.
 * The meta information will be placed first.
 *
 * Flash is erased in chunks of 4k bytes
 *
 * @param model_size = size of model file read from SD card
 */
int erase_model_flash_area(uint32_t flashSizeRequired) {
    int32_t ret = 0;

	if (init_flash() != 0) {
		return -1;
	}

#ifdef OLD
	uint32_t model_xip_addr;
	uint32_t model_info_xip_addr; // 8 bytes for model info before filename header

	// Convert to physical addresses
	uint32_t model_phys_addr;      // e.g. 0x00200000
	uint32_t model_info_phys_addr; // e.g. 0x001FFFE8

	uint32_t erase_start;
	uint32_t total_length;
	uint32_t sectors_needed;
	uint32_t sector_addr;

    // Addresses (XIP virtual used as reference)
    model_xip_addr = MODEL_XIP_ADDR;
    model_info_xip_addr = model_xip_addr - MODEL_OLD_META_SIZE; // 8 bytes for model info before filename header

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
        ret = hx_lib_spi_eeprom_erase_sector(spi_inst, sector_addr, FLASH_SECTOR);
        xprintf("  Erase sector %lu addr 0x%08lX -> result %d\n",
                (unsigned long)i, sector_addr, ret);
        if (ret != 0) {
            xprintf("Failed to erase sector at address 0x%08lX\n", sector_addr);
            ret = -1;
            break;
        }
    }
#else
    uint32_t sectors_needed;
	uint32_t sector_addr;

    // Calculate the number of 4k sectors needed
    sectors_needed = align_up(flashSizeRequired, FLASH_SECTOR_SIZE) / FLASH_SECTOR_SIZE;

    sector_addr = virt_to_phys(MODEL_XIP_ADDR);           // e.g. 0x00200000

    xprintf("Erasing %d sectors from 0x%08x to cover %lu bytes\n",
    		sectors_needed, sector_addr, (unsigned long)flashSizeRequired);

    // Disable XIP before erase
    enableXIP(false);

    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
        xprintf("Failed to take SPI mutex in erase_model_flash_area()\n");
        return -1;
    }

    for (uint16_t i = 0; i < sectors_needed; i++) {
    	ret = hx_lib_spi_eeprom_erase_sector(spi_inst, sector_addr, FLASH_SECTOR);
    	xprintf("  Erase sector %d addr 0x%08x -> result %d\n", i, sector_addr, ret);
        if (ret != 0) {
            xprintf("Failed to erase sector at address 0x%08x\n", sector_addr);
            ret = -1;
            break;
        }
        sector_addr += FLASH_SECTOR_SIZE;
    }
#endif	// OLD

    xSemaphoreGive(xSPIMutex);
    return ret;
}

/**
 * Write the model to the flash from the SD card file
 *
 * Step 1: open the model file and determine its size
 * Step 2: Erase flash to make space for the model and the meta data
 * Step 3: Write the filename info into the meta data area (and verify it)
 * Step 4: Write the model itself to the flash
 * Step 5: close the file and report the results and return status
 *
 * @param filename - such as '/MANIFEST/12V34.TFL'
 * @return 0 for success, -1 for fail
 */
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

    // Step 1: open the model file and determine its size

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

    // Step 2: Erase flash to make space for the model and the meta data

    if (erase_model_flash_area(fileSize) != 0)  {
        xprintf("Failed to erase flash for model\n");
        f_close(&file);
        return -1;
    }

    // Step 3: Write the filename info into the meta data area (and verify it)

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
    enableXIP(false);

    memset(fname_header.bytes, 0, sizeof(fname_header.bytes));
    strncpy((char *)fname_header.bytes, base, sizeof(fname_header.bytes) - 1);
    xprintf("Writing filename header: %s\n", fname_header.bytes);

    // write header (filename) via SPI (4 words)
    int wr = hx_lib_spi_eeprom_word_write(spi_inst, header_phys_addr, fname_header.words, MODEL_XIP_INFO_SIZE);
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
    if (hx_lib_spi_eeprom_word_read(spi_inst, header_phys_addr, fname_verify.words, MODEL_XIP_INFO_SIZE) != 0) {
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

    // Step 4: Write the model itself to the flash

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
        enableXIP(false);

        xprintf("Writing %u bytes (aligned %u) to 0x%08lX\n",
                (unsigned)bytesRead, (unsigned)write_size, (unsigned long)flash_address);
        if (hx_lib_spi_eeprom_word_write(spi_inst,
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
        if (hx_lib_spi_eeprom_word_read(spi_inst,
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

    // Step 5: close the file and report the results and return

    f_close(&file);

    if (totalBytesRead == fileSize) {
        xprintf("Model successfully written to physical 0x%08lX (%lu bytes)\n",
                (unsigned long)MODEL_FLASH_ADDR, (unsigned long)fileSize);
#ifdef OLD
        // Persist the model info to flash so it survives reboots
        write_persisted_model_info();
#endif // OLD

        // Re-enable XIP mode after all writes complete
        if (enableXIP(true)) {
        	xprintf("XIP mode re-enabled\n");
        	return 0;
        }
        else {
        	return -1;
        }
    }

    xprintf("Incomplete write: %lu/%lu bytes\n",
            (unsigned long)totalBytesRead, (unsigned long)fileSize);
    return -1;
}

/**
 * checks if a model with a specified name is in the XIP flash
 *
 * @param filename - name of model to look for
 * @return true if present
 */
static bool is_model_in_flash(char *filename) {
    int ret = 0;
    ModelMetaData metaDataRam;
    uint32_t meta_physical_addr;
    uint8_t maxLabels;

    if (init_flash() != 0)  {
        return false;
    }

    debug_flash_status();	// print debug info

    // Round up meta size and model size to be a multiple of 4 bytes (word aligned)
    // Actually, no need to round up.
    uint32_t meta_length = align_up(sizeof(ModelMetaData), 4);

    //Sanity checks
    if ((meta_length & 0x3) != 0) {
        return -1;  // size not word-aligned (should never happen)
    }

    // The meta data is written at the start of the XIP model area
    meta_physical_addr = virt_to_phys(MODEL_XIP_ADDR);           // e.g. 0x00200000

    // Ensure XIP is disabled before SPI access
    enableXIP(false);

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE) {
        xprintf("Failed to take SPI mutex for is_model_in_flash\n");
        return false;
    }

    // Read the flash to the local meta data structure
    ret = hx_lib_spi_eeprom_word_read(spi_inst, meta_physical_addr, (uint32_t *)&metaDataRam, meta_length);

    // TODO check all this giving and taking...
    xSemaphoreGive(xSPIMutex);

    if (ret != 0) {
    	return false;
    }

	// Enable XIP mode to permit memory access to the XIP Flash
    if (enableXIP(true)) {
		xprintf("XIP mode re-enabled\n");
    }
    else {
    	// error - should not happen
    	return false;
    }

	XP_LT_GREY;
	xprintf("Meta data found in flash\n");
	printf_x_printBuffer((const uint8_t *)&metaDataRam, sizeof(ModelMetaData));

	// Small delay for buffer to print
	vTaskDelay(pdMS_TO_TICKS(10));

	XP_WHITE;
	// Check the magic word
	if (metaDataRam.magic != LABEL_MAGIC) {
    	xprintf("Missing signature 0x%08x\n", LABEL_MAGIC);
    	return false;
	}

	printf("Magic: 0x%08x Model '%s' has %d labels of %d bytes:\n",
			(int) metaDataRam.magic, metaDataRam.modelName,
			metaDataRam.class_count, metaDataRam.label_len);

	maxLabels = metaDataRam.class_count;
	if (maxLabels > MAX_CLASSES) {
		maxLabels = MAX_CLASSES;
	}

	for (uint8_t i=0; i < maxLabels; i++) {
		xprintf("%d = '%s'\n", i, metaDataRam.labels[i]);
	}

    // Compare filename
    // Use the filename length of 13 bytes - IMAGEFILENAMELEN
    if (strncmp(filename, metaDataRam.modelName , IMAGEFILENAMELEN) == 0) {
    	xprintf("Model file name in flash is '%s'\n", filename);
    	return true;
    }
    else {
    	char flashName[IMAGEFILENAMELEN];
    	strncpy(flashName, metaDataRam.modelName, IMAGEFILENAMELEN - 1);
    	flashName[IMAGEFILENAMELEN - 1] = '\0';	// Be safe
    	xprintf("Model file name in flash is '%s', not '%s'\n", flashName, filename);
    	return false;
    }
}

/**
 * checks if a model with a specified name is in the SD card
 *
 * Search for the fike in the /MANIFEST folder
 *
 * @param filename - name of model to look for
 * @return true if present
 */
static bool is_model_in_sd(char * filename) {
	char manifest_path[10 + IMAGEFILENAMELEN];			// let's restrict this to "/12345678/" then the filename, so 10 + IMAGEFILENAMELEN
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
 * Search for 'TFL3' at the right place in flash.
 *
 * Tested using SPI read of the hearder
 *
 * @return true of a model appears present
 */
static bool valid_model_in_flash(void) {
	uint32_t model_physical_addr;
    // The 'TLF3' string should be at spi_hdr[4] for 4 bytes
    uint8_t spi_hdr[8] __attribute__((aligned(4))) = {0};

    // Ensure XIP is disabled before SPI access
    if (!enableXIP(false)) {
    	xprintf("Failed to disable XIP %d\n");
    	xSemaphoreGive(xSPIMutex);
    	return false;
    }

    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
        xprintf("Failed to take SPI mutex for valid_model_in_flash()\n");
        return false;
    }

    // The meta data is written at the start of the XIP model area
    // The model starts on a 16-byte boundary beyond the meta data
    model_physical_addr = virt_to_phys(MODEL_XIP_ADDR) + align_up(sizeof(ModelMetaData), 16);

    if (hx_lib_spi_eeprom_word_read(spi_inst, model_physical_addr, (uint32_t *)spi_hdr, sizeof(spi_hdr)) != 0) {
        xprintf("Failed to read SPI header\n");
        xSemaphoreGive(xSPIMutex);
        return false;
    }

    if (memcmp(&spi_hdr[4], "TFL3", 4) != 0) {
        // no match
        xprintf("No valid TFLite model in flash\n");
        xSemaphoreGive(xSPIMutex);
        return false;
    }

    xSemaphoreGive(xSPIMutex);
    return true;
}

// Modified load function - test BOTH SPI and XIP access
/**
 * checks if a plausible model exists in the flash memory.
 *
 * If so, the XIP access is enabled and a pointer to the start of teh model is returned.
 *
 * @return pointer to the model, or null
 */
static const tflite::Model *load_model_from_flash(void) {
#if 0
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
	hx_lib_spi_eeprom_enable_XIP(spi_inst, false, FLASH_QUAD, false);

	// Step 1: Read via SPI to confirm data exists

	uint8_t spi_hdr[32] __attribute__((aligned(4))) = {0};
	if (hx_lib_spi_eeprom_word_read(spi_inst, phys, (uint32_t *)spi_hdr, sizeof(spi_hdr)) != 0) {
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

	if (hx_lib_spi_eeprom_enable_XIP(spi_inst, true, FLASH_QUAD, true) != 0) {
		xprintf("Failed to enable XIP mode\n");
		xSemaphoreGive(xSPIMutex);
		return nullptr;
	}

	xSemaphoreGive(xSPIMutex);

	// Step 3: Verify XIP mapping works
	volatile uint8_t *xip_hdr = (volatile uint8_t *)model_xip_address;
	xprintf("XIP@0x%08lX: ", model_xip_address);
	for (int i = 0; i < 32; i++) {
		xprintf("%02X ", xip_hdr[i]);
	}
	xprintf("\n");

	// Step 4: Check if XIP matches SPI data
	bool xip_matches = (memcmp((const void *)spi_hdr, (const void *)xip_hdr, 32) == 0);

	if (xip_matches)  {
		xprintf("XIP mapping works! Using virtual address\n");
		return tflite::GetModel((const void *) model_xip_address);
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

	if (hx_lib_spi_eeprom_enable_XIP(spi_inst, true, FLASH_QUAD, true) != 0) {
		xprintf("Failed to enable XIP mode\n");
		xSemaphoreGive(xSPIMutex);
		return nullptr;
	}

	xSemaphoreGive(xSPIMutex);

	// Step 4: Check if XIP matches SPI data
	bool xip_matches = (memcmp((const void *)spi_hdr, (const void *)virt, 32) == 0);

	if (xip_matches)  {
		return tflite::GetModel((const void *) model_xip_address);
	}
	else  {
		xprintf("XIP mapping mismatch!\n");
		return nullptr;
	}
#endif // 0

#else
	// Virtual address of the start of the TFTL model
	uint32_t model_xip_address;

	if (init_flash() != 0) {
		xprintf("Flash init failed\n");
		return nullptr;
	}

	// Step 1: Read model via SPI to confirm data exists

	// Check if we have valid TFLite header via SPI
	if (!valid_model_in_flash()) {
		xprintf("No valid TFLite model in flash\n");
		return nullptr;
	}

	// Step 2: Enable XIP mode for memory-mapped access

	if (enableXIP(true)) {
		// The model starts on a 16-byte boundary beyond the meta data
		model_xip_address = MODEL_XIP_ADDR + align_up(sizeof(ModelMetaData), 16);
		return tflite::GetModel((const void *) model_xip_address);
	}
	else {
		return nullptr;
	}
	//
	//    // Step 4: Check if XIP matches SPI data
	//    bool xip_matches = (memcmp((const void *)spi_hdr, (const void *)virt, sizeof(spi_hdr)) == 0);
	//
	//    if (xip_matches)  {
	//        return tflite::GetModel((const void *) virt);
	//    }
	//    else  {
	//        xprintf("XIP mapping mismatch!\n");
	//        return nullptr;
	//    }
#endif //
}

/**
 * Enable or disable XIP
 *
 * XIP must be enabled before accessing the model via memory references
 *
 * @param enable - true to enable, false to disable
 * @return true if OK
 */
static bool enableXIP(bool enable) {
    // Take semaphore
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
        xprintf("Failed to take SPI mutex in enableXIP()\n");
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

/********************************** Functions to support saving and fetching meta data from flash ********/


/**
 *
 * @param model_name - string containing model name e.g. 12V23.TFL
 * @param src_labels - array of strings containing class names, read from SD card LABELS.TXT
 * @param classCount - number of classes used
 */
void build_meta_data(ModelMetaData * metaDataRam, const char *model_name,
                        const char src_labels[MAX_CLASSES][MAX_LABEL_LEN],
                        uint16_t class_count) {

    metaDataRam->magic       = LABEL_MAGIC;
    metaDataRam->class_count = class_count;
    metaDataRam->label_len   = MAX_LABEL_LEN;

    // Copy model name e.g. 1234V567.TFL - 8.3 format
    strncpy(metaDataRam->modelName, model_name, MAX_MODEL_NAME_LEN - 1);

    // Copy label strings
    for (uint8_t i = 0; i < MAX_CLASSES; i++) {
        strncpy(metaDataRam->labels[i], src_labels[i], MAX_LABEL_LEN - 1);
    }

    // test that the structure is OK
	XP_LT_GREY;
	xprintf("Built this metadata:\n");
    printf_x_printBuffer((const uint8_t *)metaDataRam, sizeof(ModelMetaData));
	XP_WHITE;

//    /* Optional CRC over everything except crc field */
//
//	crc16_ccitt_generate(gWrite_buf, I2CFMT_PAYLOAD_OFFSET + length, &checksum);
//
//    model_labels_ram.crc =
//        crc32((uint8_t *)&model_labels_ram,
//              offsetof(FlashModelLabels, crc));
}

/**
 * Copies the meta object to flash
 *
 * The copy uses SPI to the physical address 0x00200000
 */
int32_t write_meta_data_to_flash(ModelMetaData * metaDataRam) {
    uint32_t meta_physical_addr;
    int32_t res;

    //const ModelMetaData *src = &label_table_ram; // label_table_ram
    uint32_t numBytes = sizeof(ModelMetaData);

    /* Sanity checks */
    if ((numBytes & 0x3) != 0) {
        return -1;  // size not word-aligned (should never happen)
    }

    // The meta data is written at the start of the XIP model area
    meta_physical_addr = virt_to_phys(MODEL_XIP_ADDR);           // e.g. 0x00200000

    if ((meta_physical_addr & 0x3) != 0) {
        return -2;  // address not word-aligned
    }

    // Disable XIP before SPI write
    enableXIP(false);
    res = hx_lib_spi_eeprom_word_write(spi_inst, meta_physical_addr, (uint32_t *)metaDataRam, numBytes);

    if (res !=0) {
    	xprintf("Failed to write meta data %d\n", res);
    }
    return res;
}

/**
 * Copies model file from the SD card to the flash memory
 *
 * c.f. load_model_from_sd_to_flash()
 *
 * Step 1: allocate buffers
 * Step 2: open the model file and determine its size
 * Step 3: Erase flash to make space for the model and the meta data
 * Step 4: Write the model itself to the flash
 * Step 5: close the file and report the results and return status
 *
 * @param filename - such as '12V34.TFL'
 * @return true on success
 */
static bool copy_model_from_sd_to_flash(char * filename) {
    FRESULT res;
    FIL file;
    UINT bytesRead;
    DWORD fileSize;

    uint32_t flash_address;
    uint32_t totalBytesRead;
    uint32_t write_size;
    int32_t result;
    uint32_t flashSizeRequired;

	char manifest_path[10 + IMAGEFILENAMELEN];			// let's restrict this to "/12345678/" then the filename, so 10 + IMAGEFILENAMELEN

	// Get absolute path to the SD card file
	snprintf(manifest_path, sizeof(manifest_path), "%s/%s", CONFIG_DIR, filename);

    if (init_flash() != 0)  {
    	return false;
    }

    // Step 1: allocate buffers

    // aligned buffers taken from the heap: prevent stack buffer overflow
    uint8_t *write_buf  = static_cast<uint8_t *>(pvPortMalloc(FILE_CHUNK_SIZE));
    uint8_t *verify_buf = static_cast<uint8_t *>(pvPortMalloc(FILE_CHUNK_SIZE));

    if (!write_buf || !verify_buf) {
        vPortFree(write_buf);
        vPortFree(verify_buf);
        xprintf("Failed to allocate write_buf[], verifybuf[]\n");
        return false;
    }

// Step 2: open the model file and determine its size

    res = f_open(&file, manifest_path, FA_READ);
    if (res != FR_OK)  {
        xprintf("Failed to open model file %s (error: %d)\n", manifest_path, res);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }

    fileSize = f_size(&file);
    if (fileSize == 0)  {
        xprintf("Model file is empty\n");
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }

    xprintf("Model file size: %lu bytes\n", fileSize);

// Step 2: Erase flash to make space for the model and the meta data
    // The model data must be pushed out to align on a 16 byte boundary
    flashSizeRequired = align_up(sizeof(ModelMetaData), 16) + fileSize;

    if (erase_model_flash_area(flashSizeRequired ) != 0)  {
        xprintf("Failed to erase flash for model\n");
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }

// Step 3: Write the model itself to the flash

    // The meta data is written at the start of the XIP model area
    // The model starts on a 16-byte boundary beyond the meta data
    flash_address = virt_to_phys(MODEL_XIP_ADDR) + align_up(sizeof(ModelMetaData), 16);

    xprintf("Writing model to 0x%08x\n", flash_address);

    totalBytesRead = 0;

    // Ensure XIP is disabled
    enableXIP(false);

    // Take semaphore for each chunk write????????????????
    if (xSemaphoreTake(xSPIMutex, portMAX_DELAY) != pdTRUE)  {
        xprintf("Failed to take SPI mutex for chunk write\n");
        f_close(&file);
        vPortFree(write_buf);
        vPortFree(verify_buf);
        return false;
    }


    while (1) {
    	// Clear the buffer
        memset(write_buf, 0, FILE_CHUNK_SIZE);

    	res = f_read(&file, write_buf, FILE_CHUNK_SIZE, &bytesRead);
    	if (res != FR_OK) {
    		// handle error ;
    	    f_close(&file);
    	    xSemaphoreGive(xSPIMutex);
    	    vPortFree(write_buf);
    	    vPortFree(verify_buf);
    	    return false;
    	}
    	if (bytesRead == 0) {
    		// EOF
    		break;
    	}

    	// Round up to a number of 32-bit words
    	write_size = align_up(bytesRead, 4);

        xprintf("Writing %d bytes (aligned %d) to 0x%08x\n", bytesRead, write_size, (unsigned long)flash_address);
        result = hx_lib_spi_eeprom_word_write(spi_inst,
                flash_address,
                (uint32_t *)write_buf,
                write_size);

        if (result != 0) {
            xprintf("Flash write failed at 0x%08\n", flash_address);
    		// handle error ;
    	    f_close(&file);
    	    xSemaphoreGive(xSPIMutex);
    	    vPortFree(write_buf);
    	    vPortFree(verify_buf);
            return false;
        }

        // verify the write...
        memset(verify_buf, 0, write_size);
        result = hx_lib_spi_eeprom_word_read(spi_inst,
                flash_address,
                (uint32_t *)verify_buf,
                write_size);

        if (result != 0)  {
            xprintf("Flash verify read failed at 0x%08\n", flash_address);
    	    f_close(&file);
    	    xSemaphoreGive(xSPIMutex);
    	    vPortFree(write_buf);
    	    vPortFree(verify_buf);
    	    return false;
        }

        if (memcmp(write_buf, verify_buf, write_size) != 0) {
        	xprintf("Verify FAIL at 0x%08\n", flash_address);
        	f_close(&file);
        	xSemaphoreGive(xSPIMutex);
        	vPortFree(write_buf);
        	vPortFree(verify_buf);
        	return false;
        }

        flash_address += write_size;
        totalBytesRead += bytesRead;
    }

    // Step 5: close the file and report the results and return status

    xSemaphoreGive(xSPIMutex);

    f_close(&file);
    vPortFree(write_buf);
    vPortFree(verify_buf);

    if (totalBytesRead == fileSize) {
    	xprintf("Model successfully written to 0x%08x (%lu bytes)\n",
    			(unsigned long)MODEL_FLASH_ADDR, (unsigned long)fileSize);

    	// Re-enable XIP mode after all writes complete
        enableXIP(true);

    	return true;
    }

    xprintf("Incomplete write: %lu/%lu bytes\n",
    		(unsigned long)totalBytesRead, (unsigned long)fileSize);
    return false;
}

/**
 * Copies model labels from the SD card to the flash memory
 *
 * @param modelName = same as filename e.g. '12345V67.TFL'
 * @return true on success
 */
static bool copy_labels_from_sd_to_flash(char * modelName) {
    ModelMetaData metaDataRam;

    // Initialise
    memset(&metaDataRam, 0, sizeof(ModelMetaData));

    // TODO - copy the labels into metaDataRam
	load_labels_from_manifest();

	// Place fields into metaDataRam
	build_meta_data(&metaDataRam, modelName, g_labels, g_label_count);

	// Write metaDataRam to flash
	write_meta_data_to_flash(&metaDataRam);

    // test that the structure is OK
	// Enable XIP mode to permit memory access to the XIP Flash

    if (enableXIP(true)) {
    	xprintf("XIP mode re-enabled\n");
    }
    else {
    	// error should not happen
    }

	XP_LT_GREY;
	xprintf("Meta data now in flash:\n");
    printf_x_printBuffer((const uint8_t *)metaDataFlash, sizeof(ModelMetaData));
	XP_WHITE;

	// is there a chance of returning false:
	return true;
}

/********************************** Public Functions  *************************************/

#ifdef OLD
// Add a flag to track if we've already loaded from SD card
// TODO - omit as never used
static bool model_loaded_from_sd = false;

// Optional model number parameter, default to 1
/**
 *	Initialise TFLM model
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

	interpreter = static_interpreter;
	input = static_interpreter->input(0);
	output = static_interpreter->output(0);
	xprintf("DEBUG: 4\n");

	return 0;
}

#else

/**
 *	Initialise TFLM model
 *
 * 	@param security_enable
 *	@param privilege_enable
 *	@param project_id
 *	@param deploy_version
 *	@param woken - used to print info only on cold boots
 *	@return 0 for OK
 */
int cv_init(bool security_enable, bool privilege_enable, int project_id, int deploy_version, APP_WAKE_REASON_E woken) {
	char filename[IMAGEFILENAMELEN];	// for 8.3 this is 13, including the training \0
	uint32_t model_xip_address;

	// Allow for printing only on cold boots
	coldBoot = (woken == APP_WAKE_REASON_COLD);

	XP_GREEN;
	xprintf("\nInitialising NN\n");
	XP_WHITE;

	// Enforce clean state
	cv_deinit();

	if (_arm_npu_init(security_enable, privilege_enable) != 0) {
		return -1;
	}

	// Create a file name based on the project_id and deploy_version parameters
	snprintf(filename, sizeof(filename), "%dV%d.TFL", (int) project_id, (int) deploy_version);

	xprintf("Looking for model '%s' in flash or SD card\n", filename);

	// Option 1: named model is in flash
	if (is_model_in_flash(filename)) {
		xprintf("Flash already contains model '%s'; loading from flash.\n", filename);
		model = load_model_from_flash();
	}
	// Option 2: named model is on SD card
	else if (is_model_in_sd(filename)) {
		model = load_model_from_sd(filename);
	}
	// Option 3: any model is in flash
	else if(valid_model_in_flash()) {
		// Return any valid model
	    // The model starts on a 16-byte boundary beyond the meta data
		xprintf("Found another valid model\n");
		if (enableXIP(true)) {
			// The model starts on a 16-byte boundary beyond the meta data
			model_xip_address = MODEL_XIP_ADDR + align_up(sizeof(ModelMetaData), 16);
			model = tflite::GetModel((const void *) model_xip_address);
		}
		else {
			// Only happens with error
			xprintf("Error with enableXIP() \n");
			return -1;
		}
	}
	// Option 4: no model is available
	else {
		xprintf("Failed to load model\n");
		return -1;
	}

#ifdef PRINTMODELFINGERPRINT
	// Print information about the model
	tflm_print_ethosu_fingerprint(model);
#else
	xprintf("Model schema version: %d\n", model->version());
#endif // PRINTMODELFINGERPRINT

	// NOTE: some models might need more than  1 of these
	op_resolver_ptr = new tflite::MicroMutableOpResolver<1>();
	if (!op_resolver_ptr) {
		return -1;
	}

	if (op_resolver_ptr->AddEthosU() != kTfLiteOk) {
		xprintf("Failed to add Arm NPU support to op resolver.\n");
		return -1;
	}

	interpreter = new tflite::MicroInterpreter(
			model,
			*op_resolver_ptr,
			tensor_arena_buf,
			tensor_arena_size,
			&micro_error_reporter);

	if (!interpreter) {
		return -1;
	}

	if (interpreter->AllocateTensors() != kTfLiteOk) {
		return -1;
	}

	input  = interpreter->input(0);
	output = interpreter->output(0);

	return 0;
}
#endif // OLD


// Robust deinit: safely release interpreter and tensor resources before model reloads
int cv_deinit(void) {

#ifdef OLD
    // Disable interrupts or enter critical section if needed (to prevent concurrent access)
    taskENTER_CRITICAL();

    // Free the TFLite interpreter if it exists
    if (interpreter != nullptr)
    {
        delete interpreter;
        interpreter = nullptr;
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

#else
    if (interpreter) {
    	delete interpreter;
    	interpreter = nullptr;
    }

    if (op_resolver_ptr) {
    	delete op_resolver_ptr;
    	op_resolver_ptr = nullptr;
    }

    model = nullptr;

    // IMPORTANT: clear tensor arena
    memset(tensor_arena_buf, 0, tensor_arena_size);

    // Reset tensor pointers
    input = nullptr;
    output = nullptr;

    // Optional: shut down Ethos-U if required
    //_arm_npu_deinit();
    return 0;
#endif // OLD
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

        //const char *label = (g_labels_loaded && i < g_label_count) ? g_labels[i] : nullptr;
//        const char *label = (g_labels_loaded && (i < metaDataFlash->class_count))
//        		? metaDataFlash->labels[i] : nullptr;

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

