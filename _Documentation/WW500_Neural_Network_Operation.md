# WW500 Neural Network Operation
#### CGP — 22 April 2026

---

## Overview

The WW500 board contains two processors connected by I2C:

| Processor | Role | I2C |
|-----------|------|-----|
| nRF52832 (BLE processor) | BLE radio, LED control, power management | Master |
| HX6538 (AI processor) | Camera, neural network inference, SD card, JPEG | Slave |

The AI processor includes an **Arm Ethos-U55 NPU** (Neural Processing Unit) that accelerates TensorFlow Lite Micro (TFLM) inference. Models are compiled for this hardware using the Vela compiler and stored in the on-board SPI flash in a region that the AI processor can execute directly (XIP — eXecute In Place).

This document describes:
- How to install and update a neural network model
- How to interpret the console output during startup and inference
- How results are signalled to the BLE processor and stored in JPEG EXIF data
- A developer reference covering the key source files and functions

---

## 1. CONFIG.TXT Model Parameters

Three entries in CONFIG.TXT control model selection and detection behaviour.

| Index | Parameter | Default | Meaning |
|-------|-----------|---------|---------|
| 14 | `OP_PARAMETER_MODEL_PROJECT` | 0 | Model project ID. **0 means no model is loaded.** |
| 15 | `OP_PARAMETER_MODEL_VERSION` | 0 | Model version number. |
| 16 | `OP_PARAMETER_MODEL_THRESHOLD` | 0 | Detection logit threshold (0–127). |

The project ID and version together determine the model filename using the pattern:

```
{project_id}V{version}.TFL
```

For example, project ID 1, version 3 gives the filename `1V3.TFL`.

If `OP_PARAMETER_MODEL_PROJECT` is 0 the NN system is skipped entirely and no model is loaded.

### Detection threshold

`OP_PARAMETER_MODEL_THRESHOLD` is compared against the raw int8 logit of class 1 (the "target" class). If the logit exceeds the threshold the target is considered detected. The default of 0 means any positive logit triggers a detection — effectively very low sensitivity. Increase this value to require higher confidence before declaring a detection.

---

## 2. SD Card Files

Model files and their companion label files live in the `/MANIFEST` directory on the SD card.

| File | Purpose | Example |
|------|---------|---------|
| `{project}V{version}.TFL` | Vela-compiled TFLite model | `1V3.TFL` |
| `{project}V{version}.TXT` | Class labels, one per line | `1V3.TXT` |

The label file is plain text with one label per line, in the same order as the model's output classes. For example:

```
No Person?
Person!!!
```

These are loaded when a new model is written to flash and are stored in the flash metadata so that they are available even when the SD card is absent.

---

## 3. Flash Memory Layout

The external 16 MB SPI flash is partitioned as follows:

| Physical address | Size | Contents |
|-----------------|------|---------|
| 0x00000000–0x000FFFFF | 1 MB | Firmware Slot A |
| 0x00100000–0x001FFFFF | 1 MB | Firmware Slot B |
| **0x00200000–0x00EFFFFF** | **13 MB** | **NN model area** |
| 0x00F00000–0x00FEFFFF | 1 MB | Reserved |
| 0x00FFF000–0x00FFFFFF | 4 KB | Boot-slot selector |

When XIP is enabled, the model area is accessed at virtual address **0x3A200000**.

### Model area layout

The model area holds two items back-to-back:

```
0x3A200000   ModelMetaData struct  (magic, class count, model name, labels)
             [ 16-byte alignment padding ]
0x3A200xxx   Raw Vela TFLite model data
```

The `ModelMetaData` structure contains:

| Field | Type | Value / Example |
|-------|------|-----------------|
| `magic` | uint32_t | 0x4C41424C ("LABL" little-endian) |
| `class_count` | uint16_t | Number of output classes |
| `label_len` | uint16_t | 20 (MAX_LABEL_LEN) |
| `modelName` | char[13] | "1V3.TFL" |
| `labels[16][20]` | char | Class label strings |
| `crc` | uint32_t | Reserved, 0 |

