# NN threshold: what op 16 should be compared with

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-11-05_NN_threshold\README.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 5 November 2026, closed 6 November 2026

## Summary

Branch `261105_NN_threshold`, [PR #289](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/289).

**What this thread is for.** Issue
[#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) (labels Firmware, Data
Science, decision): OP_PARAMETER_MODEL_THRESHOLD (op 16) tests neural network output vector class 1 value alone,
while the challenges (reported by #270) and the website rank models on NN class 1 value minus class 0 value.

**Result.** Op 16 is now a percentage, 0 to 100, default 60: the camera detects when the model is at
least op 16 % sure of the target (class 1). It means the same for every model, softmax or logits. The
score rule of 5 Nov (class 1 minus class 0 above op 16) was a step on the way. Along the way #287 (the
percentages were softmax applied twice) was found and fixed. Victor's test with the challenge models is
still to do.

**The problem.** The firmware reported a detection (`NN+`) when class 1's int8 output was above op 16
(`OP_PARAMETER_MODEL_THRESHOLD`, default 18, range 0 to 127). This is `processNNOutput()` in
`image_task.c`. The rat and cat challenges
rank models on class 1 minus class 0 (`eval/score.py`), and the rat challenge's round 2 recommends setting
op 16 from that score. The two only agree when a model's outputs are mirror images (class 0 = minus
class 1). The `nnfiles` example models (`99V2` rat and `98V1` cat, 96 x 96, in `_Tools/nnfiles_example*`)
are, and so is `20V1`, the person detection model now in the WW500. The challenges' current models are not.

**Evidence in the issue** (PC run with the camera's preprocessing, 10 Oct 2026, the examples' ten frames
from `EXPECTED.CSV` in the team drive's `Data/ww500-models`; the camera matched the PC on 196 of 200 cat
frames and 40 of 40 rat frames):

- `93V1` (rat, 160 px): class 1 is between -2 and -55 on all five rats, so no op 16 value flags any of
  them. Yet its score puts four of the five rats above every frame without a rat.
- `71V1` (cat, 160 px): op 16 = 18 flags only one of the five cats.

So with the challenge models, the camera missed most of the animals the models do find.

**The options in the issue.**

1. **Firmware change.** Test class 1 minus class 0 against op 16. Op 16 then needs a signed range, which
   changes its contract with the app and the website (op parameters are a cross-repo contract).
2. **No firmware change.** The website and the challenges export models with a fixed last layer that
   makes class 0 = minus class 1; op 16 = score / 2.

