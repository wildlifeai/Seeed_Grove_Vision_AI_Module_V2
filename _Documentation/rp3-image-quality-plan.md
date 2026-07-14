# RP3 (IMX708) Image Quality Plan — kill the green tinge, match the phone

#### Claude - 10 July 2026 (branch `feat/uart-live-preview`)

**Goal:** WW500 RP3 photos that look like the mobile-phone reference shots in
`C:\Users\ww\rsp3vshm0360\reference photo\` (PXL_20260710_*.jpg): neutral-to-
slightly-warm colour, full tonal range, correct exposure.

This plan supersedes the colour part of `camera-field-tuning-roadmap.md`
Phase 0/1 with what we have learned since, and it assumes the **live preview
loop** (`preview` firmware command + `_Tools/live_view.py`, this branch) as
the iteration tool — settings changes are now visible in ~1 s instead of a
20-60 s BLE capture/download cycle.

---

## 1. Where we actually are (measured 10 Jul 2026)

Channel statistics (`_Tools/tune_stats.py`, bright-quartile = paper/wall):

| Image | G/R bright | G/B bright | luma mean | luma p5-p95 |
|---|---|---|---|---|
| Phone reference (x2 shots) | **0.94-0.95** | **1.06** | 115-119 | 34 → 225 |
| RP3 today (op27=286/op28=326 active) | **1.13-1.16** | **1.10-1.16** | 69 | 61 → 80 |

Three separate problems, in order of visual impact:

1. **No auto-exposure at all.** Exposure is one hardcoded write
   (`0x0202 = 0x0940` lines, `IMX708_exposure_setting`); analog gain
   (`0x0204`) is never adjusted. Indoors the image is dim (luma 69) and
   nearly flat (19 luma levels of range vs the phone's ~190).
2. **White balance is a single fixed gain pair** (op27/op28, software,
   applied in `img_correct.c`). It was calibrated once, under one light.
   Under today's light the image is still green (G/R 1.13 vs target 0.94).
   Fixed gains cannot track the illuminant; the phone's AWB does.
3. **No colour-correction matrix, no gamma.** Even with grey neutral, the
   Bayer channel crosstalk is uncorrected (desaturated, hue-shifted colours)
   and the tone curve is linear (flat, murky look). The Raspberry Pi ISP
   applies both; we apply neither. Likely also **uncorrected black level**
   (IMX708 pedestal = 4096/65536 ≈ 16/255) which lifts and tints shadows.

Facts already settled (do not re-litigate):

* **In-sensor per-channel digital gains (0x0210/0x0212) do nothing** —
  bench-verified 0% change (`RP3_white_balance_reencode_issue.md` §1).
  White balance must stay in software. (Roadmap test 0.1 = FAIL, fallback
  built.)
* The software pipeline exists and works: `img_correct.c` does
  YUV→RGB→per-channel gain→YUV + `sw_jpeg.c` re-encode, ~230 ms/VGA frame,
  runs on both the save path and the live preview path.
* Hardware JPEG re-encode after correction is a dead end (bus lock, ibid.).
* `camreg` staged registers (`0:/RPV3_EX.BIN`) persist across DPD/reboot and
  are applied after the init tables — the sweep mechanism for everything
  below (`camera-phase0-bench-runbook.md`).
* Two capture quirks (this branch's bench work): `capture N 0` wedges the
  IMX708 datapath (use ≥500 ms interval); the first capture sequence after a
  cold boot frame-times-out and self-heals on later sequences.

---

## 2. Reference data to reuse (don't re-derive)

Raspberry Pi's calibrated tuning for this exact sensor+lens —
`libcamera/src/ipa/rpi/vc4/data/imx708.json`:

* **Black level:** 4096 (16-bit) → subtract ≈16 (8-bit) before gains/CCM.
* **CCMs by colour temperature** (rows sum to 1.0, applied after WB):
  * 2964 K: `[ 1.721 -0.460 -0.262 | -0.300  1.569 -0.269 |  0.151 -1.133  1.982 ]`
  * 4640 K: `[ 1.530 -0.352 -0.178 | -0.283  1.671 -0.388 |  0.017 -0.572  1.555 ]`
  * 7590 K: `[ 1.414 -0.211 -0.203 | -0.176  1.717 -0.541 |  0.013 -0.631  1.618 ]`
* **AWB CT curve** (r-vs-b chrominance per CCT, 2498 K → 7433 K) — for
  sanity-clamping grey-world results and for building Daylight/Tungsten
  presets.
* **Gamma curve** with strong shadow lift (16-bit: 1024→5040, 4096→15312,
  16384→40642) — the "photographic" look our linear output lacks.

Target numbers (from the phone reference): bright-quartile **G/R 0.92-1.02**,
**G/B 0.98-1.10**, luma mean **105-130**, p95 **≥ 210** without clipping.

---

## 3. The plan

### Phase A — Exposure first (bench sweeps today, then a minimal AE loop)

WB gains computed on an underexposed frame are skewed (pedestal dominates),
so exposure comes first.

| # | Step | How | Done when |
|---|---|---|---|
| A1 | Exposure + gain sweep on the drawing scene | Live viewer running; `camreg 0202/0203` (lines) and `camreg 0204/0205` (analog gain) per runbook test 0.4 — result visible per-frame in the window; `Save frame` + `tune_stats.py` for numbers | Table of luma vs exposure/gain; usable ranges + frame-length clamp recorded |
| A2 | Pick bench/day defaults | From A1, luma mean 105-130 on the bench scene | Values staged in `RPV3_EX.BIN` and written into `IMX708_exposure_setting` defaults |
| A3 | **Minimal AE loop in firmware** | After `FRAME_READY`, mean-luma of the Y plane (already in `demosbuf`, ~1 ms subsampled); step `0x0202` (and gain above a threshold) toward target; clamp; 2-3 iterations per wake, continuous in preview mode. New op-params: AE target luma, max gain, AE enable. (Roadmap Phase 4 item, promoted — it is the biggest visible win.) | Live view converges to target luma in ≤3 frames under bench/day/dusk light; field captures well-exposed |

Effort: sweeps ~half a bench day (live loop makes them minutes each); AE loop
~1 day firmware + bench validation.

### Phase B — Auto white balance (grey-world in the existing software path)

| # | Step | How | Done when |
|---|---|---|---|
| B1 | Confirm target with the reference | `tune_stats.py` on phone shots vs saved RP3 frames of the same scene | Written acceptance band (see §2) |
| B2 | **Grey-world AWB** in `img_correct.c` | During the existing per-pixel pass (or a subsampled pre-pass) accumulate R/G/B sums; gains = G̅/R̅, G̅/B̅ × warmth bias (~0.95 on R to match the phone's rendering); clamp via the imx708.json CT curve endpoints (≈[0.9, 2.2]); IIR-smooth across preview frames. op27/op28 (0 = auto) become manual overrides. Subtract black level (≈16) before the stats and the gains. | Grey card and drawing neutral under bench light, window daylight, and dusk without touching anything |
| B3 | IR-night bypass | When the IR flash fired (`ledFlash` state already feeds EXIF), skip AWB/CCM — IR frames are not colour | Night captures unchanged |
| B4 | Validate across lights + persist | Live viewer A/B vs phone shots; defaults persisted | Acceptance band met in all bench lighting |

Effort: ~1 day firmware + half a day validation.

### Phase C — Colour matrix + tone curve ("looks like a photo")

| # | Step | How | Done when |
|---|---|---|---|
| C1 | Apply the 4640 K CCM after WB | Integer Q8.8 3×3 in the same RGB pass; clamp; rows renormalised | Colours visibly saturate/correct on the drawing (markers match the phone shot) |
| C2 | Gamma LUT | 256-entry LUT from the imx708.json contrast curve, applied per channel after CCM | Shadows open up; luma histogram spreads toward the phone's 34→225 |
| C3 | CCT-switched CCM (optional) | Estimate CCT from the AWB gains (CT curve inverse); blend 2964 K/4640 K/7590 K matrices | Only if C1's fixed matrix visibly fails under tungsten or deep shade |
| C4 | Lens shading (assess only) | Check corners vs centre on a uniform target; imx708.json ALSC tables exist but are heavy | Decision recorded: needed or not |

Budget: pipeline currently 230 ms/frame; CCM+LUT adds roughly 100-200 ms —
fine at ~1 fps preview and one capture per field wake.
Effort: 1-2 days including A/B tuning.

### Phase D — Productize (folds back into the existing roadmap)

* Promote validated exposure/AE/AWB/CCM defaults into the init tables and
  op-param defaults; keep `camreg`/`RPV3_EX.BIN` for field experiments.
* App UI = roadmap Phase 2 unchanged, except 2.4's "Fix colour from this
  scene" becomes trivial: the firmware already computes grey-world gains —
  the app just toggles auto/manual.
* EXIF MakerNote already records WB gains + flash per photo (traceability).

### Phase E — Capture-path quirks (fix opportunistically)

* Cold-boot first-capture frame timeouts: add a settling delay or one
  throwaway frame after MIPI start before the first real capture.
* `capture N 0` datapath wedge: enforce a minimum interval in `prvCapture`.

---

## 4. Measurement harness & bench protocol

1. `py _Tools\live_view.py --port COM13` (self-arming; power-cycle the WW500
   if it is asleep). Keep the drawing + a grey/white card in frame; phone
   reference shots of the same scene under the same light.
2. Change a setting (`camreg …`, `setop 27/28 …`) in the command box; effect
   visible next frame. `Save frame` for the record.
3. `py _Tools\tune_stats.py "saved.jpg" "reference.jpg"` — compare
   bright-quartile G/R, G/B and luma percentiles against the acceptance band.
4. A phase is done when its "Done when" numbers hold under bench light,
   window daylight, and dusk, and the frames look right next to the phone
   shots.

## 5. Risks

| Risk | Mitigation |
|---|---|
| CPU cost of AWB+CCM+gamma per frame | All integer/LUT in the existing pass; budget ≤500 ms/frame; measured each step |
| Grey-world fails on colour-dominant scenes (all-green bush) | Clamp gains to the CT curve band; IIR smoothing; manual override op-params stay |
| HW5x5 output may already bake in some processing (unknown gamma?) | A1 sweeps establish the sensor→YUV transfer empirically before we shape it |
| AE hunting / flicker under mains light | Damped steps, ±tolerance band around target luma, max 3 iterations per wake |
| uint16 op-params | Gains stay Q8.8 (fits); AE target luma 0-255 (fits) |
