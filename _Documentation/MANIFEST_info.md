# Manifest Folder - SD Card Configuration

## Overview

The Manifest folder system provides a flexible way to configure the WW500 camera trap, supporting both AI-enabled and AI-disabled operation modes. The system automatically handles model deployment and configuration based on what files are present on the SD card.

NOTE: The MANIFEST folder must be compressed using STORE compression method only

---

## Directory Structure

```
SD Card Root
├── IMAGES000                         (Created automatically to store captures)
├── manifest.zip                      (Optional - extracted automatically on boot)
└── Manifest/                         (Created automatically if missing)
  ├── <model_id>_V<version_num>.TFL   (Optional - TensorFlow Lite model file)
  ├── HMSTB1.BIN                      (Optional - Register sensor settings)
  ├── MODEL.TFL                       (Optional - TensorFlow Lite model file)
  ├── LABELS.TXT                      (Optional - Class labels for model output)
  ├── CONFIG.TXT                      (Required - Device configuration file, auto generated if missing)
  ├── README.TXT                      
  └── config_file.md                  (Optional - User guide to operational parameter settings) 
```

---

## Scenarios

### Scenario 1: Model + Labels in Manifest.zip

**Setup:**
- Go to https://wildlifewatcher.streamlit.app/ to download your up-to-date manifest folder 
- Copy the downloaded manifest.zip file onto your SD card

**Behavior:**
1. On power-up, the firmware detects `manifest.zip` on the SD card root
2. The zip file is automatically extracted to `/MANIFEST/` directory
3. The firmware scans `/MANIFEST/` for any `.tfl` file
4. If found, the model is loaded and neural network inference is enabled
5. Labels from `labels.txt` are loaded and written to EXIF metadata
6. Captured images include:
   - EXIF tags with classification results
   - Confidence scores for each class
   - Human-readable labels in metadata

**Use Case:** Full AI-enabled target species classification with labeled results

**Example labels.txt:**
```
rat
no rat
```

---

### Scenario 2: No Model in Manifest.zip (Configuration Only)

**Setup:**
- Go to https://wildlifewatcher.streamlit.app/ and select the "no model" option to download your up-to-date manifest folder

**Behavior:**
1. On power-up, the firmware detects `manifest.zip` and extracts it
2. The firmware scans `/MANIFEST/` but finds no `.tfl` file
3. Neural network processing is **skipped** - no AI inference runs
4. Camera operates normally with both motion detection AND/OR time-lapse wake sources
   - Motion detection: Enabled if `OP_PARAMETER_MD_INTERVAL` > 0 (from CONFIG.TXT)
   - Time-lapse: Enabled if `OP_PARAMETER_TIMELAPSE_INTERVAL` > 0 (from CONFIG.TXT)
   - Both modes can be active simultaneously
5. Images are captured when motion is detected or at time-lapse intervals
6. EXIF metadata includes device info but no classification data
7. Processing is faster (~150ms saved per capture without NN execution)

**Use Case:** Simple camera trap operation without AI overhead, configuration management

---

### Scenario 3: No Manifest.zip (First Boot / Factory Reset)

**Setup:**
- Fresh SD card with no `manifest.zip` file

**Behavior:**
1. On power-up, firmware detects missing `/MANIFEST/` directory
2. `/MANIFEST/` directory is automatically created
3. A default `CONFIG.TXT` is generated with factory settings
4. No model is loaded - operates without AI inference
5. Camera operates with default wake source configuration:
   - Motion detection: Configured per `OP_PARAMETER_MD_INTERVAL` default
   - Time-lapse: Configured per `OP_PARAMETER_TIMELAPSE_INTERVAL` default
   - Capture behavior depends on CONFIG.TXT operational parameters

**Use Case:** Initial deployment, testing, or factory reset

---

### Scenario 4: Manifest Directory Already Exists

**Setup:**
- `/MANIFEST/` directory present from previous boot
- May contain model, labels, and config from earlier extraction

**Behavior:**
1. On power-up, firmware detects existing `/MANIFEST/` directory
2. **No extraction occurs** - existing files are preserved
3. If `.tfl` model exists, it is loaded and used
4. If `labels.txt` exists, labels are loaded
5. Configuration is read from existing `CONFIG.TXT`

