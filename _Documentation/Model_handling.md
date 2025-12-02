# cvapp.cpp - Computer Vision Application Summary

## Overview

`cvapp.cpp` is the core machine learning inference engine for the Wildlife Watcher camera trap system. It manages TensorFlow Lite Micro model loading, execution, and result processing on the Himax WE2 processor with Arm Ethos-U55 NPU acceleration.

**File Location:** `EPII_CM55M_APP_S/app/ww_projects/ww500_md/cvapp.cpp`

**Primary Responsibilities:**
- Neural network model initialization and management
- Model loading from multiple sources (SD card, flash memory, compiled binary)
- Image preprocessing and inference execution
- Confidence score calculation using TFLite's softmax implementation
- Flash memory management for model persistence
- Dynamic model switching and versioning

---

## Key Components

### 1. Model Loading System

The system supports three model loading modes (configured via `MODEL_LOAD_MODE`):

#### a) **MODEL_FROM_C_FILE**
- Loads pre-compiled model embedded in firmware
- Uses `g_person_detect_model_data_vela` array
- Fastest boot time, no external storage required

#### b) **MODEL_FROM_FLASH**
- Loads model directly from SPI flash memory
- Uses XIP (Execute-In-Place) for zero-copy access
- Persistent across power cycles

#### c) **MODEL_FROM_SD_CARD** (Primary Mode)
- Loads models from SD card at runtime
- Supports dynamic model updates via manifest.zip
- Automatic flash caching for performance
- File format: `{ProjectID}V{Version}.tfl` (e.g., `1234V5.tfl`)

**Model Loading Flow:**
```
1. Extract manifest.zip → /Manifest/ directory
2. Parse ProjectID and DeployVersion from filename
3. Check if model exists in flash (filename comparison)
4. If not cached: Copy SD → Flash with verification
5. Enable XIP mode and load via memory-mapped access
6. Initialize TFLite interpreter
```

### 2. Flash Memory Management

#### **Physical Layout**
```
Flash Address Space:
  0x001FFFE8 - Model Info (8 bytes)
    [0-3]: project_id (uint32_t)
    [4-7]: deploy_version (uint32_t)
  0x001FFFF0 - Filename Header (16 bytes)
    ASCII filename (e.g., "1234V5.tfl")
  0x00200000 - Model Data Start
    TFLite model binary
  
Virtual (XIP) Address Space:
  0x3A1FFFE8 - Model Info
  0x3A1FFFF0 - Filename Header
  0x3A200000 - Model Data Start (XIP mapped)
```

#### **Key Functions**

**`init_flash()`**
- Initializes SPI EEPROM interface
- Creates mutex for thread-safe access
- Reads and verifies flash ID
- Must disable XIP before SPI operations

**`load_model_from_sd_to_flash(char *filename)`**
- Erases required flash sectors (4KB aligned)
- Writes model info, filename header, and model data
- Verifies each write operation
- Re-enables XIP mode after completion
- Returns 0 on success, -1 on failure

**`load_model_from_flash()`**
- Validates TFLite header magic bytes ("TFL3")
- Enables XIP mode for memory-mapped access
- Returns `tflite::Model*` pointer
- Zero-copy operation via virtual addressing

**`erase_model_flash_area(uint32_t model_size)`**
- Calculates required sectors (rounds up to 4KB)
- Erases model info + header + data regions
- Sector-by-sector erase with verification

**`is_model_in_flash(char *filename)`**
- Compares filename header in flash vs. requested
- Returns true if cached model matches

### 3. TensorFlow Lite Micro Integration

#### **Initialization (`cv_init`)**

```cpp
int cv_init(bool security_enable, bool privilege_enable, 
            int project_id, int deploy_version)
```

**Steps:**
1. Calculate tensor arena size from linker symbols
2. Read persisted model info from flash (if available)
3. Initialize Ethos-U55 NPU with IRQ handler
4. Load model based on configured mode
5. Load class labels from `/Manifest/labels.txt`
6. Validate model schema version
7. Create `MicroMutableOpResolver` with EthosU support
8. Allocate `MicroInterpreter` with tensor arena
9. Allocate tensors and store input/output pointers

