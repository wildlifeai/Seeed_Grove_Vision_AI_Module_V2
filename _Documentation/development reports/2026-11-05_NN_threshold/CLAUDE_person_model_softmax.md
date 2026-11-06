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
two kinds of model apart. Adding the output's scale and zero point to it would.

## Software changes

| Where | Change |
|---|---|
| `outputAsPercentage()` in `cvapp.cpp` | If the output is int8 with scale 1/256 and zero point -128, the percentage is `(value + 128) / 256 x 100`, with no softmax. Otherwise dequantize and apply softmax as now. Correct the comment that says logits are typically "scale 1/256, zero_point = 0" |
| Fingerprint in `cvapp.cpp` | Print the output tensor's scale and zero point, and whether that means probabilities or logits |
| `WW500_Neural_Network_Operation.md` | Say the outputs are probabilities for softmax models and logits otherwise; it calls them logits throughout |
| Website (Victor) | The EXIF UserComment percentages for these models will change (the format does not). Images already taken carry the squashed values |

**Detection does not change.** For a softmax model the #270 score, class 1 minus class 0, is
256 x (p(person) - p(no person)), so it still ranks frames correctly. Op 16 = 18 means "person" above
about 53.5%. That makes op 16 easier to explain for these models: op 16 = 256 x (2p - 1) for a
wanted probability p.

## Using the tool

```
python _Tools/tflite_inspect.py <model.TFL | model_data.cc> [--all]
```

It takes a model file or a scenario app's C array, prints the layers (`--all` for every one) and the
input and output tensors, and ends with a verdict: probabilities or logits. It needs only the Python
standard library.

## Issue to post

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
