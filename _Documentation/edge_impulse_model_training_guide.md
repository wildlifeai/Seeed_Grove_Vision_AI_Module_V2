# Edge Impulse Model Training Guide for WW500 Camera Trap

## Device Constraints Summary

Your Himax WE2 (HX6538) device has strict memory limitations:

| Resource | Available | Notes |
|----------|-----------|-------|
| **SRAM** | ~1.88 MB total | Shared between tensor arena, heap, stack, and image buffers |
| **ROM (Flash)** | 256 KB | For compiled code only (not model storage) |
| **Tensor Arena** | 1.5 MB | Maximum for TFLite inference |
| **Model Storage** | External flash | Models loaded from SD/SPI flash via XIP |

### Critical Constraints

1. **Tensor Arena Limit**: Models must require ≤ 1.5 MB of working memory (activations)
2. **ROM Footprint**: TFLite operators linked into firmware must fit in 256 KB
3. **Image Processing**: If capturing 4× 640×480 images (~1.2 MB), you cannot hold them all in RAM simultaneously
4. **Model File Size**: While stored in external flash, smaller is better for transfer and loading

---

## Edge Impulse Configuration Guide

### Step 1: Project Setup

1. **Create New Project** in Edge Impulse Studio
2. **Set Target Device**: 
   - Go to **Dashboard** → **Project Info**
   - Set target: **Himax WE-I Plus** or **Cortex-M55** (closest match)
   - This enables appropriate optimizations

### Step 2: Data Acquisition

#### Image Resolution Recommendations

| Use Case | Recommended Input | Tensor Arena Impact | Notes |
|----------|------------------|---------------------|-------|
| **Simple detection** | 96×96 grayscale | ~200-400 KB | Best for basic presence/absence |
| **Standard detection** | 160×160 RGB | ~400-800 KB | Good balance for most wildlife |
| **High quality** | 320×240 RGB | ~600-1.2 MB | Maximum recommended |
| **⚠️ Avoid** | 640×480 RGB | >1.3 MB | Exceeds device limits |

**Recommendation**: Start with **160×160 RGB** for multi-class wildlife detection.

#### Data Collection Tips

- Collect at least 50-100 images per class
- Include variations: lighting, angles, distances, partial occlusions
- Balance your dataset (similar counts per class)
- Add "empty" or "background" class for negative examples

### Step 3: Impulse Design

#### Processing Block

Choose: **Image** processing block

**Settings**:
```
Image width: 160 pixels
Image height: 160 pixels
Resize mode: Fit shortest axis
Color depth: RGB (or Grayscale to save memory)
```

**Why these settings?**
- 160×160 is a good compromise between accuracy and memory
- "Fit shortest axis" maintains aspect ratio
- RGB provides more information but uses 3× memory vs grayscale

#### Learning Block

Choose: **Transfer Learning (Images)** or **Object Detection (Images)**

**For Classification (single animal per image):**
- **Neural Network Architecture**: MobileNetV2 0.35 or 0.1
- **Output**: Softmax classification

**For Object Detection (multiple animals):**
- **Architecture**: MobileNetV2 SSD or FOMO (Faster Objects, More Objects)
- **FOMO** is specifically optimized for microcontrollers

### Step 4: Model Architecture Selection

#### Recommended Architectures (in order of preference)

1. **FOMO (Faster Objects, More Objects)** ⭐ BEST CHOICE
   - Specifically designed for constrained devices
   - Typical arena: 200-500 KB
   - Good accuracy for bounding box detection
   - Handles multiple objects
   
2. **MobileNetV2 (Alpha 0.1 or 0.35)**
   - Very small, fast
   - Typical arena: 300-600 KB
   - Good for classification tasks
   
3. **MobileNetV1 (Alpha 0.25)**
   - Older but reliable
   - Typical arena: 250-500 KB
   - Simpler architecture, easier to optimize

4. **EfficientNet-Lite0** (use cautiously)
   - More accurate but larger
   - Typical arena: 800 KB - 1.2 MB
   - Only if you need high accuracy and can sacrifice image buffers

#### ⚠️ Architectures to AVOID

- **MobileNetV2 (Alpha 1.0)**: Too large (>2 MB arena)
- **ResNet variants**: Way too large
- **EfficientNet B1-B7**: Exceed memory constraints
- **YOLOv5/YOLOv8**: Designed for larger devices

### Step 5: Training Configuration

#### In Edge Impulse Studio

**Navigate to**: Impulse Design → Transfer Learning (or Object Detection)

**Training Settings**:

```yaml
Number of training cycles: 50-100
Learning rate: 0.0005 (default is usually fine)
Validation set size: 20%
Auto-balance dataset: Yes
Data augmentation: Enabled
```

