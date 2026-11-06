# The person model already outputs probabilities

#### File: `CLAUDE_person_model_softmax.md`
#### Path: `_Documentation\development reports\2026-11-05_NN_threshold\CLAUDE_person_model_softmax.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 6 November 2026

## Summary

The neural network gives two numbers per photo, one for "no person" and one for "person". The firmware
treats them as raw scores and converts them to percentages. But the person model we use already gives
percentages (in a coded form), so the firmware converts them twice. That squashes every answer towards
50%: a photo the model is 87% sure shows a person is reported as 68%.

Detection (`NN+` / `NN-`) is not affected. Only the percentages printed on the console and written to
the EXIF UserComment are wrong. The two `nnfiles` example models are built the same way.
The fix is small: recognise these models from their output settings and skip the second conversion.

## Where the person model came from

The model we load as `20V1` (MobileNetV1, 96 x 96 grayscale input, labels `no person` and `person`)
is Himax's person detection example. `20V1.TFL` is byte-for-byte identical (251,568 bytes) to the array
in `EPII_CM55M_APP_S/app/scenario_app/allon_sensor_tflm/person_detect_model_data_vela.cc`. It is also
the `person_detection.tflite` (same size) in the MANIFEST zip described in `_Documentation/MANIFEST_info.md`.

Himax ship this model in two forms in the scenario apps:

| Form | File | Used by | Size |
|---|---|---|---|
| Compiled by Vela for the Ethos-U55 NPU | `person_detect_model_data_vela.cc` | `allon_sensor_tflm`, `allon_sensor_tflm_fatfs`, `ei_standalone_inferencing/allon_sensor_tflm_freertos` | 251,568 bytes |
| Not compiled by Vela (runs on the CPU with CMSIS-NN) | `person_detect_model_data.cc` | `allon_sensor_tflm_cmsis_nn` | 300,568 bytes |

They are the same network: the same tensor names (`MobilenetV1/...`), input and output shapes, and
quantization. The layer names and the 96 x 96 MobileNetV1 design suggest it is the TensorFlow Lite
Micro "person detection" example, but nothing in the repo says so.

The Himax documentation does not say what the outputs mean. `allon_sensor_tflm/readme.txt` says only
"This app appears to run a person detect model", and the apps print the raw class 1 value as
`person_score`.

## Why we suspected percentages, not logits

1. **The two outputs are always exact opposites.** On the bench (logs `ai1.txt`, `ai2.txt`), every
   frame had class 0 = minus class 1: 94 / -94, -45 / 45, 14 / -14. Raw scores (logits) from a model's
   last layer have no reason to do that. Probabilities do: an int8 softmax output stores probability
   p as `256 p - 128`, and as the two probabilities add up to 1, the two stored values add up to 0.
2. **The printed percentages were too timid.** Raw 94 printed as 68% "person", and no frame in either
   log went above 70% or below 30%, even for clear photos. A second softmax on values that are already
   between 0 and 1 always gives results close to 50%.

## How the model bytes show it

A `.tflite` (or `.TFL`) file is a flatbuffer that lists the layers in order and, for each tensor, its
name, shape, type and quantization (the scale and zero point that turn an int8 value into a real number:
`real = scale x (value - zero point)`). [`_Tools/tflite_inspect.py`](../../../_Tools/tflite_inspect.py)
reads these.

**The CMSIS-NN form shows the softmax directly.** Its last layer is `SOFTMAX`, writing
`MobilenetV1/Predictions/Reshape_1`, and its op resolver in `allon_sensor_tflm_cmsis_nn/cvapp.cpp`
registers `AddSoftmax()` for it.

**The Vela form hides it.** Vela folds the whole network, softmax included, into one `ethos-u` layer.
What remains is the output tensor's quantization, and in all three models it is scale 1/256, zero
point -128. TFLite's int8 quantization spec fixes a softmax output at exactly those values, so int8
-128..127 means probability 0..255/256. A model ending in logits would have a scale chosen to fit its
own range.

