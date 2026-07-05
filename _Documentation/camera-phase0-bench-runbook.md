# Phase 0 Bench Runbook — IMX708 register validation

Companion to [camera-field-tuning-roadmap.md](camera-field-tuning-roadmap.md). Goal: verify the four register-level unknowns on real hardware before Phase 1 is built. Expect 2–4 hours at the bench once the firmware is flashed.

## New firmware commands (added for this phase)

Two CLI commands were added to `ww500_md` (in `CLI-commands.c`), so registers can be swept **without rebuilding firmware between tests**:

| Command | What it does |
|---|---|
| `camreg <addr> <val>` | Writes sensor register (hex), **and** stages it to `0:/RPV3_EX.BIN` on the SD card. Staged registers are re-applied automatically after the sensor init tables at every sensor init (`cis_file_process()` hook in `image_task.c`), so they survive sleep (DPD) cycles and reboots. |
| `camreg <addr>` | Reads a sensor register now (sensor must be powered). |
| `camreg list` | Shows the staged registers (loaded from the SD file at sensor init). |
| `camreg clear` | Removes all staged registers (empties the SD file). |
| `vcm probe` | Checks the focus actuator responds at I2C `0x0C`. |
| `vcm <pos>` | Moves the focus lens, 0–1023 (0 = infinity). **Not persistent** — re-apply after each wake; persistence is a Phase 3 task. |

From the **serial console** type the commands directly. From the **mobile app / BLE terminal** prefix with `AI `, e.g. `AI camreg 0210 01`.

## Setup

1. In `EPII_CM55M_APP_S/app/ww_projects/ww500_md/ww500_md.mk` select the RP v3 camera:
   `CIS_SUPPORT_INAPP_MODEL = cis_imx708` (the default in the repo is `cis_hm0360`).
2. Build, flash, insert SD card, attach the Camera Module 3 (standard).
3. Bench lighting: stable, daylight-ish if possible. Have a grey card (or plain white paper) filling most of the frame for the WB tests.
4. Keep the device awake while working: `setop 8 60000` extends the delay before DPD to 60 s. **Restore afterwards** (`setop 8 1000`).
5. To see the effect of a change: `capture 1 0`, then inspect the JPEG (pull the SD card, or download via the app preview).

Note: staged registers are applied **after** the driver's init tables, so they override the hardcoded defaults (including the fixed exposure `0x0202 = 0x0940`).

## Test 0.1 — Does per-channel digital gain work?

The IMX708 definitely has global digital gain at `0x020E/0F`. Whether the per-channel registers (standard Sony layout, 16-bit big-endian, `0x0100` = 1.0×) are honoured is the key unknown — they are the whole white-balance mechanism.

| Registers | Channel |
|---|---|
| `0x020E`/`0x020F` | Gr (green in red rows) |
| `0x0210`/`0x0211` | Red |
| `0x0212`/`0x0213` | Blue |
| `0x0214`/`0x0215` | Gb (green in blue rows) |

Procedure:
1. Baseline: `capture 1 0`, keep the image.
2. Set red gain to 4×: `camreg 0210 04`, then `camreg 0211 00`, then `capture 1 0`.
3. Compare:
   - Image is strongly **red-tinted** → per-channel gain works. **PASS — record it.**
   - Image is uniformly brighter or unchanged → gains are ganged or ignored. **FAIL** — fall back to software WB (see roadmap risk table).
4. Reset: `camreg clear`, confirm with another capture.

## Test 0.2 — Find neutral WB gains (only if 0.1 passes)

1. Frame the grey card. Suggested starting point: R = 1.75× (`camreg 0210 01`, `camreg 0211 C0`), B = 1.625× (`camreg 0212 01`, `camreg 0213 A0`).
2. Capture, inspect the grey card in the image:
   - too red → lower R gain; too blue → lower B gain; still greenish → raise both.
   - Steps of `0x20` (0.125×) converge quickly.
3. Repeat outdoors and under indoor light if possible.
4. **Record the final R/B gain pair per lighting condition** — these become the Phase 1 defaults and presets.

## Test 0.3 — Focus actuator (VCM)

1. `vcm probe` while the camera is awake → expect "VCM present at 0x0c".
   If not detected, the register writes in `prvVcm` (DW9807-style map: control `0x02`, MSB `0x03`, LSB `0x04`) may not match this actuator — capture the console output for debugging.
2. Place one object ~30 cm away and keep a distant background.
3. Sweep within one awake window (the position is lost when the camera powers down):
   `vcm 0` → capture → `vcm 300` → capture → `vcm 600` → capture → `vcm 900` → capture.
4. **Record which DAC value focuses at which distance** (roughly: 0 = infinity, larger = closer). This is the calibration for the Phase 3 distance presets.

## Test 0.4 — Exposure and analog gain ranges

Exposure: 16-bit line count at `0x0202/0x0203` (default `0x0940`). Analog gain: 16-bit code at `0x0204/0x0205`.

1. Exposure sweep (fixed scene): `camreg 0202 01` + `camreg 0203 00` (dark), then `04 00`, `09 40` (default), `0C 00`. Capture each; note brightness and where it stops changing (frame-length clamp).
2. Gain sweep (restore exposure default first): `camreg 0204 00`+`camreg 0205 70`, then `01 C0`, `03 C0`. Note brightness and noise.
3. **Record usable min/max/step for both** — these become the slider ranges in the Phase 2 app UI.

## Results template

| Test | Result | Values recorded |
|---|---|---|
| 0.1 per-channel gain | PASS / FAIL | |
| 0.2 neutral WB (daylight) | | R = ___, B = ___ |
| 0.2 neutral WB (indoor) | | R = ___, B = ___ |
| 0.3 VCM present | YES / NO | 30 cm ≈ ___, 1 m ≈ ___, ∞ ≈ ___ |
| 0.4 exposure range | | min ___, max ___, default ___ |
| 0.4 gain range | | min ___, max ___ |

When done: `camreg clear`, `setop 8 1000`, and paste the table into the roadmap doc (or hand it to whoever does Phase 1).
