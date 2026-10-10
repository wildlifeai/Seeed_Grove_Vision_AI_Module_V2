# Some measures to increase the speed of the transfer of files to the app.

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-10-06_speedImageTx\README.md`
#### Author: Charles Palmer
#### Date: 6 October 2026

## Status

Closed for the AI processor, 7 October 2026. The remaining work is in the BLE processor code,
and maybe the app.

## Outcome

- The download (`AI txfile`) is now instrumented. The AI processor console prints a
  `Download timing` block at the end, splitting each packet into SD card read, AI processor time
  and BLE processor time.
- The AI processor's per-packet console output is suppressed during a download, cutting its
  time from 18.5 ms to 0.5 ms per packet. The SD card read is 0.4 ms, so reading larger chunks
  would not help.
- The download still runs at about 1.5 KB/s. The link moves one packet per BLE connection
  interval, about 155 ms, because only uploads hold the fast 15 to 30 ms interval. Speeding it
  up needs BLE processor (and maybe app) changes, described in
  [Fast_SD_to_app_transfer_BLE_processor_work.md](Fast_SD_to_app_transfer_BLE_processor_work.md).

PR: [#264](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/264), branch
`261006_speedImageTx` into `dev`.

Documents:
- [CLAUDE_download_speed_proposal.md](CLAUDE_download_speed_proposal.md): the proposal, with
  status and measured results.
- [Fast_SD_to_app_transfer_BLE_processor_work.md](Fast_SD_to_app_transfer_BLE_processor_work.md):
  a standalone brief for the BLE processor developers.
- [BLE_speed_improvements.md](BLE_speed_improvements.md): the BLE processor history, written by
  Claude in the ww-hardware repo.

## Open items

- Fast connection interval for downloads, in the BLE processor (and maybe the app): GitHub
  issue in ww-hardware to be filed, linking the brief above.

## Purpose

To plan how the time taken to transfer files from the SD card to the app can be reduced.

To document this, understanding that an optimal solution is likely to involve changes to code in:
* the app
* the BLE processor
* the AI processor

In the immediate term, to propose (then implement) changes in the AI processor code only.

## Background 

We worked on speeding the transfer of files from the app to the SD card. 
This included reducing console output (both processors), and other measures.

We also tried to improve the transfer speed of images from the SD card to the app by making changes in the 
BLE processor code. Until now we have not tried to optimise speed in the AI processor code.

Today I noticed that that the AI processor was still printing the binary data of the JPEG file to the console and I wondered
if there were speed improvements that could be made here also.

I asked Claude in the BLE repo to summarise what had been done and make some suggestions of the AI processor.
The other Claude created [BLE_speed_improvements.md](BLE_speed_improvements.md) 
in this folder. These are its suggestions - this Claude should consider these but make its own recommendations. 

I think the improvements in the speed of transfers from the app to the SD card involved changes to code in:
* the app
* the BLE processor
* the AI processor

In contrast, so far improvements in the speed of transfers from the SD card to the app have only been in the BLE processor code.

Improvements in the speed of transfers from the SD card to the app could include:
1. Changes in the AI codebase alone.
2. A more holistic solution involving changes to the app and BLE processor 
as well, perhaps mirroring the transfers in the opposite direction.

For the AI code, reductions in console output might be useful but are perhaps less important than the BLE processor
as it runs at a much higher baud rate. 

I suspect that SD card read operations might be adding to the delay, and that reading larger chunks of the file
at one time might reduce the time. 

Perhaps the AI code could be instrumented to see where the delays live. 

---

## Task for Claude

1.	Read this document and `BLE_speed_improvements.md`
2.	Look at code where relevant.
3. 	Consider where delays exist in the transfer of files _from_ the app and where delays might exist 
in transfer of files _to_ the app.
4.	Ask questions where helpful.
5. Produce a proposal markdown document (in this folder) for improvements in the speed of transfers from the SD card to the app
that could be made in two parts: (a) what could be done in the AI codebase alone; (b) a more holistic solution involving changes to the 
app and BLE procsssor as well.
6.	I will ask for code changes only _after_ reviewing the proposal document. 



