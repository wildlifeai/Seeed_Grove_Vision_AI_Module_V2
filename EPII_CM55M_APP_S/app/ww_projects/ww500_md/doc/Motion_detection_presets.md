# Motion-Detection Presets — HM0360 parameter blueprint

**Status:** design + roadmap (2026-07). **Goal:** replace the current 3-knob
`MD_SENSITIVITY` with four project-level, user-facing presets that map real
deployment profiles (noise rejection, slow invertebrates, fast predators, small
lizards) onto the HM0360's motion-detection registers.

> **The current values are unvalidated.** The MD registers today hold Himax's
> generic reference init (from `github.com/stevehuang82/for_wildlife_ai`), never
> characterised against skinks, stoats, or waving grass. Section 5 is the plan to
> fix that; the numbers in Section 3 are **starting hypotheses**, not final values.

---

## 1. Architecture reality (read this first)

Two facts reshape any "motion tuning" work on the WW500:

1. **Motion detection is the HM0360 sensor's job, not the HX6538's.** The HX6538
   (WiseEye WE2: Cortex-M55 + Ethos-U55 microNPU) is in **deep power-down** while
   watching for motion; the HM0360 runs its onboard MD engine at ~270 µA and wakes
   the SoC by interrupt. Even on the RP-camera (IMX708) build, an HM0360 is fitted
   purely as the MD sensor (see [`hm0360_md.c`](../hm0360_md.c)). The HX6538
   datasheets describe the **AI SoC** — they contain no MD/optical-flow block.

2. **There is no optical flow, no velocity vector, no block motion-estimation.**
   The HM0360 does **frame differencing → a 16×16 macroblock "changed?" bitmap
   (`MD_ROI_OUT`, 32 registers `0x20A1–0x20C0`) → block-count + persistence + IIR**.
   "Velocity" can only be *approximated* by the sample interval (Δt) and the
   latency/persistence frames. Any spec that assumes directional motion vectors is
   assuming hardware this camera does not have.

## 2. What's tunable today vs. what's fixed

`cisdp_sensor_set_md_sensitivity()` ([`cisdp_sensor.c`](../cis_sensor/cis_hm0360/cisdp_sensor.c))
selects one of `OFF/LOW/MEDIUM/HIGH` (OP 17), and each table bends **only three
knobs**: `MD_TH_STR`, `MD_LIGHT_COEF`, `MD_IIR_PARAM`. The powerful knobs sit at the
one-time init defaults — and the author already **scaffolded `MD_BLOCK_NUM_TH` + ROI
as commented-out lines** in those same tables:

| Register | Addr (ctx-B) | Current | Varied by preset today? | Controls |
|---|---|---|---|---|
| `MD_TH_STR_L/H` | `0x35AA/0x35A9` | 0x10–0x30 | ✅ (sensitivity) | per-block change magnitude (↓ = more sensitive) |
| `MD_LIGHT_COEF` | `0x35A5` | 0x21–0x41 | ✅ | illumination compensation |
| `MD_IIR_PARAM` | `0x209A` | 0x00/0x80/0xF0 | ✅ | background-model IIR *(transfer fn undocumented)* |
| `MD_BLOCK_NUM_TH` | `0x35A6` / `0x209B` | **0x04** | ❌ fixed (scaffolded) | **# blocks that must change to fire → target/cluster size** |
| `MD_LATENCY_TH` | `0x209D` | **0x33** | ❌ fixed | **frame persistence `[7:4]=set,[3:0]=clear`** |
| `MD_LATENCY` / `MD_CTRL[5:4]` | `0x209C` / `0x2080=0x31` | **0x01 / lat-sel** | ❌ fixed | latency depth / select |
| `MD_TH_MIN` | `0x2083` | **0x01** | ❌ fixed | threshold floor |
| `ROI_V / ROI_H` | `0x35A7/0x35A8` | **0xE0/0xF0 = full frame** | ❌ fixed | **watch-window (4-bit start/end on the 16-grid)** |
| Δt (`PMU_CFG_8/9`) | `0x3029/0x302A` | 0x1560 | via **OP 11 `MD_INTERVAL`** at runtime | **inter-sample interval** |

