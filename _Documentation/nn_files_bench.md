# Scoring a model on frames from the SD card: `nnfiles`

`nnfiles <folder> [first]` runs the loaded model on a folder of frames stored on the SD card,
one after another, exactly as a capture would (the frame is placed in the camera's raw buffer,
squashed to the model's input size and offset by -128, then the model is invoked), and writes the
scores and the time each inference took. It exists so candidate models can be compared on the
same frames on the real camera, without a lens or a scene. Added 5 October 2026 for the rat
challenge.

To try it with a known model and ten frames, follow `_Tools/nnfiles_example/README.md`.

## On the PC

1. Put the frames on the card with `_Tools/nnfiles_prepare.py`:

   ```
   python _Tools/nnfiles_prepare.py test_frames/ E:/NNTEST
   ```

   It writes `F0001.BIN`, `F0002.BIN`, ... (raw 640x480 8-bit grayscale, 307,200 bytes each) and
   `FRAMES.CSV` (frame, source file, label). The folder name is 8.3: up to 8 upper-case characters,
   at the root of the card. Keep a folder to 1,000 frames, the script's default, about 300 MB, and
   put more frames in more folders (see the limits below).
2. Put the model on the card as usual (`MANIFEST/<id>V<ver>.TFL` and `.TXT`, or the app's
   firmware update) and load it (`loadmodel <id> <ver>`).

## On the camera

```
setop 8 60000      hold the board awake; the run resets the inactivity timer per frame, but start wide
nnfiles NNTEST     or: nnfiles NNTEST 501   to resume from frame 501
```

The command refuses to start if no model is loaded, if the image task is not idle, or if a run
is already going. It stops by itself at the first missing frame number, on a read or write error,
or if the model fails to run, and ends with a line such as
`nnfiles: no more frames. 20 frames classified in 4s, results in /NNTEST/RESULTS.CSV`. Put op 8
back afterwards (`setop 8 1000`).

A frame takes about 0.17 s at the start of a folder, so 1,000 frames take about 3 minutes
(MobileNetV2 0.35 at 96 px, 45 to 46 ms of it inference, 5 October 2026).

Do not capture, transfer files from the app, or change the model while it runs: the run owns the
raw buffer and the FatFS task's open file.

## What it writes

`<folder>/RESULTS.CSV`, one line per frame, appended as it goes (a resumed run appends too):

```
# labels: not rat,rat
frame,ms,raw...,pct...
F0001.BIN,61,-108,108,3,97
```

- `ms` is the time from placing the frame to the model's answer: the squash plus the invoke,
  which is what a capture pays too.
- `raw...` is the model's int8 output per class, in class order; `pct...` the firmware's softmax of
  it as whole percentages. The class order is the model's, as in its `.TXT` labels file, printed
  on the first line.

The same line goes to the console as `nnfiles: F0001.BIN,61,...`, and the model's own per-class
prints follow each frame, so a console log of the run is a complete record too.

## Limits and traps

- Frames must be exactly 640x480 8-bit grayscale. A frame of the wrong size stops the run with a
  message; `nnfiles_prepare.py` cannot produce one.
- Big folders are slow. In a folder of 9,999 frames the run did 347 frames a minute at the start and
  144 near the end, 3,075 s in all, while inference stayed at 45 to 46 ms. The time goes in opening
  files, which FatFS finds by reading the folder from its start, so a later frame costs more.
- For a model that already ends in softmax, `pct...` is a second softmax and tops out near 73% for
  two classes. Rank on `raw...`.
- On the RP3 image the raw buffer is YUV420, 1.5 bytes a pixel; the frame fills its Y plane, which
  is all the model reads. On the HM0360 image the buffer is the grayscale frame itself.
- The console prints of a frame's result are the firmware's normal capture output. At 921600 baud
  they do not slow the run.
- The run uses the FatFS task's transfer file handle, shared with file transfers from the app, so
  the two cannot overlap.