| Model | Layers | Output quantization | Output is |
|---|---|---|---|
| `person_detect_model_data.cc` (CMSIS-NN) | 31, last `SOFTMAX` | 1/256, -128 | probabilities |
| `20V1.TFL` = `person_detect_model_data_vela.cc` | 1, `ethos-u` | 1/256, -128 | probabilities |
| `99V2` (rat) and `98V1` (cat), `_Tools/nnfiles_example*` | 1, `ethos-u` | 1/256, -128 | probabilities |

So the person probability is `(value + 128) / 256`. Raw 94 is 87%, raw -84 is 17%. The challenge models
(`93V1`, `71V1`, `72V1`) are on the team drive, not checked here; their outputs are not mirrored, which
suggests logits, and the tool will say.

**What the firmware shows today.** On a cold boot `cvapp.cpp` prints the "TFLM Ethos-U55 Model
Fingerprint" (the layers, input and output shapes):

```
Operators      : 1
  Op 0: CUSTOM (ethos-u), weight bytes: 250576
Input tensor   : i8 [1,96,96,1]
Output tensor  : i8 [1,2]
```

That cannot show the softmax (Vela hid it) and does not print the quantization, so it cannot tell the
two kinds of model apart. Adding the output's scale and zero point to it would (done, see below).

## Software changes

Made 6 November 2026; benched on both images (README, bench tests 3 and 4):

| Where | Change |
|---|---|
| `outputAsPercentage()` in `cvapp.cpp` | If the output is int8 with scale 1/256 and zero point -128, the percentage is `(value + 128) / 256 x 100`, with no softmax. Otherwise dequantize and apply softmax as now. Correct the comment that says logits are typically "scale 1/256, zero_point = 0" |
| Fingerprint in `cvapp.cpp` | Print each tensor's scale, zero point and name, the builtin number of any `OTHER` operator, and an `Output values` line: probabilities or logits. Both it and `outputAsPercentage()` use one test, `isProbabilityOutput()` |
| `WW500_Neural_Network_Operation.md` | Fingerprint example and table, "Understanding the output values" (probabilities for softmax models, logits otherwise), UserComment section, `cv_run()` row, `USE_PERCENTAGE` row (it said the switch was commented out; it is defined) |
| Website (Victor), not done | The EXIF UserComment percentages for these models will change (the format does not). Images already taken carry the squashed values |

What the fingerprint should now show for `20V1`:

```
Input tensor   : i8 [1,96,96,1], scale 0.007843138, zero point -1
                 'input'
Output tensor  : i8 [1,2], scale 1/256, zero point -128
                 'MobilenetV1/Predictions/Reshape_1_cpu'
Output values  : probabilities (softmax quantization, scale 1/256, zero point -128). Percent = (value + 128) / 2.56
```

And for raw 94 / -94 the percentages should be 87% "person", 13% "no person" (before: 68% / 32%). As
each is rounded, a pair can sum to 101% (raw 32 gives 62.5% and 37.5%).

