# NN threshold: what op 16 should be compared with

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-11-05_NN_threshold\README.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 5 November 2026

## Summary

Branch `261105_NN_threshold`.

**What this thread is for.** Issue
[#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) (labels Firmware, Data
Science, decision): op 16 tests class 1 alone, while the challenges and the website rank models on class 1
minus class 0.

**The problem.** The firmware reports a detection (`NN+`) when class 1's int8 output is above op 16
(`OP_PARAMETER_MODEL_THRESHOLD`, default 18, range 0 to 127). This is `processNNOutput()` in
`image_task.c`, which carries a TODO saying it only suits person detection. The rat and cat challenges
rank models on class 1 minus class 0 (`eval/score.py`), and the rat challenge's round 2 recommends setting
op 16 from that score. The two only agree when a model's outputs are mirror images (class 0 = minus
class 1). The 96 px example models are. The challenges' current models are not.

**Evidence in the issue** (PC run with the camera's preprocessing, 10 Oct 2026, the examples' ten frames
from `EXPECTED.CSV` in the team drive's `Data/ww500-models`; the camera matched the PC on 196 of 200 cat
frames and 40 of 40 rat frames):

- `93V1` (rat, 160 px): class 1 is between -2 and -55 on all five rats, so no op 16 value flags any of
  them. Yet its score puts four of the five rats above every frame without a rat.
- `71V1` (cat, 160 px): op 16 = 18 flags only one of the five cats.

So with the challenge models deployed today, the camera misses most of the animals the models do find.

**The options in the issue.**

1. **Firmware change.** Test class 1 minus class 0 against op 16. Op 16 then needs a signed range, which
   changes its contract with the app and the website (op parameters are a cross-repo contract).
2. **No firmware change.** The website and the challenges export models with a fixed last layer that
   makes class 0 = minus class 1; op 16 = score / 2.

No decision has been recorded on the issue yet (no comments as of 5 Nov).

## Proposed approach (Charles, 5 Nov)

A third option that keeps op 16 unsigned (0 to 127), because making op values signed would have spinoffs
elsewhere:

- The decision is based on the score, class 1 minus class 0 (an `int16_t`, -255 to +255).
- A negative score means the model has not found the target.
- A positive score is compared with op 16: detection if the score is above it.

As op 16 is never negative, this is one line in `processNNOutput()`: `score > op16`. Its range and type
do not change, but its **meaning** does (a threshold on the score, not on class 1), so it is still a
change to the op 16 contract, but a smaller one than option 1.

What to be aware of:

1. **Existing values change meaning.** On mirrored models (the 96 px examples) the score is twice
   class 1, so today's `class 1 > 18` is `score > 36`. **Decided (Charles, 5 Nov): the default stays
   18.** It was chosen arbitrarily, and op 16 will have to be tuned for each model anyway.
2. **The threshold cannot go below 0.** Op 16 = 0 is "class 1 beats class 0", the model's own call. The
   camera cannot be made more sensitive than that.
3. **Open question:** does a good cut-off for `93V1` and `71V1` lie at a score of 0 or above? If the
   challenges' best cut-off is negative, this scheme misses those animals and only option 1 would catch
   them. The challenges' `eval/score.py` results answer it.
4. Scores above 127 cannot be used as thresholds. Unlikely to matter.

**Testing.** Victor to test the change against his models (the challenge models and the 96 px
examples), using `nnfiles` on the examples' frames and comparing with `EXPECTED.CSV`, before the change
is merged.

## What has to change

The code change is small; most of the work is in documents and other repos.

