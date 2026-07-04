# AE-Register-Based Light Sensor — Analysis & Roadmap

**Date:** 4 July 2026  
**Authors:** Victor Anton, CGP  
**Status:** Phase 1 Complete — Awaiting firmware implementation approval

---

## 1. Objective

Use the HM0360 Auto-Exposure (AE) registers — already read after each image capture — as a
**software light sensor** to automatically determine when the scene is dark and a flash LED
is needed. The firmware already has a `FLASH_MODE_AE` stub in `ledFlash.c`; this work provides
the data-driven thresholds and implementation roadmap to complete that feature.

---

## 2. Background

The WW500 reads five AE status registers from the HM0360 sensor via I2C after every image
capture (`hm0360_md_getGainRegs()` in `hm0360_md.c`). These values are embedded in each JPEG's
EXIF MakerNote tag:

| Register | Address | Range | What it measures |
|----------|---------|-------|------------------|
| Integration time | 0x0202–0x0203 | 0–65535 lines | Exposure duration (higher → darker) |
| Analog gain | 0x0205 bits 4-6 | 0–7 | Pre-ADC amplification (higher → darker) |
| Digital gain | 0x020E–0x020F | 0–255 | Post-ADC amplification (higher → darker) |
| **AE Mean** | **0x205D** | **0–255** | **Average scene brightness** |
| AE Converged | 0x2060 | 0/1 | Whether the AE loop settled |

The existing `ledFlashNewAEValues()` function in `ledFlash.c` receives these values but currently
contains only a TODO placeholder with no threshold logic.

---

## 3. Phase 1 — Data Analysis (COMPLETE)

### 3.1 Dataset

**303 images** across 3 day/night cycles, captured by the WW500 camera and stored in
`ae register examples/IMAGES.000–002`. AE MakerNote EXIF data was extracted using
`_Tools/jpegAE-batch.py`.

### 3.2 Classification

Using a composite scoring heuristic, images were classified as:

- **Dark:** 174 images (57.4%)
- **Light:** 129 images (42.6%)

### 3.3 Per-Category Statistics

| Register | Light: Mean | Light: Range | Dark: Mean | Dark: Range |
|----------|-------------|--------------|------------|-------------|
| **AE Mean** | **71.4** | **56–89** | **10.3** | **0–86** |
| Integration | 87.4 | 2–376 | 376.0 | 376–376 (saturated) |
| Analog Gain | 0.1 | 0–3 | 4.0 | 3–4 |
| Digital Gain | 64.3 | 64–67 | 186.6 | 64–192 |

### 3.4 Optimal Thresholds (Single-Register)

Each register was swept to find the value that best separates dark from light:

| Register | Threshold | Direction | Accuracy |
|----------|-----------|-----------|----------|
| **Analog Gain** | **> 2** | Dark when above | **99.7%** |
| Digital Gain | > 67 | Dark when above | 98.3% |
| **AE Mean** | **< 56** | **Dark when below** | **96.7%** |
| Integration | > 188 | Dark when above | 94.7% |

### 3.5 Correlations

All registers are strongly correlated (|r| > 0.79). Digital Gain vs AE Mean has the strongest
correlation at r = −0.962, meaning they move almost perfectly in opposition.

### 3.6 Transition Zone (Dusk/Dawn)

11 images fell in the dusk/dawn transition zone where the sensor is at maximum integration
time (376) but AE Mean is still in the 56–86 range. These represent the boundary where flash
activation is most uncertain.

### 3.7 Recommendation

**Use AE Mean (register 0x205D) as the primary threshold register.**

While Analog Gain scored higher on accuracy (99.7%), it only has 8 discrete values (0–7),
making it impossible to tune. AE Mean provides a continuous 0–255 range and is the most
semantically meaningful register — it directly represents average scene brightness.

---

## 4. Proposed Configuration

### 4.1 Three Threshold Settings

Based on the analysis, three conservative threshold settings are proposed for testing.
**AE Mean values below the threshold indicate darkness and trigger the flash:**

| Setting | AE Mean Threshold | Meaning | Expected Behaviour |
|---------|------------------|---------|--------------------|
| **Conservative** | **< 70** | Flash activates early at dusk | Catches all dusk transition images |
| **Moderate** (recommended default) | **< 65** | Flash activates at mid-dusk | Good balance of coverage vs battery |
| **Minimal** | **< 60** | Flash only in deeper darkness | Saves battery, misses some dusk shots |

### 4.2 New Operational Parameters

Two new entries to add to the `OP_PARAMETERS_E` enum:

