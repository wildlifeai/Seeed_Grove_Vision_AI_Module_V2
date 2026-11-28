# Try `nnfiles` with a cat model

A small cat classifier and ten frames, a second known model for checking `nnfiles` on a WW500. The runbook is
`_Documentation/nn_files_bench.md`; the rat example beside this folder works the same way.

| File | What it is |
|---|---|
| `MANIFEST/98V1.TFL`, `MANIFEST/98V1.TXT` | A MobileNetV2 0.35 cat classifier (96x96 grayscale), compiled with Vela 5.2.0 (`--optimise Performance`, arena 153 KiB). Labels `not cat`, `cat` |
| `frames/`, `frames.csv` | Five cat frames and five without a cat (possum, rabbit, hedgehog, bird, empty) |
| `EXPECTED.CSV` | What the same model gives for each frame on a PC (TensorFlow Lite) and on a WW500 (9 October 2026) |
| `sources.csv` | Where each frame comes from, and its licence |

## Steps

1. From the repo root, with the SD card as `E:`:

   ```
   python _Tools/nnfiles_prepare.py _Tools/nnfiles_example_cat/frames.csv E:/CATDEMO
   ```

2. Copy `MANIFEST/98V1.TFL` and `MANIFEST/98V1.TXT` into the card's `MANIFEST` folder.
3. Put the card in the camera, press RESET and, on the console:

   ```
   setop 8 60000
   loadmodel 98 1
   nnfiles CATDEMO
   ```

4. Compare `CATDEMO/RESULTS.CSV` on the card with the camera columns of `EXPECTED.CSV`. They came from a WW500
   with the HM0360 image of Seeed `dev` f889923d plus this branch: the same firmware and model should give the
   same calls, within a step or two. Copy `RESULTS.CSV` off the card before another run: a new run without a
   start frame writes the file afresh.

   The PC columns show TensorFlow Lite's reference kernels on the same frames. The camera's NPU rounds
   differently, and for this 96x96 model the difference is large enough to change one call: `cat_4.jpg`
   scores 55 for cat on the PC and 16 for not cat on the camera. On the five frames without a cat the two agree
   within 3 steps.
5. Afterwards, `setop 8 1000`, and load your own model again with `loadmodel`, because
   `loadmodel 98 1` replaces it.

The model is for testing the command, not for the field.

## Sources

The frames are camera-trap images from three LILA BC datasets, each published under the Community Data License
Agreement, Permissive, version 1.0 (https://cdla.dev/permissive-1-0/): Trail Camera Images of New Zealand Animals,
Wellington Camera Traps (Anton et al., 2018) and Island Conservation Camera Traps. Each was converted to 640x480
grayscale; `sources.csv` lists the dataset of each. The model was trained by wildlife.ai for the WW Cat Challenge
(`wildlifeai/ww-cat-challenge`, the `wildlifeai_baseline` entry) on frames from LILA BC datasets under CDLA-Permissive
1.0 and on iNaturalist photos under CC0, CC BY or public domain, none under a non-commercial licence.