### Reading the metadata hex dump

The cold-boot console prints the raw metadata bytes. Here is an example with annotations:

```
000: 4c 42 41 4c 02 00 14 00  31 56 33 2e 54 46 4c 00 LBAL....1V3.TFL.
     \_________/ \_/ \_/ \_________________________________/
        magic    cc  ll         modelName "1V3.TFL"
     (0x4C41424C) 2  20

010: 00 00 00 00 00 4e 6f 20  50 65 72 73 6f 6e 3f 00 .....No Person?.
                   \___________label[0] "No Person?__________/

020: 00 00 00 00 00 00 00 00  00 50 65 72 73 6f 6e 21 .........Person!
     \___________________/   \________label[1]_______
       (padding to 20 B)

030: 21 21 00 00 00 00 00 00  00 00 00 00 00 00 00 00 !!..............
     \_/
     "!!!" (end of "Person!!!")
```

`cc` = class_count = 2, `ll` = label_len = 0x14 = 20.

---

## 4. Model Loading Sequence

At each cold boot `cv_init()` follows this priority order:

1. **Named model in flash** — the filename stored in the flash metadata matches what CONFIG.TXT requests. Model is loaded directly (no SD card required).
2. **Named model on SD card** — the `.TFL` file is present in `/MANIFEST`. It is copied to flash (erasing the old model first), then the companion `.TXT` labels are stored in the flash metadata.
3. **Any valid model in flash** — the correct filename is absent but a valid TFLite model exists. Used as a fallback.
4. **No model** — NN processing is disabled for this session.

---

## 5. Console Output at Cold Boot

A cold boot occurs when the device powers on from completely unpowered state (as opposed to a warm boot from deep-power-down sleep). Verbose NN diagnostics are printed only on cold boots.

### Case A: model already in flash

```
Initialising NN with 2412/ETHOS-U 2411 library. Arena size 524288
Ethos-U55 device initialised
Looking for model '1V3.TFL' in flash or SD card
Flash ID 0xEF Initialised
XIP mode re-enabled
Meta data found in flash
000: 4c 42 41 4c 02 00 14 00  31 56 33 2e 54 46 4c 00 LBAL....1V3.TFL.
010: 00 00 00 00 00 4e 6f 20  50 65 72 73 6f 6e 3f 00 .....No Person?.
020: 00 00 00 00 00 00 00 00  00 50 65 72 73 6f 6e 21 .........Person!
030: 21 21 00 00 00 00 00 00  00 00 00 00 00 00 00 00 !!..............
<snip>
Magic: 0x4c41424c Model '1V3.TFL' has 2 labels of 20 bytes:
0 = 'No Person?'
1 = 'Person!!!'
Model file name in flash is '1V3.TFL'
Flash already contains model '1V3.TFL'; loading from flash.
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 1
  Op 0: CUSTOM (ethos-u), weight bytes: 250576
Input tensor   : i8 [1,96,96,1]
Output tensor  : i8 [1,2]
Total weight bytes : 250612
---------------------------------
There are 2 classes (2)
Initialised neural network.
NN Initialisation took 57ms TODO - consider doing this after taking the picture!
```

**What each line means:**