**Memory Management:**
- **Tensor Arena:** Dynamic allocation between `__tensor_arena_start__` and `__tensor_arena_end__` (linker symbols)
- **Op Resolver:** Heap-allocated `MicroMutableOpResolver<1>` (persists for interpreter lifetime)
- **Interpreter:** Heap-allocated `MicroInterpreter` (managed via `int_ptr`)

#### **Inference Execution (`cv_run`)**

```cpp
TfLiteStatus cv_run(int8_t *outCategories, uint16_t categoriesCount)
```

**Processing Pipeline:**

1. **Image Preprocessing**
   - Rescale raw image to 96×96 using bilinear interpolation
   - Convert to int8 with zero-point adjustment (subtract 128)
   - Load directly into input tensor

2. **Model Invocation**
   - Execute `interpreter->Invoke()`
   - NPU accelerated via Ethos-U55 delegate

3. **Confidence Calculation** (NEW: Uses TFLite SDK)
   ```cpp
   // Dequantize int8 logits → float
   float logits_float[classes];
   for (int i = 0; i < classes; ++i) {
       logits_float[i] = (results[i] - zero_point) * scale;
   }
   
   // Apply TFLite reference softmax
   tflite::SoftmaxParams softmax_params;
   softmax_params.beta = 1.0f;
   tflite::RuntimeShape shape({1, classes});
   tflite::reference_ops::Softmax(softmax_params, shape, 
                                  logits_float, shape, probabilities);
   
   // Convert to percentage
   float confidence = probabilities[i] * 100.0f;
   ```

4. **Result Display**
   - Color-coded output:
     - **Green:** ≥50% confidence
     - **Yellow:** 20-49% confidence
     - **Red:** <20% confidence
   - Format: `Class 'label': XX.X% (raw: YY)`

5. **Data Storage**
   - Store confidence percentages and labels in `g_last_confidence_data`
   - Available for EXIF metadata embedding via `cv_get_confidence_data()`

#### **Cleanup (`cv_deinit`)**

```cpp
int cv_deinit()
```

- Enters critical section (disable interrupts)
- Deletes interpreter and op_resolver
- Resets tensor pointers
- Does NOT reset `flash_initialized` flag
- Exits critical section

---

## Key Data Structures

### **ClassConfidenceData**
```cpp
struct ClassConfidenceData {
    int class_count;                    // Number of classes (≤ MAX_CLASSES)
    int confidence_percent[MAX_CLASSES]; // Integer percentages (0-100)
    const char* labels[MAX_CLASSES];    // Pointers to label strings
};
```
**Global Instance:** `g_last_confidence_data`

### **Model Versioning**
```cpp
static int g_project_id = 0;        // Model project identifier
static int g_deploy_version = 0;    // Model deployment version
```

### **Labels Array**
```cpp
static char g_labels[MAX_LABELS][MAX_LABEL_LEN];  // 64 labels × 48 chars
static int g_label_count = 0;
static bool g_labels_loaded = false;
```

---

## Image Processing

### **Bilinear Interpolation (`img_rescale`)**

Rescales raw camera image (e.g., 640×480) to model input size (96×96):

**Algorithm:**
- Fixed-point arithmetic (8 fractional bits)
- 2D bilinear interpolation using 4 neighboring pixels
- Conversion to int8 with zero-point = 128
- Output range: [-128, 127]

**Formula:**
```
For each output pixel (x, y):
  1. Compute source coordinates with sub-pixel precision
  2. Find 4 neighboring pixels (floor/ceil in x and y)
  3. Compute interpolation weights (fraction_x, fraction_y)
  4. Weighted average: (1-fy)*[(1-fx)*p00 + fx*p10] + fy*[(1-fx)*p01 + fx*p11]
  5. Subtract 128 for int8 conversion
```

---

## Helper Functions

### **Model Information Management**

**`cv_get_model_info(int *project_id, int *deploy_version)`**
- Returns current model identifiers

**`cv_set_model_info(int project_id, int deploy_version)`**
- Updates model identifiers
- Resets `model_loaded_from_sd` flag if changed

**`read_persisted_model_info()`**
- Reads 8-byte model info from flash
- Validates against 0xFFFFFFFF (uninitialized)
- Returns 0 on success

**`write_persisted_model_info()`**
- Persists current model info to flash
- Called after successful SD → Flash copy

### **Confidence Data Access**