| Index | Name | Default | Range | Description |
|-------|------|---------|-------|-------------|
| 23 | `OP_PARAMETER_AE_DARK_THRESHOLD` | 65 | 0–255 | AE Mean below this → dark → flash needed |
| 24 | `OP_PARAMETER_AE_CHECK_INTERVAL` | 15 | 0–1440 | Interval (minutes) between periodic AE light-level checks. 0 disables. |

These follow the existing Operational Parameter pattern: stored in `CONFIG.TXT`, tuneable
via CLI (`setop 23 70`) or the mobile app (`AI setop 23 70`).

### 4.3 Periodic AE Check

Rather than only evaluating light levels when motion triggers a capture, the camera will
**periodically take a single-frame AE check** (e.g. every 15 minutes) to determine whether
flash mode should be active. This means:

- The flash state is updated before the next motion-triggered capture
- No unnecessary photos are saved — the check only reads AE registers
- The interval is configurable via `OP_PARAMETER_AE_CHECK_INTERVAL`

---

## 5. Implementation Roadmap

### Phase 2 — Firmware Changes

#### 5.1 Add Operational Parameters

**Files:** `fatfs_task.h`, `fatfs_task.c`

- Add `OP_PARAMETER_AE_DARK_THRESHOLD` (index 23, default 65) to the enum
- Add `OP_PARAMETER_AE_CHECK_INTERVAL` (index 24, default 15) to the enum
- Add default values to the `op_parameter[]` initialiser array

> **Note:** The BLE processor's `aiProcessor.h` must be updated to match.

#### 5.2 Implement `ledFlashNewAEValues()`

**File:** `ledFlash.c`

Replace the TODO stub with:

```c
void ledFlashNewAEValues(HM0360_GAIN_T * gainRegs) {

    if ((flashMode != FLASH_MODE_AE) || (gainRegs == NULL)) {
        return;
    }

    uint16_t threshold = fatfs_getOperationalParameter(OP_PARAMETER_AE_DARK_THRESHOLD);

    // AE Mean below threshold means the scene is dark → flash needed
    flashActive = (gainRegs->aeMean < threshold);

    xprintf("AE light check: AE Mean = %d, threshold = %d → flash %s\n",
            gainRegs->aeMean, threshold, flashActive ? "ON" : "OFF");

    ledFlashActivate();
}
```

#### 5.3 Periodic AE Check Timer

**File:** `image_task.c` (or new timer in `ledFlash.c`)

Implement a FreeRTOS timer that wakes every `OP_PARAMETER_AE_CHECK_INTERVAL` minutes,
triggers a single HM0360 frame capture, reads the AE registers, and calls
`ledFlashNewAEValues()`. No image is saved to SD card — this is a lightweight sensor check.

> **Design note:** The exact mechanism (FreeRTOS software timer vs timelapse-style DPD
> wake cycle) needs discussion. A software timer is simpler but keeps the processor awake;
> a DPD wake cycle is more power-efficient but requires coordination with the existing
> timelapse and motion-detection wake mechanisms.

#### 5.4 Update Documentation

**File:** `_Documentation/Operational_Parameters.md`

Add entries for index 23 and 24 to the Operational Parameters Table.

---

## 6. Verification Plan

### 6.1 Threshold Validation

1. Deploy camera with `OP_PARAMETER_AE_DARK_THRESHOLD` set to each of 60, 65, 70
2. Capture images across a full day/night cycle
3. Review whether flash activated at the correct times
4. Tune threshold if needed

### 6.2 Periodic Check Validation

1. Set `OP_PARAMETER_AE_CHECK_INTERVAL` to a short value (e.g. 2 minutes) for testing
2. Monitor console output to confirm periodic AE reads occur
3. Verify flash state transitions at dusk/dawn

---

## 7. Analysis Tools Created

| File | Description |
|------|-------------|
| `_Tools/jpegAE-batch.py` | Extracts AE MakerNote data from JPEG files (pre-existing) |
| `_Tools/ae_threshold_analysis.py` | Analyses AE data, generates charts and threshold recommendations (new) |

The analysis can be re-run on any future dataset:

```bash
python jpegAE-batch.py --input_folder <images> --output_csv ae_data.csv
python ae_threshold_analysis.py --csvs ae_data.csv --outdir <output>
```

---

## 8. Open Items

- [ ] Agree on default threshold value (60, 65, or 70)
- [ ] Decide periodic check mechanism (FreeRTOS timer vs DPD wake)
- [ ] Sync BLE processor `aiProcessor.h` with new OP indices
- [ ] Field test across multiple deployment environments
