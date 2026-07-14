# PR #140: RP3 image quality: live preview, auto-exposure, auto white balance + colour pipeline

> Offline copy of the pull-request description (https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/140). The canonical discussion lives on GitHub; this file exists so the summary is readable from a clone without internet access.

Stacks on #139 (which stacks on #138) — merge chronologically: #138 → #139 → this. GitHub will retarget this PR to `dev` when #139 merges.

## What this does

Fixes the RP3 (IMX708) image quality end to end. The camera now adapts itself per capture with no manual tuning — validated on the bench against phone reference photos of the same scene.

### 1. Live preview over the console UART (`preview.c`, `_Tools/live_view.py`)
New `preview <0|1|2>` CLI command streams each captured frame as base64 JPEG in the Himax/SSCMA INVOKE framing (~1 fps VGA @ 921600). The Python viewer displays the stream **and keeps the CLI usable**, so `camreg`/`vcm`/`setop` changes are visible one frame later — this is the tuning loop that made everything below measurable. `_Tools/tune_stats.py` scores frames against reference photos.

### 2. Highlight-metered auto-exposure (`ae.c`, op29/op30)
The IMX708 had one hardcoded exposure register and no AE at all. The new loop measures each frame's Y-plane (32-bin histogram) and steps exposure (8–5000 lines) then analog gain (to 16×) so the **bright quartile** lands just below white — mean metering drags high-key scenes to grey. Damped, deadbanded, 3 steps/wake for battery (unbounded during preview). Bench: converges in 3–5 frames, both directions (dark room ↔ daylight).

### 3. Auto white balance + full colour pipeline (`img_correct.c`, op31)
Fixed WB gains can't track the illuminant (indoor-calibrated gains left daylight at G/R 1.37 — the long-standing green tinge). op31=1 (default) measures each frame with a warmth-biased grey-world estimator; flash-lit/too-dark frames fall back to manual op27/28. The correction pass now mirrors the Raspberry Pi ISP order using libcamera's imx708.json calibration: **black level (16) → WB gains → 4640 K CCM → gamma LUT**.

## Measured results (same scene, phone reference vs before/after)

| | G/R bright-quartile | Notes |
|---|---|---|
| Phone reference | 0.939 | target rendering |
| Before (fixed gains, daylight) | **1.372** | the green tinge |
| After (auto, full pipeline) | **0.97–1.01** | white paper renders white, colours correct |

Also fixes: capture frame-timeout retries now dwell progressively (100–500 ms, 5 retries) before sensor restart — instant back-to-back restarts of the marginal MIPI bring-up were observed failing where a delayed restart succeeds.

## Docs
- `_Documentation/live_preview.md` — preview usage, protocol, known quirks (0 ms capture interval wedges the IMX708 datapath; cold-boot first-capture timeouts self-heal)
- `_Documentation/rp3-image-quality-plan.md` — the measured plan, sweep data, and imx708.json reference values
- `Operational_Parameters.md`, `MANIFEST/config_file.md`, `CONFIG.TXT` — ops 29–31

## Known follow-ups (documented, out of scope)
- Warm/watchdog-reboot sensor bring-up remains flaky (cold boots and DPD wakes are fine; mainly affects bench flash workflows)
- MKL62BA `aiProcessor.h` op-param mirror for the app (ops 29–31)
- CCT-switched CCM blending and lens shading (plan Phase C3/C4)

## Test
Flash, then: `py _Tools\live_view.py --port <console COM>` → power-cycle the device → live video appears; type `setop 30 <n>` / `setop 31 <0|1|2>` and watch the effect next frame.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