**`cv_get_confidence_data(ClassConfidenceData *data)`**
- Thread-safe copy of last inference results
- Used by EXIF metadata generation in `image_task.c`
- Returns true on success

### **Labels Loading**

**`load_labels_from_manifest_or_root()`**
- Prioritizes `/Manifest/labels.txt`
- Fallback to root directory
- Loads one label per line
- Calls `fatfs_load_labels()` from `fatfs_task.c`

### **File System Utilities**

**`file_exists(const char *path)`**
- Uses FatFS `f_stat()` to check file presence

---

## Debug Functions

### **`debug_flash_status()`**
- Prints flash initialization state
- Reads and displays flash ID
- Requires SPI mode (disables XIP)

### **`debug_sd_model_integrity(const char *filename)`**
- Validates TFLite magic bytes ("TFL3")
- Prints first 16 bytes of model header
- Checks file size

### **`compare_sd_spi_xip_chunked(const char *filename)`**
- Byte-by-byte comparison: SD ↔ SPI ↔ XIP
- Chunk size: 512 bytes
- Reports all mismatches with offsets

### **`test_basic_spi_operations()`**
- Erases test sector
- Writes/reads test pattern (0xDEADBEEF, etc.)
- Verifies read-back data

---

## Memory Layout

### **Linker Symbols**
```cpp
extern uint8_t __tensor_arena_start__;
extern uint8_t __tensor_arena_end__;
```
- Defined in linker script (`.ld` file)
- Tensor arena size calculated at runtime: `&end - &start`

### **Static vs. Dynamic Allocation**

**Static (BSS/Data):**
- `g_labels[64][48]` - Label storage
- `g_last_confidence_data` - Inference results
- `ethosu_drv` - NPU driver state

**Dynamic (Heap):**
- `MicroInterpreter` - TFLite interpreter
- `MicroMutableOpResolver` - Op registration

**Flash (XIP):**
- Model data accessed via virtual address
- Zero-copy execution

---

## Thread Safety

### **SPI Mutex**
```cpp
static SemaphoreHandle_t xSPIMutex = NULL;
```

**Protected Operations:**
- Flash initialization
- SPI read/write operations
- XIP mode switching
- Model info persistence

**Pattern:**
```cpp
xSemaphoreTake(xSPIMutex, portMAX_DELAY);
// ... SPI/flash operations ...
xSemaphoreGive(xSPIMutex);
```

---

## Error Handling

### **Return Codes**
- **0:** Success
- **-1:** Generic failure
- **TfLiteStatus:** `kTfLiteOk` or `kTfLiteError`

### **Critical Checks**
1. Flash initialization before any SPI operation
2. Model magic bytes validation ("TFL3")
3. Tensor allocation success
4. Write verification after flash operations
5. Schema version compatibility

### **Failure Recovery**
- Flash operations: Return error, caller decides retry
- Model loading: Falls back to different source if available
- Inference failure: Returns status, calling code handles

---

## Integration Points

### **Dependencies**

**External Modules:**
- `image_task.c` - Calls `cv_run()`, reads confidence data
- `fatfs_task.c` - Provides `fatfs_unzip_manifest()`, `fatfs_load_labels()`
- `cisdp_sensor.h` - Provides `app_get_raw_addr/width/height()`

**Libraries:**
- TensorFlow Lite Micro - Inference engine
- FreeRTOS - Task management, mutexes
- FatFS - SD card file system
- Ethos-U driver - NPU hardware acceleration

### **Public API**

```cpp
// Initialization & cleanup
int cv_init(bool security_enable, bool privilege_enable, 
            int project_id, int deploy_version);
int cv_deinit();

// Inference execution
TfLiteStatus cv_run(int8_t *outCategories, uint16_t categoriesCount);

// Model information
void cv_get_model_info(int *project_id, int *deploy_version);
void cv_set_model_info(int project_id, int deploy_version);
int get_model_id();

// Confidence data access
bool cv_get_confidence_data(ClassConfidenceData *data);
```

---

## Configuration

### **Compile-Time Constants**

```cpp
#define INPUT_SIZE_X 96          // Model input width
#define INPUT_SIZE_Y 96          // Model input height
#define LOCAL_FRAQ_BITS 8        // Fixed-point fractional bits

#define MAX_LABELS 64            // Maximum class labels
#define MAX_LABEL_LEN 48         // Maximum label string length
#define MAX_CLASSES 16           // Maximum classes for EXIF

#define VERIFY_CHUNK_SIZE 512    // Flash verification chunk size
```