Confidence note: `MD_TH_STR`, `MD_BLOCK_NUM_TH`, ROI and Δt have clear, monotonic
meaning. `MD_IIR_PARAM` and the `MD_LATENCY_TH` nibble semantics are **inferred from
the reference init comments** (no HM0360 datasheet exists) and must be characterised
before being trusted (Section 5, Phase 1).

## 3. The four presets

Starting hypotheses — the backbone knobs are directionally sound; IIR/latency are to
be pinned by the bench sweep.

| Lever | P1 High-Noise / Medium | P2 Macro / Ultra-slow | P3 High-Velocity | P4 Micro / Small |
|---|---|---|---|---|
| `MD_TH_STR` | high **0x28** | low-med **0x18** | med **0x20** | **low 0x10** |
| `MD_BLOCK_NUM_TH` | **high 0x0A–0x10** | **low 0x01–0x02** | low-med **0x03** | **lowest 0x01** |
| `MD_LATENCY_TH` | **high (persist)** | low | **≈0 (fire on 1st frame)** | med |
| `MD_IIR_PARAM` † | strong bg-absorb | **off (0x00)** | low | med |
| ROI | **exclude top ⅓** | full / macro-centre | full | **ground band** |
| Δt (OP 11) | ~1 s | **multi-second** | **short / fast** | medium |
| `MD_LIGHT_COEF` | high 0x41 | low 0x21 | 0x31 | 0x31 |

† IIR direction pending Phase-1 characterisation.

**Rationale.** **P1** rejects noise on multiple axes at once (bigger change, bigger
cluster, longer persistence) and drops the moving canopy via ROI. **P2**'s core trick
is a **wide Δt with the background model not absorbing the subject** — a slug displaces
enough between far-apart samples to differ. **P3** trades power for a fast sample rate +
near-zero persistence so an agile predator is caught before it exits. **P4** minimises
both the per-block threshold and the block-count so a 1–2-block skink registers.

