WW500 Configuration Files - CGP 5 May 2026 (updated 8 July 2026)
================================================

The MANIFEST folder is a folder to place onto the SD card containing configuration
files. The website's "Prepare SD Card" page generates a ready-made copy of this
folder (Setup_Package.zip) with the latest firmware images and model files included.

As of the time of writing, the key configuration file(s) are:

* CONFIG.TXT = the Operational Parameters: time lapse interval, flash / AE light
  sensor settings, white-balance gains and more. See config_file.md. Parameters
  not present in the file fall back to firmware defaults.
* 1V2.TFL - optionally, a neural network model file (numbering: project 1, version 2)
* 1V2.TXT - optionally, a text file with class names for 1V2.TFL

The neural network model files should match operational parameters #14 and #15.
The firmware reads CONFIG.TXT and the model files from the MANIFEST folder.

Firmware images (AI processor)
------------------------------
The WW500 holds TWO firmware images in A/B flash slots - one per camera - and
switches between them ('slots' and 'switchslot' console commands, or the camera
selector in the mobile app):

* R......IMG = RP v3 colour camera (IMX708) firmware
* H......IMG = HM0360 night/IR camera firmware

Website-generated images are named VYMDDHMM.IMG after the firmware build time:
V = camera variant (R or H), Y = last digit of the year, M = month (1-9,
A = Oct, B = Nov, C = Dec), DD = day, H = hour (0-9, A-N = 10-23), MM = minute.

Firmware updates are normally applied from the mobile app (Firmware Update
screen), which programs the image into the matching slot and reboots into it.
The console command 'firmware <filename>' programs an image file directly.

Note: the extra camera register file (HM0360EX.BIN, RPV2_EX.BIN or RPV3_EX.BIN
depending on the camera) lives in the ROOT of the SD card, not in MANIFEST. It
is created and maintained automatically by the 'camreg' CLI command - do not
place it here.

* config_file.md is not used by the WW500. It provides documentation for the user.