### **Build Modes**

Set in `common_config.h`:
```cpp
#define MODEL_FROM_C_FILE 0
#define MODEL_FROM_FLASH 1
#define MODEL_FROM_SD_CARD 2

#define MODEL_LOAD_MODE MODEL_FROM_SD_CARD  // Active mode
```

### **Flash Configuration**

```cpp
static USE_DW_SPI_MST_E spi_id = USE_DW_SPI_MST_Q;  // SPI controller
#define MODEL_FLASH_ADDR 0x00200000             // Physical address
#define FLASH_SECTOR_SIZE 4096                  // 4KB sectors
```

---

## Recent Changes

### **Softmax Implementation Update**

**Previous:** Manual softmax calculation with `expf()` and custom scaling

**Current:** TFLite SDK reference implementation
- Uses `tflite::reference_ops::Softmax()`
- Proper dequantization with tensor params (`scale`, `zero_point`)
- Standard temperature (beta = 1.0)
- More accurate and maintainable

**Benefits:**
- ✅ Uses tested TensorFlow implementation
- ✅ Handles numerical stability internally
- ✅ Proper quantization parameter support
- ✅ Reduced code complexity (~30 lines removed)

---

## Performance Characteristics

### **Model Loading Times**
- **C File:** Instant (already in memory)
- **Flash (cached):** ~100ms (XIP enable + validation)
- **SD → Flash:** ~3-10s (depends on model size)

### **Inference Times**
- **96×96 input:** ~50-200ms (model dependent)
- **NPU acceleration:** 10-50× faster than CPU-only
- **Rescaling overhead:** ~5-10ms

### **Memory Usage**
- **Tensor Arena:** 125-200 KB (dynamic)
- **Model Storage:** 50-500 KB (flash)
- **Labels:** ~3 KB (64 × 48 bytes)
- **Stack:** <4 KB per call

---

## Troubleshooting

### **Common Issues**

**"Flash init failed"**
- Check SPI wiring and power
- Verify `USE_DW_SPI_MST_Q` configuration
- Ensure XIP is disabled before init

**"Failed to allocate tensors"**
- Tensor arena too small
- Check linker script memory allocation
- Verify model complexity fits available RAM

**"XIP mapping mismatch"**
- Flash write verification failed
- Try re-writing model to flash
- Check for hardware issues (bad sectors)

**"No valid TFLite model in flash"**
- Flash uninitialized (first boot)
- Model corrupted during write
- Check TFL3 magic bytes in header

### **Debug Output**

Enable verbose logging:
```cpp
xprintf("Model invoked.\n");
xprintf("Class '%s': %d.%d%% (raw: %d)\n", ...);
```

Color-coded confidence output helps identify issues:
- **Green text:** High confidence (expected for correct detections)
- **Yellow text:** Medium confidence (review thresholds)
- **Red text:** Low confidence (possible misclassification)

---

## Future Improvements

### **Potential Enhancements**

1. **Multi-model Support**
   - Maintain multiple cached models in flash
   - Dynamic switching without erase/rewrite

2. **Model Compression**
   - Support for compressed model formats
   - Decompression during load

3. **Warm-up Inference**
   - Run dummy inference after load to initialize NPU

4. **Metrics Collection**
   - Track inference time, confidence distributions
   - Model performance monitoring

5. **Fallback Mechanisms**
   - Automatic retry on inference failure
   - Graceful degradation if NPU unavailable

---

## References

### **Key Files**
- `cvapp.h` - Header with public API and data structures
- `image_task.c` - Caller of inference functions
- `common_config.h` - Build configuration
- Linker script - Memory layout definitions

### **External Documentation**
- TensorFlow Lite Micro: https://www.tensorflow.org/lite/microcontrollers
- Arm Ethos-U55: https://developer.arm.com/ip-products/processors/machine-learning/arm-ethos-u
- FatFS: http://elm-chan.org/fsw/ff/00index_e.html

---

*This documentation reflects the state of `cvapp.cpp` as of December 2025, incorporating the TFLite SDK softmax implementation and dynamic model loading features.*
