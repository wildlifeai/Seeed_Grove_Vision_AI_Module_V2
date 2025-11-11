# Memory Configuration for WW500

## HX6538 Memory Layout

The Himax WE2 (HX6538) has the following memory:

- **Total SRAM**: 2MB (0x00200000 bytes)
- **Available for app**: ~1.88MB (0x001e1000 bytes) after bootloader
- **ROM**: 256KB
- **APP_DATA**: 256KB

## Current Memory Allocation (CM55M_S_SRAM)

### Fixed Allocations:
- **Heap**: 40KB (0xa000)
- **Stack**: 40KB (0xa000)
- **Tensor Arena**: 1.5MB (1536KB) - **INCREASED from 512KB**

### Why the Increase?

The original 512KB tensor arena was sufficient for small models (e.g., person detection ~300KB).
However, larger models like MOD00007.tfl require **1.3MB of tensor memory**.

The error showed:
```
Requested: 1316512 bytes (1.3MB)
Available: 523708 bytes (512KB)
Missing: 792804 bytes (~773KB)
```

## Memory Budget Breakdown

| Component | Size | Notes |
|-----------|------|-------|
| Tensor Arena | 1.5MB | For TFLite model working memory |
| Code/Data | ~280KB | Estimated (varies with build) |
| Heap | 40KB | Dynamic allocations |
| Stack | 40KB | Task stacks (FreeRTOS) |
| **Total** | ~1.86MB | Within 1.88MB limit |

## Model Size Considerations

### Small Models (< 512KB tensor requirements):
- Person detection
- Simple classifiers
- Low-resolution CNNs

### Large Models (1-1.5MB tensor requirements):
- MOD00007.tfl (640x480 input, complex architecture)
- High-resolution object detection
- Multi-stage networks

### Warning: Very Large Models (> 1.5MB):
If a model requires more than 1.5MB of tensor arena:
1. The linker script can be adjusted further (max ~1.7MB)
2. Consider model optimization:
   - Quantization (int8 instead of float32)
   - Pruning
   - Architecture simplification
3. Reduce heap/stack if safe for your application

## Troubleshooting Memory Issues

### "Failed to resize buffer" Error
- Means model needs more tensor arena than allocated
- Check the "Requested" vs "Available" values
- Increase tensor arena in `ww500_md.ld`

### Linker Error "region RAM overflowed with stack"
- Means data + heap + stack + tensor arena exceeds SRAM
- Reduce heap size, stack size, or tensor arena
- Check `.map` file for actual usage

### Runtime Crashes or Stack Overflow
- May need to increase stack size
- Monitor with `uxTaskGetStackHighWaterMark()` in FreeRTOS

## Build and Verify

After changing the linker script:

```bash
cd EPII_CM55M_APP_S
make clean && make
```

Check the `.map` file:
```bash
grep "tensor_arena" obj_epii_evb_icv30_bdv10/*.map
```

Should show the tensor arena at correct size (1572864 bytes = 1.5MB).

## Future Optimization

If you need to support both small and large models efficiently:
1. **Dynamic allocation**: Allocate tensor arena from heap at runtime
2. **Multiple builds**: Create separate firmware for small/large models
3. **External RAM**: Use external PSRAM if available (hardware dependent)

For now, 1.5MB is a good compromise that supports MOD00007 while still fitting in SRAM.
