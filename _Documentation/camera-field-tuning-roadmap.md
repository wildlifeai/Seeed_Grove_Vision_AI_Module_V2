# Camera Field-Tuning Roadmap

**Goal:** Fix the greenish images from the Raspberry Pi Camera Module 3 (IMX708) and let mobile-app users adjust camera parameters in the field — take a test shot, tweak, repeat — then walk away with the settings persisted while the camera monitors autonomously.

**Repos involved:**
- Firmware: `Seeed_Grove_Vision_AI_Module_V2` → `EPII_CM55M_APP_S/app/ww_projects/ww500_md`
- Mobile app: `wwmobile`

**Why images are green:** The IMX708 is a raw Bayer sensor with no ISP. On a Raspberry Pi, the Pi's ISP applies white-balance gains and a colour-correction matrix; on the WE2 the HW5x5 block only demosaics with unity gains, so green (2 of 4 Bayer sites, highest sensitivity) dominates. Exposure is also hardcoded (`IMX708_EXPOSURE_SETTING 0x940` → reg `0x0202`), so there is no AE either.

**Existing plumbing to reuse (no new protocol needed):**
- Firmware `op_parameter[]` array persisted to `CONFIG.TXT` on SD (`fatfs_task.c/.h`), settable over BLE with `AI setop <index> <value>` / `AI getop`.
- App `commandRegistry.ts` (`setop`, `getops`, `capture`, `txfile`) and `useCapturePreview.ts` (set → capture → download → preview loop), UI in `AdvancedSettingsSection.tsx` / `CameraViewSection.tsx`.

---

## Phase 0 — Bench validation (no app changes, ~2–4 days)

De-risk the register-level unknowns before building anything on top.

> **Status:** firmware tooling is DONE — `camreg` (read/write/stage sensor registers, persisted to
> `0:/RPV3_EX.BIN` and re-applied at every sensor init) and `vcm` (focus lens position) CLI commands
> were added to `ww500_md`, so all tests below run without rebuilding between iterations.
> See [camera-phase0-bench-runbook.md](camera-phase0-bench-runbook.md) for the step-by-step procedure.
> Remaining: run the tests on hardware and record the results.

| # | Task | Details | Done when |
|---|------|---------|-----------|
| 0.1 | Verify per-channel digital gain on IMX708 | Sony's standard layout: `0x020E/F` (GR), `0x0210/11` (R), `0x0212/13` (B), `0x0214/15` (GB). The Linux imx708 driver only uses `0x020E` globally; siblings (IMX219/IMX258) expose all four. Hand-write test values via a temporary CLI command or the init table and inspect captures. | Confirmed whether R/B channels can be gained independently in-sensor |
| 0.2 | Find neutral WB gains | Grey-card test shots outdoors/indoors; sweep R and B gains (expect roughly R≈1.8×, B≈1.6×). | A gain pair that produces neutral grey in daylight |
| 0.3 | Verify VCM (focus) actuator access | Camera Module 3 lens actuator (DW9817-type) at I2C `0x0C`. Probe it from firmware, write a lens position, confirm focus shift in captures. | Focus visibly changes with register writes |
| 0.4 | Verify exposure/gain ranges | Sweep `0x0202` (exposure) and `0x0204` (analog gain); note usable ranges and units (lines vs ms) and interaction with frame length. | Documented min/max/step for exposure and gain |

**Fallback if 0.1 fails:** software white balance on the YUV buffer (scale U/V or per-pixel RGB gain on the CM55M) before JPEG — slower and needs a memory-to-JPEG re-encode path; only pursue if in-sensor gains don't work.

---

## Phase 1 — Kill the green tint + core parameters (firmware, ~1 week)

Smallest change that fixes colour and makes exposure/gain adjustable. Testable immediately with the existing `AI setop` command — before any app UI exists.

| # | Task | Files | Notes |
|---|------|-------|-------|
| 1.1 | Apply fixed WB gains from 0.2 in sensor init | `cis_sensor/cis_imx708/IMX708_common_setting.i` or `cisdp_sensor.c` | Immediate default fix for every unit |
| 1.2 | Add camera OpParams | `fatfs_task.h` (enum), `fatfs_task.c` (defaults array) | `CAM_EXPOSURE`, `CAM_GAIN`, `CAM_WB_RED_GAIN`, `CAM_WB_BLUE_GAIN`. Values are `uint16_t`; define units explicitly (e.g. exposure in sensor lines, gains ×256 fixed-point, 0 = "use default") |
| 1.3 | Apply OpParams at capture time | New `cisdp_sensor_apply_params()` in `cis_sensor/cis_imx708/cisdp_sensor.c`, called from `image_task.c` where `cisdp_sensor_init()` runs | Replaces the hardcoded `IMX708_exposure_setting` table |
| 1.4 | Regression check | — | MD-triggered captures, timelapse, DPD wake cycle, and CONFIG.TXT round-trip all still work; params survive reboot |

**Acceptance:** grey-card image is neutral by default; changing exposure/gain/WB via `AI setop` visibly changes the next capture; settings persist across power cycles.

