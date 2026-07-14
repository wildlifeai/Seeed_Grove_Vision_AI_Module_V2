# PR #141: Camera features combined

> Offline copy of the pull-request description (https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/141). The canonical discussion lives on GitHub; this file exists so the summary is readable from a clone without internet access.

> **Note:** re-opened copy of #138 (dev was rolled back so this can be reviewed before merging; GitHub keeps the old PR marked merged).

## Summary

Consolidates the camera-focused firmware work from several feature branches into
one branch. It adds dual-camera (RP3 colour / HM0360 night-IR) day/night
switching — manual **and automatic (op26)** — a camera field-tuning CLI, an
auto-exposure-driven flash light sensor, a software white-balance fix for the
RP3 colour camera, EXIF provenance/telemetry in every JPEG, field-found
stability fixes, an operational-parameter persistence fix, and a canonical
firmware update + recovery guide — plus supporting CI, tooling and
documentation. ~4,860 insertions across 57 files (45 commits).

> The individual feature branches (`feat/camera-tuning-cli`,
> `feat/camera-day-night-switching`, `feature/ae-light-sensor`,
> `docs/ae-light-sensor-roadmap`) are **fully contained in this branch**. Their
> open PRs should be **closed in favour of this one**, not merged separately —
> merging them too would double-apply the same commits.

## What's in this PR

### 1. Dual-camera day/night switching (manual + automatic)
The WW500 holds two firmware images in A/B flash slots — RP3 colour (IMX708) and
HM0360 night/IR — and switches between them. Adds slot metadata tracking (the
WWSM record), the `slots` / `switchslot` console+BLE commands, and **automatic
light-based switching**: with `op26 = 1`, each AE light check compares the
hysteresis-filtered dark/bright decision with the running variant; on a mismatch
(dark on colour, or bright on night-IR) — and only if the other slot is labelled
with the wanted variant — the device flips the boot slot and resets at its next
sleep, announcing `Auto camera switch: ...` over BLE first. The decision is
persisted across DPD (no switch-back loop after reboot) and a latch prevents
double-switching before the reset. A periodic RTC wake (op24) means dawn/dusk is
noticed without motion; op26 alone never fires the flash.
- **Files:** `camera_switch.c/.h` (incl. `cameraSwitch_autoSwitchCheck()`),
  `xip_manager.c/.h` (slot metadata, flash init), `image_task.c`, `ledFlash.c`
- **Docs:** `EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/slot_selector.md`
- **Op param:** `26 OP_PARAMETER_SLOT_SWITCH` (0 = manual `switchslot` only;
  1 = automatic)

### 2. Camera field-tuning CLI (`camreg` / `vcm`)
Per-register camera tuning and VCM focus control from the console/BLE, with an
extra camera-register file maintained on the SD card root for field adjustment
without reflashing.
- **Files:** `CLI-commands.c` (`camreg`, `vcm`), `cis_file.c/.h`,
  `MANIFEST/README.TXT`
- **Docs:** `_Documentation/camera-field-tuning-roadmap.md`,
  `_Documentation/camera-phase0-bench-runbook.md`, `_Documentation/ble_commands.md`

### 3. AE light-sensor flash control
The flash is now driven by the HM0360 auto-exposure registers (dark → flash on),
replacing the old time-of-day / always-on modes, with separate motion-detection
illumination settings. A bench finding showed a *single* AE_MEAN reading is
unreliable (the AE loop limit-cycles, ~37% misreads in a dark box), so the
decision uses a multi-frame aggregate with a gain-railed override and hysteresis.
Two further defects were found and fixed during hardware validation
(roadmap §8.5.1):
- **`MAX_AGAIN` decode:** the ceiling register (0x202b) keeps its code in the
  LOW bits, unlike the `ANALOG_GAIN` readout — the old `>> 4` decode silently
  disabled the gain-railed override.
- **Sleeping sensor:** on the RP3 image with MD off the HM0360 sits in
  MODE_SLEEP and reads AE_MEAN = 0 (permanent "dark"); it is now woken to
  MODE_SW_CONTINUOUS for the sampling window (500 ms settle) and restored after.

The light check runs whenever a consumer is enabled — AE flash (op13 ∈ {1,2})
**or** auto switching (op26 = 1); with both off the op25 decision goes stale
(documented as the "stuck BRIGHT" trap).
- **Files:** `ledFlash.c/.h`, `hm0360_md.c/.h` (`hm0360_md_getAEStats`),
  `image_task.c`, `fatfs_task.c/.h`
