# Unresponsive BLE processor

#### File: README.md
#### Author: Claude (Opus 5.5), reviewed by Charles Palmer
#### 28 September 2026

## Status

**Open.** Started 17 September 2026 from a field report (Victor, 16 September: a camera showing "AI NACK" whose AI
processor never slept). On 28 September 2026 the AI processor gained detection and mitigation of an unresponsive BLE
processor (branch `minimalFreeRTOS`, built and tested on a board with no BLE processor). The cause of the field fault is not
settled, and no GitHub issues have been filed.

## Outcome

- **Unresponsive BLE Processor Detection and Mitigation (28 September 2026):** at every boot the AI processor's first
  message to the BLE processor is a probe with a 300 ms timeout. If it is not read, nothing more is sent to the BLE
  processor, the device still enters DPD, and the condition is shown by the `ble` command and self test bit 14. This also
  fixes `ww500_md` never entering DPD when its "Sleep" message was not read, which matches the field symptom. Details:
  [CLAUDE_Unresponsive_BLE_Processor_Detection_and_Mitigation.md](CLAUDE_Unresponsive_BLE_Processor_Detection_and_Mitigation.md).
- **Analysis of the field report** (Charles and Claude, 17-19 September): in
  [CLAUDE_BLE_processor_unresponsive.md](CLAUDE_BLE_processor_unresponsive.md). It includes a check that the `dev` branch
  lacks the BLE-side GPIOTE interrupt fix (`c3f14b9`, `ww-hardware`), the reasons for keeping `MISSINGMASTERTIME` at 4000 ms,
  and the single-slot `savedMessage` problem in `if_task.c` (26 messages deferred and none issued in the field log). Charles
  has noted that the draft GitHub issue there probably diagnoses the wrong problem.

## Open items

No GitHub issues filed yet (ask Charles before filing).

- Find the cause of the field fault. Charles's captures of 19 September (`ai_log_1.txt`, `ble_log_1.txt`,
  `teraterm_ble*.txt`, `teraterm_himax*.txt`) and his commit `26214bb` ("Added debug messages to diagnose unresponsive BLE
  processor", `ww-hardware`, `charles_fileTxFix`) have not been reviewed by Claude.
- Port the GPIOTE fix (`c3f14b9`) to the BLE processor's `dev` branch, if it is still wanted.
- The single-slot `savedMessage` in `if_task.c` (a small queue, or at least a warning when a message is overwritten).
- Test the detection and mitigation on a board with a BLE processor (only the 300 ms limit on the first message should
  differ), including a file transfer.
- Self test bit 14 (`SELF_TEST_AI_NO_BLE`) in the BLE processor's `selfTest.h` and in the app.
- Whether MD sensitivity 0 (op 17) should also stop the HM0360's motion detection frames, which still cost power.

## Files

- [CLAUDE_BLE_processor_unresponsive.md](CLAUDE_BLE_processor_unresponsive.md): the task, the field report and its analysis
- [CLAUDE_Unresponsive_BLE_Processor_Detection_and_Mitigation.md](CLAUDE_Unresponsive_BLE_Processor_Detection_and_Mitigation.md):
  the 28 September change
- `teraterm160926.txt`: Victor's AI processor console log from the field camera
- `ai_log_1.txt`, `ble_log_1.txt`, `teraterm_ble.txt`, `teraterm_ble_2.txt`, `teraterm_himax.txt`, `teraterm_himax_2.txt`:
  Charles's captures of 19 September
