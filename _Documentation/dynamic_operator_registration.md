# Dynamic Operator Registration System

## Overview
The WW500_MD project now supports **automatic dynamic operator registration** for TensorFlow Lite Micro models. This means you can upload ANY TFLite model to flash, and the system will automatically detect and register all required operators at runtime.

## How It Works

### 1. Model Scanning
When `cv_init()` is called:
- The model's `operator_codes()` array is scanned
- Each operator type used by the model is identified
- A switch statement maps each operator to its registration function

### 2. Automatic Registration
For each operator found in the model:
- The corresponding `AddXXX()` method is called on the op resolver
- Success/failure is logged with operator name
- If any operator fails to register, initialization fails with clear error

### 3. Supported Operators
The system currently handles 50+ common TFLite operators including:

#### Convolution Operations
- `CONV_2D` - Standard 2D convolution
- `DEPTHWISE_CONV_2D` - Depthwise separable convolution
- `TRANSPOSE_CONV` - Transposed convolution (deconvolution)

#### Pooling Operations
- `AVERAGE_POOL_2D` - Average pooling
- `MAX_POOL_2D` - Max pooling

#### Fully Connected
- `FULLY_CONNECTED` - Dense/linear layers

#### Activation Functions
- `RELU` - Rectified Linear Unit
- `RELU6` - ReLU capped at 6
- `LEAKY_RELU` - Leaky ReLU
- `HARD_SWISH` - Hard Swish activation
- `LOGISTIC` - Sigmoid activation
- `TANH` - Hyperbolic tangent
- `PRELU` - Parametric ReLU

#### Arithmetic Operations
- `ADD` - Element-wise addition
- `SUB` - Element-wise subtraction
- `MUL` - Element-wise multiplication
- `DIV` - Element-wise division
- `MAXIMUM` - Element-wise maximum
- `MINIMUM` - Element-wise minimum
- `NEG` - Negation
- `EXP` - Exponential
- `SQRT` - Square root
- `RSQRT` - Reciprocal square root

#### Comparison Operations
- `EQUAL` - Equality comparison
- `NOT_EQUAL` - Inequality comparison
- `GREATER` - Greater than
- `GREATER_EQUAL` - Greater than or equal
- `LESS` - Less than
- `LESS_EQUAL` - Less than or equal

#### Logical Operations
- `LOGICAL_AND` - Boolean AND
- `LOGICAL_OR` - Boolean OR
- `LOGICAL_NOT` - Boolean NOT

#### Shape Manipulation
- `RESHAPE` - Reshape tensor
- `TRANSPOSE` - Transpose dimensions
- `EXPAND_DIMS` - Add dimension
- `SQUEEZE` - Remove dimension
- `SLICE` - Extract slice
- `STRIDED_SLICE` - Extract with stride
- `SPLIT` - Split tensor
- `SPLIT_V` - Split with size array
- `PACK` - Stack tensors
- `UNPACK` - Unstack tensors
- `CONCATENATION` - Concatenate tensors
- `GATHER` - Gather elements
- `SHAPE` - Get tensor shape

#### Spatial Operations
- `RESIZE_BILINEAR` - Bilinear image resize
- `RESIZE_NEAREST_NEIGHBOR` - Nearest neighbor resize
- `PAD` - Padding
- `PADV2` - Padding v2
- `BATCH_TO_SPACE_ND` - Batch to space
- `SPACE_TO_BATCH_ND` - Space to batch

#### Normalization/Reduction
- `MEAN` - Reduce mean
- `SOFTMAX` - Softmax normalization

#### Quantization
- `QUANTIZE` - Quantize floating point to int8
- `DEQUANTIZE` - Dequantize int8 to floating point

#### Tensor Info
- `ARG_MAX` - Index of maximum value
- `ARG_MIN` - Index of minimum value
- `CAST` - Type conversion
- `FLOOR` - Floor function

#### Custom Operators
- `ETHOSU` - Ethos-U NPU custom operator (always added)

## Configuration

### Resolver Capacity
```cpp
tflite::MicroMutableOpResolver<50> *op_resolver_ptr = nullptr;
```
- Set to **50 slots** to handle complex models
- Increase if you encounter "resolver size is too small" errors
- Each slot uses minimal memory (~12 bytes)

### Adding New Operators
To support additional operators not in the current list:

1. Find the operator enum in `schema_generated.h`:
   ```cpp
   tflite::BuiltinOperator_YOUR_OP
   ```

2. Add a case to the switch statement in `cv_init()`:
   ```cpp
   case tflite::BuiltinOperator_YOUR_OP:
       op_name = "YOUR_OP";
       status = op_resolver_ptr->AddYourOp();
       break;
   ```

3. Rebuild and test

## Runtime Output
When a model is loaded, you'll see:
```
Scanning model for required operators...
Model uses 8 operator types
  + Added CONV_2D
  + Added DEPTHWISE_CONV_2D
  + Added RESHAPE
  + Added AVERAGE_POOL_2D
  + Added FULLY_CONNECTED
  + Added SOFTMAX
  + Added ADD
  + Added RELU
Operator registration complete
```

## Error Handling

### Missing Operator
If an operator is in the model but not in the switch statement:
```
[WARNING] Unsupported operator code 123 (not in handler list)
```
- Model will attempt to continue
- Will fail at AllocateTensors if the operator is actually needed

### Registration Failure
If an operator fails to add to the resolver:
```
[ERROR] Failed to add CONV_2D
```
- `cv_init()` returns -1
- Check resolver capacity
- Check TFLite Micro library version compatibility

## Benefits

1. **Universal Model Support** - Upload any TFLite model without code changes
2. **Minimal Memory** - Only registers operators actually used by the model
3. **Clear Debugging** - See exactly which operators are registered
4. **Future-Proof** - Easy to extend with new operators as TFLite evolves
5. **Runtime Flexibility** - Switch between different model architectures on-the-fly

## Memory Considerations

### Resolver Overhead
- Each operator registration: ~12 bytes
- 50 slot resolver: ~600 bytes total
- Actual memory depends on operators used

### Arena Size
The tensor arena size (512KB default) should be sufficient for most models. If you encounter allocation failures:
- Check the printed arena size at runtime
- Verify model hasn't been corrupted
- Consider models optimized for embedded (e.g., MobileNet, EfficientNet-Lite)

## Testing Different Models

To test with different models:

1. Export your model to TFLite format
2. Optimize for Ethos-U using Vela (if using NPU)
3. Copy to SD card as `MOD00001.tfl` through `MOD00009.tfl`
4. Use CLI command: `modeltest N` (where N = 1-9)
5. System will automatically:
   - Load model from SD to flash
   - Scan for operators
   - Register required operators
   - Allocate tensors
   - Run inference

## Limitations

- Custom operators beyond Ethos-U must be added manually
- Some experimental/preview TFLite ops may not be available in TFLite Micro
- Model must fit in 512KB arena (or modify linker script for larger arena)

## Troubleshooting

### "Tensor arena size = 873641472 bytes"
- Fixed in latest code (runtime calculation)
- If you see this, ensure you have latest cvapp.cpp

### "Failed to allocate tensors"
- Check that all model operators are supported and registered
- Verify arena size is reasonable (~500KB)
- Check model hasn't been corrupted during flash write

### Model works on desktop but not on device
- Ensure model was compiled/optimized for Cortex-M and Ethos-U
- Check for unsupported operations (e.g., some dynamic shapes)
- Verify quantization (int8 models preferred for embedded)