---

## Phase 2 — App UI: Camera Tuning card + test-shot loop (app, ~1–2 weeks, parallel with Phase 3)

| # | Task | Files | Notes |
|---|------|-------|-------|
| 2.1 | Add camera-setting commands | `src/ble/protocol/commandRegistry.ts` | Reuse `setop`; add typed helpers mapping named settings → OpParam indexes (single source of truth for index↔name↔unit↔range) |
| 2.2 | "Camera Tuning" card | New component under `src/screens/Deployments/components/`, mounted in `AdvancedSettingsSection.tsx` | Exposure slider, gain slider, WB preset selector (Daylight / Cloudy / Shade / Custom R+B sliders) |
| 2.3 | "Apply & test shot" button | Reuse `useCapturePreview.ts` | Batch **all** changed params in one setop sequence, then one capture → preview. Each iteration costs a full BLE capture+download cycle (~20–60 s), so never round-trip per slider |
| 2.4 | One-shot WB calibration ("Fix colour from this scene") | App-side | Compute grey-world R/B gains from the downloaded preview image, write them back as `CAM_WB_*` gains, take a confirmation shot |
| 2.5 | Persist settings with deployment | `DeploymentService.ts`, Supabase schema | Store chosen values in the deployment record so they're auditable and re-appliable to other devices |
| 2.6 | Read-back on connect | `getops` on device connect | UI shows the device's actual current values, not stale app state |

**Acceptance:** a field user with no register knowledge can connect, tap "Fix colour", nudge exposure, see a correct preview, press Start Monitoring, and walk away; the deployment record shows the settings used.

---

## Phase 3 — Focus, HDR, resolution (firmware + app, ~2–3 weeks)

| # | Task | Notes |
|---|------|-------|
| 3.1 | VCM focus driver | Small I2C driver for the DW9817-type actuator (validated in 0.3). New OpParam `CAM_FOCUS` (cm, 0 = infinity). App UI: presets 0.5 m / 1 m / 2 m / ∞ + custom. Verify the actuator retains/re-applies position after DPD wake — re-apply in `cisdp_sensor_apply_params()` |
| 3.2 | HDR mode | IMX708 on-sensor HDR needs an alternate mode register table (`IMX708_mipi_2lane_*.i` variant) and changes frame timing — validate MIPI/datapath timing on the WE2. OpParam `CAM_HDR` (0/1), app toggle |
| 3.3 | Runtime resolution | Today sizes are compile-time in `cisdp_cfg.h`. Make `cisdp_dp_init()` accept a resolution profile (the `APP_DP_INP_SUBSAMPLE_E` enum already exists). **Hard limit: WE2 hardware JPEG encoder max 640×640** — larger outputs only via the existing raw BMP path. Offer fixed profiles (e.g. 640×480 JPEG / full-res BMP), not free-form width/height |
| 3.4 | Optional `AI cam <name> <value>` CLI | Friendlier alias over setop indexes in `CLI-commands.c`; keeps app decoupled from raw indexes |

---

## Phase 4 — Nice-to-haves / later

- **Brightness/contrast approximations:** brightness via exposure/gain offset; contrast via a tone-mapping API for the IMX708 mirroring the existing HM0360 `cisdp_sensor_set_tone()`. Present these as "Brightness"/"Contrast" in the UI even though they map to exposure/tone underneath.
- **Simple AE (auto-exposure) loop:** compute mean luma from a capture, adjust exposure/gain, recapture — bounded to 2–3 iterations per trigger to protect battery.
- **Sharpness / denoise: intentionally out of scope** — no hardware support on the WE2; don't expose controls that do nothing.
- **Firmware-side settings versioning:** a `CAM_SCHEMA_VERSION` OpParam so the app can detect old firmware and hide unsupported controls.

---

## Risks & constraints

| Risk | Mitigation |
|------|------------|
| Per-channel digital gain not honoured by IMX708 | Phase 0.1 verifies first; fallback is software WB on the YUV buffer |
| `op_parameter[]` is `uint16_t` | Encode gains as ×256 fixed-point; document units in one shared table (firmware header + app constants) |
| JPEG encoder 640×640 hardware limit | Resolution offered as fixed profiles; large images via BMP path |
| HDR/resolution changes break MIPI/datapath timing | Bench-validate each mode table; keep default mode untouched |
| Tuning loop is slow over BLE (~20–60 s/shot) | Batch param writes; set expectations in UI ("taking test shot…"); keep preview at 640×480 |
| DPD wake may reset sensor state | All params applied in `cisdp_sensor_apply_params()` on every init, sourced from persisted OpParams |

## Suggested sequence

```
Week 1      Phase 0 (bench validation)
Week 2      Phase 1 (firmware: WB fix + exposure/gain OpParams)   ← green tint fixed here
Weeks 3–4   Phase 2 (app tuning UI)  ‖  Phase 3.1 (focus driver)
Weeks 5–6   Phase 3.2–3.4 (HDR, resolution, CLI alias)
Later       Phase 4
```
