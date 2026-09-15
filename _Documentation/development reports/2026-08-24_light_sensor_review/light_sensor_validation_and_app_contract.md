# Light sensor: validating the algorithm, and wiring it to the app

#### File: light_sensor_validation_and_app_contract.md
#### Author: Claude (Opus 5), reviewed by Victor Anton
#### 1 September 2026, folded into this thread on 3 September

Two strands from Charles's light-sensor work on `ae_review`: whether the dark/bright decision
uses the right signal, tested against his June time-lapse; and what the mobile app needs in
order to use `AI light`, a cross-repo contract. Charles's own review is his and lives beside
this file; this covers the validation and the app-facing consequences.

## The registers, scored against the clock

`AE_Light_Sensor_Roadmap.md` §3.4 ranks analog gain at 99.7% against AE Mean at 96.7%, but
§3.2 labelled the frames with a heuristic built from those same registers, so the ranking is
circular. The 303 frames are 15-minute time-lapse with timestamps, so they can be labelled by
the clock instead, which knows nothing about the sensor ([`analyse_ae_light.py`](analyse_ae_light.py),
with the UTC timestamps shifted to New Zealand time: day 08:00 to 16:00, night 18:00 to
06:00, dawn and dusk left out rather than guessed):

| Register | Threshold | Accuracy vs clock |
|---|---|---|
| Analog gain | > 3 | 100.0% |
| Digital gain | > 67 | 100.0% |
| AE Mean | < 56 | 98.8% |
| Integration time | > 188 | 96.5% |

Charles is right: the gain registers are the stronger discriminators, and the roadmap chose
the third-place one for tunability. AE Mean only works because these nights rail the sensor
(integration 376, analog gain 4; three of 150 night frames converged), which collapses the
mean to about 2.

## The threshold matters more than the register

Simulating the mean-based decision that shipped until ee65771f and is now kept behind
`AE_DECISION_GAIN_BASED`, `dark = gainRailed OR (meanAE < op23)`:

| op23 | Day called dark | Night called bright | Overall |
|---|---|---|---|
| 65, the default | 18 of 108 | 3 of 150 | 91.9% |
| 56 | 0 of 108 | 3 of 150 | 98.8% |
| 40 | 0 of 108 | 3 of 150 | 98.8% |

Day AE Mean runs 56 to 89, so 65 sits inside the daytime distribution and one daylight frame
in six is called dark. `gainRailed` alone gets 147 of 150 night frames and no day frame: the
gain registers already are the detector, and at 65 the mean test only adds errors.

`AE_HYSTERESIS` went from 12 to 0 on `ae_review`. On its own that is a regression: 37 decision
flips across the 303 frames at op23 = 65, against 9 at 56 or 40 and about 10 real day/night
transitions. The hysteresis was masking the misplaced threshold, so the two changes have to
land together, and the default is mirrored in the app's `FACTORY_DEFAULTS`. Filed as [#204].

The gain-based rule that replaced it in ee65771f, `dark = not converged OR analog gain > 2`,
scores 99.2% on the same frames: two day frames called dark, no night frame called bright,
and nine flips, the same as the mean rule at its best threshold.

## `AI light` over BLE

- **A silent failure with the camera disabled.** `disable`, then `light`: the app receives
  `Checking light level...` and nothing else, while `Can't capture - camera system not
  enabled` goes to the console only (`image_task.c:658`; a normal capture takes the branch
  that reports it). A stopped deployment leaves the camera disabled, so this is routine.
  Filed as [#202]. Log: [`logs/bench_light_op10.log`](logs/bench_light_op10.log).
- **The documented reply did not exist.** `ble_commands.md` promised `Light level: 71 (DARK)`.
  The firmware answers `Checking light level...` at once and the reading arrives afterwards
  as telemetry, the `HM0360 AE regs:` block and the `AE light check:` line (about two seconds
  later on the 16-frame build of the time; a single throwaway frame since ee65771f).
  Corrected in `ble_commands.md` on this branch, with the reason: a blocking version
  deadlocked over BLE, because the IF task's I2C_RX state does not clear until the CLI
  replies while the telemetry send needs that same link free.

## Two false results, recorded because they are easy to repeat

1. `setop 10 0` does not disable a running camera. op10 is read only when the image task
   starts (`image_task.c:1652`); `enable` and `disable` change both. The first bench run
   never reached the state it was testing.
2. `AE light check` is not a unique marker: it also appears in `[LS] Skipping NN processing
   (AE light check).` during the capture phase, so a test waiting on the substring matches
   the previous command. Match `AE light check: mean AE =` (`AE light check: AGain =` since
   ee65771f) and drain the port between steps.

## Files

| File | What it is |
|---|---|
| [`logs/ae_303_frames_june2026.csv`](logs/ae_303_frames_june2026.csv) | The 303 frames with AE registers and EXIF capture times, extracted with `_Tools/jpegAE-batch.py`. Times are UTC; the camera was on NZ time |
| [`logs/bench_light_op10.log`](logs/bench_light_op10.log) | Serial log of the camera-disabled test, `ae_review` RP3 at 4dc43587 |
| [`analyse_ae_light.py`](analyse_ae_light.py) | Reproduces every table above from the CSV with `python analyse_ae_light.py logs/ae_303_frames_june2026.csv --utc-offset 12 --day 8-16 --night 18-6`: scores each register against the clock label, simulates the mean-based and the gain-based decisions, counts the flips |
| [`mobile_app_three_way_bench/`](mobile_app_three_way_bench/README.md) | The three-way logs (app, nRF, Himax) behind #202, #203 and #204 |

## Open items

| Issue | What | Repo |
|---|---|---|
| [#202] | A dropped `light` request is never reported to the app | this repo |
| [#203] | `AE light check` is truncated to 150 bytes before it reaches the app | this repo |
| [#204] | Decide: move op23 off 65 and drop `AE_HYSTERESIS`, as one change | this repo |
| [ww-mobile-app#250] | Make the photo optional on the Light Sensor screen, using `AI light` | ww-mobile-app |
| [ww-mobile-app#251] | Parse the `AE light check` line to show the decision and its inputs | ww-mobile-app |
| [ww-mobile-app#252] | Light measurement with photo download works on RP3 but not HM0360 | ww-mobile-app |

Still needing Charles rather than a unilateral change: the op23 value itself (30 to 55 is
safe on this dataset, but it is one camera, one indoor scene, five days); whether the mean
comparison earns its place once op23 is right; and whether the flash and the camera switch
should share one decision. Three night frames are a room light being switched on: gain says
dark, AE Mean says bright, and both are right for their own consumer. The flash wants scene
brightness, op26 wants true ambient darkness, and a single boolean may be the wrong shape.

Older light-sensor issues that `ae_review` substantially closes, worth checking before anyone
picks them up: [#182], [#181], [#154].

[#202]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/202
[#203]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/203
[#204]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/204
[#154]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/154
[#181]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/181
[#182]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/182
[ww-mobile-app#250]: https://github.com/wildlifeai/ww-mobile-app/issues/250
[ww-mobile-app#251]: https://github.com/wildlifeai/ww-mobile-app/issues/251
[ww-mobile-app#252]: https://github.com/wildlifeai/ww-mobile-app/issues/252
