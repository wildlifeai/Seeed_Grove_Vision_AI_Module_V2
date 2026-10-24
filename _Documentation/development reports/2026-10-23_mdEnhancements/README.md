# Explore motion detection enhancemnts

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-10-23_mdEnhancements\README.md`
#### Author: Charles Palmer
#### Date: 23 October 2026


## Background

The HM0360 has motion detection (MD) capabilities that are used at present. 
There are many MD regsiters in the HM0360 and some of these are available to the user through
Operational Parameters. It is possible if other regsiter settings were available to the user through
new Operational Parameters, then the user might be able to get finer adjustments of MD behaviour.

I don't have a view of the MD enhancements yet, but changing the threshold required to issue a MD interrupt would be 
one.

## Live View

There is a live preview facility documeted in the PR #140 documents 
(Live preview over the console UART (preview.c, _Tools/live_view.py))

At the time of writing I have not had this working. I want to get it running. I can see the pythin script runs on 
my machine, and what I assume is jpeg data being sent, but the image view area on my laptop stays black.
We should get that going.

I would like this feature to be documented.

Since the live view work was doen we have also done work to increase the speed of JPEG file transfers to the 
app via BLE. The live view uses the UART rather than BLE, but possibly there are some lessons that could be
transferred to the live view transfer.

## Adding MD grid on the live view screen

The HM0360 reports which of the 16 x 16 array of cells has MD chnages. It should be possibe to superimpose a
grid onto the image, so that the cells that have chnaged could be viewed.

This python script culd also have buttons, sliders etc that allowed some MD values to be changed by the user
dynamically. The script should be based on the live view work.


---

## Task for Claude

1.	Read this document and any dpcumentation that you can find concerning the live view facility.
2.	Look at code where relevant.
3.	Create a markdown document that proposes dealing with issues raised above in a series of 
steps that we can carry out one at a time. Place it in the same folder as this document.
4.	Don't write any code until after we have both reviewed that proposal document.

---

## Status

Open. Step 2 of the proposal done and bench-checked on 23 October 2026 (HM0360 and RP3 builds). Step 4 (MD grid overlay) benched 24 October: orientation right, 16 rows. Step 5 started: the viewer shows the motion flag against the MD interrupt (coded and first benched 24 October). Finding: MD INT stays set and MD_CTRL1 reads 0x06 although the firmware writes 0 each capture; a console test is pending (see the proposal, 24 October section). Steps 3 and 6 not started.

## Outcome (so far)

The live view now works on both builds (HM0360 and RP3): Free run, Stop, One image and Save frame all
produce visible frames. The black image area had two causes:

1. **Every frame was corrupted.** The IF task's console prints, logging the motion/AE message
   sent to the BLE processor, landed inside each frame line. Fixed in firmware: the IF task
   stays quiet while a frame is sent (`preview_isSending()`, `consoleQuiet()`).
2. **The viewer predated the 13 October cold-boot capture refusal**, and its typing kept the
   device awake (every console character means 60 s awake). Fixed in `live_view.py`: it tells
   cold and warm boots apart, sends `dpd` after a cold boot, and `--wake N` sets a timelapse
   wake so streaming starts by itself.

Details: [CLAUDE_mdEnhancements_proposal.md](CLAUDE_mdEnhancements_proposal.md). Durable doc:
[live_preview.md](../../live_preview.md).

## Open items

None filed yet. The remaining work is steps 3 to 6 of the proposal (speed, MD grid overlay,
live MD controls, op parameters); file issues as each is agreed.

## Files

| File | What it is |
|---|---|
| [CLAUDE_mdEnhancements_proposal.md](CLAUDE_mdEnhancements_proposal.md) | Stepped plan, with bench findings from 23 Oct and the 24 Oct register-bank and MD interrupt notes |
| [cross_repo_changes.md](cross_repo_changes.md) | Running list of changes the BLE processor and app need to know about, for the PR |