**Detection.** With the score rule of 5 Nov, detection was not affected by #287: for a softmax model
the score is 256 x (p(person) - p(no person)), so it ranked frames correctly, and op 16 = 18 meant
"person" above about 53.5%. That finding led to op 16 becoming a percentage on 6 Nov (README, "The
approach adopted"): detection now uses the probabilities this fix makes correct.

## Using the tool

[`_Tools/tflite_inspect.py`](../../../_Tools/tflite_inspect.py) runs on a PC with only the Python
standard library. From the repo root:

```
python _Tools/tflite_inspect.py <model.TFL | model_data.cc> [--all]
```

It takes either a model file (`.TFL`, `.tflite`) or a scenario app's C array (`.cc`, `.c`, `.h`), and
prints:

- the file size and TFLite schema version;
- the operator types the model uses;
- the layers in order with the tensor each one writes (the first and last three; `--all` for every one);
- the input and output tensors: name, type, shape, scale and zero point;
- a verdict: **probabilities** (the last layer is softmax, or the output has softmax's quantization,
  scale 1/256 and zero point -128) or **logits**.

The scale prints as a decimal here (0.00390625 is 1/256), unlike the firmware's fingerprint.

### The Vela version (`20V1`, as loaded on the WW500)

```
python _Tools/tflite_inspect.py 20V1.TFL
```

```
20V1.TFL: 251568 bytes, schema version 3
Operator types: ethos-u
Layers (1):
  ethos-u -> MobilenetV1/Predictions/Reshape_1_cpu
Input : 'input' i8 [1, 96, 96, 1], scale 0.0078431377, zero point -1
Output: 'MobilenetV1/Predictions/Reshape_1_cpu' i8 [1, 2], scale 0.00390625, zero point -128
Verdict: the output is PROBABILITIES (softmax quantization: scale 1/256, zero point -128). Probability = (value + 128) / 256.
```

Vela has folded the whole network into one `ethos-u` layer, so the softmax cannot be seen; the verdict
comes from the output quantization. The `_cpu` on the output name is added by Vela. Run on
`EPII_CM55M_APP_S/app/scenario_app/allon_sensor_tflm/person_detect_model_data_vela.cc`, the output is
the same apart from the file name, as the two are the same bytes.

### The CMSIS-NN version (not compiled by Vela)

```
python _Tools/tflite_inspect.py EPII_CM55M_APP_S/app/scenario_app/allon_sensor_tflm_cmsis_nn/person_detect_model_data.cc --all
```

```
EPII_CM55M_APP_S/app/scenario_app/allon_sensor_tflm_cmsis_nn/person_detect_model_data.cc: 300568 bytes, schema version 3
Operator types: AVERAGE_POOL_2D, CONV_2D, DEPTHWISE_CONV_2D, RESHAPE, SOFTMAX
Layers (31):
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_0/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_1_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_1_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_2_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_2_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_3_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_3_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_4_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_4_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_5_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_5_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_6_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_6_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_7_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_7_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_8_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_8_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_9_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_9_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_10_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_10_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_11_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_11_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_12_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_12_pointwise/Relu6
  DEPTHWISE_CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_13_depthwise/Relu6
  CONV_2D -> MobilenetV1/MobilenetV1/Conv2d_13_pointwise/Relu6
  AVERAGE_POOL_2D -> MobilenetV1/Logits/AvgPool_1a/AvgPool
  CONV_2D -> MobilenetV1/Logits/Conv2d_1c_1x1/BiasAdd
  RESHAPE -> MobilenetV1/Logits/SpatialSqueeze
  SOFTMAX -> MobilenetV1/Predictions/Reshape_1
Input : 'input' i8 [1, 96, 96, 1], scale 0.0078431377, zero point -1
Output: 'MobilenetV1/Predictions/Reshape_1' i8 [1, 2], scale 0.00390625, zero point -128
Verdict: the output is PROBABILITIES (the last layer is softmax). Probability = (value + 128) / 256.
```

Here every layer is visible. The tensor named `Logits` is the output of the last convolution, before
the softmax; the model's output is the softmax's. Input and output tensors match the Vela version
exactly, which is how we know the two are the same network.

### Reading the output

- **Probabilities:** `p = (value + 128) / 256`. Raw 94 is 87%. Do not apply softmax again.
- **Logits:** apply softmax across the classes for probabilities, as `outputAsPercentage()` does.
- **Input scale and zero point** show what the model expects: here scale 1/127.5 and zero point -1, so
  int8 -128..127 covers about -1 to +1. The firmware feeds pixel - 128.

## Issue to post

Posted as [#287](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/287), 6 November 2026.

**Title:** NN confidence percentages are too close to 50% (converted twice)

**Body:**

> The camera reports how sure the neural network is as a percentage, on the console and in each photo's
> EXIF UserComment. For the person model (`20V1`) these percentages are wrong: the model already gives
> percentages, and the firmware converts them a second time. That pulls every answer towards 50%. A
> photo the model is 87% sure shows a person is reported as 68%.
>
> Detection (`NN+` / `NN-`) is not affected, only the reported percentages. The `nnfiles` example models
> (`99V2`, `98V1`) are affected the same way; models that output raw scores are not.
>
> Fix: the firmware recognises these models from their output settings and skips the second
> conversion. The website should know that the percentages for these models will change; photos
> already taken keep the old values.
>
> Details, evidence and the changes needed:
> [CLAUDE_person_model_softmax.md](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/261105_NN_threshold/_Documentation/development%20reports/2026-11-05_NN_threshold/CLAUDE_person_model_softmax.md)
> (thread `2026-11-05_NN_threshold`, related to #270).
