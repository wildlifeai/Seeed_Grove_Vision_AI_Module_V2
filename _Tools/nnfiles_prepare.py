"""Prepare a folder of frames for the WW500's 'nnfiles' console command.

    python _Tools/nnfiles_prepare.py <images or list.csv> <SD card>/<FOLDER> [--limit N] [--start N]

Writes F0001.BIN, F0002.BIN, ... into the folder: each is one raw 8-bit grayscale frame of
640x480 = 307,200 bytes, the size and layout of the camera's own frame buffer, so the firmware
feeds it to the model exactly as it feeds a capture (bilinear squash to the model's input, then
pixel - 128). Also writes FRAMES.CSV (index, source file, label) beside them, which the scoring
script joins with the RESULTS.CSV the camera writes.

The input is a folder of images (jpg or png, any size: a 4:3 image is resized, anything else is
squashed to 4:3 the way the camera would see it) or a CSV with a `path` column and an optional
`label` column. The folder name on the card must be 8.3 (up to 8 characters, no extension).
Runbook: _Documentation/nn_files_bench.md.
"""
import argparse, csv, os, sys
from PIL import Image

W, H = 640, 480


def sources(arg):
    if arg.lower().endswith(".csv"):
        rows = list(csv.DictReader(open(arg, encoding="utf-8")))
        return [(r["path"], r.get("label", "")) for r in rows]
    names = sorted(n for n in os.listdir(arg) if n.lower().endswith((".jpg", ".jpeg", ".png", ".bmp")))
    return [(os.path.join(arg, n), "") for n in names]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src"); ap.add_argument("dest")
    ap.add_argument("--limit", type=int, default=1000, help="at most this many frames (bigger folders slow the camera down; the command stops at the first gap)")
    ap.add_argument("--start", type=int, default=1, help="index of the first frame")
    a = ap.parse_args()
    folder = os.path.basename(os.path.normpath(a.dest))
    if len(folder) > 8 or "." in folder or folder.upper() != folder:
        sys.exit(f"folder name {folder!r} must be up to 8 upper-case characters, no extension (FatFS has no long names)")
    os.makedirs(a.dest, exist_ok=True)
    items = sources(a.src)[: a.limit]
    with open(os.path.join(a.dest, "FRAMES.CSV"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f); w.writerow(["frame", "source", "label"])
        for i, (path, label) in enumerate(items, a.start):
            im = Image.open(path).convert("L")
            if im.size != (W, H):
                im = im.resize((W, H), Image.BILINEAR)
            raw = im.tobytes()
            assert len(raw) == W * H
            name = f"F{i:04d}.BIN"
            with open(os.path.join(a.dest, name), "wb") as out:
                out.write(raw)
            w.writerow([name, path, label])
    print(f"{len(items)} frames written to {a.dest} (F{a.start:04d}.BIN to F{a.start + len(items) - 1:04d}.BIN), {len(items) * W * H / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
