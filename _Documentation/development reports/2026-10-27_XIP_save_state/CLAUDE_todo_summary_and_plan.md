# Outstanding TODO items: where each one stands, and a proposed order

#### File: `CLAUDE_todo_summary_and_plan.md`
#### Path: `_Documentation\development reports\2026-10-27_XIP_save_state\CLAUDE_todo_summary_and_plan.md`
#### Author: Claude (Opus 5.5), for review by Charles Palmer
#### Date: 27 October 2026

## The TODO list (the answer to the task)

The TODO swimlane on the project board holds nine issues. None of them has been fixed. Each was
checked against the code on `261027_XIP_save_state`.

| # | Plain description | Where it stands |
|---|---|---|
| [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56) | The AI processor's clock stops while it sleeps. After a motion wake, photos are stamped with the time it went to sleep, and the error grows with every sleep. | Open. The quick clock read after a wake is still used (`ww500_md.c:689`). You deferred it to #251. |
| [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152) | Every restart (a reset, a camera switch, a firmware update) sets the clock to 1 January 2024. Photos stay dated 2024 until the BLE processor sends the time, and it can only do that if it has the time itself. | Open, on purpose so far (`ww500_md.c:642`). You deferred it to #251. |
| [#158](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/158) | The RP3 forgets its exposure every time it sleeps, so the first photos after a wake in bright light can come out white. | Open. The exposure is still only held in RAM (`ae.c:63`). It fits with #251. |
| [#251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251) | Most of the delay before the first photo after a wake is reading the SD card. Saving the state in flash would make the wake faster. | Open. There is a proposal (`2026-09-30_App_Comms_Bugs/CLAUDE_Time_to_first_photo.md`), but no code yet. |
| [#245](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/245) | The sleep timer and the clock run about 5 % fast, so a 90 s timelapse is really about 85 s, and the clock gains about 3 minutes an hour. | Open. You found trim 33 in `ww500_minimal`, but `ww500_md` does not apply a trim yet. |
| [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247) | With motion detection off, the HM0360 keeps taking frames while the camera sleeps, which wastes power (about 175 to 330 uA). | Open, parked. Your sleep-mode test still showed extra current that nobody can explain (`WW500_Power_Measurements.md`). |
| [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246) | Self-test bit 14 ("the BLE processor did not answer at boot") is cleared before anyone outside the AI processor can see it. | A decision is needed. Victor chose (10 Oct) to keep the bit set until a `selftest` reply has carried it. Your comment (4 Oct) said no action was needed. The code still clears it (`if_task.c:904`). |
| [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) | The camera reports a detection when class 1 alone is above op 16. The model challenges rank models on class 1 minus class 0, so with some models (the rat model, for example) the camera can never flag a detection. | A decision is needed: change the firmware test (op 16 becomes signed, which affects the app and the website), or change how models are exported (no firmware change). |
| [#262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262) | What should the LoRaWAN messages sent to the server contain? Today: a regular ping, and a count of positive detections. | A decision is needed. It is mostly a BLE processor and server question, and the AI processor's part depends on the answer. |

### Proposed order

1. **Decide the three questions** (#246, #270, #262). No code. If #246 is a yes, it is a few lines
   in `if_task.c`.
2. **#245, the clock trim.** Small, and easy to measure. Doing it first means the clock work in
   step 3 starts from a timer that keeps good time.
3. **#251 with #56, #152 and #158.** First review the existing proposal together, then build it in
   stages: saved state, then the clock, then the RP3 exposure. This step may need BLE processor
   changes for the time.
4. **#247** stays parked until power work resumes.

---

The rest of this document covers **all 52 open issues** in the repo. It was written before the
TODO list was clarified, and is kept for reference.

## How this was done

- **The list:** the 52 open issues in this repo on 27 October 2026. The project board view needs a
  login, so I used the repo's open issues, which are what the board's filtered view shows. Two open
  PRs (#277 `dev` to `main`, #134 upstream sync) are not TODO items and are left out.
- **The check:** each issue was read, with its comments, and compared with the code on
  `261027_XIP_save_state` (from `261023_mdEnhancements`, last commit e3c4d18a).
- **Limits:** I read the code but did not run anything on a board. "Done" below means the code now
  does what the issue asked. It does not mean a bench test has confirmed it.

## Summary

| Status | Count | Issues |
|---|---|---|
| Partly or nearly done (the issue text needs cutting down) | 6 | section 1 |
| Still open, firmware work | 33 | section 2 |
| Documents, bench work or other people's work | 8 | section 3 |
| Only a decision so far | 5 | section 4 (it also lists #209 part 2 and #246) |

Nothing on the list has been completely fixed and left open by mistake. Six issues have been partly
fixed, and the issue text no longer matches the code.

---

## 1. Partly or nearly done

| # | What it is about | Where it stands |
|---|---|---|
| [#207](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/207) | A setting changed just before sleep was lost, while the app was told it had been saved. | **Main bug fixed**: the save no longer goes to an unmounted card (`fatfs_task.c:763`). Still left: the reply says "Set OpParam" even when the save failed, and the event name table has no entry for 0x0908. |
| [#266](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/266) | The HM0360 motion sensor can stop waking the camera, and nothing tells the user. | Part 1 (`inithm0360` in the colour image) done in #260. Part 2 is open: a way for the app to see "armed but never triggered". Charles's idea of re-programming the HM0360 before each sleep has not been tried. |
| [#250](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/250) | A list of small fixes from the #240 review. | One item done (#259). Still open: the console buffer overrun (`CLI-commands.c:2745`), the `ww500_minimal` image name, four doc fixes, and the stray log file in `development reports/`. |
| [#184](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/184) | Running with only one camera should be a normal setup, not something that looks like a fault. | Item 1 done: automatic camera switching (op 26) now defaults to off. Still open: do not wake to check the light when there is no other camera to switch to, and make `slots` show "one image installed" instead of `unknown`. |
| [#209](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/209) | Some settings only take effect after the next sleep. | Part 1 (flash mode op number) done. Part 2 is a decision, see section 4. |
| [#231](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/231) | If the RP3 refuses to start streaming, the app is not told. | **Probably eased by #275:** the new frame timer should now send `Capture failed ... No frame` after about 12 s, so the app is no longer left waiting forever. Not tested on a board. The root cause is still there: the start failure is ignored (`cisdp_sensor_start()` returns nothing). |

---

## 2. Still open: firmware

### Wake-up speed, the clock and saved state

| # | Plain description |
|---|---|
| [#251](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/251) | Most of the delay before the first photo after a wake is reading the SD card. Saving the state in flash instead would make it faster. There is a proposal (`2026-09-30_App_Comms_Bugs/CLAUDE_Time_to_first_photo.md`). |
| [#56](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/56) | The AI processor's clock does not move on while it sleeps, so photos taken after a motion wake are stamped too early, and the error grows with each sleep. Charles deferred this to #251. |
| [#152](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/152) | Every restart (not a wake from sleep) sets the clock to 1 January 2024 on purpose. Photos stay dated 2024 until the BLE processor sends the time, which it can only do if it has the time itself. Deferred to #251. |
| [#158](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/158) | The RP3 forgets its exposure every time it sleeps, so the first photos after a wake in bright light can come out white. Fix: save the exposure with the rest of the state (fits #251). |
| [#245](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/245) | The sleep timer runs about 5 % fast, so a 90 s timelapse is really about 85 s, and the clock gains about 3 minutes an hour. You found trim 33 in `ww500_minimal`, but `ww500_md` does not apply a trim yet. |
| [#217](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/217) | Remove comment lines from CONFIG.TXT. They slow every wake and every save, and use 3 KB of RAM. Agreed, low priority. |

### The app is not told when something goes wrong

| # | Plain description |
|---|---|
| [#208](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/208) | If the gap between photos in a burst is longer than the inactivity time, the camera goes to sleep half way through the burst and says "Sleep" to the app. Still in the code: `lastMessageSent` is never cleared, and the shutdown barrier counts calls, not tasks. |
| [#241](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/241) | Loading a bad model file keeps the camera awake until the power is cycled, so motion detection stops. A missing check (`cvapp.cpp:489-496`), a few lines to fix. |
| [#230](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/230) | `setgps` says "Device GPS set" even when it could not read the coordinates. This hid an app bug where GPS was never written to photos. |
| [#234](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/234) | An SD card taken out and put back while the board has power is never used again until the power is cycled, so a routine card swap makes a working camera look broken. |
| [#212](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/212) | Once a burst of photos starts there is no way to stop it, so the app's "Stop" button in the motion test only stops listening. |
| [#195](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/195) | The app cannot ask the firmware what it supports, so a newer app on older firmware hangs instead of saying so. Also `getop -1` used to return only some of the values. It needs a check on a board to see if that still happens now that there are 37. |

### Safety and robustness

| # | Plain description |
|---|---|
| [#225](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/225) | Loading any model that is not a simple classifier writes past the end of a 16-byte buffer and corrupts memory. Not seen on a board, but the website used to offer such models. Fix: limit the copy and refuse such models at load. |
| [#267](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/267) | In the colour image, the `md` and `inithm0360` commands can take over the shared camera bus while a photo is being taken, sending RP3 commands to the HM0360. Not seen on the bench. |
| [#233](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/233) | The white LED is seen to blink every few minutes when the IR LED is selected and nothing is moving. Not investigated yet. First step is to watch a board and match each blink to the console log. |

### Small features and corrections

| # | Plain description |
|---|---|
| [#151](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/151) | After an interrupted firmware update, `slots` can say `unknown`, or name the wrong camera, for a slot. Label the slot on every boot, using the slot the code is actually running from. |
| [#269](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/269) | `slots` says which camera is in each slot but not which build, so a half-finished update cannot be seen. Pairs with #151. |
| [#265](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/265) | After the app sets a deployment ID, photos still go into the `MEDIA/00000000` folder until the next restart. |
| [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246) | Self-test bit 14 ("BLE processor did not answer at boot") is cleared before the app can ever see it. Victor's decision of 10 Oct was to keep it set until a `selftest` reply has carried it. Your comment of 4 Oct said no action was needed. Needs a final yes or no. |
| [#214](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/214) | In time-of-day flash mode, each periodic wake takes a photo it does not need, all night. Also add the flash mode to the light check line so the app can show it. |
| [#278](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/278) | Skip the photo when nearly the whole motion grid changes at once (a light switched on, the camera knocked). New op parameter, about 30 lines. App and CONFIG.TXT changes too. |
| [#213](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/213) | The app's motion test takes a full photo for every frame, when it only needs the 32-byte motion grid, so it cannot run as fast as the app offers. A grid-only mode would fix that. The new live view (#276) shows the grid but still takes a photo each time. |
| [#279](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/279) | When a camera fault happens, print the camera's internal state once, so the fault can be diagnosed later. Plus a `camdiag` command. The code exists on the closed PR #144. |
| [#242](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/242) | Add an EXIF tag saying which burst a photo belongs to, so the website can group them without guessing from the time. Cross-repo. |
| [#187](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/187) | Record in EXIF whether the white or the IR LED lit the photo. Today it only says "flash fired". Cross-repo. |

### Tidying the RP3 camera-register code (from the PR #141 review)

| # | Plain description |
|---|---|
| [#157](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/157) | The camera register file lives at the SD card root. It should be in `/MANIFEST` with the other files. Changes where users put the file. |
| [#160](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/160) | Remove a duplicate file name variable, a leftover debug print, and a second read of the same file at boot. |
| [#162](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/162) | The tool that turns a text file into the register file cannot turn it back, so settings saved from the camera cannot be read. Python only. |
| [#163](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/163) | Out-of-date comments and docs, for example op 26 still marked "PLANNED". |
| [#168](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/168) | Make the code that raises the inactivity time during a file transfer a named function, to match the one that restores it. Cosmetic. |

### Larger restructuring

| # | Plain description |
|---|---|
| [#185](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/185) | Move the EXIF building code out of `image_task.c` (now 3227 lines) into its own file. Best done before #187 and #242. |
| [#248](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/248) | CI does not build `ww500_minimal`, and its 30 copied files have drifted from `ww500_md`. |
| [#180](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/180) | Apply the C file layout standard across the code, with format-only commits. The include-order rule is done in code (#252) but not yet written into `c_file_format.md`. |

---

## 3. Documents, bench work, other owners

| # | Plain description |
|---|---|
| [#253](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/253) | `_Documentation/` has 50 files in one folder, with no index, and several cover the same topic. |
| [#171](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/171) | One write-up per major feature. Done so far: `light_sensor.md`, `camera_error_handling.md`, `live_preview.md`. Still to do: auto-exposure, white balance, camera switching, fast file transfer. |
| [#172](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/172) | One document for Himax listing the RP3 problems we had to work around. |
| [#173](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/173) | The RP3 colour correction and JPEG encode take about 230 ms per photo in software. Measure this, and ask Himax about the hardware accelerators. Goes with #172. |
| [#174](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/174) | Check RP3 colour against a standard colour test card. Tommy owns this. |
| [#57](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/57) | About one board in four or five, the HM0360 does not start at power-on, and only a power cycle fixes it. Hardware question for you: a capacitor on XSLEEP, or a switched supply on the next PCB (ww-hardware#70). |
| [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247) | With motion detection off, the HM0360 keeps taking frames, which wastes power. Your sleep-mode test still showed extra current. Parked until power work resumes. |
| [#280](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/280) | Higher-resolution RP3 photos (1216x960), proven on the closed PR #144. A large job, and it needs app, website and BLE download-speed work too. |

## 4. Waiting on a decision

| # | The question |
|---|---|
| [#204](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/204) | Op 23 no longer does anything, but the app still sets it and three documents (including `config_file.md`) still describe it as the light setting. Make the gain threshold a new op parameter, or say in the docs that it is fixed? Either way the docs need fixing. |
| [#209](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/209) part 2 | Should `setop` apply flash and sleep-timer settings at once, or should the app send `dpd` after changing them? |
| [#246](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/246) | Keep bit 14 set until the app has seen it (Victor), or no action (Charles)? |
| [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) | Detection tests class 1 only, but the model challenges score class 1 minus class 0. Change the firmware (op 16 becomes signed, cross-repo), or change how models are exported? |
| [#262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262), [#263](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/263) | What should LoRaWAN messages to and from the server contain? Mostly a BLE processor and server question. |
| [#170](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/170) | How do we mark which code is for the bench only (live preview, `camreg`) and which runs in the field? |

---

## 5. Proposed order

Each step is a branch and a PR you can test on its own. Small, low-risk fixes come first. The big
wake-up redesign comes after them, so it does not hold the quick fixes back.

**Step 1. GitHub housekeeping (no code).** I draft short comments for you to post on the six issues
in section 1, so each one says what is left. I also draft the decision questions in section 4, so
Victor can answer them while we work on steps 2 and 3.

**Step 2. Quick safety fixes.** Small changes that each fix a crash or a lie to the app:
- #225, limit the model output copy and refuse models it cannot handle
- #241, a bad model file fails cleanly and the camera sleeps
- #250, the console buffer overrun (in both apps)
- #230, `setgps` reports a failed parse
- #207, the leftover: say when a setting was not saved

**Step 3. Tell the app when a capture goes wrong.** These all touch the capture path, so it makes
sense to do them together:
- #208, no sleep half way through a burst, and fix the shutdown barrier
- #231, report a refused RP3 start straight away
- #212, a command to stop a burst
- #267, keep the console commands off the camera bus while a photo is being taken

**Step 4. Slots and folders:** #151 and #269 (label each slot correctly, with its build), #265
(change the image folder when the deployment ID changes), #184 items 2 and 3 (single-camera setups).

**Step 5. Field robustness:** #234 (remount a card put back after a swap), #266 part 2 (spot a stuck
motion sensor, and try your re-program-before-sleep idea), and the bench check for #233.

**Step 6. Wake-up speed and the clock (#251 and friends).** The biggest step, so it starts with a
design review of the existing proposal before any code. It brings in #56, #152 and #158, and
probably #217 (CONFIG.TXT is the state file) and #245 (the clock trim). It may need changes on the
BLE processor side.

**Step 7. RP3 register tidy:** #157, #160, #162, #163, #168. Low risk, and mostly in one area.

**Step 8. Small features, once the decisions are made:** #214, #213, #278, #279, #195, #246, then #185
(move the EXIF code) followed by #187 and #242, which go in the new EXIF file. Most of these need
matching app or website changes, which should be filed as issues at the same time.

**Step 9. Documents and housekeeping, done alongside the other steps when there is time:** #253,
#171, #172 with #173, #180, #248, #170.

**Not scheduled:** #57 (hardware), #247 (power, parked), #174 (Tommy), #262 and #263 (LoRaWAN
decisions), #280 (high resolution, to plan after step 6).

## Questions for you

1. Is the order right? In particular, do you want step 6 (#251) earlier? It is the biggest gain for
   users, but also the largest change.
2. Should `261027_XIP_save_state` hold all the steps, or should each step get its own dated branch from
   it?