| Line | Meaning |
|------|---------|
| `Initialising NN with 2412/ETHOS-U 2411 library` | TFLM library version tag. `Arena size 524288` is the tensor workspace (512 KB) allocated by the linker script. |
| `Ethos-U55 device initialised` | The NPU hardware and its IRQ handler are ready. |
| `Looking for model '1V3.TFL'` | Project ID 1, version 3, derived from CONFIG.TXT. |
| `Flash ID 0xEF Initialised` | SPI EEPROM identified (0xEF = Winbond). Flash driver is ready. |
| `XIP mode re-enabled` | Flash switched back to memory-mapped mode after SPI initialisation. |
| `Meta data found in flash` | Valid "LABL" magic found; hex dump follows. |
| `Magic: 0x4c41424c Model '1V3.TFL' has 2 labels` | Metadata parsed: 2 classes, each label is 20 bytes. |
| `0 = 'No Person?'` / `1 = 'Person!!!'` | Class names read from flash metadata. |
| `Flash already contains model '1V3.TFL'` | Filename in metadata matches what was requested — no SD card copy needed. |
| `TFLM Ethos-U55 Model Fingerprint` | Model structure decoded from the FlatBuffers schema. |
| `Schema version : 3` | TFLite schema version — must match the library. |
| `Op 0: CUSTOM (ethos-u)` | The model contains one operator delegated to the Ethos-U55 NPU. |
| `Input tensor : i8 [1,96,96,1]` | The model expects a batch of 1, 96×96 grayscale image, int8. |
| `Output tensor : i8 [1,2]` | Two int8 class scores (logits) are produced per image. |
| `There are 2 classes (2)` | Count from the output tensor matches the metadata label count. A mismatch here is printed in red as a warning. |
| `NN Initialisation took 57ms` | Time from start of `cv_init()` to completion (model already in flash is fast; writing from SD can take several seconds — see section 6). |

---

## 6. Console Output When Updating the Model from SD Card

Place the new model files in the `/MANIFEST` directory, 
update `OP_PARAMETER_MODEL_PROJECT` and `OP_PARAMETER_MODEL_VERSION` 
in CONFIG.TXT, then re-boot the device (cold boot or warm boot). 
The system will detect the mismatch and update automatically.

```
Initialising NN with 2412/ETHOS-U 2411 library. Arena size 524288
Ethos-U55 device initialised
Looking for model '1V1.TFL' in flash or SD card
Flash ID 0xEF Initialised
XIP mode re-enabled
Model file name in flash is '1V3.TFL', not '1V1.TFL'
Looking for '/MANIFEST/1V1.TFL'
Model file size: 251568 bytes
Erasing 4 x 64KB blocks from 0x00200000 to cover 251920 bytes
  Erase block 0 addr 0x00200000 -> result 0
  Erase block 1 addr 0x00210000 -> result 0
  Erase block 2 addr 0x00220000 -> result 0
  Erase block 3 addr 0x00230000 -> result 0
Writing model to 0x00200160
Writing 4096 bytes (aligned 4096) to 0x00200160
<snip>
Writing 1712 bytes (aligned 1712) to 0x0023d160
Model successfully written to 0x00200000 (251568 bytes)
Copied 1V1.TFL to flash OK
Looking for labels in /MANIFEST/1V1.TXT
Loaded 2 labels from /MANIFEST/1V1.TXT
Built this metadata:
000: 4c 42 41 4c 02 00 14 00  31 56 31 2e 54 46 4c 00 LBAL....1V1.TFL.
...
XIP mode re-enabled
Meta data now in flash:
000: 4c 42 41 4c 02 00 14 00  31 56 31 2e 54 46 4c 00 LBAL....1V1.TFL.
...
Copied labels to flash for 1V1.TFL
There are 2 classes (2)
Initialised neural network.
NN Initialisation took 4849ms TODO - consider doing this after taking the picture!
```

**What each key line means:**

| Line | Meaning |
|------|---------|
| `Model file name in flash is '1V3.TFL', not '1V1.TFL'` | The metadata in flash holds the old model name; it does not match the requested `1V1.TFL`. |
| `Looking for '/MANIFEST/1V1.TFL'` | SD card is searched for the new model. |
| `Model file size: 251568 bytes` | Size of the `.TFL` file on the SD card. |
| `Erasing 4 x 64KB blocks from 0x00200000` | Flash erased in 64 KB blocks. The number of blocks is rounded up to cover the metadata header plus the model data. `result 0` means success. |
| `Writing model to 0x00200160` | The model data is written starting at address 0x00200000 + 16-byte-aligned metadata size (0x160 = 352). |
| `Writing 4096 bytes (aligned 4096) to 0x...` | Data is written in 4 KB chunks; each chunk is read back and verified before proceeding. |
| `Model successfully written` | All bytes verified. |
| `Copied 1V1.TFL to flash OK` | Model copy complete. |
| `Looking for labels in /MANIFEST/1V1.TXT` | Companion label file is sought. |
| `Loaded 2 labels from /MANIFEST/1V1.TXT` | Both class names loaded from the `.TXT` file. |
| `Built this metadata:` | The `ModelMetaData` struct constructed in RAM before writing (hex dump). |
| `Meta data now in flash:` | Metadata read back from flash for verification (hex dump). |
| `NN Initialisation took 4849ms` | Most of this time is the flash erase + write + verify cycle. |