- **Docs:** `_Documentation/AE_Light_Sensor_Roadmap.md` (incl. §8.5/§8.5.1),
  `_Documentation/Operational_Parameters.md`
- **Tools:** `_Tools/ae_threshold_analysis.py`, `_Tools/ae_monitor.py`
- **Op params:** `21 MD_FLASH_LED` (2=IR), `22 MD_FLASH_BRIGHTNESS_PERCENT` (5),
  `23 AE_DARK_THRESHOLD` (65), `24 AE_CHECK_INTERVAL` (15 min),
  `25 AE_FLASH_STATE` (runtime)

### 4. RP3 colour camera: software white balance (green-tinge fix)
The RP (IMX708) colour camera has no white balance anywhere in its hardware
pipeline (raw Bayer sensor + WE2 demosaic-only), so images came out green. The
frame is corrected in software (per-channel Q8.8 gains) and re-encoded with a
small baseline JPEG encoder. The hardware/TPG re-encode path wedges the bus under
this firmware and could not be used — see the linked report. Correction runs on
RP builds only and is app-tunable; a gain of 0 restores the untouched hardware JPEG.
- **Files:** `img_correct.c/.h`, `sw_jpeg.c/.h` (new), `image_task.c`,
  `cis_sensor/cis_imx708/cisdp_sensor.c`, `ww500_md.mk`
- **Docs:** `_Documentation/RP3_white_balance_reencode_issue.md`
- **Tools:** `_Tools/wb_configs/` (tuned / uncorrected / sw-no-WB test configs)
- **Op params:** `27 OP_PARAMETER_WB_RED_GAIN` (286), `28 OP_PARAMETER_WB_BLUE_GAIN`
  (326) — Q8.8 (256 = 1.0×, 0 = off)

### 5. EXIF provenance & telemetry in every JPEG
Each image now records which camera and firmware produced it and the exposure
state behind it — the join keys the cloud pipeline uses to link photos to
firmware versions and light conditions:
- **Model** = camera variant (`WW500 RP3` / `WW500 HM0360`), **Software** =
  board + build version (matches the firmware table in the DB), **Flash**
  (0x9209) = whether the flash fired.
- **MakerNote** = 8-field CSV: integration lines, analog gain, digital gain,
  AE mean, AE converged, WB red/blue gains, flash fired.
- NN classification label/confidence made available unconditionally
  (`cv_getLabel`, `USE_PERCENTAGE`).
- **Files:** `exif_builder.c/.h`, `image_task.c`, `cvapp.h/.cpp`
- The ww-website companion branch parses these fields on ingestion.

### 6. Firmware stability fixes (found on the bench)
- **IF-task deadlock:** a `MSG_TO_MASTER` arriving mid-transaction never returned
  `xI2CTxSemaphore`, so `sendMsgToMaster()` blocked forever. Now dropped +
  acknowledged, with a bounded 3 s wait. (`if_task.c`, `image_task.c`)
- **SPI flash init race:** `init_flash()` could run concurrently with other SPI
  users, corrupting slot reads. Serialised under `xSPIMutex`, with a new
  `xip_manager_preinit()` before the tasks start. (`xip_manager.c/.h`, `ww500_md.c`)
- **Capture frame-timeout retry:** the first frame after a sensor start sometimes
  never arrives; instead of tearing the camera down and wedging every later
  capture, the sensor restarts in place and retries up to 3× before giving up.
  (`image_task.c`)

### 7. Operational-parameter persistence
`setop` (console/BLE) now saves `CONFIG.TXT` immediately (without unmounting), so
a parameter change survives the next Deep Power Down even with no capture before
sleep. This also fixes a timelapse interval set via `setop 7` appearing to
"drop" after a sleep. Runtime-only state (e.g. the AE flash decision) does not
trigger a save.
- **Files:** `app_msg.h` (`APP_MSG_FATFSTASK_SAVE_CONFIG`), `fatfs_task.c`,
  `CLI-commands.c`
- **Docs:** `_Documentation/Operational_Parameters.md`

### 8. Firmware update & recovery guide
New canonical documentation for getting firmware onto (and un-bricking) a WW500:
the app's two-pass pair update (`AI firmware`, CRC gate, inactive-slot write,
verify, selector flip), the bootloader X-Modem recovery runbook, the slot-label
reset behaviour of X-Modem burns (labels self-heal on first boot), and a caveat
for pre-June-14 firmware whose slot writer produced unbootable images (stale
descriptor CRC → secure-boot reject → recover via X-Modem).
- **Docs:** `_Documentation/firmware_update_and_recovery.md` (new),
  `_Documentation/ble_commands.md`, `_Documentation/bootloader.md`,
  `MANIFEST/README.TXT` + `config_file.md` (dual-image `VYMDDHMM.IMG` naming),
  `_Documentation/Compile_and_flash.md` (superseded header)

