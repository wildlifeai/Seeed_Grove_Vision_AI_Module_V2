#Himax Model Zoo - interpretation
#### CGP 24/1/26

The model zoo folder contains several subfolders which in turn contain .tflite model files.
After I got the person detection model code working satisfactorily, and dynamic model loading working, I decided to 
test some of these models. 

I did not provide .TXT files with class names. (Does not crash without these!)

Results below include the console output from our code, 
and an anlysis from ChatGPT of some of the results.

Before I list the results I provide a summary:

## Executive Summary

The code is resiliant in the face of models it was not designed for. I can get error messages, and perhaps meaningless
results, but the models load from SD card and execute.

The input and output tensor sizes are extracted from the model. The input image is presuambly scaled to the size
defined by the input tensor information in the model. Though our code only works with mon input tensors at present.

Even large models (c. 2M bytes) load and execute OK.

There are some models that involve bounding boxes - these are identified in the "fingerprint" message.
Our code does not process these sensibly.

Some models require "resolvers" that need to be added by code. Discussed [here by ChatGPT]()
As an example, the `tflm_yolo11_od` scenario app has this code, which loads two
extra resolvers in addition to the EthosU resolver required for the NN processor:
```
		static tflite::MicroMutableOpResolver<3> yolo11n_ob_op_resolver;

		yolo11n_ob_op_resolver.AddTranspose();
		yolo11n_ob_op_resolver.AddBatchMatMul();
		if (kTfLiteOk != yolo11n_ob_op_resolver.AddEthosU()){
			xprintf("Failed to add Arm NPU support to op resolver.");
			return false;
		}
```
If the model expects these operators and the code has not loaded them you get an error.
As an experiment I added some of these to give the models a better chance of running.
(The person detection model and some others require just the EthosU resolver.)
		
When I tried to add `AddBatchMatMul()` to my project I got a compiler error (function not found).

It turns out that Himax supply two different TFLM libraries. ChatGPT tell me the numbers are timestamps:

* tflmtag2209_u55tag2205 - 2022 version does not include `AddBatchMatMul()`
* tflmtag2412_u55tag2411 - 2024 version includes `AddBatchMatMul()`

If you look at the various scenario apps you see one or other (or both) referenced in the makefile,
such as this (old library commented out and replaced):
```
# LIB_SEL = pwrmgmt sensordp tflmtag2209_u55tag2205 spi_ptl spi_eeprom hxevent img_proc
LIB_SEL = pwrmgmt sensordp tflmtag2412_u55tag2411 spi_ptl spi_eeprom hxevent img_proc
```

I made the change to our project as well, in the makefile. It turns out the the functions in the new library
have some different signatures than the old library, and I had to adjust for that as well -
see the `#ifdef TFLM_2412` sections.

Some models need more arena area than our code allows. 

Some applications have multiple models which are run in succession, such as the gender ID model:
"This is the scenario_app which will run face detection first and then choose
 the biggest face to do the gender classification."


## tflm_fd_fm

There are 3 models in here. The 'README.md' file says this is a face mesh application and shows a bounding box.
It might be searching for a face then creating a mesh then doing something else. The code has these model pointers:

```
    static const tflite::Model*model = tflite::GetModel((const void *)fd_model_addr);
	static const tflite::Model*FM_model = tflite::GetModel((const void *)fm_model_addr);
	static const tflite::Model*IL_model = tflite::GetModel((const void *)il_model_addr);
```	
The 3 models will be loaded in different parts of the XIP flash and the code will use them in succession, I think

The source code certainly processes bounding boxes and so I don't expect this to run in any meaningful way with our 
current code.

I copied and renamed the models as follows, and I report on the results below:

* 0_fd_0x2000000.tflite -> 11V2.TFL