---

## 7. Console Output When the NN Runs

After each image is captured, it is passed to the NN. The following is printed regardless of boot type:

```
Input image is 640 x 480 (460800 bytes)
Input tensor is 96 x 96 (1 channels)
Model invoked.
DEBUG: cv_run says there are 2 classes
Class 0 'No Person?' = logit 71
Class 1 'Person!!!' = logit -71
Target object not detected.
```

Or if the target is detected:

```
Class 0 'No Person?' = logit -71
Class 1 'Person!!!' = logit 71
TARGET OBJECT DETECTED!
```

**What each line means:**

| Line | Meaning |
|------|---------|
| `Input image is 640 x 480` | Dimensions and byte size of the raw camera image. |
| `Input tensor is 96 x 96 (1 channels)` | The model requires a 96×96 single-channel (grayscale) image. The raw image is rescaled using bilinear interpolation and converted to int8. |
| `Model invoked.` | The Ethos-U55 NPU has completed the inference. |
| `DEBUG: cv_run says there are 2 classes` | Class count from the output tensor (temporary debug print). |
| `Class 0 'No Person?' = logit 71` | Raw int8 output score for class 0. Logits are in the range −128 to +127. Higher is more confident. Class labels come from the flash metadata. |
| `Class 1 'Person!!!' = logit -71` | Raw int8 output score for class 1. |
| `Target object not detected.` | Class 1 logit (−71) did not exceed `OP_PARAMETER_MODEL_THRESHOLD`. |
| `TARGET OBJECT DETECTED!` | Class 1 logit exceeded the threshold. |

### Understanding logit values

Logits are the raw, unnormalised scores from the final layer of the neural network, stored as int8 (−128 to +127). For a two-class model the two logits are approximately equal in magnitude and opposite in sign — a high positive score for one class means a low (negative) score for the other.

The `OP_PARAMETER_MODEL_THRESHOLD` (index 16 in CONFIG.TXT) is compared against the logit of class 1 (the "target" class). Examples:

| logit[1] | threshold | result |
|----------|-----------|--------|
| 71 | 0 | detected |
| −71 | 0 | not detected |
| 40 | 50 | not detected |
| 60 | 50 | detected |

A threshold of 0 (default) means any positive logit counts as a detection.

---

## 8. Results Sent to the BLE Processor

After each inference the AI processor sends a short ASCII message to the BLE processor over I2C using `sendMsgToMaster()`. The BLE processor uses these messages to flash LEDs and update the mobile app:

| Message | Meaning |
|---------|---------|
| `NN+` | Target detected (logit[1] > threshold) |
| `NN-` | Target not detected |


The BLE processor uses this information to flash the red or green LED briefly.

If the app is connected it receives the `NN+` and `NN-` messages.

The information is available to be sent via LoRaWAN to the cloud.

The BLE processor also receives messages at other times — for example a camera exposure summary after each capture, and update/erase completion notifications when a model change is performed.

---

## 9. NN Results in JPEG EXIF Data

Each saved JPEG contains an EXIF APP1 segment with custom tags that record the NN output.

### Tag 0xC000 — Raw NN data (always present)

A byte array containing the raw int8 logit values:

```
byte 0:  total payload length (= class count + 1)
byte 1:  class count
byte 2+: int8 logit for each class
```

This tag is always written and exists for backwards compatibility with earlier firmware.

### Tag 0x9286 — UserComment (present when a model is loaded)