### 9. CI & tooling: build both camera variants
The firmware workflow builds both variants (`cis_imx708`, `cis_hm0360`) via a
matrix, detects each variant from image content, and uploads both to Supabase
with a `camera_variant` tag. Toolchain pinned to Arm GNU 14.3.rel1, RC24M image
profile. The WSL bench-build script now derives the repo path from its own
location (portable across machines).
- **Files:** `.github/workflows/build_and_upload_firmware.yml`,
  `scripts/upload_firmware.js`, `_Tools/build_ww500.sh`
- **Docs:** `_Documentation/building_firmware.md`

## New / changed operational parameters

| Idx | Name | Default | Purpose |
|-----|------|---------|---------|
| 21 | MD_FLASH_LED | 2 | LED for MD illumination (0 none, 1 visible, 2 IR) |
| 22 | MD_FLASH_BRIGHTNESS_PERCENT | 5 | MD illumination brightness |
| 23 | AE_DARK_THRESHOLD | 65 | AE Mean below which the scene is "dark" |
| 24 | AE_CHECK_INTERVAL | 15 | Minutes between periodic AE light checks |
| 25 | AE_FLASH_STATE | 0 | Last AE flash decision (runtime state) |
| 26 | SLOT_SWITCH | 0 | Auto light-based camera switching (0 off, 1 on) |
| 27 | WB_RED_GAIN | 286 | RP3 software WB red gain, Q8.8 (0 = off) |
| 28 | WB_BLUE_GAIN | 326 | RP3 software WB blue gain, Q8.8 (0 = off) |

## Verification

- **Builds:** clean `make clean` build **and link of both variants**
  (`cis_imx708`, `cis_hm0360`), Arm GNU 14.3.rel1 — exit 0, no undefined
  references. CI matrix run green with both variants uploaded to the dev DB.
- **Firmware update (hardware):** two consecutive clean pair updates via the
  app (both variants written to the inactive slot, checksum-verified, device
  finishing on the starting camera); bootloader X-Modem recovery exercised on
  both slots after an intentional/incidental brick.
- **Light sensor (hardware, serial-traced):** sealed dark box → DARK/flash ON
  (mean ~0–35, 18/18); room light → BRIGHT (mean ~78, 15/15); hysteresis held
  the decision at mean 67 with threshold 65 and released at ≥77; a threshold
  change (`setop 23 45`) was followed and survived DPD; gain-railed override
  fired correctly with gains railed at 4/192.
- **RP3 white balance (hardware):** green cast corrected (G/R 1.59 → 0.97 on
  properly-exposed content); ~16.5 KB files at Q85.
- **Persistence:** an op-param change survived a DPD round-trip.
- **Stability:** deadlock, SPI race and capture-retry fixes each reproduced and
  confirmed fixed on the bench.
- **Pending on hardware:** the MODE_SLEEP sensor-wake path and the op26
  box/unbox auto-switch cycle (both build-verified; test protocol documented) —
  plus multi-day field soak.

## Companion work (separate repos)

- **wwmobile** — dual-camera pair update flow ("Update both cameras", per-image
  progress, battery gate), camera selector, Light Sensor flow with op26 toggle,
  WB tuning UI (op27/28), device health banner (HM0360-disconnect self-test)
- **ww-hardware** — `aiProcessor.h` op-param enum synced (21–28); BLE release
  0.30.41
- **ww-backend** — `camera_variant` firmware-table migration + variant-scoped
  single-active trigger (merged)
- **ww-website** — MANIFEST bundles both variant images; EXIF MakerNote parsing
  branch

## Notes for reviewers

- Recommend **squash-merge**: this branch has merge commits from the consolidated
  feature branches.
- `prebuilt_libs/gnu/*.a` show small binary deltas (rebuilt during feature work).
  Benign; can be reverted before merge if a pristine diff is preferred.
- Intermediate commits were not each individually compiled; the final tree builds
  and links for both variants.

## Follow-ups (not in this PR)

- Field validation of the light thresholds and auto-switching across deployment
  environments (dusk/dawn, multi-day soak).
- Automatic (grey-world) white balance for the RP3 — current gains are fixed
  per-deployment; residual tint is illuminant-dependent.
- Decide default WB gains after field validation (286/326 dim-scene vs ~390–407
  bright-scene).
- Fix the WSL build step that writes a stray Windows `NUL` file
  (`.gitignore` or correct the redirect).


