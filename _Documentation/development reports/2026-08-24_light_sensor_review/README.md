# Review of some aspects of Light Sensor Operation

#### File: `README.md`
#### Author: Charles Palmer
#### Date: 26 August 2026. Status, outcome and open items written 3 September 2026 by Victor Anton with Claude (Fable 5.1)

File opened in accordance with instructions in the higher-level [README.md](../README.md)

I (Charles) am instructing Claude in [CLAUDE_light_sensor_review.md](CLAUDE_light_sensor_review.md)

## Status

Open. Every task in Charles's instruction file is complete on `ae_review` (e8b7feb5) except one:
the flash modes. The app work that depended on this thread has shipped. What remains is that
one firmware feature, three decisions for Charles, and the defects the bench work turned up.

## Outcome

Done on `ae_review`:

- **The light/dark decision is gain-based** (`AE_DECISION_GAIN_BASED` in `lightSensor.c`):
  dark when the AE loop has not converged or analog gain is above the threshold. The
  mean-based rule is kept behind the define.
- **`AI light`**, an on-demand check over the console and BLE. Two-phase by design: the reply
  `Checking light level...` comes at once and the decision line follows as telemetry, because
  a blocking version deadlocked over BLE. It runs through the image task on the real capture
  path ([light_command_via_image_task_proposal.md](light_command_via_image_task_proposal.md)).
- `lightSensor.c` and `.h` split out; `hm0360_md_getAEStats()` once per loop; the light
  sensor prints tidied; `_Tools/ae_stream.py` and `ae_monitor.py` to run `light` continuously.
- **The STROBE pin no longer stays armed through a light check** (e8b7feb5, "LED was flashing
  during light sensing"), the flicker Charles reported.
- The FAT task stack overflow fix ported from Victor's branch
  ([fat_task_stack_overflow_missing_fix.md](fat_task_stack_overflow_missing_fix.md)); built,
  not yet verified on a device.
- **Validation against the 303-frame June time-lapse**
  ([light_sensor_validation_and_app_contract.md](light_sensor_validation_and_app_contract.md)):
  the gain registers are the detector; op23 at 65 sits inside the daytime distribution; the
  hysteresis removal and the threshold move must land together.
- `ble_commands.md` now documents the `AI light` contract and how to parse the decision line.
- The mobile app: measure without a photo (wildlifeai/ww-mobile-app#254; issues 250, 251,
  252 closed), the readings screen scoring both rules (#263), and Capture Picture holding the
  device awake with an interim forced flash (#264).

## Open items

### The flash-mode op parameter, for Charles

The one review task still to do (`CLAUDE_light_sensor_review.md`, "Add other options for
enabling the flash LED"). A new op parameter selecting one of four modes:

| Mode | Meaning |
|---|---|
| Always off | The LED never fires |
| Always on | Every capture fires it |
| Light sensor | Today's behaviour: fires when the last light check said dark (op25) |
| Time of day | Fires between a start time and a duration, as the old `FLASH_LED_START_TIME` and `FLASH_LED_DURATION` did |

The design is in [flash_led_modes_proposal.md](flash_led_modes_proposal.md) (section 5), with
two gaps Charles spotted on 3 September (sections 6 and 7: the AE state goes stale under the
other modes, and time of day needs periodic re-checking) and his open questions (section 8).
Three constraints from the app side:

- **The index has to be agreed first.** The proposal starts at 32; the app already binds 32
  (`CAM_RESOLUTION`) and 33 (`MD_BLOCK_NUM_MAX`) from other branches and reads the length
  of `getop -1` to decide what the firmware supports. Start at 34, or renumber the app once.
- op13 keeps meaning which LED, and op13 = 0 stays "off" under every mode.
- Until the parameter exists the app forces a flash on Capture Picture by writing op25 = 1
  before the capture (`TODO(flash-mode-op)` in `useCapturePicture.ts`). That write goes the
  day the mode lands, and the app then needs the mode in its device settings and deployment
  defaults.

### Decisions, needing Charles

- [#204]: move op23 off 65 and drop `AE_HYSTERESIS`, as one change. Anything from about 30 to
  55 is safe on the one dataset we have.
- Whether a passive `light` should write its verdict to op25 at all: today a diagnostic
  reading decides the next capture's flash.
- Whether the flash and the camera switch should share one decision. Three night frames in
  the dataset are a room light: gain says dark, AE Mean says bright, and both are right for
  their own consumer.

### Defects

| Issue | Repo | Status |
|---|---|---|
| [#202] A dropped `light` request is never reported to the app | Seeed | open |
| [#203] `AE light check` truncated at 150 bytes | Seeed | open. The e8b7feb5 wording fits under the clip, so it no longer shows; the clip and the pointer lifetime it describes remain |
| [#205] An inactivity event during an I2C reply leaves the device awake until power-cycled | Seeed | open, high; found on the capture bench, reproduced on demand |
| The nRF parses the sleep stats and forwards only `Sleep` | ww-hardware | not filed |
| Capture-bench findings B, C, F, H, I, J (config save lost, sleep mid-retry, op7, commands mid-stream, transfer rate, `Finished sending`) | Seeed and ww-hardware | drafted on branch `docs/capture-bench-findings`, filed one at a time |

### Not yet in this folder

`light_sensor_ground_truth.md`, the bench-verified state of the light sensor written for
Charles on 2 September and brought up to e8b7feb5 on 3 September, is held back until Victor
has reviewed it.

## Files

| File | What it is |
|---|---|
| [CLAUDE_light_sensor_review.md](CLAUDE_light_sensor_review.md) | Charles's instructions and the log of what was done against them |
| [flash_led_modes_proposal.md](flash_led_modes_proposal.md) | The flash-mode parameter: history, complications, proposed design, open questions |
| [light_command_via_image_task_proposal.md](light_command_via_image_task_proposal.md) | Why `AI light` goes through the image task, and the accepted side effects |
| [fat_task_stack_overflow_missing_fix.md](fat_task_stack_overflow_missing_fix.md) | The stack overflow in `save_configuration()` and the port of the fix |
| [lightSensor.h](lightSensor.h) | The header as reviewed |
| [light_sensor_validation_and_app_contract.md](light_sensor_validation_and_app_contract.md) | The 303-frame validation, the threshold and hysteresis finding, the `AI light` contract |
| [analyse_ae_light.py](analyse_ae_light.py) | The script behind that validation's tables; one command reproduces them from the CSV |
| [logs/](logs/) | The 303-frame CSV and the camera-disabled serial log behind that validation |
| [mobile_app_three_way_bench/](mobile_app_three_way_bench/README.md) | The three-way logs (app, nRF, Himax) behind #202, #203 and #204, and the logger |

[#202]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/202
[#203]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/203
[#204]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/204
[#205]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/205
