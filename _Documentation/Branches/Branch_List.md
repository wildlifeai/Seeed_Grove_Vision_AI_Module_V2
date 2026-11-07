# Git Branch List
#### File: Branch_List.md
#### Author: Charles
#### Date: 30 September 2026, updated 7 November 2026

The idea of this file is to try to document what is in git branches. It is apparent that
I forget why I set up some branches.

Also, as an experiment, I will start naming branches with the date I create them (yymmdd)

(I am doing this with the BLE processor repo also.)

| Date created | Name                | Purpose | 
|--------------|---------------------|---------|
| 30 Sept 2026 | 260930_appCommsBugs | To test recent changes (unresponsive BLE processor, large file tx from app, and some of the github issues |
| 30 Sept 2026 | 260930_branchListSkill | Add this naming convention to the agent skill and the workflow doc; point the ship-check doc and tool at #252 |
| 2 Oct 2026   | 261002_useRP3Fixes  | Correcting  places where HM0360 code is missing |
| 2 Oct 2026   | 261002_todoRebench  | Bench evidence: the Todo issues re-run on dev before Charles picks them up |
| 2 Oct 2026   | 261002_pb7SwdHm0360 | On top of 260930_appCommsBugs (#257): keep PB7 as SWCLK in the HM0360 build (#200), PR #258, closed in favour of #260 |
| 3 Oct 2026   | 261003_victorsPriorities  | Address some outstanding issues selected by Victor |
| 5 Oct 2026   | 261005_nnfiles      | 'nnfiles' console command: run the loaded model on frames from the SD card and write RESULTS.CSV, to score candidate models (rat challenge) |
| 6 Oct 2026   | 261006_speedImageTx  | Some measures to increase the speed of the transfer of image files to the app. |
| 9 Oct 2026   | 261009_tidyAfter260 | Tidy-ups promised in the #260 review |
| 9 Oct 2026   | 261009_powerMeasurements | LED flash power measurements |
| 11 Oct 2026  | 261011_shipCheckPhotoButtons | Ship check tool: buttons for a black and white (HM0360) or colour (RP3) photo without flashing |
| 11 Oct 2026  | 261011_cameraConnectorPressure | Bench findings: pressure on the HM0360 connector stops the boot or kills a running capture |
| 12 Oct 2026  | 261012_cameraErrors | Better handling of unexpected camera error messages |
| 23 Oct 2026  | 1261023_mdEnhancements  | checked live preview, enahnced motion detection handling   | 
| 27 Oct 2026  | 261027_XIP_save_state  | Save operational parameters in XI memory to speed time to first image   |
| 1 Nov 2026   | 261101_AI_RTC       | Photo times from the BLE processor's clock (setutc every wake), wake message first (PR #284) |
| 4 Nov 2026   | 261104_selfTest     | Self-test bit 14 kept until a selftest reply has carried it (#246) |
| 5 Nov 2026   | 261105_NN_threshold | Op 16 as a % probability, double softmax fixed (#270, #287, PR #289) |
| 6 Nov 2026   | 261106_lorawanPing  | LoRaWAN health message: `sdused` command, selftest bit 15 provisional (#290) |
