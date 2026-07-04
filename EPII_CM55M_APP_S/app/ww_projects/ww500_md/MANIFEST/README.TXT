WW500 Configuration Files - CGP 5 May 2026
================================================

The MANIFEST folder is a folder to place onto the SD card containing configuration files.

As of the time of writing, the key configuration file(s) are:

* CONFIG.TXT = contains the Operational Parameters which define things like time lapse interval.
* HM0360EX.BIN = Extra register settings for the HM0360 sensor (optional, usally not present).
* 1V2.TFL - optionally, a neural network model file (numbering: project 1, version 2)
* 1V2.TXT - optionally, a text file with class names for 1V2.TFL

The neural network model files should match the operational parameters #14 and #15

At the time of writing the firmware looks for the above four files in the MANIFEST folder.

* config_file.md is not used by the WW500. It provides documentation for the user.

Firmware files for the AI processor can be placed here and programmed into the AI processor
with the commands 'firmware <filename>'