**Use Case:** Normal operation after initial setup

**Note:** To update the model, either:
- Delete the `/MANIFEST/` directory and replace `manifest.zip`
- Directly update files in `/MANIFEST/` (advanced users)
- Update the parameters via the mobile app

---

## Model Loading Logic

The firmware uses a flexible model discovery system:

1. **Scans `/MANIFEST/` directory** for any file with `.tfl` extension
2. **No specific filename required** - first `.tfl` file found is used
3. **Fallback search**: If `/MANIFEST/` is empty, checks SD card root
4. **Model validation**: Verifies model format before loading
5. **Memory check**: Ensures sufficient RAM for model execution

### Model Matching
- Model filename is extracted (e.g., `my_model_id.tfl` → `my_model_id`)
- System checks if model matches what's already loaded in flash memory
- If different, model is loaded from SD card to internal memory
- If identical, uses cached version (faster boot)

---

## Label Loading

Labels are loaded from `/MANIFEST/labels.txt` if present:

- **Format**: One label per line, UTF-8 text
- **Order**: Must match model output class order (class 0 = first line)
- **Fallback**: If labels missing, EXIF uses generic class numbers (e.g., "Class 0", "Class 1")

---

## EXIF Metadata Behavior

### With Model + Labels:
```
UserComment: "NN: rat (92%), no rat (8%)"
Tag 0xF300: 92  (confidence for class 0)
Tag 0xF301: 8   (confidence for class 1)
...
```

### Without Model:
```
UserComment: "NN: skipped (no model)"
(No confidence tags written)
```

---

## Configuration File (CONFIG.TXT)

**Location**: Always stored in `/MANIFEST/CONFIG.TXT`

**Format**: Key-value pairs, one per line

**Persistence**: State is saved after each capture cycle

See `Operational_Parameters.md` for more.

---

## File System Case Sensitivity

Firmware is compiled to 8.3 file name format. Ensure all capitals (eg. CONFIG.TXT)

There is a failsafe method to support all manifest.zip case scenarios:

**Manifest.zip variants (all supported):**
- `/MANIFEST.ZIP`
- `/Manifest.zip`
- `/manifest.zip`
- `/MANIFEST.zip`
- `/Manifest.ZIP`
- `/manifest.ZIP`

**Directory:**
- Created as `/MANIFEST/` (upper case)
- Case-insensitive file lookups

**Config file:**
- Searched as `/MANIFEST/CONFIG.TXT`
- Uppercase convention for consistency

---

## Troubleshooting

### Model Not Loading
1. Check that `.tfl` file exists in `/MANIFEST/`
2. Verify model is TensorFlow Lite format (not TensorFlow)
3. Ensure model is quantized (int8) - float models not supported
4. Check serial console for error messages during boot

### Labels Not Appearing in EXIF
1. Verify `labels.txt` exists in `/MANIFEST/`
2. Check label count matches model output classes
3. Ensure UTF-8 encoding (no BOM)
4. Verify line endings (LF or CRLF both work)

### Manifest.zip Not Extracting
1. Ensure zip uses **STORE method** (no compression)
2. Maximum file size limits apply
3. Check SD card filesystem integrity
4. Verify sufficient free space on SD card

### Images Corrupted
1. Ensure firmware includes semaphore-based buffer protection
2. Check SD card write speed (Class 10 or better recommended)
3. Verify power supply stability during writes

---

## Summary Table

| Manifest.zip Contents | Model Loaded | AI Inference | Labels in EXIF | Capture Speed |
|----------------------|--------------|--------------|----------------|---------------|
| Model + Labels + Config | ✅ Yes | ✅ Enabled | ✅ Yes | Normal (~500ms) |
| Config Only | ❌ No | ❌ Skipped | ❌ No | Fast (~300ms) |
| (No zip file) | ❌ No | ❌ Skipped | ❌ No | Fast (~300ms) |
| Model Only (no labels) | ✅ Yes | ✅ Enabled | ⚠️ Generic | Normal (~500ms) |

---

## Related Documentation

- **Model Training Guide**: `edge_impulse_model_training_guide.md`
- **EXIF Structure**: `EXIF_Support_for_WW.md`
- **Operational Parameters**: `Operational_Parameters.md`
- **Model Handling**: `Model_handling.md`