```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 1
  Op 0: CUSTOM (ethos-u), weight bytes: 468572
Input tensor   : i8 [1,160,160,1]
Output tensor  : i8 [1,5,5,18]
Total weight bytes : 468628
---------------------------------
```
The ChatGPT Analysis is [here](https://chatgpt.com/share/69745cb0-c11c-8005-8226-2f3f1bd23f54)

It is worth noting that our code does not crash, and it does load the model

* 1_fm_0x2800000.tflite -> 11V1.TFL

```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 7
  Op 0: CUSTOM (ethos-u), weight bytes: 660776
  Op 1: OTHER, weight bytes: 32
  Op 2: CUSTOM (ethos-u), weight bytes: 660836
  Op 3: OTHER, weight bytes: 32
  Op 4: CUSTOM (ethos-u), weight bytes: 660912
  Op 5: OTHER, weight bytes: 32
  Op 6: CUSTOM (ethos-u), weight bytes: 677828
Input tensor   : i8 [1,192,192,3]
Output tensor  : i8 [1,1,1,1404]
Total weight bytes : 684980
---------------------------------
Didn't find op for builtin opcode 'PAD' version '1'. An older version of this builtin might be supported. Are you using an old TFLite binary with a newer model?

Failed to get registration from op code PAD
```

The ChatGPT Analysis is [here](https://chatgpt.com/share/69746060-3320-8005-9e14-d7897fd750ad)

I added code for the missing operator:
```
op_resolver_ptr->AddPad()
```
and on the next build I got a different message:
```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 7
  Op 0: CUSTOM (ethos-u), weight bytes: 660776
  Op 1: OTHER, weight bytes: 32
  Op 2: CUSTOM (ethos-u), weight bytes: 660836
  Op 3: OTHER, weight bytes: 32
  Op 4: CUSTOM (ethos-u), weight bytes: 660912
  Op 5: OTHER, weight bytes: 32
  Op 6: CUSTOM (ethos-u), weight bytes: 677828
Input tensor   : i8 [1,192,192,3]
Output tensor  : i8 [1,1,1,1404]
Total weight bytes : 684980
---------------------------------
There are 124 classes (0)
```
The model runs but gives meaningless results from the output tensor.

* 2_it_0x32A0000.tflite -> 11V2.TFL

```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 3
  Op 0: CUSTOM (ethos-u), weight bytes: 750220
  Op 1: OTHER, weight bytes: 32
  Op 2: CUSTOM (ethos-u), weight bytes: 780832
Input tensor   : i8 [1,64,64,3]
Output tensor  : i8 [1,15]
Total weight bytes : 786776
---------------------------------
Didn't find op for builtin opcode 'PAD' version '1'. 
An older version of this builtin might be supported. 
Are you using an old TFLite binary with a newer model?

Failed to get registration from op code PAD
```
The ChatGPT Analysis is [here](https://chatgpt.com/share/6974612b-4de4-8005-9ff1-4c481bdea147)

I added code for the missing operator:
```
op_resolver_ptr->AddPad()
```
and on the next build I got a different message:
```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 3
  Op 0: CUSTOM (ethos-u), weight bytes: 750220
  Op 1: OTHER, weight bytes: 32
  Op 2: CUSTOM (ethos-u), weight bytes: 780832
Input tensor   : i8 [1,64,64,3]
Output tensor  : i8 [1,15]
Total weight bytes : 786776
---------------------------------
There are 15 classes (0)
```
The model does run and may well be giving meaningful outputs (the lables are absent):
```
Model invoked.
DEBUG: cv_run says there are 15 classes
Class 0 '' = logit 104
Class 1 '' = logit 75
Class 2 '' = logit -102
Class 3 '' = logit 111
Class 4 '' = logit 75
Class 5 '' = logit -102
Class 6 '' = logit 105
Class 7 '' = logit 64
Class 8 '' = logit -103
Class 9 '' = logit 98
Class 10 '' = logit 75
Class 11 '' = logit -102
Class 12 '' = logit 104
Class 13 '' = logit 86
Class 14 '' = logit -103
TARGET OBJECT DETECTED!
```

* -> 11V3.TFL
```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 1
  Op 0: CUSTOM (ethos-u), weight bytes: 468572
Input tensor   : i8 [1,160,160,1]
Output tensor  : i8 [1,5,5,18]
Total weight bytes : 468628
---------------------------------
There are 18 classes (0)
```
 Again the output shape suggest bounding boxes.
 
## tflm_yolo11_od

The model zoo has 2 entries - both about 2M bytes in size.

I copied and renamed the models as follows, and I report on the results below:

* yolo11n....nopost_241230.tflite -> 22V1.TFL

The large (c. 2M byte) model loaded OK into flash.

```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 12
  Op 0: CUSTOM (ethos-u), weight bytes: 1959200
  Op 1: OTHER, weight bytes: 16
  Op 2: CUSTOM (ethos-u), weight bytes: 1940304
  Op 3: OTHER, weight bytes: 16
  Op 4: CUSTOM (ethos-u), weight bytes: 1975992
  Op 5: OTHER, weight bytes: 0
  Op 6: CUSTOM (ethos-u), weight bytes: 1942732
  Op 7: OTHER, weight bytes: 16
  Op 8: OTHER, weight bytes: 0
  Op 9: CUSTOM (ethos-u), weight bytes: 1940004
  Op 10: OTHER, weight bytes: 16
  Op 11: CUSTOM (ethos-u), weight bytes: 1966024
Input tensor   : i8 [1,224,224,3]
Output tensor  : i8 [1,28,28,144]
Total weight bytes : 2025617
---------------------------------
There are 144 classes (0)
```

When it ran:
```
Input tensor is 224 x 224 (3 channels)
DEBUG: ledFlashDisable()
Model invoked.
DEBUG: cv_run says there are 144 classes
Class 0 '' = logit 46
Class 1 '' = logit 45
Class 2 '' = logit 20
Class 3 '' = logit 7
<snip>
Class 142 '' = logit -77
Class 143 '' = logit -71
TARGET OBJECT DETECTED!
<snip>
NN processing took 80ms
```
Because the output tensor shape had more entries, the output values have been misinterpreted,
but the model apparently loaded and ran, and the input image size of 244 x 244 x 3 colours at least did not cause a crash.
(I don't know if the resizing function handles more than 1 colour at present). 

* yolo11n....matmul_vela.tflite -> 22V2.TFL

```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 20
  Op 0: CUSTOM (ethos-u), weight bytes: 1962344
  Op 1: OTHER, weight bytes: 16
  Op 2: CUSTOM (ethos-u), weight bytes: 1945280
  Op 3: OTHER, weight bytes: 16
  Op 4: CUSTOM (ethos-u), weight bytes: 1981980
  Op 5: OTHER, weight bytes: 0
  Op 6: CUSTOM (ethos-u), weight bytes: 1947708
  Op 7: OTHER, weight bytes: 16
  Op 8: OTHER, weight bytes: 0
  Op 9: CUSTOM (ethos-u), weight bytes: 1944980
  Op 10: OTHER, weight bytes: 16
  Op 11: CUSTOM (ethos-u), weight bytes: 1954684
  Op 12: OTHER, weight bytes: 16
  Op 13: CUSTOM (ethos-u), weight bytes: 1951284
  Op 14: OTHER, weight bytes: 16
  Op 15: CUSTOM (ethos-u), weight bytes: 1954532
  Op 16: OTHER, weight bytes: 16
  Op 17: CUSTOM (ethos-u), weight bytes: 1945412
  Op 18: OTHER, weight bytes: 16
  Op 19: CUSTOM (ethos-u), weight bytes: 1948712
Input tensor   : i8 [1,192,192,3]
Output tensor  : i8 [1,84,756]
Total weight bytes : 2033416
---------------------------------
Failed to resize buffer. Requested: 1076720, available 515080, missing: 561640
No model found.
```

The "Failed to resize buffer" message seems to come from an area buffer allocator

## tflm_yolov8_gender_cls

There are 2 models in here. Its README.md file says:
"This is the scenario_app which will run face detection first and then choose the biggest face to do the gender classification. "

I copied the models as follows:

* 0_fd_0x200000.tflite -> 33V1.TFL

```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 1
  Op 0: CUSTOM (ethos-u), weight bytes: 468572
Input tensor   : i8 [1,160,160,1]
Output tensor  : i8 [1,5,5,18]
Total weight bytes : 468628
---------------------------------
There are 18 classes (0)
```

This suggests a 160 x 160 mono input, but the output tensor shape suggests bounding boxes etc.
The model did run, though the interpretation of teh outputs is meaningless.


* 1_gender_cls_0x280000.tflite -> 33V2.TFL

This gives
```
TFLM Ethos-U55 Model Fingerprint
---------------------------------
Schema version : 3 (expected 3)
Subgraphs      : 1
Operators      : 1
  Op 0: CUSTOM (ethos-u), weight bytes: 1253436
Input tensor   : u8 [1,160,160,3]
Output tensor  : u8 [1,2]
Total weight bytes : 1253604
---------------------------------
There are 2 classes (0)
```
The input size is 160 x 160, 3 colours. The output size this time suggest just 2 categories (male and female?)
Output results include:
```
Model invoked.
DEBUG: cv_run says there are 2 classes
Class 0 '' = logit -109
Class 1 '' = logit 109
TARGET OBJECT DETECTED!
<snip>
NN processing took 32ms
```

