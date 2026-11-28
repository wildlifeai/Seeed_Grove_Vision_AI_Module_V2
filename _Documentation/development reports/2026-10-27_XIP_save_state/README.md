# Save State in XIP Memory

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-10-27_XIP_save_state\README.md`
#### Author: Charles Palmer (summary by Claude (Opus 5.5), 31 October 2026)
#### Date: 27 October 2026

## Summary

**What it set out to do.** The thread started as a clean-up of the outstanding TODO issues on the
project board (nine issues, [the table below](#the-todo-list-where-each-item-stands)). The survey
([CLAUDE_todo_summary_and_plan.md](CLAUDE_todo_summary_and_plan.md)) showed that several of them
came down to one idea: keep some state in the XIP flash rather than only on the SD card. So the
thread was re-purposed to do that (the original brief is at the end).

**What we actually did** (27 to 31 October 2026, branch `261027_XIP_save_state`, benched on both
camera images):

___Allocated some of the XIP Flash memory to save a copy of some of the Operational Parameters, so that 
these can be read quickly after boot, so let us grab an image more quickly. Also use XIP to save RTC calibration data.___

1. **The first frame after a wake comes in about a third of the time** ([implements #251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251)). The camera
   starts from a copy of its settings in flash (the fast-start record) instead of waiting for the
   SD card. With the image task above the FatFS task, the NN started after the capture, two 10 ms
   waits removed and the HM0360 motion grid read in one I2C burst:

   | Wake | Before | Now |
   |---|---|---|
   | RP3, timer | 329 ms | 115 to 119 ms |
   | RP3, motion | not measured | about 120 ms |
   | HM0360, timer | about 270 ms | 75 ms |
   | HM0360, motion | about 270 ms | 92 ms, set by the sensor |

2. **The RP3 keeps its exposure across sleep** ([fixes #158](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/158)):
 Two new operational parameters OP_PARAMETER_CAM_AE_EXPOSURE and OP_PARAMETER_CAM_AE_GAIN. 
3. **The sleep timer is corrected on each board** ([fixes #245](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/245)). The RTC runs 2 to 4.6 % fast,
   differently per board, and the RC32K trim has no effect in DPD. Each board measures its own rate
   while awake, keeps it in flash and lengthens the alarm. A 300 s sleep went from 286 to 293 s to
   298.9 to 299.6 s.
4. **Found on the way:** with no SD card, a `setop` was lost after one sleep. The record is now
   saved in the XIP memory without a card.

**Deferred to the next thread:** the photo's time of day ([#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56), [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152), and the
clock's drift between updates from the BLE processor). The other TODO items are parked or wait on
decisions (table below).

**How it works now:** `ww500_md/doc/WW500_XIP_saved_state.md`.

## The documents

| Document | What it is |
|---|---|
| [CLAUDE_todo_summary_and_plan.md](CLAUDE_todo_summary_and_plan.md) | The survey of the TODO issues, 27 Oct: the starting point |
| [CLAUDE_XIP_save_state_proposal.md](CLAUDE_XIP_save_state_proposal.md) | The design and Charles's decisions, before any code |
| [CLAUDE_XIP_save_state_steps.md](CLAUDE_XIP_save_state_steps.md) | Each step as it was done, with every bench result (save state and wake speed) |
| [CLAUDE_time_accuracy.md](CLAUDE_time_accuracy.md) | The clock: the two problems, the experiments and the sleep timer fix (Results 1 to 8), and ideas for the time of day |
| `logs/` | The bench logs the documents refer to |

## Status

Closed with the PR from `261027_XIP_save_state`, 31 October 2026. The time of day continues in a
new thread on a new branch.

## Outcome

- **The flash record is a cache; CONFIG.TXT stays the master.** A card's values win when it loads,
  and differences are printed. One shared fast-start record, not one per camera image. Records are
  written only when something has changed, to spare the flash.
- **Nothing on the path to the first frame waits for the card** any more. Whatever needs the card
  (file name, EXIF, counters, the photo) waits for it later, while the frame is already in RAM.
- **The HM0360's 92 ms after a motion wake is a known limit, not an issue:** the sensor delivers
  its first frame then however early the capture starts. Why is not known.
- **The RTC is corrected in software, per board, by self-calibration.** The trim register was ruled
  out by the bench (no effect in DPD), and a per-board value set at production was not needed: the
  awake measurement predicts the sleep within the alarm's one-second resolution.
- **The RP3's light matters when comparing wake times:** its capture time follows the restored
  exposure (24 ms in bright light, up to 88 ms in dim).

## Cross-repo

Op parameters and BLE commands are contracts with ww-hardware, ww-mobile-app, ww-website and
ww-backend (`.agents/skills/references/cross-repo-contracts.md`). This thread adds to them; it
changes nothing that already existed.

- **New op parameters 37 and 38** (`OP_PARAMETER_CAM_AE_EXPOSURE`, `OP_PARAMETER_CAM_AE_GAIN`): the
  RP3's auto-exposure state, set by the firmware, not by the user (0 in CONFIG.TXT; `setop` refuses
  values out of range). Defined in `MANIFEST/config_file.md`.
  - ww-hardware `aiProcessor.h` needs them. Until then the BLE processor ignores them, which is
    safe.
  - ww-mobile-app, ww-website, ww-backend: need them only if they keep their own list of ops. They
    should be shown read-only, if at all.
  - There are now 39 ops, the most that is certain to fit the "Sleep" message to the BLE processor
    (240 of 244 bytes with every value at 65535). An op 39 would need that message looked at first.
    The AI processor's buffer for it was 150 bytes and is now the full 244 (`if_task.c`).
- **New console and BLE commands** `AI xipstate [test]` and `AI rtccal [measure | on | off]`
  (`_Documentation/ble_commands.md`). For the bench; the app does not need them.
- **No EXIF changes.**
- **Not filed yet:** the ww-hardware issue for ops 37 and 38.

## The TODO list: where each item stands

31 October 2026.

| # | Item | Where it stands |
|---|---|---|
| [#251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251) | Save state in flash for a faster wake | **Done.** Close on merge |
| [#158](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/158) | RP3 forgets its exposure at sleep | **Done.** Close on merge. ww-hardware `aiProcessor.h` still needs ops 37 and 38 |
| [#245](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/245) | Sleep timer about 5 % fast | **Done.** Close on merge; the clock's drift moves to #56 |
| [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56) | Clock stale after a motion wake | Deferred to the next thread |
| [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152) | A restart sets the clock to 2024 | Deferred to the next thread |
| [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247) | HM0360 keeps taking frames with MD off | Parked, with the power work |
| [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246) | Self-test bit 14 never reaches the app | Waiting on a decision |
| [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) | Detection tests class 1 alone | Waiting on a decision |
| [#262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262) | LoRaWAN message content | Waiting on a decision |

## Open items

- [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56), [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152): the time of day, next thread. Starting point: Problem 2 in
  [CLAUDE_time_accuracy.md](CLAUDE_time_accuracy.md).
- [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247), [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246), [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270), [#262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262): as in the table.
- Not yet filed (Charles to decide):
  - a fast paste into the console hung it, then the IF task stayed in `I2C RX State` and the
    camera never slept (step log, "Parked: console stopped responding");
  - not tested with a second make of SD card since the card's settling wait was removed;
  - ww-hardware: ops 37 and 38 in `aiProcessor.h` (an issue in that repo).

---

## The original brief (Charles, 27 October 2026)

### Background

The github.com repo contains a large number of issues and TODO items. 

Many of these will have been dealt with already, but some still need work.

As of the time of writing, the mis-named `1261023_mdEnhancements` branch is the 'latest'
branch on my laptop, as PRs have not yet been finalised on github.com. I have created the 
`261027_XIP_save_state` branch from `1261023_mdEnhancements` 

The TODO list is [here](https://github.com/orgs/wildlifeai/projects/3/views/3?filterQuery=repo%3Awildlifeai%2FSeeed_Grove_Vision_AI_Module_V2) 

--- 

### Save State in XIP Memory

After Claude created `CLAUDE_todo_summary_and_plan.md` it is apparent that many of the TODO items relate 
to saving state in the XIP memory rather than (or as well as) the SD card.

So this development thread has been re-purposed, form `Deal with outstanding TODO items`
to `Save State in XIP Memory`
 
Github already contains ideas about doing this - these should be referenced.
 
See new instructions for Claude below.

### Clock adjustments

We tuned the HX6538 clock on a single board, to reduce drift.

This could usefully be done on every board, but I suspect that the drift will differ between boards,
so the adjustment becomes unique to each board. The adjustment value could be placed into XIP memory.

---

### Task for Claude

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
