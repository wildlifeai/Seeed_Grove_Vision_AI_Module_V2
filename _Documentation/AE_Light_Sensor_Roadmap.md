# AE-Register-Based Light Sensor — Analysis & Roadmap

**Date:** 4 July 2026  
**Authors:** Victor Anton, CGP  
**Status:** Phase 2 Implemented (see notes added to sections 5.2/5.3)

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

> **As implemented:** the RTC-alarm DPD wake was chosen - a FreeRTOS software timer
> cannot fire while the device is in DPD, where it spends nearly all its time. The rules:
>
> - **Timelapse enabled:** its captures already refresh the AE registers on every wake,
>   so no extra wakes are added (no additional battery cost).
> - **Timelapse disabled + flash in AE mode + interval > 0:** the DPD RTC alarm is set to
>   `OP_PARAMETER_AE_CHECK_INTERVAL`. The resulting timer wake captures a single frame,
>   reads the AE registers, and saves nothing (NN and file save are skipped).
> - **Interval = 0:** no alarm is armed - the check is disabled (no timer object exists,
>   so there is no zero-period timer hazard).
> - The RTC alarm parameter is uint16_t seconds, so the interval is clamped to ~18 hours.
>
> Two details the original sketch missed, now implemented:
>
> 1. **The flash decision is persisted** in a third parameter, `OP_PARAMETER_AE_FLASH_STATE`
>    (index 25, runtime state): RAM is lost in DPD and a motion-triggered capture happens
>    before any fresh AE reading exists, so without persistence the first (usually only)
>    photo of every wake would never use the flash. `FLASH_MODE_AE` setup restores it.
> 2. `ledFlashActivate()` is intentionally called unconditionally after each AE evaluation:
>    it already dispatches on `flashActive` internally, calling `ledFlashDisable()` when the
>    scene is bright - this is what turns the flash off again at dawn.

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

## 8. Flash Configuration (Simplified) and Motion-Detection Illumination

### 8.1 Time-of-day mode removed

To remove complexity, the flash no longer has a time-of-day mode (or an always-on mode).
`OP_PARAMETER_FLASH_LED_START_TIME` (21) and `OP_PARAMETER_FLASH_LED_DURATION` (22) have been
removed, along with `FLASH_MODE_TIME_OF_DAY`, `FLASH_MODE_ALWAYS_ON` and `ledFlashNewTime()`.
The capture flash is now either **off** (`OP_PARAMETER_FLASH_LED` = 0) or **driven by the AE
light sensor** (`OP_PARAMETER_FLASH_LED` = 1 or 2), tuned with `AE_DARK_THRESHOLD` /
`AE_CHECK_INTERVAL`; `AE_FLASH_STATE` (default 0) carries the decision across sleep.

### 8.2 How motion detection uses the flash (research findings)

Yes - dark-scene motion detection depends on the flash. While the AI processor sleeps in DPD,
the HM0360 keeps taking motion-detection frames, and its **STROBE pin hardware-gates the LED
driver** during each frame's exposure (`hm0360_md_configureStrobe()`, STROBE_CFG mode 3).
The LED selection (visible/IR) and brightness come from the **PCA9574 I/O expander**, which
retains its settings while the processor sleeps:

- Brightness is a **4-bit hardware setting** (BRSEL0-3 select PSU feedback resistors), i.e.
  **16 discrete levels** (~6.7 % steps) - not CPU PWM, so it works in DPD.
- VISENABLE / IRENABLE bits select the LED.
- The strobe only fires if it was armed on the way into DPD, which happens when the AE light
  sensor last judged the scene dark (`flashActive`) and motion detection is enabled.

### 8.3 Separate settings for MD illumination and capture flash

Because the PCA9574 settings can be rewritten at each sleep/wake transition, the motion
detection illumination and the capture flash are independently configurable:

| Purpose | When written | LED | Brightness |
|---------|--------------|-----|------------|
| Motion-detection illumination | On the way into DPD (`image_sleepNow()`) | `OP_PARAMETER_MD_FLASH_LED` (21, default 2 = IR) | `OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT` (22, default 5) |
| Capture flash | At wake (`setupLEDFlash()`) | `OP_PARAMETER_FLASH_LED` (13) | `OP_PARAMETER_LED_BRIGHTNESS_PERCENT` (9) |

