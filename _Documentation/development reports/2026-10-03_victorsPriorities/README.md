# Addressing some outstanding issues (Victor's priorities)

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-10-03_victorsPriorities\README.md`
#### Author: Charles Palmer
#### Date: 3 October 2026

## Purpose

This set of work will address some issues outstanding - some from Victor's email of 2/10/26.

## Deferred programming of PB7 as SENSOR_ENABLE

PB7 is both SWCLK and SENSOR_ENABLE (RP3 camera power). Making it a GPIO early (so an RP3 camera stays off in the
HM0360 build) stopped the SWD programmer taking control after reset.

At cold boot the RTC is now set first (about 1.4 s), and PB7 becomes a GPIO afterwards, in `checkForCameras()`. That
gives the SWD programmer time to take control. After a DPD wake (warm boot) the window is still short.

The PCA9574 must still drive FLASHEN low early, so it moved to a new `initFlashEarly()`, before the RTC set.

Confirmed working with the `Download bootloader to the board using SWD` section of
[Compile_and_flash.md](../../Compile_and_flash.md).

Related GitHub issues:

* [#200](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/200) (open): firmware that takes PB7
  breaks SWD. Addressed here, differently from its plan: PB7 is still taken in both builds, but only after the
  SWD window. Still to do from #200: document the hazard in `bootloader.md` and `SWD100_SWD_Programmer.md`.
* [#201](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/201) (closed, folded into #200):
  an `SWD_ENABLE` CONFIG.TXT key. Not needed.
* [PR #258](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/258) (open, on top of
  [PR #257](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/257)): keeps PB7 as SWCLK in the HM0360
  build only. Superseded by this change (suggested rejecting, 3 Oct), but its `bootloader.md` and
  `SWD100_SWD_Programmer.md` text could be reused for #200.

Comments to post (replace `#PR` with the PR number):

**#200:**
> Addressed in #PR, differently from the plan above. PB7 is still made a GPIO in both builds, but on a cold boot
> only after the RTC is set (about 1.4 s), so an SWD programmer can take control after a reset. Tested with the
> `Download bootloader to the board using SWD` steps in `Compile_and_flash.md`. After a DPD wake the window is still
> short. The HM0360 build keeps PB7 low so a fitted RP3 camera stays off. Still to do here: document the hazard in
> `bootloader.md` and `SWD100_SWD_Programmer.md`.

**#201:**
> For the record: not needed. #PR gives SWD about 1.4 s after every reset in both builds, without a CONFIG.TXT key.

**PR #258:**
> Superseded by #PR (see #200): SWD works after a reset in both builds, and the HM0360 build still holds an RP3
> camera off. Closing this; the `bootloader.md` and `SWD100_SWD_Programmer.md` text may be reused to finish #200.

## Others - from Victor's email of 2/10/26:

*  [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152) 
Every reset rewinds the clock - I suggest handling this as part of 
 [#251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251)
 'Restructure the way state is saved across DPD to decrease time to the first photo'
* [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56) - also handle in #251
* [#211](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/211) is fixed I think.
*  [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247)
I repeated the experiment and find that I can't use the lowest power mode. 
 This is documented in ww500_md/doc/WW500_Power_Measurements.md - keep this open and revisit later 
 when we want to spend time on further improvements to power consumption.
* [#158](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/158)
I still have not allocated any time to look at how Claude modified the RP3 camera code. 
 I should get round to it in due course, but don't propose looking at this now.
* [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246)
AI processor error bits - AFAIK there is no problem to address here. I have added a comment at this issue.

