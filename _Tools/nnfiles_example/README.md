# Try `nnfiles` with a known model

A small model and ten frames, so anyone can check `nnfiles` on their own WW500. The runbook is
`_Documentation/nn_files_bench.md`.

| File | What it is |
|---|---|
| `MANIFEST/99V2.TFL`, `MANIFEST/99V2.TXT` | A MobileNetV2 rat classifier (96x96 grayscale), compiled with Vela 5.2.0. Labels `not rat`, `rat` |
| `frames/`, `frames.csv` | Five rat frames and five without a rat (mouse, hedgehog, possum, cat, empty) |
| `EXPECTED.CSV` | What the same model gives for each frame on a PC |

## Steps

1. From the repo root, with the SD card as `E:`:

   ```
   python _Tools/nnfiles_prepare.py _Tools/nnfiles_example/frames.csv E:/NNDEMO
   ```

2. Copy `MANIFEST/99V2.TFL` and `MANIFEST/99V2.TXT` into the card's `MANIFEST` folder.
3. Put the card in the camera and, on the console:

   ```
   setop 8 60000
   loadmodel 99 2
   nnfiles NNDEMO
   ```

4. Compare `NNDEMO/RESULTS.CSV` on the card with `EXPECTED.CSV`. The camera should make the same
   call on all ten frames. Its numbers can differ from the PC's by a few steps.
5. Afterwards, `setop 8 1000`, and load your own model again with `loadmodel`, because
   `loadmodel 99 2` replaces it.

The model is for testing the command, not for the field.

## Sources

The frames are from Wellington Camera Traps (Anton et al., 2018), published by LILA BC under the
Community Data License Agreement, Permissive, version 1.0 (https://cdla.dev/permissive-1-0/). The
model was trained by wildlife.ai on Wellington Camera Traps frames and on iNaturalist photos under
CC0 and CC BY.