Examples of user configurations:

- IR for motion detection at 5 %, IR capture flash at the same 5 %: `21=2, 22=5, 13=2, 9=5`
- IR for motion detection at 5 %, white capture flash at 50 %: `21=2, 22=5, 13=1, 9=50`
- IR for motion detection at 10 %, white capture flash at 20 %: `21=2, 22=10, 13=1, 9=20`

Both illumination paths only operate when the AE light sensor judged the scene dark, so no
battery is spent on daytime flashes.

### 8.4 Migration note

Devices updating with an existing `CONFIG.TXT` will have 0 at indexes 21/22 (the old
start-time/duration values), which now means "no MD illumination" - motion detection will not
see in the dark until the app sets the new values. The app should push the new defaults
(21=2, 22=5) as part of the firmware-update flow.

---

## 8.5 Bench finding: single-frame AE_MEAN is unreliable — use a multi-frame aggregate

On-hardware bench testing (WW500_C02, RP3 firmware, device sealed in a fully
dark box) showed that a **single** `AE_MEAN` reading is not a usable light
sensor. `AE_MEAN` is the *output* of the HM0360's automatic-exposure control
loop, not a raw brightness measurement, and that loop limit-cycles: in total
darkness the value oscillated frame-to-frame between ~3 and ~66, straddling the
dark threshold of 65. Sampling one frame per wake (as the original
`ledFlashNewAEValues()` did) therefore misreported "bright → flash OFF" on
**9 of 24 readings (~37%)** in a pitch-black box.

Measured behaviour:

| Condition        | AE_MEAN (single frames)      | Mean | Gain (analog / digital)          |
|------------------|------------------------------|------|----------------------------------|
| Dark box         | oscillates ~3 ↔ ~66          | ~35  | rails to max (4 / 192) on dark frames |
| Normal room      | ~70–89, comparatively stable | ~80  | low (0–4 / 64–128)               |

Integration time was pinned at 376 lines throughout (only gain moved), so it is
not a useful discriminator on its own.

What does *not* fix it:
- **A settling / calibration delay** — the oscillation is a persistent limit
  cycle, not a start-up transient. It was still swinging 3↔66 after 90 s awake.
- **Reading gain or integration on a single frame** — gain oscillates in
  lockstep with AE_MEAN, and integration barely moves.

What does fix it (implemented):
- **Average AE_MEAN over several successive frames** (`AE_SAMPLE_COUNT` = 8,
  `AE_SAMPLE_GAP_MS` = 40). The mean collapses the oscillation: ~35 in the dark
  box vs ~80 in room light — a clean separation. See `hm0360_md_getAEStats()`.
- **Gain-railed override** — if analog *and* digital gain reach their configured
  maxima (`MAX_AGAIN`/`MAX_DGAIN`) on the majority of sampled frames, the scene
  is darker than the sensor can expose for; force flash ON regardless of the
  (then meaningless) AE_MEAN. This is an independent, monotonic dark signal.
- **Hysteresis** (`AE_HYSTERESIS` = 12) — turn the flash ON below the threshold
  but only OFF again once well above it, so it does not chatter near dusk.

Decision path now lives in `ledFlashNewAEStats()`; `image_task.c` calls
`hm0360_md_getAEStats()` on each AE-check wake when the flash mode is AE.

Other candidate light-sensor signals considered (for future work): a
fixed-exposure metering frame (disable AE, set a known gain, read the raw mean —
a true brightness measure with no loop to oscillate) would be the most rigorous
option but requires reconfiguring the sensor on each wake; the averaged-AE +
gain-railed approach was chosen as the lower-risk change that reuses the
existing MD/AE path.

## 9. Open Items

- [x] Agree on default threshold value - 65 (moderate) implemented as the default, tuneable via `setop 23`
- [x] Decide periodic check mechanism - RTC-alarm DPD wake (see section 5.3)
- [x] Sync BLE processor `aiProcessor.h` with new OP indices (also removes the never-implemented OP20-27 deployment-chunk entries)
- [ ] Field test across multiple deployment environments
- [ ] App: expose MD illumination (21/22) and capture flash (13/9) settings; push new defaults on firmware update (see 8.4)
- [ ] Resolve OP index clash before merging: this branch and `feat/camera-day-night-switching` both claim index 23
