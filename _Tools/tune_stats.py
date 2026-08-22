"""Channel statistics for tuning: overall + brightest-quartile (grey-world and
white-patch estimators), for the mobile reference photos and RP3 frames."""
import glob
import sys

import numpy as np
from PIL import Image


def stats(path):
    img = Image.open(path).convert("RGB")
    img.thumbnail((800, 800))
    a = np.asarray(img, dtype=np.float64)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    luma = 0.299 * r + 0.587 * g + 0.114 * b

    def ratios(mask=None):
        if mask is None:
            mr, mg, mb = r.mean(), g.mean(), b.mean()
        else:
            mr, mg, mb = r[mask].mean(), g[mask].mean(), b[mask].mean()
        return mr, mg, mb, mg / max(mr, 1e-6), mg / max(mb, 1e-6)

    # white-patch-ish: brightest 25% of pixels by luma (paper/wall)
    thresh = np.percentile(luma, 75)
    bright = luma >= thresh

    mr, mg, mb, gr, gb = ratios()
    wr, wg, wb, wgr, wgb = ratios(bright)
    print(f"{path.split(chr(92))[-1]}")
    print(f"  overall  R={mr:6.1f} G={mg:6.1f} B={mb:6.1f}   G/R={gr:5.3f} G/B={gb:5.3f}")
    print(f"  bright25 R={wr:6.1f} G={wg:6.1f} B={wb:6.1f}   G/R={wgr:5.3f} G/B={wgb:5.3f}")
    print(f"  luma mean={luma.mean():5.1f}  p5={np.percentile(luma,5):5.1f}  p95={np.percentile(luma,95):5.1f}")


for pattern in sys.argv[1:]:
    for p in sorted(glob.glob(pattern)):
        stats(p)