**Data Augmentation** (enable these):
- ✅ Random rotation (±15°)
- ✅ Random zoom (90-110%)
- ✅ Random brightness (±20%)
- ✅ Random horizontal flip (for symmetric animals)
- ❌ Disable vertical flip (animals don't appear upside down)

#### Advanced: Manual Architecture Tuning

If default models are too large, click **"Choose a different model"**:

1. Select **MobileNetV2** or **MobileNetV1**
2. Set **Alpha (width multiplier)**: 
   - Start with `0.35`
   - If still too large, try `0.25` or `0.1`
3. Enable **Quantization** (next step)

### Step 6: Model Optimization (CRITICAL)

#### Enable INT8 Quantization

**Location**: After training, go to **Deployment** or **EON Tuner**

**Settings**:
```
Quantization: INT8 (full integer quantization)
Optimization: Memory
```

**Why INT8?**
- Reduces model size by ~4× (vs float32)
- Reduces tensor arena by ~4×
- Faster inference on Cortex-M55
- Minimal accuracy loss (<2% typically)

**Verification**:
After quantization, check:
- **RAM usage**: Must be < 1.5 MB
- **Flash usage**: Model file size
- **Inference time**: Should be <500ms for real-time

#### Use EON Tuner (Recommended)

Edge Impulse's **EON Tuner** automatically finds optimal architectures:

1. Go to **EON Tuner** in left menu
2. Set target:
   ```
   Target device: Himax WE-I Plus
   Max RAM: 1500 KB
   Max inference time: 500 ms
   ```
3. Click **Start tuning**
4. Review suggestions and select best model

### Step 7: Testing and Validation

#### Before Deployment

1. **Model Testing** tab:
   - Test on validation set
   - Check confusion matrix
   - Verify F1 score > 0.8 for production

2. **Live Classification** (if using phone/webcam):
   - Test real-time performance
   - Check for false positives

3. **Performance Metrics**:
   - Target accuracy: >85% for wildlife detection
   - Target inference time: <500ms
   - Target RAM: <1.2 MB (leaves room for overhead)

### Step 8: Deployment

#### Option 1: TensorFlow Lite (Recommended)

1. Go to **Deployment** tab
2. Select **TensorFlow Lite (int8)**
3. **Optimizations**:
   - ✅ Enable EON Compiler (reduces RAM by 20-50%)
   - ✅ INT8 quantization
   - ✅ Enable hardware acceleration
4. Click **Build**

#### Option 2: Ethos-U55 Optimized (Advanced)

If using Arm Ethos-U55 NPU (your device has this):

1. Select **TensorFlow Lite for Microcontrollers**
2. Enable **Vela compiler** optimization:
   ```
   Target: Ethos-U55-128
   System config: Ethos-U55_High_End_Embedded
   Memory mode: Shared_Sram
   ```
3. This generates `.tflite` file optimized for your NPU

#### Download Files

You'll receive:
- `model.tflite` or `model_vela.tflite` (optimized for Ethos-U)
- `model_metadata.json` (input/output info)

**Expected file size**:
- FOMO: 50-200 KB
- MobileNetV2 0.35: 200-800 KB
- MobileNetV2 0.1: 50-300 KB

### Step 9: Deploy to WW500 Device

1. **Rename model**:
   ```bash
   mv model_vela.tflite MOD00007.tfl
   ```
   (Use MOD00001.tfl through MOD00009.tfl)

2. **Copy to SD card**:
   ```bash
   cp MOD00007.tfl /path/to/sdcard/
   ```

3. **Flash firmware** (if needed)

4. **Load model via CLI**:
   ```
   modeltest 7
   ```

5. **Monitor output**:
   - Check `Calculated tensor arena size` in logs
   - Verify `AllocateTensors` succeeds
   - Test inference with real images

---

## Troubleshooting Common Issues

### Issue: "Failed to allocate tensors" / Arena too small

**Problem**: Model requires more than 1.5 MB tensor arena

**Solutions**:
1. **Reduce input resolution**: Try 96×96 instead of 160×160
2. **Use smaller backbone**: MobileNetV2 0.1 instead of 0.35
3. **Switch to FOMO**: Much smaller than SSD
4. **Enable EON Compiler**: Can reduce RAM by 30-50%
5. **Use grayscale**: 3× smaller than RGB

### Issue: Model file too large (>5 MB)

**Problem**: Model file doesn't fit or takes too long to transfer

**Solutions**:
1. **Enable INT8 quantization**: Should reduce by ~4×
2. **Prune model**: Use Edge Impulse's pruning options
3. **Reduce architecture size**: Lower alpha/width multiplier
4. **Remove unused classes**: Fewer outputs = smaller model

### Issue: Poor accuracy (<70%)

**Problem**: Model is too small or undertrained

**Solutions**:
1. **Collect more data**: At least 50-100 images per class
2. **Increase training cycles**: Try 100-200 epochs
3. **Enable data augmentation**: Helps with small datasets
4. **Balance dataset**: Equal samples per class
5. **Try slightly larger model**: MobileNetV2 0.35 instead of 0.1
6. **Increase input resolution**: 160×160 instead of 96×96

### Issue: Inference too slow (>1 second)

**Problem**: Model is too complex for real-time use

**Solutions**:
1. **Enable Vela optimization**: Compiles for Ethos-U55 NPU
2. **Use smaller model**: FOMO or MobileNetV1 0.25
3. **Reduce input resolution**: 96×96 is much faster
4. **Disable debug logging**: xprintf calls slow down inference

### Issue: ROM overflow (linker error)

**Problem**: Too many TFLite operators linked into firmware

**Solution**:
- See `_Documentation/operator_optimization.md`
- Disable extended operators in `cvapp_config.h`
- Only enable ops your model actually uses

---

## Example: FOMO Object Detection Configuration

Here's a complete example for **multi-animal detection** optimized for WW500:

### Impulse Design
```
Input: 160×160 RGB image
Processing: Image (fit shortest axis)
Learning: Object Detection (FOMO)
```

### Model Settings
```yaml
Architecture: FOMO (Faster Objects, More Objects)
Base model: MobileNetV2 0.35
Input: 160×160×3 (RGB)
Grid size: 20×20
Classes: ["deer", "rabbit", "bird", "empty"]
```

### Training
```yaml
Epochs: 100
Learning rate: 0.001
Batch size: 32
Validation split: 20%
Data augmentation: Enabled
```

### Optimization
```yaml
Quantization: INT8
EON Compiler: Enabled
Target device: Cortex-M55 (Ethos-U55)
```

### Expected Performance
```
Model file size: ~300 KB
Tensor arena: ~400 KB
Inference time: ~200ms
RAM total: ~500 KB (leaves 1.3 MB for other tasks)
Accuracy: 85-92% (with good training data)
```

---

## Memory Budget Planning

### Scenario 1: Single-Image Processing (Recommended)

```
Tensor arena:        1.5 MB
Image buffer (1×):   0.3 MB (640×480 grayscale)
Heap + Stack:        0.08 MB
Code/Data:           ~0.1 MB (varies)
-----------------------------------
Total:               ~1.98 MB (slightly tight, but workable)
```

**Recommendation**: Process images one at a time, use streaming

### Scenario 2: Small Model, Multiple Images

```
Tensor arena:        0.5 MB (FOMO with 96×96 input)
Image buffers (4×):  0.15 MB each = 0.6 MB
Heap + Stack:        0.08 MB
Code/Data:           ~0.1 MB
-----------------------------------
Total:               ~1.28 MB (comfortable fit)
```

**Recommendation**: Use FOMO + small input resolution

### Scenario 3: External Processing (Future)

```
Tensor arena:        1.5 MB
Image buffer (1×):   0.3 MB
Send to cloud:       Offload heavy inference
-----------------------------------
Total:               ~1.8 MB (on-device is just preprocessing)
```

**Recommendation**: Use on-device for motion detection, cloud for classification

---

## Quick Reference: Model Size Guidelines

| Model Type | Input Size | Expected Arena | File Size | Best For |
|------------|-----------|----------------|-----------|----------|
| **FOMO** | 96×96 | 200-400 KB | 100-200 KB | Multiple objects, tight memory |
| **FOMO** | 160×160 | 400-600 KB | 200-400 KB | Multiple objects, balanced |
| **MobileNetV1 0.25** | 96×96 | 250-400 KB | 150-300 KB | Classification, very small |
| **MobileNetV2 0.1** | 160×160 | 300-500 KB | 200-400 KB | Classification, small |
| **MobileNetV2 0.35** | 160×160 | 600-900 KB | 400-800 KB | Classification, better accuracy |
| **MobileNetV2 0.35** | 224×224 | 900-1.2 MB | 600-1 MB | Classification, max quality |
| **SSD MobileNetV2** | 300×300 | 1.2-2 MB | 1-3 MB | ⚠️ Too large for device |

---

## Additional Resources

### Edge Impulse Documentation
- [Image Classification Tutorial](https://docs.edgeimpulse.com/docs/tutorials/image-classification)
- [Object Detection Tutorial](https://docs.edgeimpulse.com/docs/tutorials/detect-objects)
- [FOMO Guide](https://docs.edgeimpulse.com/docs/edge-impulse-studio/learning-blocks/object-detection/fomo-object-detection-for-constrained-devices)
- [EON Tuner](https://docs.edgeimpulse.com/docs/edge-impulse-studio/eon-tuner)

### Model Optimization
- [TensorFlow Lite for Microcontrollers](https://www.tensorflow.org/lite/microcontrollers)
- [Arm Vela Compiler](https://pypi.org/project/ethos-u-vela/)
- [Post-Training Quantization](https://www.tensorflow.org/lite/performance/post_training_quantization)

### WW500 Specific
- See `memory_configuration.md` for detailed memory layout
- See `dynamic_operator_registration.md` for operator requirements
- See `operator_optimization.md` for ROM optimization

---

## Summary Checklist

Before deploying to WW500, verify:

- ✅ Input resolution ≤ 224×224 (preferably 160×160)
- ✅ Model uses FOMO or MobileNet (small variant)
- ✅ INT8 quantization enabled
- ✅ EON Compiler optimization enabled
- ✅ Tensor arena requirement < 1.5 MB
- ✅ Model file size < 2 MB (preferably < 500 KB)
- ✅ Inference time < 500ms
- ✅ Accuracy > 85% on validation set
- ✅ Tested with real wildlife images
- ✅ Model renamed to MOD0000X.tfl format

**If all checks pass**, your model should work successfully on the WW500 device! 🎉