**Honest limits.** P3 "velocity" is emulated by rate+latency, *not measured*. And
**camera-swing** (in P1's environment) produces whole-frame motion that MD cannot cleanly
separate from a real subject — that is a **mounting/mechanical** fix first, not a register
fix (see [`WW500_camera_operation.md`](./WW500_camera_operation.md) for mounting notes).

## 4. Firmware & app integration

- **Model.** A preset is a *bundle* (register table + Δt + ROI), so don't overload the
  3-knob `MD_SENSITIVITY`. Add **`OP_PARAMETER_MD_PRESET`** (next free index, OP 29 — see
  [`MANIFEST/config_file.md`](../MANIFEST/config_file.md)) selecting one of four
  `HX_CIS_SensorSetting_t` preset tables that extend the existing `HM0360_md_sensitivity_*`
  pattern (un-comment + populate the `MD_BLOCK_NUM_TH`/ROI lines, add `MD_LATENCY_TH`). The
  preset also sets `OP 11 MD_INTERVAL` (Δt).
- **Backend / project scope.** Projects already carry `activity_detection_sensitivity_id`
  (ww-backend seed) — extend that to the 4 presets; it reaches the device via CONFIG.TXT /
  the manifest like other OPs.
- **BLE.** The app already writes OPs over BLE (`setop`, persisted immediately to
  CONFIG.TXT). One value travels: `MD_PRESET = 1..4`.
- **Crash-safe apply (critical).** Never rewrite MD registers mid-stream. Apply on
  *change* only, at DPD-entry (`hm0360_md_prepare()`), in this order:
  **`MODE_SLEEP` → write preset table (both contexts) → set Δt (`PMU_CFG_8/9`) →
  enable MD interrupt → DPD.** The HM0360 retains its registers across the SoC's DPD, so
  no per-wake reprogramming is needed. `hm0360_md_setMode()` already routes through
  `MODE_SLEEP` before writes — reuse it.

## 5. Roadmap

- **Phase 0 — Instrumentation (prereq). _Done (2026-07-11)._** Read/write every MD register
  on real hardware over the **stock `camreg`** command (no reflash needed) — the headless
  sweep harness is `_Tools/md_sweep.py` (reconnect-pin, per-step re-pin, `MD_ROI_OUT`
  popcount / `INT_INDIC` fired bit). The `md read/write/grid/dump` CLI + `hm0360_md_printGrid`
  are the flashed-firmware equivalent. **Serial map:** Himax MD console = **COM13 @ 921600**
  (COM14 = nRF). **Key finding:** the HM0360 MD engine only runs on the **sleep path** — a
  register poll while the SoC is pinned awake reads 0 at every threshold.
- **Phase 1 — Bench characterisation.** Two rigs now exist: (a) `md_sweep.py` for headless
  register sweeps, and (b) the **live MD overlay in `live_view.py`** — preview arms MD
  concurrently (`hm0360_md_prepare(true,…)` in `preview.c`) and streams the 16×16 grid, so
  you watch which blocks light up over the live image while editing `camreg`/`setop`. Run
  §6's rigs and **measure** detection vs each register; replace §3's placeholders with data
  (detection-rate vs false-trigger-rate per preset). This is the "fit for purpose" work that
  has never been done. (Note: the true field trigger is a sleep→motion→**wake**, so also
  score via the sleep-wake path — watch the nRF's `AI processor sends stats` on COM14.)
- **Phase 2 — Firmware presets.** Encode the 4 validated tables + `OP_PARAMETER_MD_PRESET`;
  wire block-num / ROI / latency / Δt into the bundle.
- **Phase 3 — Config + BLE plumbing.** OP over BLE → CONFIG.TXT → manifest; backend
  `activity_detection_sensitivity_id` → preset; app UI stub.
- **Phase 4 — Field validation (free ground truth).** Deploy each preset and score MD
  against the SD-card upload via the website's Cloud AI: **"MD woke but Cloud AI says
  *blank*" = false trigger; "animal in frame, no wake" = miss.** The dual-AI pipeline is the
  MD validation oracle — no manual labelling.
- **Phase 5 — App UX.** Ship the four named presets.

## 6. QA & physical-testing matrix

| Axis | Method | Validates | Presets |
|---|---|---|---|
| Speed | servo/stepper rail or pendulum carrying a cut-out at calibrated mm/s; metronome for cadence | latency + Δt (catch-before-exit) | P3, P2 |
| Ultra-slow | linear actuator at mm/**min** (snail rig) | wide-Δt detection, IIR-off | P2 |
| Size | graded cut-outs (3 cm skink → 30 cm possum) + laser dot at fixed distance → known block footprint | `MD_BLOCK_NUM_TH`, `MD_TH_STR` | P4, P1 |
| Env. noise | fan + artificial foliage; projected moving shadow; strobe for shifting light | false-trigger rate, IIR, `LIGHT_COEF` | P1 |
| Camera-swing | pendulum/spring camera mount | whole-frame-motion rejection (expose the limit) | P1 |
| Combos | full `{size × speed × noise}` grid per preset | preset separation | all |

**Metrics per preset:** detection rate, false triggers/hour, entry→trigger latency (ms),
average current (µA), miss rate — each logged with the 256-block grid so failures are
diagnosable, cross-checked against Cloud AI verdicts in the field.

## References

- [`hm0360_regs.h`](../hm0360_regs.h) — register addresses.
- [`cisdp_sensor.c`](../cis_sensor/cis_hm0360/cisdp_sensor.c) — current `MD_SENSITIVITY` tables.
- [`hm0360_md.c`](../hm0360_md.c) — MD mode/interrupt/ROI-readout + Δt (`calculateSleepTime`).
- `cis_hm0360/HM0360_OSC_Bayer_640x480_setA_VGA_setB_QVGA_md_8b_ParallelOutput_R2.i` — init defaults.
- [`MANIFEST/config_file.md`](../MANIFEST/config_file.md) — Operational Parameters (OP 11, 17).
