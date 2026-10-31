# Save State in XIP Memory

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-10-27_XIP_save_state\README.md`
#### Author: Charles Palmer
#### Date: 27 October 2026

> **Speed improvements (30 October 2026).** The time from a wake to the first frame (when the
> scene is captured) is now about a third of what it was:
>
> | Wake | Before this thread | Now |
> |---|---|---|
> | RP3, timer | 329 ms | 115 to 119 ms |
> | RP3, motion | not measured | about 120 ms (135 before the burst read) |
> | HM0360, timer | about 270 ms (estimate) | 75 ms |
> | HM0360, motion | about 270 ms (estimate) | 92 ms, set by the sensor (see Outcome) |
>
> What made the difference: the camera no longer waits for the SD card (settings cached in
> flash), the image task runs above the FatFS task, the NN starts after the capture starts, two
> fixed 10 ms waits removed, and the motion grid read in one I2C transaction (19 ms to 4 ms).
> Times are ms after `app_main()`, with a model in flash, in good light: the RP3's first frame
> takes longer in dim light (longer exposure). Details in the Outcome below.


## Background

The github.com repo contains a large number of issues and TODO items. 

Many of these will have been dealt with already, but some still need work.

As of the time of writing, the mis-named `1261023_mdEnhancements` branch is the 'latest'
branch on my laptop, as PRs have not yet been finalised on github.com. I have created the 
`261027_XIP_save_state` branch from `1261023_mdEnhancements` 

The TODO list is [here](https://github.com/orgs/wildlifeai/projects/3/views/3?filterQuery=repo%3Awildlifeai%2FSeeed_Grove_Vision_AI_Module_V2) 

--- 

## Save State in XIP Memory

After Claude created `CLAUDE_todo_summary_and_plan.md` it is apparent that many of the TODO items relate 
to saving state in the XIP memory rather than (or as well as) the SD card.

So this development thread has been re-purposed, form `Deal with outstanding TODO items`
to `Save State in XIP Memory`
 
Github already contains ideas about doing this - these should be referenced.
 
See new instructions for Claude below.

## Clock adjustments

We tuned the HX6538 clock on a single board, to reduce drift.

This could usefully be done on every board, but I suspect that the drift will differ between boards,
so the adjustment becomes unique to each board. The adjustment value could be placed into XIP memory.

---

## Task for Claude

1.	Read this document and the items in the TODO list on github.com
2.	Create a markdown document `CLAUDE_todo_summary_and_plan.md` that summarises each of the TODO items. 
Place it in the same folder as this document. 
In particular the document should say whether they have been dealt with already. 
If they have not been dealt with, provide a simple jargon-free description of the problem.
3.	Then, propose a sequece for dealing with the TODO iitems raised above in a series of 
steps that we can carry out one at a time. 

** After creation of `CLAUDE_todo_summary_and_plan.md`:

4. Leave `CLAUDE_todo_summary_and_plan.md` as a list of work to do.
5. Create a new proposal document to use XIP memory to save some operational parameters and also the clock adjustment settings.
Include a brief summary of the points already documented, but write them afresh, and succinctly.
6. Ask questions as required.
7. Don't write any code until after we have both reviewed that proposal document.

---

## Status

Open. Save state in flash is done and benched on both camera images (steps 1 to 7, 27 to 30
October 2026). The sleep timer (#245) is corrected and benched (31 October). The time of day
(#245's clock half, #56, #152) is next.

## Outcome so far

**Time from wake to the first frame** (the measure that matters: when the scene is captured, not
when the photo is saved), in ms since `app_main()`:

| | Before | Now (30 Oct, model in flash) |
|---|---|---|
| RP3 timer wake | 329 | 115 to 119 |
| RP3 motion wake | not measured | about 120 (capture starts at 97; 135 in log20 before the burst read) |
| HM0360 timer wake | about 270 (estimate) | 75 |
| HM0360 motion wake | about 270 (estimate) | 92 |

How: a fast-start copy of the settings in flash, so the camera no longer waits for the SD card;
the image task above the FatFS task; the NN start-up moved after the capture starts; two fixed
10 ms waits removed; the HM0360 motion grid read at a motion wake in one I2C transaction (4 ms
instead of 19).

**Known limit (not an issue):** after a motion wake the HM0360 delivers its first frame about
92 ms after `app_main()`, however early the capture starts and whatever the motion-detection
interval (log26, log27). So on the HM0360 image the firmware is no longer what sets the
motion-wake time. Why the sensor takes that long is not known. CONFIG.TXT is still the master copy. How it works now:
`ww500_md/doc/WW500_XIP_saved_state.md`. Step by step, with every bench log:
[CLAUDE_XIP_save_state_steps.md](CLAUDE_XIP_save_state_steps.md). Design and decisions:
[CLAUDE_XIP_save_state_proposal.md](CLAUDE_XIP_save_state_proposal.md).

**Sleep timer (31 Oct):** the RTC runs 2 to 4.6 % fast, differently on each board, and the RC32K
trim does not affect it in DPD. Each board now measures its own rate while awake (`rtc_cal.c`, at a
cold boot and daily), keeps it in flash and lengthens the alarm. A 300 s sleep went from 286 to
293 s to 298.7 to 299.7 s, on three boards and both images. `rtccal` shows it. Also found: with no
SD card a `setop` was lost after one sleep; the fast-start record is now saved without a card.
Results: [CLAUDE_time_accuracy.md](CLAUDE_time_accuracy.md) (Results 1 to 8).

## The TODO list: where each item stands (31 October 2026)

| # | Item | Where it stands |
|---|---|---|
| [#251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251) | Save state in flash for a faster wake | **Done** (steps 1 to 7). Ready to close once merged |
| [#158](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/158) | RP3 forgets its exposure at sleep | **Done** (ops 37, 38, also in the flash record). Closing comment drafted in the step log. The BLE processor's op list (ww-hardware `aiProcessor.h`) still needs ops 37 and 38 |
| [#245](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/245) | Sleep timer and clock 5 % fast | **Sleep timer done** (self-calibration, 31 Oct). The time of day still runs fast between updates from the BLE processor: next, with #56 and #152 |
| [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56) | Clock stale after a motion wake | Not started, after #245 |
| [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152) | A restart sets the clock to 2024 | Not started, after #245 |
| [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247) | HM0360 keeps taking frames with MD off | Parked, with the power work |
| [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246) | Self-test bit 14 never reaches the app | Waiting on a decision |
| [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) | Detection tests class 1 alone | Waiting on a decision |
| [#262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262) | LoRaWAN message content | Waiting on a decision |

So two of the nine are done, the sleep half of a third is done, three (the time of day) are next
in this thread, one is parked and three wait on decisions.

## Open items

- [#245](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/245),
  [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56),
  [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152): the time of day, next in this thread (the sleep timer is done). Analysis and ideas:
  [CLAUDE_time_accuracy.md](CLAUDE_time_accuracy.md).
- Found along the way, not yet filed as issues (Charles to decide):
  - a fast paste into the console hung it, then the IF task stayed in `I2C RX State` and the
    camera never slept (step log, "Parked: console stopped responding");
  - not tested with a second make of SD card after the card's settling wait was removed.