| Where | What changes |
|---|---|
| [`ww500_md/image_task.c`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c), `processNNOutput()` | Compute the score as `int16_t`, test `score > op16`; print the score in the `Score ... (Threshold ...)` line; remove the "person detection only" TODOs |
| [`ww500_md/cvapp.h`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/cvapp.h) | `MODEL_THRESHOLD` comment ("Logit value") and the `USE_PERCENTAGE` comment ("stays a logit comparison") (default stays 18) |
| [`ww500_md/fatfs_task.h`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/fatfs_task.h) | Op 16 comment: "Score threshold (class 1 minus class 0) for detection (0-127)" |
| [`ww500_md/MANIFEST/config_file.md`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/MANIFEST/config_file.md) | Op 16 row (meaning, default) |
| [`_Documentation/WW500_Neural_Network_Operation.md`](../../WW500_Neural_Network_Operation.md) | Op 16 table row (it says default 0, which is already wrong), "Detection threshold" section, worked examples table, `NN+` row, `processNNOutput()` row |
| [`_Documentation/nn_files_bench.md`](../../nn_files_bench.md) | Paragraph saying the camera's detection is not the challenges' score |
| [`.agents/skills/references/bench.md`](../../../.agents/skills/references/bench.md) | Same point in the `nnfiles` bullet ("class 1 above op 16 is not the challenges' score") |
| [ww-mobile-app](https://github.com/wildlifeai/ww-mobile-app) (Victor) | Op 16 label, help text and default, if the app shows or sets it |
| Website / backend (Victor) | Any op 16 recommendation or default given with a model |
| [`ww-rat-challenge`](https://github.com/wildlifeai/ww-rat-challenge), [`ww-cat-challenge`](https://github.com/wildlifeai/ww-cat-challenge) (private) | Round 2 advice: op 16 = score (no longer derived via class 1) |
| Issue [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) | Record the decision |

Not affected: op 1 and 2 (the NN counters still count `nnResultPositive`), the `NN+`/`NN-` messages to
the BLE processor, the EXIF confidence data, and `RESULTS.CSV` (it records each class's output, not the
decision).

## Bench test 1 (5 Nov)

Log [logs/ai1.txt](logs/ai1.txt): AI processor console, HM0360 image, model `20V1` (96 px person), op 16
= 18, one cold boot then ten motion wakes.

| Wake | Class 0 | Class 1 | Score | Result | Percent (person) |
|---|---|---|---|---|---|
| 1 | -94 | 94 | 188 | `NN+` | 68% |
| 2 | -62 | 62 | 124 | `NN+` | 62% |
| 3 | -57 | 57 | 114 | `NN+` | 61% |
| 4 | -45 | 45 | 90 | `NN+` | 59% |
| 5 | 45 | -45 | -90 | `NN-` | 41% |
| 6 | -80 | 80 | 160 | `NN+` | 65% |
| 7 | 103 | -103 | -206 | `NN-` | 31% |
| 8 | 14 | -14 | -28 | `NN-` | 47% |
| 9 | -62 | 62 | 124 | `NN+` | 62% |
| 10 | 87 | -87 | -174 | `NN-` | 34% |

- **The decision works:** 6 `NN+` and 4 `NN-`, each matching score > op 16, and the `Score` line shows
  the score and threshold.
- **Old and new rules agree on this model**, as expected: its outputs are exact mirrors, so the score is
  twice class 1. No frame had class 1 between 9 and 18, the only range where the two would differ (the
  new rule is the more sensitive there).
- **Percentages** are whole numbers and each pair sums to 100%.
- **Colour:** not checkable from the log (Tera Term drops the escape codes). The log does show other
  tasks printing between `TARGET OBJECT DETECTED!` and the `Score` line, which is why the colour leaked.
- **Double softmax:** still open. If `20V1` ends in softmax, wake 1's "person" is (94 + 128) / 256 = 87%,
  not the 68% printed.

## Bench test 2 (5 Nov)

Log [logs/ai2.txt](logs/ai2.txt): AI processor console, RP3 image (IMX708), model `20V1`, op 16 = 18, one
cold boot then eleven motion wakes.

| Wake | Class 0 | Class 1 | Score | Result | Percent (person) |
|---|---|---|---|---|---|
| 1 | -111 | 111 | 222 | `NN+` | 70% |
| 2 | -99 | 99 | 198 | `NN+` | 68% |
| 3 | -90 | 90 | 180 | `NN+` | 67% |
| 4 | -87 | 87 | 174 | `NN+` | 66% |
| 5 | 104 | -104 | -208 | `NN-` | 31% |
| 6 | 109 | -109 | -218 | `NN-` | 30% |
| 7 | 105 | -105 | -210 | `NN-` | 31% |
| 8 | -85 | 85 | 170 | `NN+` | 66% |
| 9 | -103 | 103 | 206 | `NN+` | 69% |
| 10 | 81 | -81 | -162 | `NN-` | 35% |
| 11 | 91 | -91 | -182 | `NN-` | 33% |

Same findings as test 1: 6 `NN+` and 5 `NN-` (each sent to the BLE processor), all matching score > op 16;
outputs mirrored, so the old rule would have agreed; percentages whole and summing to 100%. The RP3 frames
give stronger scores than the HM0360's (no frame within 160 of zero).

Both camera variants now pass with the person model. Still needed: Victor's test with the challenge models (`93V1`, `71V1`, `72V1`), where the old and new rules
do differ.

## Status

Open, 5 November 2026. The changes in this repo (the first seven rows of the table) are made, built and
benched on both images with the person model (bench tests 1 and 2). Then: Victor tests against his models; the app and challenge repos; record the
decision on #270.

Notes on the code change:

- Fewer than 2 classes is never a detection (before, class 1 was read regardless).
- The comparison is done in `int32_t`, because op 16 is not range-checked by `setop` (any 0 to 65535
  is accepted) and a large value must not wrap negative.
- The console line after each inference is now `Score <n> (class 1 - class 0, threshold <op 16>)`.

Found on the bench (5 Nov), also fixed:

- **Percentages are whole numbers** (`outputAsPercentage()` in `cvapp.cpp`). The decimal was computed
  after rounding the whole number, so 56.6% printed as "57.0" and pairs summed to 100.4%. A decimal
  place also implied precision the model does not have. The EXIF UserComment already used whole numbers.
- **Console colour.** After `TARGET OBJECT DETECTED!` the console stayed green through
  `sendMsgToMaster()`, so other tasks' lines came out green. It now resets to white first, as the
  not-detected branch already did; each percentage line also resets its own colour.

Not fixed, to check: the percentages may be softmax applied twice. The 96 px model's outputs are exact
mirrors (84 / -84), as an int8 softmax layer gives (scale 1/256, zero point -128). If so, "person" for
raw -84 is (-84 + 128) / 256 = 17%, not the 34% printed. Printing the output tensor's scale and zero
point at model load would settle it.

## Outcome

To be written.

## Open items

- [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270): agree the proposed
  approach (or option 1 or 2); settle point 3 with the challenges' score results; Victor to test against
  his models.