A human-readable ASCII summary of the NN result. The format depends on the `USE_PERCENTAGE` build switch in `cvapp.h`:

**`USE_PERCENTAGE` disabled (current build)** — raw logit values, one per class:

```
No Person?: 71; Person!!!: -71;
```

**`USE_PERCENTAGE` enabled** — softmax-normalised confidence percentages (computed by `outputAsPercentage()` in `cvapp.cpp` using `tflite::reference_ops::Softmax()`), up to 4 classes:

```
No Person?: 98%; Person!!!: 2%;
```

### Tag 0xF200 — Deployment ID

The 128-bit deployment UUID set by the mobile app (e.g. `12345678-0000-0000-0000-000000abc666`). Present only when a deployment is active (i.e. non-zero UUID).

---

## 10. Developer Reference

### Key source files

| File | Language | Purpose |
|------|----------|---------|
| `cvapp.cpp` | C++ | TFLM model initialisation and inference; image rescaling; confidence calculation |
| `xip_manager.c` | C | SPI flash driver; XIP management; model and metadata copy from SD card to flash |
| `xip_manager.h` | C | `ModelMetaData` struct; `metaDataFlash` macro; public function declarations |
| `image_task.c` | C | FreeRTOS task; orchestrates capture → inference → EXIF → save; sends results to BLE |
| `fatfs_task.c` | C | SD card access; CONFIG.TXT read/write; label file loading |
| `common_config.h` | C | Build-time switches (e.g. `USE_PERCENTAGE`, camera type) |

### Key functions

#### `cvapp.cpp`

| Function | Signature | Description |
|----------|-----------|-------------|
| `cv_init()` | `int cv_init(bool sec, bool priv, uint16_t project_id, uint16_t deploy_version, APP_WAKE_REASON_E woken)` | Initialises the NPU and loads the model. Returns 0 on success, −1 if no model is available. Verbose diagnostics are printed only when `woken == APP_WAKE_REASON_COLD`. |
| `cv_run()` | `TfLiteStatus cv_run(int8_t *outCategories, uint8_t *categoriesCount)` | Rescales the current camera image, runs inference, and returns the int8 logit array. If `USE_PERCENTAGE` is defined, also applies softmax and prints confidence percentages. |
| `cv_deinit()` | `int cv_deinit(void)` | Frees the TFLM interpreter and op resolver; clears the tensor arena. Called before a model reload. |
| `cv_modelLoaded()` | `bool cv_modelLoaded(void)` | Returns true if a model is ready for inference. |
| `cv_newModel()` | `void cv_newModel(uint16_t project_id, uint16_t deploy_version)` | Called by image_task when a model update is requested via BLE. Internally calls `cv_init()` then updates CONFIG.TXT. |
| `cv_eraseModel()` | `void cv_eraseModel(void)` | Erases the model from flash. Useful for testing or starting fresh. |
| `cv_get_confidence_data()` | `bool cv_get_confidence_data(ClassConfidenceData *data)` | Copies the most recent softmax confidence percentages and labels (populated when `USE_PERCENTAGE` is defined). Used by `image_task.c` to write EXIF UserComment. |
| `cv_getLabel()` | `const char *cv_getLabel(uint8_t index)` | Returns the class label string from flash metadata. Available when `USE_PERCENTAGE` is not defined. |
| `img_rescale()` | `void img_rescale(...)` | Bilinear interpolation: resizes raw camera image (e.g. 640×480) to model input size (96×96) and converts to int8 (zero-centred at 128). |

#### `xip_manager.c` / `xip_manager.h`