**Decision (Charles): neither.** First (5 Nov) the score rule below; then (6 Nov), once #287 showed
what the outputs are, op 16 as a percentage (see [The approach adopted](#the-approach-adopted-charles-6-nov-op-16-as-a-percentage)).
Not yet recorded on #270 (no comments as of 6 Nov).

## The first approach (Charles, 5 Nov): the score rule

Superseded on 6 Nov by [op 16 as a percentage](#the-approach-adopted-charles-6-nov-op-16-as-a-percentage);
kept as the record. Bench tests 1 to 3 ran with it.

A third option that keeps op 16 unsigned (0 to 127), because making op values signed would have spinoffs
elsewhere:

- The decision is based on the score, class 1 minus class 0 (an `int16_t`, -255 to +255).
- A negative score means the model has not found the target.
- A positive score is compared with op 16: detection if the score is above it.

As op 16 is never negative, this is one line in `processNNOutput()`: `score > op16`. Its range and type
do not change, but its **meaning** does (a threshold on the score, not on class 1), so it is still a
change to the op 16 contract, but a smaller one than option 1.

What to be aware of:

1. **Existing values change meaning.** On mirrored models (`20V1` and the example models) the score is twice
   class 1, so the old `class 1 > 18` is `score > 36`. **Decided (Charles, 5 Nov): the default stays
   18.** It was chosen arbitrarily, and op 16 will have to be tuned for each model anyway.
2. **The threshold cannot go below 0.** Op 16 = 0 is "class 1 beats class 0", the model's own call. The
   camera cannot be made more sensitive than that.
3. **Open question:** does a good cut-off for `93V1` and `71V1` lie at a score of 0 or above? If the
   challenges' best cut-off is negative, this scheme misses those animals and only option 1 would catch
   them. The challenges' `eval/score.py` results answer it.
4. Scores above 127 cannot be used as thresholds. Unlikely to matter.

**Testing.** Victor to test the change against his models (the challenge models and the example
models `99V2` and `98V1`), using `nnfiles` on the examples' frames and comparing with `EXPECTED.CSV`, before the change
is merged.

## What changed

The table is the state now (6 Nov), with the percentage rule.

| Where | What changes | Done |
|---|---|---|
| [`ww500_md/image_task.c`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/image_task.c), `processNNOutput()` | Detection if class 1's percentage (`cv_getTargetPercent()`) is at least op 16; `Target '<label>' <n>% (threshold <op 16>%)` line; "person detection only" TODOs removed; colour reset before `sendMsgToMaster()` | Yes |
| [`ww500_md/cvapp.cpp`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/cvapp.cpp) | `computeProbabilities()` on every run, whatever `USE_PERCENTAGE` says (dequantize for softmax models, softmax for logit models, #287); `cv_getTargetPercent()`; `outputAsPercentage()` only prints and keeps the EXIF data, whole numbers, each line resets its colour; fingerprint prints quantization and `Output values` (#287) | Yes |
| [`ww500_md/cvapp.h`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/cvapp.h) | `MODEL_THRESHOLD` 60, `MODEL_THRESHOLD_MAX` 100, `cv_getTargetPercent()`, `USE_PERCENTAGE` comment | Yes |
| [`ww500_md/fatfs_task.h`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/fatfs_task.h), [`fatfs_task.c`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/fatfs_task.c) | Op 16 comment; `setop` rejects op 16 above 100; CONFIG.TXT load replaces it by 60 | Yes |
| [`ww500_md/MANIFEST/CONFIG.TXT`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/MANIFEST/CONFIG.TXT) | Op 16 = 60 | Yes |
| [`ww500_md/MANIFEST/config_file.md`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/MANIFEST/config_file.md) | Op 16 row | Yes |
| [`_Documentation/WW500_Neural_Network_Operation.md`](../../WW500_Neural_Network_Operation.md) | Op 16 row, "Detection threshold" section, example output, "Understanding the output values", examples, fingerprint, UserComment, `NN+` row, `processNNOutput()` and `cv_run()` rows | Yes |
| [`_Documentation/nn_files_bench.md`](../../nn_files_bench.md) | Choose op 16 from the class 1 `pct` column of `RESULTS.CSV` | Yes |
| [`.agents/skills/references/bench.md`](../../../.agents/skills/references/bench.md) | Same point in the `nnfiles` bullet; `tflite_inspect.py` | Yes |
| [ww-mobile-app](https://github.com/wildlifeai/ww-mobile-app) (Victor) | Op 16 label, help text, range 0 to 100 and default 60, if the app shows or sets it | No, #270 |
| Website / backend (Victor) | Any op 16 recommendation or default given with a model; the EXIF percentages change for softmax models (#287) | No, #270 |
| [`ww-rat-challenge`](https://github.com/wildlifeai/ww-rat-challenge), [`ww-cat-challenge`](https://github.com/wildlifeai/ww-cat-challenge) (private) | Round 2 advice: op 16 = the probability cut-off in % | No, #270 |
| Issue [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270) | Record the decision | No |

Not affected: op 1 and 2 (the NN counters still count `nnResultPositive`), the `NN+`/`NN-` messages to
the BLE processor, and the format of the EXIF data and `RESULTS.CSV`.

Notes on the code:

- Fewer than 2 classes is never a detection (before, class 1 was read regardless).
- The score rule (5 Nov) compared in `int32_t` because op 16 was not range-checked. Now `setop`
  rejects op 16 above 100.

Found on the bench (5 Nov), also fixed:

- **Percentages are whole numbers.** The decimal was computed after rounding the whole number, so 56.6%
  printed as "57.0" and pairs summed to 100.4%. A decimal place also implied precision the model does
  not have. The EXIF UserComment already used whole numbers.
- **Console colour.** After `TARGET OBJECT DETECTED!` the console stayed green through
  `sendMsgToMaster()`, so other tasks' lines came out green. It now resets to white first, as the
  not-detected branch already did.

Fixed 6 Nov, issue [#287](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/287): the percentages are softmax applied twice for `20V1` and the example
models, which already end in softmax. Raw 94 is 87% "person", not the 68% printed. Where the model came
from, how the model bytes show it, and the changes needed:
[CLAUDE_person_model_softmax.md](CLAUDE_person_model_softmax.md). New tool:
[`_Tools/tflite_inspect.py`](../../../_Tools/tflite_inspect.py).

## Bench test 1 (5 Nov)

Log [logs/ai1.txt](logs/ai1.txt): AI processor console, HM0360 image, model `20V1` (person detection, 96 x 96, the model now in the WW500), op 16
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

Both camera images pass with the person model. These tests show the change does no harm; only the
challenge models (`93V1`, `71V1`, `72V1`), whose outputs are not mirrored, can show it fixes #270.

## Bench test 3 (6 Nov): #287 fix

Log [logs/ai3.txt](logs/ai3.txt): AI processor console, RP3 image (IMX708), model `20V1`, op 16 = 18, one
cold boot then six motion wakes, with the #287 fix.

The fingerprint at the cold boot recognises the model as already giving probabilities:

```
Input tensor   : i8 [1,96,96,1], scale 0.007843138, zero point -1
                 'input'
Output tensor  : i8 [1,2], scale 1/256, zero point -128
                 'MobilenetV1/Predictions/Reshape_1_cpu'
Output values  : probabilities (softmax quantization, scale 1/256, zero point -128). Percent = (value + 128) / 2.56
```

| Wake | Class 0 | Class 1 | Score | Result | Person % | Expected (class 1 + 128) / 2.56 | Before the fix |
|---|---|---|---|---|---|---|---|
| 1 | -46 | 46 | 92 | `NN+` | 68% | 68.0% | 59% |
| 2 | -14 | 14 | 28 | `NN+` | 55% | 55.5% | 53% |
| 3 | 95 | -95 | -190 | `NN-` | 13% | 12.9% | 32% |
| 4 | 100 | -100 | -200 | `NN-` | 11% | 10.9% | 31% |
| 5 | -99 | 99 | 198 | `NN+` | 89% | 88.7% | 68% |
| 6 | -3 | 3 | 6 | `NN-` | 51% | 51.2% | 51% |

The "before" column is what the old double softmax gives for the same
raw values.

- **Percentages are right:** each matches (raw + 128) / 2.56, and each pair sums to 100%.
- **Detection:** all six match score > op 16. Wake 6 (score 6) is a near miss, correctly `NN-`.
- **First frame where the #270 rule changes the result:** wake 2 has class 1 = 14, which the old rule
  (class 1 > 18) would have called `NN-`. The new rule (score 28 > 18) calls it `NN+`, at 55% "person".
  That is the mirrored-model range (class 1 from 9 to 18) where the new rule is the more sensitive.

## Bench test 4 (6 Nov): percentage rule and #287 on the HM0360

Log [logs/ai4.txt](logs/ai4.txt): AI processor console, HM0360 image, model `20V1`, op 16 = 60 (the
percentage rule), one cold boot then six motion wakes.

The fingerprint shows `Output values  : probabilities (...)`, as on the RP3 (test 3).

| Wake | Raw class 1 | Person % | Expected (raw + 128) / 2.56 | Result |
|---|---|---|---|---|
| 1 | 28 | 61% | 60.9% | `NN+` |
| 2 | 106 | 91% | 91.4% | `NN+` |
| 3 | | | | no frame (see below) |
| 4 | 87 | 84% | 84.0% | `NN+` |
| 5 | 15 | 56% | 55.9% | `NN-` (after one frame fault and retry) |
| 6 | -56 | 28% | 28.1% | `NN-` |

- **#287 on the HM0360:** every percentage matches (raw + 128) / 2.56.
- **Percentage rule:** each `Target 'person' <n>% (threshold 60%)` line shows the same percentage as the
  `Class 'person'` line, and `NN+` appears exactly when it is 60 or more. Wake 1 (61%) is a near pass,
  wake 5 (56%) a near miss.
- **Not tested here:** `setop 16 101` being refused (checked later, see bench test 5).

**Frame faults, not caused by this thread.** Two wakes had `EDM WDT2 timeout` (no frame within 5 s).
Wake 3 failed again on its retry (`Capture failed after 0 of 1 images`); wake 5's retry worked. In both,
`setutc` from the BLE processor arrived about 70 to 80 ms after the capture started, before the frame was
ready. In the other four wakes, and in every wake of bench test 1 (HM0360, 5 Nov), it arrived after the
frame (or not at all), and there was no fault. The NN code runs only after a frame, so this thread's
changes cannot cause it; it points to `setutc` handling during an HM0360 capture. Filed as
[#288](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/288). Parked (Charles, 6 Nov): his
finger may have been on the HM0360 connector at the time, which can disturb the signals (see thread
`2026-10-11_camera_connector_pressure`); to be looked at only if it happens again.

## Bench test 5 (6 Nov): percentage rule on the RP3

Log [logs/ai5.txt](logs/ai5.txt): AI processor console, RP3 image (IMX708), model `20V1`, op 16 = 60, one
cold boot then eight motion wakes. The fingerprint shows `Output values  : probabilities (...)`.

| Wake | Raw class 1 | Person % | Expected (raw + 128) / 2.56 | Result |
|---|---|---|---|---|
| 1 | -37 | 36% | 35.5% | `NN-` |
| 2 | 29 | 61% | 61.3% | `NN+` |
| 3 | 8 | 53% | 53.1% | `NN-` |
| 4 | 1 | 50% | 50.4% | `NN-` |
| 5 | -98 | 12% | 11.7% | `NN-` |
| 6 | 71 | 78% | 77.7% | `NN+` |
| 7 | -37 | 36% | 35.5% | `NN-` |
| 8 | 28 | 61% | 60.9% | `NN+` |

Every percentage matches, each `Target` line repeats the `Class 'person'` percentage, and `NN+` appears
exactly when it is 60 or more. No frame faults; no `setutc` arrived in this run, so it says nothing about
#288. `setop 16 101` was checked separately afterwards: refused (Charles, 6 Nov).

## The approach adopted (Charles, 6 Nov): op 16 as a percentage

Proposed and adopted on 6 Nov, default 60, coded and benched the same day (tests 4 and 5). Victor to
comment on #270.

**Why.** Now that we know `20V1` gives probabilities (#287), the score-based op 16 turns out to be a
scaled number: the raw values are `256 x p - 128`, so class 1 minus class 0 is 256 x (p(target) -
p(no target)), and op 16 / 2.56 is the required gap in percentage points. Op 16 = 18 means "target beats
no target by 7 points", that is target at least 53.5%. Correct, but not something a user can guess. And
the documented maximum, 127, caps the threshold at about 75% confidence.

**Proposal.** Op 16 becomes the camera's required confidence, as a percentage, 0 to 100: a detection
when the model is at least op 16 % sure the photo shows the target (class 1). For a user that is the
whole rule: "set op 16 to 70 to report a person only when the camera is at least 70% sure". It is the
same percentage the console and the EXIF UserComment show for the target class. Class 0 does not come
into it. (With two classes the two percentages add up to 100, so class 0 is then at most 100 - op 16.)

What it gives:

1. **A setting users can understand,** in the units they already see.
2. **The full range, unsigned:** op 16 below 50 detects even when the model leans towards "no target",
   the more sensitive setting the score rule could not offer (point 2 of the approach above). Above 75
   is now possible.
3. **It works for more than two classes** (the target's probability, not a difference of two).
4. **The same meaning for every model, logits included.** For a logit model the firmware applies
   softmax to the logits, after converting them with the model's own scale and zero point, which gives
   the model's probabilities; `outputAsPercentage()` already does this. The per-model scaling that makes
   the score rule mean different things for different models drops out. For two classes the probability
   rises steadily with the challenges' score, so frames rank in the same order and a score cut-off
   converts exactly to a probability. What it does not do is make models equally reliable: an
   over-confident model's 70% is worth less than a well-calibrated one's, so op 16 is still checked per
   model, but from a sensible starting point.

How it is coded:

- **Both kinds of model:** `computeProbabilities()` in `cvapp.cpp` gives each class's probability
  (dequantized for softmax models, softmax for logit models). Detection compares class 1's.
- **The number compared is the whole-number percentage that is printed and written to the EXIF.** The
  proposal said to compare the exact value; using the printed number means what a user sees is what is
  compared (a frame shown as 60% with op 16 = 60 is a detection). The rounding is half a percent.
- **"At least":** detection when the percentage is at least op 16 (the score rule used "more than").
- **Independent of `USE_PERCENTAGE`:** the probabilities are worked out whenever a model runs. The
  switch now only controls printing them and writing them to the EXIF.
- **Range:** `setop` rejects op 16 above 100; a CONFIG.TXT value above 100 is replaced by 60 at load.

Decided:

1. **Default 60** (Charles, 6 Nov).
2. **Op 16 changes meaning again,** in the app, the website and `config_file.md`. #270 is not merged,
   so this is the cheapest time. A card whose CONFIG.TXT still holds 18 will now detect at 18%, nearly
   every frame: set op 16 again.
3. **The challenges hand over a probability, not a score.** They know their models' scales, so they can
   convert their best score cut-off (for softmax models, probability = 50 + score / 5.12).

Bench tests 1 to 3 checked the score rule; detection needs benching again with this rule (the #287
percentages are unaffected).

## Status

Closed, 6 November 2026. The percentage rule (#270) and the #287 fix are coded and benched on both
camera images (tests 3 to 5). `setop 16 101` is refused (checked by Charles, 6 Nov). The remaining work
is in the open items.

## Outcome

**Op 16 is now a percentage.** The camera reports a detection when the model is at least op 16 % sure
that the photo shows the target (class 1): op 16 = 70 means "at least 70% sure". Range 0 to 100, default
60; `setop` rejects values above 100 and CONFIG.TXT load replaces them by 60. The percentage compared is
the whole number the console prints and the EXIF UserComment carries. It means the same for every model:
the firmware turns the outputs into probabilities first. Op 16 stays unsigned, so the op parameter
format did not change; its meaning did, which the app, the website and the challenges still have to
follow (#270).

How we got there: #270 asked for class 1 minus class 0 instead of class 1 alone. A score rule (class 1
minus class 0 above op 16) was coded and benched first (tests 1 to 3). Looking at the percentages then
showed that the person model `20V1`, and the `nnfiles` example models, already end in softmax, so the
firmware applied softmax twice (#287, fixed: the percentages were squashed towards 50%). Knowing the
outputs are probabilities made the score a scaled number with a cap at about 75%, so op 16 became a
percentage instead.

Also done: the cold boot fingerprint prints each tensor's quantization and says whether the outputs are
probabilities or logits; the new `_Tools/tflite_inspect.py` shows the same on a PC
([CLAUDE_person_model_softmax.md](CLAUDE_person_model_softmax.md)). Console percentages are whole numbers
that sum to 100%, and the console no longer stays green after a detection. A CONFIG.TXT that still holds
op 16 = 18 now means "at least 18% sure", nearly every frame: set it again.

Found, not fixed: HM0360 frame faults that coincided with `setutc` arriving during a capture (#288),
parked because a finger on the camera connector may have caused them.

How detection works now is in [`WW500_Neural_Network_Operation.md`](../../WW500_Neural_Network_Operation.md)
and [`config_file.md`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/MANIFEST/config_file.md).

## Open items

- [#270](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/270): record the decision
  ([op 16 as a percentage](#the-approach-adopted-charles-6-nov-op-16-as-a-percentage), default 60);
  Victor to test with the challenge models before merge; update the app, website and challenge repos
  for the new meaning of op 16. (Point 3 of the first approach, a negative score cut-off, no longer
  arises: op 16 below 50 covers it.)
- [#287](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/287): the percentages
  (console and EXIF UserComment) are softmax applied twice for models that end in softmax, such as
  `20V1` ([CLAUDE_person_model_softmax.md](CLAUDE_person_model_softmax.md)). Fixed and benched on both
  images; Charles to close it.
- [#288](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/288): HM0360 frame fault
  (`EDM WDT2 timeout`) when `setutc` arrives during a capture (bench test 4). Parked until it happens
  again (possibly a finger on the camera connector).
