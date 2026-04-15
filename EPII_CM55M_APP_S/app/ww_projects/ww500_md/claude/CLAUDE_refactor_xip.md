# Task: Refactor XIP flash access
#### CGP - 16 April 2026

## Neural Network Model

The HX6538 processor has an internal neural network (NN) processor that executes Tensor Flow Lite for Micro models, converted to Vela format.

This app is written to pass its captured images to the model, if present.

We allow users to change the models to suit their application. Models are placed in an SD card file like `1V2.TFL` where the two numbers are read from the configuration file CONFIG.TXT - these two entries:

```
	OP_PARAMETER_MODEL_PROJECT,		// 14 Model project ID used for the NN model
	OP_PARAMETER_MODEL_VERSION,		// 15 Model version number used for the NN model
```

There may also be a corresponding text file `1V2.TXT` that contains text representations of the output vector classes.

Code in the `cvapp.cpp` file is responsible for checking the presence of a NN model
 in the flash memory and/or on the SD card, and if necessary re-programming the XIP flash memory with a new model. The class names from the text file are also programmed (if present).

## XIP architecture

The HX6538 uses an external serial EEPROM chip connected as an XIP device. This is a 16M byte device. Memory is used as follows (starting from the lowest memry address):

* Firmware Image Slot A (1Mbyte, bootloader plus firmware image)
* Firmware Image Slot B (1Mbyte, bootloader plus firmware image)
* Space for NN models (13Mbytes)
* Slot A or Slot B firmware selector (last sector in the chip) 

The two firmware images and the selector sector are used to safely update firmware with new images. These are currently managed by downloading new firmware via a serial port (XMODEM) using the bootloader.

The NN model memory is under our control. Functions in `cvapp.cpp` can select a model in XIP memory and prepare it to be executed by NN library code.

The code is currently working as desired.

## Refactoring

I want to refactor the XIP code presently in `cvapp.cpp` for 2 reasons:

1. To clean it up, and remove functions that move NN models between the SD card files and the XIP memory, from the `cvapp.cpp` file and place them in their own file - let's call it `xip_manager.c` and `xip_manager.h` 

2. Shortly I expect that we will replace the existing firmware update mechanism so that instead of having the bootloader program the eeprom chip via the serial port, our own code will update the eeprom from a file loaded on the SD card. It will be useful to re-use XIP management functions currently used for the NN model, for this operation.

I have manually made some preparation for this refactoring, by looking for functions related to the XIP programming and renaming them with an "xip_" prefix - such as `xip_load_model_from_sd()`. These functions are probably the main ones that need to move to `xip_manager.c` but there will also be dependencies (such as variables) that will need cleaning up.

Note:

1. My function naming convention is that functions that are globally referenced should have a name starting the same way as the file name (e.g. "xip_" )
2. Functions that are purely internal to a file should not have that name.
3. In this case we have a mixture of C and C++ files.
4. I like to separate sections in the source files with lines like these (from `directory_manager.c`), to make it more human-readable:

```
/*************************************** Definitions *******************************************/

/*************************************** Global variables **************************************/

```

## Two phases

The refactoring should probably be done in 2 steps (Claude to advise).

* Step 1: Move the existing code to the new files - so this can be tested.
* Step 2: Several extra tasks:

1. It would be good to clean up the code in the new file to make it more elegant, maintainable and readable.
2. Add stubs that might allow for updating of the firmware (slots A, B and selector) - however we don't yet have enough documentation on the processor to complete this.
3. A level of documentation will be required - similar to what is in this file. 

## Tasks for Claude

1. Ask questions where that is helpful
2. Read the relevant files and propose a clean refactoring of XIP memory management.
   Key files are within the `ww500_md` folder; it is unlikely files outside it will
   be needed.
3. Place the analysis/proposal in a markdown file in this /claude folder for the user to review.
4. After review (and possible iteration), make the agreed changes.
