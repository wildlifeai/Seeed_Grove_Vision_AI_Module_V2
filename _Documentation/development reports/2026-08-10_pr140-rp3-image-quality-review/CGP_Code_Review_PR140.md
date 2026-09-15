# Code Review of PR # 140 By Charles
#### 8-9 August 2026
#### File: CGP_Code_Review_PR140.md
#### Author: Charles Palmer

__Search for ? before finishing!__

## Background

Victor made many changes in 3 PRs (see his email of 12/07/2026 21:43).

1. __feat/camera-features-combined (PR #141)__
2. __feat/ble-fast-transfer (PR #142)__
3. __feat/uart-live-preview (PR #140, still WIP)__

This document is the review by Charles of the third of these: PR #140
I have done this by using meld to compare the previous branch `feat/ble-fast-transfer` with the current review
branch `feat/uart-live-preview` so I can identify and examine the files identified as different.

## PR #140

This is summarised by [REVIEW_PR140.md](REVIEW_PR140.md) which identifies 3 areas of change:

1. Live preview over the console UART
2. Highlight-metered auto-exposure
3. Auto white balance + full colour pipeline

(Also this is Victor's email: "What it is: live camera preview over the console UART plus auto-exposure/white-balance 
tuning tools — built to sort the RP3 colour/quality issues. Functional but rough; 
review for direction rather than polish.")

## Operational Parameters Table - three new parameters:

These relate to the auto-exposure and white balance changes.

| Index | Name                                  | Default Value | Notes                                                |
| ----- | ------------------------------------- | ------------- | ---------------------------------------------------- |
|    29 | OP_PARAMETER_CAM_AE_ENABLE 			| 1             | RP camera auto-exposure: 0 = off (init-table exposure), 1 = on. Highlight-metered loop steps sensor exposure (8-5000 lines) then analog gain (to 16x) toward the target - see `ae.c` |
|    30 | OP_PARAMETER_CAM_AE_TARGET 			| 110           | Auto-exposure target: raw bright-quartile (p75) luma, 0-250 (0 = built-in default 95). Bright parts of the scene render just below white after the tone curve |
|    31 | OP_PARAMETER_CAM_WB_MODE 				| 1             | RP camera white balance: 0 = off (hardware JPEG), 1 = auto (warmth-biased grey-world measured per frame), 2 = manual op27/op28. Auto falls back to manual for flash-lit or too-dark frames - see `img_correct.c` |

These are reflected in the MANIFEST directory and [_Documentation/Operational_Parameters.md](../../Operational_Parameters.md)
 as well as the source code. 

#### New documentation:

* [_Documentation/live_preview.md](../../live_preview.md) - documents the live streaming of video over UART.
* [_Documentation/rp3-image-quality-plan.md](../../rp3-image-quality-plan.md) - documents a sophisticated process to improve the quality of RP3 images. Exact process is not 
described, nor are the results and what is done with the results.

#### New Python Tools:

These are related to the live preview feature.

* [_Tools/live_view.py](../../../_Tools/live_view.py) - streams JPEG images through the UART.
* [_Tools/tune_stats.py](../../../_Tools/tune_stats.py) "scores frames against reference photos"

`live_view.py` works with the new CLI `preview` command to accept live stream of JPEG images and display it on a laptop.

#### New or Changed Code:

`CLI-commands.c` has a new `preview` command that allows streaming of camera output to the UART, via `preview_setMode()`.

`preview.c` & `preview.h` - manage setting the mode and transferring the JPEG

`image_task.c` - several changes t.b.d.?

`ae.c` & `ae.h` - implements 'Highlight-metered auto-exposure'. There seems to be no stand-alone document about this:
only a brief mention on `REVIEW_PR140.md` and the source code in `ae.c`. This adjusts the RP3 camera AE and 
analog gain regsiters depending on the scene. This seems to happen on every image capture. More doc required!

`image_correct.c` has a new public function: 
```
bool img_correct_process_mode(uint8_t mode, uint16_t rManualQ8, uint16_t bManualQ8, bool flashLit);
```
which seems to perform colour correction based on whether the flash is on or not.

Also new private functions: 
* `wb_measure_yuv420()` - measures the average colour of the image
* `wb_auto_gains()` calculates new red and blue gains
These are then used to correct the image 

Also there is gamma correction applied, and also a colour-correction matrix. 

## C file formatting

The AI-generated .c and .h files did not group material into distinct, labeled section (includes,
defines etc). I asked Claude to create `doc/c_file_format.md` to define good practise. This can later move to be a 
generic document. I asked it to apply this style to these which were AI-generated:

* `ae.c & .h`
* `camera_switch.c & .h`
* `cis_file.c & .h`
* `img_correct.c & .h`
* `preview.c & .h`
* `sw_jpeg.c & .h`

## Build problem

For this build I placed the review branches in a different location which gave a compile error.
Claude concluded that the Windows path name got too long. It proposed a fix which is commented at the 
top of `ww500_md.mk` - involves use of this as a folder where the compiled object files are placed `WW500_BUILD_ROOT := D:/hxbuild`. 

That left another issue - the output .elf file was in a different location than usual. 
So I got Claude to edit the makefile to copy it to the right location. 

Then, on a roll, I have got it to do the remaining steps of 
generating output.img (hitherto done manually, documented in compile_and_flash.md section 3b)
and added this to the makefile. There is a new makefile with these steps at `ww500_md/mk/image_gen.mk`
and this generates the original `output.img` and also the file in the 8.3 format `VYMDDHMM.IMG` 
as documented in `_Documentation/firmware_update_and_recovery.md`

As a bonus I asked about the time it takes to build the cmsis_nn and tensorflow libraries when you do 'clean'.
It turns out that the SDK includes makefile switches to avoid the recompile, but they are turned off.
Claude has made a change in the makefile to override this. So a clean is now fast!

___QUESTIONS AND COMMENTS___

1.  I am very interested in discussing this PR140 work with Victor, so see how Claude has gone down this path.
Whose idea was it to do the UART streaming? Did Claude find my old `Sensecraft.md` document by itself?
2. Once I have completed the 3 code reviews I suggest a 2-stage operation: (i) merge this all into the dev branch. 
(ii) address some of the tasks again, to improve either the code or the documentation or both. This should be done
at least initially on Victor's machine in the hope that Claude there has retained some memory of how the tasks were 
completed, to inform the re-write.
3.  I have not attempted to move the task documentation into `_Documentation/development reports` as advocated
by Victor. We need to agree when this is done - e.g. before or after the merge.
4. We need to identify which of the new features and source files are purely for engineering work. For example
establishing IMX708 register settings that will be used every time in production. Perhaps that could could be separated
from the code used in the field e.g. with `#ifdef ENGINEERING` or placed into separate files. This to make it clearer
for future s/w engineers that have to maintain this code (it is getting increasingly complex).
5. Consider collating all of the RP3 deficits and fixes into one file for sending to Himax.
6. We need documentation on the major features that have been added by Claude. This should include the background of the issue,
where algorithms came from, how they can be tested by humans, how they can be used by our users 
(or engineers who are configuring the feature for users). We should discuss what and where it should be placed.
7. When we revisit some tasks it will be useful to document CONFIG.TXT settings used to demonstrate the feature.
This list is getting longer and it is becoming more difficult to figure out how to change settings.

Specific features discussed next, in the order identified at the top of this file:

8. __Live preview over the console UART__ I want to see this work, and understand how it has been used so far: just by Claude to make
autonomous tests, or by humans?
9. __Highlight-metered auto-exposure__ We need to understand the `ae.c` code - where did the aglorithm come from? what about the magic numbers and the IMX708 register
addresses? What is it doing, exactly? And importantly, how do we see if it works? Do we need to grab multiple
images for the algorithm to converge?
10. Can the AE adjustment values be saved for the next image? Most of the time the light conditions will be the same. 
Possibly the same situation as for the HM0360 AE determination: wake every 15 minutes to calibrate.
11. __Auto white balance + full colour pipeline__ Question: is all the white balance (and other?) code just converting 8-bit raw camera values to other 8-bit values? 
If so could all of the image correction be performed at the server? Or would the JPEG compression cause data to be lost
and so revent colour correction offline? There are obviously advantages in having the correction carried out on the WW500, 
but if it causes trouble like taking excessive time or power then maybe offline processing would be advantageous?
12. What is the new img_correct code doing? (Gamma correct etc etc). How are the magic numbers derived? 
Why are flash-lit images treated differently from non-flash lit? Have humans viewed the results and are we happy with them? 
Do we need a [colour test card](https://en.wikipedia.org/wiki/Test_Card_F)?
13. Should we add some code to measure how long some of these image corection operations take? (They involve doing arithmetic on every pixel so could be 
quite slow). Is the code optimised? I can't help thinking that the hardware accelerators in the HX6538
could accelerate this. Perhaps Himax can comment. Indeed, I am suprised that we have to do this
correction for the RP camera - if Himax are using it then why have they not dealt with it?