| Function | Returns | Description |
|----------|---------|-------------|
| `xip_is_model_in_flash(filename, cold_boot)` | bool | Reads and validates the metadata magic; compares the stored model name against `filename`. Prints verbose hex dump when `cold_boot` is true. |
| `xip_is_file_in_sd(filename)` | bool | Checks whether `/MANIFEST/<filename>` exists on the SD card. |
| `xip_valid_model_in_flash()` | bool | Checks for the `TFL3` magic bytes at the expected model offset in flash. |
| `xip_get_model_xip_address()` | uint32_t | Validates the model, enables XIP, and returns the virtual address to pass to `tflite::GetModel()`. Returns 0 on failure. |
| `xip_copy_model_from_sd_to_flash(filename)` | bool | Opens `/MANIFEST/<filename>`, erases required flash blocks, writes the model in 4 KB chunks with per-chunk read-back verification. Does not write metadata. |
| `xip_copy_metadata_to_flash(modelName)` | bool | Loads labels from `/MANIFEST/<modelName>.TXT`, builds a `ModelMetaData` struct, and writes it to the start of the flash model area. |
| `xip_erase_model_flash_area(size)` | int | Erases the minimum number of 64 KB blocks to accommodate `size` bytes, starting at 0x00200000. |

#### `image_task.c`

| Function | Description |
|----------|-------------|
| `processNNOutput(outCategories, classCount)` | Compares logit[1] against `OP_PARAMETER_MODEL_THRESHOLD`; sends `NN+` or `NN-` to the BLE processor via I2C; prints detection result; increments counters in CONFIG.TXT. |
| `prepareJpegFile(outCategories, classCount, extraBlock)` | Assembles the two-buffer JPEG+EXIF output: calls `build_exif_segment()` then queues the write request for `fatfs_task`. |
| `build_exif_segment(outCategories, categoriesCount)` | Builds a TIFF-structured EXIF APP1 block in `exif_buffer[]`. Writes tag 0xC000 (raw logits), optionally 0x9286 (UserComment confidence string), and 0xF200 (deployment ID). |
| `sendMsgToMaster(str)` | Queues an I2C message to the BLE processor via `if_task`. |

### Thread safety

All SPI flash accesses (XIP enable/disable, read, write, erase) are serialised by an internal FreeRTOS mutex (`xSPIMutex`). XIP mode is always disabled before any SPI transfer and re-enabled afterwards.

### Build switches

| Switch | File | Effect |
|--------|------|--------|
| `USE_PERCENTAGE` | `cvapp.h` (currently commented out) | Enables softmax confidence calculation in `cv_run()` and percentage-format UserComment EXIF data. When disabled, the UserComment tag contains raw logits instead. |
| `PRINTMODELFINGERPRINT` | `cvapp.cpp` | Enables the TFLM model structure summary at cold boot (schema version, operators, tensor shapes). Enabled by default. |
| `TFLM_2412` | Makefile | Selects the newer TFLM 2412 / Ethos-U 2411 library with a slightly different `MicroInterpreter` API. |
| `EXTRARESOLVERS` | `cvapp.cpp` | Registers additional op resolvers (Pad, Transpose, BatchMatMul) for models that require them beyond the basic EthosU delegate. |

### `ModelMetaData` struct (`xip_manager.h`)

```c
typedef struct {
    uint32_t magic;                          // 0x4C41424C ("LABL")
    uint16_t class_count;                    // number of output classes
    uint16_t label_len;                      // bytes per label (MAX_LABEL_LEN = 20)
    char modelName[MAX_MODEL_NAME_LEN];      // 8.3 filename, e.g. "1V2.TFL"
    char labels[MAX_CLASSES][MAX_LABEL_LEN]; // up to 16 class labels × 20 bytes
    uint32_t crc;                            // reserved, written as 0
} ModelMetaData;
```

The macro `metaDataFlash` (defined in `xip_manager.h`) is a typed pointer directly into XIP-mapped flash at `MODEL_XIP_ADDR` (0x3A200000). It is valid only while XIP mode is enabled.

### Tensor arena

The tensor arena — the working memory that TFLM uses during inference — is allocated by the linker script between the symbols `__tensor_arena_start__` and `__tensor_arena_end__`. Its size is printed at startup (`Arena size 524288` = 512 KB). If a model requires more arena than is allocated, `AllocateTensors()` will fail.

---

*Document produced 22 April 2026. Reflects source code on branch `firmware_updates`.*
