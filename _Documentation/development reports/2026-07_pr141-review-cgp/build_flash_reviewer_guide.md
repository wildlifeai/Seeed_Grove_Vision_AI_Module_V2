# WW500 firmware review — companion guide for Charles

*Prepared for the July 2026 review of the Himax (AI processor) and nRF (BLE processor) firmware changes. Everything below was cross-checked against the code on the open PR branches (file:line references throughout), not just the PR descriptions.*

**TL;DR for your last email:** your six steps are essentially right. The two docs you were missing are `_Documentation/building_firmware.md` (build + image generation) and `_Documentation/firmware_update_and_recovery.md` (programming + recovery) — both new in PR #141. Three corrections that will save you a bricked afternoon: (1) both variant builds emit the same `output.img` path, so rename between runs; (2) the SD filename must be **8.3** (e.g. `RP3.IMG`, `HM0360.IMG`) — long names hit a truncation bug; (3) slot metadata is *not* written when an image is downloaded — the label is cleared at flash time and each image labels **its own slot on first boot**, so program image 1 → `reset` → program image 2 → `reset`, and only then will `slots` show both variants. Details in Part A; the exact console messages to expect are in Parts A5 and E.

---

## Part A — Your dual-image build/flash questions (PR #141 topic 1)

Short answer to "is there any doc on making the two images": **yes, two new docs in this PR, meant to be read together** — `_Documentation/building_firmware.md` (build + image generation) and `_Documentation/firmware_update_and_recovery.md` (getting images onto the device + recovery). No single doc yet covers the full two-image chain end to end; the joins between them are exactly where your questions land, so they're spelled out below (and worth folding back into the docs as part of your review).

Your six steps, annotated:

### Step 1 — "Compile the ww500_md app twice, with different settings" ✔ (right, and you remembered the exact values)

`ww500_md.mk:91` holds `CIS_SUPPORT_INAPP_MODEL = cis_imx708` (RP3/colour is the in-repo default; `cis_hm0360` is the commented-out line 84). You don't need to edit the file — a command-line override wins:

```
cd EPII_CM55M_APP_S
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_imx708    # RP3 day/colour
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_hm0360    # HM0360 night/IR
```

- **`make clean` between variants is mandatory** (objects don't encode the `-D` flags; an incremental build links stale objects — `building_firmware.md:24–25`).
- The value selects both the sensor-driver directory that gets compiled (`ww.mk:90–95`) and the defines (`cis_imx708` → `-DCIS_IMX -DUSE_RP3 -DUSE_HM0360_MD`; `cis_hm0360` → `-DUSE_HM0360`, `ww500_md.mk:95–112`). Note the RP3 build still compiles HM0360 motion-detect support (`USE_HM0360_MD`) — both variants drive the HM0360 for MD/light sensing; the variant chooses the *capture* camera.
- Toolchain: **Arm GNU 14.3.rel1** (pinned in CI; the top-level Himax `README.md` still says 13.2 — ignore it).
- Both variants produce the same ELF path (`obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf`), overwritten each build.
- `_Tools/build_ww500.sh [cis_model]` is a WSL convenience wrapper for exactly this (always clean-builds, `-j8`) — but note **it stops at the ELF; it does not run image generation**.

### Step 2 — "Go through the script files to create the output.img for each, giving them different names" ✔ with two traps

Use **`we2_image_gen_local_dpd`** with the **`project_case1_blp_wlcsp_rc24m.json`** profile (not the plain `project_case1_blp_wlcsp.json` from the older docs — the RC24M profile is what CI and `building_firmware.md:36–53` use):

```
cd we2_image_gen_local_dpd
cp ../EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf input_case1_secboot/
./we2_local_image_gen project_case1_blp_wlcsp_rc24m.json      # (we2_local_image_gen.exe on Windows)
# -> output_case1_sec_wlcsp/output.img
```

- **Trap 1:** both variant runs write the *same* `output_case1_sec_wlcsp/output.img` — copy/rename it out of the way between runs or run 2 silently overwrites run 1. (`building_firmware.md` doesn't mention this; CI does the renaming in its own workflow step.)
- **Trap 2 (naming):** for the SD-card route the filename must be **8.3** — see step 3. Suggested: `RP3.IMG` and `HM0360.IMG`, or the Setup-Folder scheme from `MANIFEST/README.TXT:25–30`: `VYMDDHMM.IMG` where the first letter is `R` (RP3) or `H` (HM0360). CI's own artifacts are named `WW500_C02_RP3_<timestamp>.img` / `WW500_C02_HM0360_<timestamp>.img` — those names are **too long for the device** (next step).

### Step 3 — "Place the files on the SD card and run the command to program the images, or via XMODEM" ✔ command is `firmware`

Files go in **`/MANIFEST`** on the SD card (not the root), with **8.3 filenames** — two independent reasons: FatFs is built with long-file-name support off (`ffconf.h:116`, `FF_USE_LFN 0`), and `xip_update_firmware_from_sd()` builds its path in a buffer sized for a 13-char name (`MAX_FIRMWARE_NAME_LEN`, `xip_manager.c:1577`), silently truncating anything longer — see Part F, finding 1.

```
firmware RP3.IMG 0x1A2B      ← console; or  AI firmware RP3.IMG 0x1A2B  from the app
```

- One required argument (filename), one optional (CRC16-CCITT as `0xNNNN`) — with the CRC given, the file is checked **before flash is touched** (`Error: CRC mismatch - file 0x…, expected 0x…. Flash NOT modified.`). Get the CRC from the device's `crc <filename>` command (note: it resolves against the current directory, so `cd MANIFEST` first) or on the PC with `scripts/compute_firmware_crc.js`.
- **There is no slot argument — it always programs the INACTIVE slot** (`firmware: current active slot is 0` / `firmware: programming slot 1 from …`), erases, writes with per-chunk read-back verify, does a full second verify pass, and only then flips the selector to the new slot. A failed update leaves the running image bootable and the selector untouched.
- It finishes by clearing the freshly written slot's camera label to `unknown` (deliberate — see step 4) and printing `firmware: slot N updated OK. Type 'reset' to boot the new image.` The CLI response is `Firmware update OK. Executes at next reset.` — type `reset`.
- **Loading both images is therefore a two-pass sequence** (this is exactly what the app's "Update both cameras" flow automates):
  1. `firmware RP3.IMG 0x….` → `reset` → device boots the RP3 image (which labels its slot).
  2. `firmware HM0360.IMG 0x….` → `reset` → device boots the HM0360 image (labels the other slot).
  3. Optionally `switchslot` (+ let it sleep) to finish on the camera you want active.
- **XMODEM alternative:** that's the ROM bootloader's menu, not an app command — hold a key on the console at power-up (`[1] Xmodem download and burn FW image`), 921600 baud, or use the script: `cd xmodem && python xmodem_send.py --port COM13 --baudrate 921600 --file <path>\RP3.IMG`. The bootloader burns into the slot it was trying to boot and alternates A↔B on consecutive burns; it also **rewrites the whole selector sector, wiping both camera labels** (normal; they self-heal as each slot first boots). Full runbook: `firmware_update_and_recovery.md:41–65`.
- One safety rule from that doc worth internalising: **devices built before 14 Jun 2026 have a defective on-device slot writer** (stale descriptor CRC → secure-boot reject) — their *first* update must go via XMODEM, and always install the **pair from the same build**.

### Step 4 — "After each image is downloaded, the camera selection metadata is written automatically" ✘ — the opposite, then self-healing

At download/flash time the label is **cleared**: `firmware`'s last step writes `XIP_SLOT_VARIANT_UNKNOWN` (0) into the WWSM record for the slot it just programmed (`xip_manager.c:1627–1633` — "the new image's camera variant is unknown until it boots and labels itself"). The label is then written by the **image itself on its first boot**: `vImageTask()` calls `cameraSwitch_labelBootSlot()` (`image_task.c:1535`), which compares the recorded variant with the build's own (`USE_RP3`/`USE_HM0360` compile-time constant) and rewrites the sector only on mismatch. So:

- Immediately after `firmware`: that slot reads `'unknown'` in `slots` — expected, not a fault.
- First boot of the new image: console prints `Slot A labelled variant 2` (2 = RP3, 1 = HM0360) — once, never again for that image.
- XMODEM burns wipe **both** labels; each heals when its slot first boots.
- Consequence for auto-switching: op26 will refuse to switch into an `'unknown'` slot, so after fresh programming, each image must boot once (the two-pass sequence in step 3 does this naturally) before automatic switching can work.

### Step 5 — "Some test or messages to confirm this has worked" ✔ here they are

In order of appearance:

| When | Console evidence |
|---|---|
| During `firmware` | The `firmware:`/`erase_firmware_slot:`/`write_firmware_from_sd:`/`verify_firmware_slot:` progress trace, ending `write_slot_selector: slot N selector written OK` + `firmware: slot N updated OK.` |
| Boot | Banner `**** WW500 MD. (WW500_C02) Built: <time> <date> ****`, then `Git branch: …`, then **`Camera: RP v3 (IMX708)`** or **`Camera: HM0360`** — the fastest confirmation of which image is running (it's also the string CI greps to identify a build). |
| First boot of a new image | `Slot A labelled variant 2` / `Slot B labelled variant 1` |
| Any time | `slots` → `Active slot 0 running 'RP3 (day/colour)'. Slot A: 'RP3 (day/colour)', Slot B: 'HM0360 (night/IR)'. Auto-switch: off` |
| Any time | `ver` (board + build time), `camera` (camera string), `dump-sel` (raw hex of the selector sector: `HIMAXWE2` header + `WWSM` record at offset 32) |
| After `switchslot` | `Switched to slot 1 ('…'). Reset scheduled.` → at next sleep `>>> Reset by watchdog` → other image's banner |

### Step 6 — "Some app commands to confirm" ✔ same commands, `AI `-prefixed

All CLI commands work from the app with the `AI ` prefix (same parser): `AI slots`, `AI switchslot`, `AI camera`, `AI ver`, `AI getop 26` / `AI setop 26 <0|1>`, `AI firmware <file> [0xCRC]`, `AI reset`. Documented with example responses in `_Documentation/ble_commands.md:153–190` (note 4 there is a compact summary of the whole dual-image scheme). When an automatic switch is scheduled, the app also receives a spontaneous message: `Auto camera switch: light level wants the night (HM0360) camera - switching at next sleep`.

## Part B — Where the documentation lives

For this topic, read in this order:

| Doc | What it gives you |
|---|---|
| `_Documentation/building_firmware.md` (new) | Canonical build recipe: toolchain 14.3.rel1, variant override, RC24M image generation, non-reproducibility caveats (secure-boot signer + `__DATE__`), prebuilt-libs churn note |
| `_Documentation/firmware_update_and_recovery.md` (new) | The four update paths (app pair-update / console `firmware` / bootloader XMODEM / SWD), safety rules, un-brick runbook, console quick reference |
| `ww500_md/doc/slot_selector.md` (yours, extended) | Selector-sector format + the new WWSM record (offset 32, variant values 0/1/2), XMODEM label-reset note |
| `MANIFEST/README.TXT` + `MANIFEST/config_file.md` | SD `/MANIFEST` contents, `R…/H….IMG` naming, op-param file format incl. op26–28 |
| `_Documentation/ble_commands.md` | App-side command table (`AI slots` / `AI switchslot` / `AI firmware …`) with responses |
| `_Documentation/Operational_Parameters.md` | The op20–28 table (op26 = SLOT_SWITCH) |
| `_Documentation/AE_Light_Sensor_Roadmap.md` §8.5/§8.5.1 | Why the light decision is a filtered multi-frame aggregate; the two bench-found fixes |
| `_Documentation/camera-field-tuning-roadmap.md`, `camera-phase0-bench-runbook.md` | The tuning-CLI context for topic 2 of the PR (the runbook is the only other place documenting the `CIS_SUPPORT_INAPP_MODEL` override) |
| `ww500_md/doc/WW500_crc_checks.md` | Design of the `crc` command and the CRC gate on `firmware` |
| `_Documentation/Compile_and_flash.md` | **Superseded** (kept for the TeraTerm/Eclipse walkthroughs); its header says what replaced it |
| `swd_debugging/swdflash/` (new tools in this PR) | pyOCD-based `burn_slot.py` / `check_slots.py` / `parse_selector.py` / `set_selector.py` — SWD-side equivalents of `firmware`/`slots`/`dump-sel`, plus `firmware_update_investigations.md` explaining the pre-June-14 defective-flasher story |

## Part C — Scope of the review, and what's new since 12 July

### The ask is unchanged: the three PRs from Victor's 12 July email

The stack and the diff bases are exactly as that email described — each PR's diff shows only its own work:

| Order | PR | Branch | Diff base | What it is |
|---|---|---|---|---|
| 1 | **#141** | `feat/camera-features-combined` | `origin/dev` | The 9-topic camera PR you are working through now (dual-camera slots, tuning CLI, AE light sensor, RP3 white balance, EXIF, stability fixes, op-param persistence, update/recovery guide, CI matrix). Re-opened copy of #138. |
| 2 | **#142** | `feat/ble-fast-transfer` | `origin/feat/camera-features-combined` | Himax half of the fast BLE transfer (SD write path, I2C re-arm in TX-done ISR, log suppression, 4 s missing-master window, slot-label retry). **Note: it also flips two defaults — auto camera switching ON (op26=1) and MD IR brightness 50% (op22).** Re-opened copy of #139. |
| 3 | **#140** | `feat/uart-live-preview` | `origin/feat/ble-fast-transfer` | Live preview over console UART + IMX708 auto-exposure (op29/30) + auto white balance and colour pipeline (op31). Still "review for direction rather than polish", though it now carries measured bench results. |

For your two-folder Meld setup, that base column is the branch to keep in your *reference* folder as you move through the stack: `dev` ↔ `feat/camera-features-combined` for #141, then `feat/camera-features-combined` ↔ `feat/ble-fast-transfer` for #142, then `feat/ble-fast-transfer` ↔ `feat/uart-live-preview` for #140. Each branch tip has a `REVIEW_PR<N>.md` at the repo root — an offline copy of the PR description readable without GitHub access.

Push protocol reminder for your comment rewrites (from the 12 July email): commit directly onto the branch the fix belongs to with conventional prefixes (`fix:`, `docs:` …), **no rebase or force-push** (the stack breaks); if branch rules refuse the push, use `review/cgp-<topic>` and PR into the feature branch. Anything fixed on #141 gets merged upward through the stack.

### New since 12 July — for your awareness only, no review asked

These landed after the original ask. Nothing to do here; they're listed so the repo state doesn't surprise you:

| PR | Stacks on | What it is |
|---|---|---|
| Seeed **#146** (`fix/ios-session-inactivity`) | #142 | Himax half of the iOS fix: transfer-session inactivity timeout 5 s → 15 s, riding out CoreBluetooth renegotiation stalls (the Apple-phone issue Tommy hit). Small diff. |
| ww-hardware **#30** (`fix/ios-conn-param-retry`) | #27 | nRF half of the same fix (BLE 0.30.48): defends the fast connection parameters when iOS renegotiates mid-transfer. `ble_actions.c` only (~150 lines). Together: #30 re-asserts the fast parameters from the peripheral side, #146 keeps the Himax session alive through the stall. |
| Seeed **#143** (`feat/md-instrumentation`) | #140 | Motion-detection instrumentation, live HM0360 motion overlay in preview, `camreg` sweep harness, op33. |
| Seeed **#144** (`feat/hires-capture-fixes`) | #143 | Hi-res capture working end-to-end at **1216×960** — four stacked root-cause fixes. This is the follow-through on the resolution point you raised in the BLE review (whether the app needs full-size JPEGs). |
| Seeed #147, #148 | `dev` | CI housekeeping (manual-dispatch environment honouring; PR-Agent review). |

### BLE processor (`ww-hardware`) — status, nothing further asked

Your #27 review is done; the four questions and answers are recorded in the PR thread. For reference: #27 stacks on #26 (`feature/ai-state-machine-updates`), and `BLE_Fast_File_Transfer.md` at the branch root remains the deep-dive doc (its last section is a 4-point review checklist, if you ever want to revisit). #30 above is the only BLE change since, and it's FYI.

## Part D — Answers to the open items in your notes (CGP_Code_Review_July26.md)

### D0. The function you flagged "Complex — I need to understand this": `cameraSwitch_autoSwitchCheck()`

It is a guard chain — seven cheap early-outs, then the switch. In order (`camera_switch.c:73–121`):

1. **Latch:** a function-`static bool switchScheduled` — once a switch has been scheduled, every later call returns false until the reboot actually happens (prevents double-switching while waiting for the next sleep).
2. **op26 gate:** `fatfs_getOperationalParameter(OP_PARAMETER_SLOT_SWITCH) != 1` → false.
3. **Build participates?** `cameraSwitch_thisVariant()` == UNKNOWN (an RP2/IMX477 build) → false.
4. **What does the light want?** `dark = (op25 == 1)`; wanted = dark ? HM0360 : RP3. If wanted == the running variant → false. **Key insight: it does not read the sensor.** op25 (`AE_FLASH_STATE`) is the *persisted, hysteresis-filtered* dark/bright decision written by `ledFlashNewAEStats()` (`ledFlash.c:420–475`) from a 16-sample AE burst (120 ms apart): gain-railed → dark; mean < op23 → dark; mean > op23+12 → bright; in the dead band it keeps the previous decision. Because op25 is persisted to CONFIG.TXT, the decision survives the reboot — that is what prevents a switch-back loop after the device comes up on the other camera.
5. **Active slot readable?** else false.
6. **The other slot must be labelled with the wanted variant** — never switch into an unknown or mismatched image. Mismatch logs `Auto camera switch: light wants '…' but other slot holds '…' - staying`.
7. **Do it:** `xip_switch_slot()` (which itself refuses if the other slot has no secure-boot image), set the latch, `app_setResetRequest(true)`, print `Auto camera switch: light is DARK -> slot 1 ('HM0360 (night/IR)'). Reset scheduled.` and notify the app over BLE.

The reboot is deferred: the reset-request flag is consumed in `image_sleepNow()` (`image_task.c:2584`) — instead of entering DPD the device arms a 100 ms watchdog and prints `>>> Reset by watchdog`. That ordering is deliberate, so the BLE announcement reaches the app before the link drops.

Call site: `image_task.c:868`, immediately after each AE light-level decision in `handleEventForCapturing()` — and the AE sampling burst itself only runs when a consumer is enabled (AE flash mode or op26 = 1, `image_task.c:853–854`). On the way into DPD, if op26 (or AE flash) is enabled and timelapse is off, an RTC alarm of op24 minutes (default 15) is armed (`image_task.c:2626–2639`) so dawn/dusk is noticed without motion. Note the asymmetry with the manual path: `switchslot` deliberately has **no label requirement** — only "other slot contains a programmed image" (`CLI-commands.c:874–876`).

### D1. Your TODO: "check `xip_copy_metadata_to_flash()` for pre-existing metadata"

Different metadata — that function has nothing to do with the camera-variant labels. It writes the **NN model** metadata record (magic `"LABL"`: class labels + model filename) at the start of the model area (0x00200000). The camera-variant record is the 8-byte **WWSM** record at offset 32 of the selector sector (0x00FFF000), handled by `read_slot_meta()` / `write_selector_sector()`.

To answer the underlying question anyway: `xip_copy_metadata_to_flash()` → `write_metadata_to_flash()` (`xip_manager.c:676–704`) does a bare word-write with **no erase and no read-modify-write**. On NOR flash that ANDs new bits into old, so calling it over an existing record would corrupt it. It is safe today only because its single call site (`cvapp.cpp:407`, in `load_model_from_sd()`) runs immediately after `xip_copy_model_from_sd_to_flash()`, which erases the whole region first (`xip_manager.c:767–777`). The erase-first precondition is **not stated in its header comment** (`xip_manager.h:147–152`) — a fair review finding; a future standalone caller (e.g. re-labelling a model without reflashing) would silently corrupt the record. Related: `ModelMetaData.crc` is "reserved for future integrity check, written as 0" — no integrity check exists yet.

The WWSM camera labels, by contrast, **are** explicitly preserved across selector-sector rewrites: `write_slot_selector()` re-reads the meta record before erasing (`xip_manager.c:1483–1486`, "Preserve the camera variant labels across the sector rewrite").

### D2. Your sequence reconstruction of `cameraSwitch_labelBootSlot()`

Your 5-step reconstruction is correct, with two refinements:

1. `cameraSwitch_labelBootSlot()` (`camera_switch.c:129–144`) first calls `cameraSwitch_thisVariant()` — **compile-time only** (`USE_RP3`/`USE_HM0360` defines), no flash access. A build for neither camera (RP2/IMX477) returns UNKNOWN and the function exits without touching flash at all.
2. `xip_get_active_slot()` reads the 20-byte bootloader header (SPI read #1).
3. `xip_set_slot_variant(slot, variant)` re-reads the header (#2, needed so a rewrite would preserve the bootloader's slot choice) and reads the 8-byte WWSM record (#3).
4. If the recorded variant already matches: return — **total 3 SPI reads, 0 erases, 0 writes** (each read is a disable-XIP/enable-XIP pair under `xSPIMutex`).
5. Only on mismatch (first boot of a new image): one 4 KB sector erase + two word-writes, and the console prints `Slot A labelled variant 2` (or equivalent).

**Your "could have been simpler and more efficient (fewer flash reads)" comment: the code agrees with you.** `xip_manager.c:917–919` and `:954–956` carry a "Simplification opportunity" comment noting the selector sector is already XIP-mapped at `0x3AFFF000` and could be read directly, without the disable/enable-XIP round-trips. That would remove the mutex-held mode switches from the boot path. Also note the `firmware` update path currently erases the selector sector **twice** per update (once in step 5 `write_slot_selector()`, once in step 6 when clearing the new slot's label to UNKNOWN) — a candidate for a combined write if you want to reduce flash wear.

One more nit in your direction: `cameraSwitch_labelBootSlot()` ignores the return value of `xip_set_slot_variant()` (`camera_switch.c:143`). PR #142 partially addresses this ("slot label robustness": every failure path logged + one retry), so review that alongside.

### D3. Your comment: label at DPD-entry instead of at wake, to protect time-to-first-picture

The code's stated rationale for boot-time labelling (`image_task.c:1530–1534`): the label is correct as soon as the device is queryable, and gets written even if the session never reaches a clean sleep. Labelling at sleep would leave a freshly updated image unlabelled (and `slots`/the app reporting wrong data, and auto-switch unable to trust the record) for the entire first wake session — and after `firmware` + `reset` the device might be power-cycled before ever sleeping cleanly.

On cost: the steady-state price at wake is the 3 small SPI reads above — the sector erase happens only once per newly flashed image. `cameraSwitch_labelBootSlot()` runs at `image_task.c:1535`, before `configure_image_sensor()` (:1544) and `cv_init()` (:1595), both of which dwarf it (the NN init even logs itself as a candidate to move after the capture). The `init_flash()` it triggers (~10 ms delay inside) would be paid by `cv_init()` moments later anyway. So the latency argument favours leaving it where it is; the cheaper win is the XIP-mapped read simplification in D2, which deletes most of the remaining cost. If you still want it later in the wake, the constraint to preserve is: **it must run in any session that can be queried or that can auto-switch** — moving it after `cv_init()` (rather than to sleep) would keep the guarantee while getting it off the critical path to the first frame.

There is one real (if unlikely) serialisation to be aware of: the label read takes `xSPIMutex`, so a console `slots` or a model load mid-flight blocks the image task briefly. That mutex itself is a PR #141 fix for a field-observed hang (`xip_manager.c:304–308`), created pre-scheduler by `xip_manager_preinit()`.

### D4. Your comment: "maybe we might just want to use the HM0360 by itself" (e.g. AI predator ID, cost/power)

The flexibility you're asking for already exists in the mechanism, in three layers:

- **op26 = 0** (manual-only) — the default *in this PR*. But note **PR #142 flips the default to 1 (auto ON)**, so if you want HM0360-only deployments after #142, set `setop 26 0` per deployment (persisted in CONFIG.TXT), or ship a CONFIG.TXT.
- Even with op26 = 1, the auto-switch **only** fires into a slot whose recorded label matches the wanted variant (`camera_switch.c:100–108` — "never into an unknown or mismatched image"). A device with HM0360 images in both slots (or only one programmed slot) logs `Auto camera switch: light wants 'RP3 (day/colour)' but other slot holds 'HM0360 (night/IR)' - staying` and keeps running. So an HM0360-only unit is safe even if op26 is left on.
- The build itself: `cis_hm0360`-only builds are first-class; nothing requires an RP3 image to exist.

Worth adding a sentence to `slot_selector.md`/the roadmap docs saying exactly this, so the day/night framing doesn't read as the only supported deployment — that's a docs improvement your review can propose.

### D5. Stale comments you'll want to fix while you're re-commenting

You said you're rewriting comments as you go — these two are actively wrong and worth fixing first: `fatfs_task.h:89` and `fatfs_task.c:234` still describe op26 automatic switching as "(PLANNED - see camera_switch.c)". It is implemented and wired end-to-end in this branch (`cameraSwitch_autoSwitchCheck()` at `camera_switch.c:73–121`, called from `image_task.c:868`); `MANIFEST/config_file.md` and the PR description already describe it as shipping. Two other latent TODOs the trace turned up, for your list rather than for fixing now: the bootloader selector checksums are hard-coded captured constants (`xip_manager.c:66–70`, "TODO: verify these values if the bootloader is ever updated"), and non-RP3/HM0360 builds (RP2/IMX477) are excluded from switching by design (`camera_switch.c:35–36`).

## Part E — Suggested bench verification sequence for topic 1

Once you have both images built (Part A), this sequence exercises every path of the feature and tells you at each step what the console should say. It doubles as the missing hardware validation for the op26 auto-switch cycle, which the PR records as **build-verified but not yet hardware-verified** — your bench run would close that gap, so please record the serial logs.

1. **Program the two slots.** Use `firmware <file> [0xCRC]` twice (each call writes the **inactive** slot, verifies, flips the selector, and clears that slot's label to unknown), or two XMODEM burns. Expect after each: `firmware: slot N updated OK. Type 'reset' to boot the new image.`
2. **First boot of image #1.** Watch for `Slot A labelled variant 2` (RP3) or `... variant 1` (HM0360) — the self-labelling write. On every later boot of the same image this line does **not** appear (label already correct, no write).
3. **Check the half-labelled state is reported honestly.** `slots` (or `AI slots` from the app) should show the running slot's variant and the other slot as `'unknown'` if it has never booted. This is the designed state, not a fault.
4. **Manual switch.** `switchslot` → `Switched to slot N ('unknown'). Reset scheduled.` (the name may read unknown precisely because that slot hasn't booted yet — the command intentionally doesn't require a label, only a programmed image). Let the device go to sleep: expect `>>> Reset by watchdog`, then the other image boots and labels its own slot.
5. **Now fully labelled.** `slots` → `Active slot 1 running 'HM0360 (night/IR)'. Slot A: 'RP3 (day/colour)', Slot B: 'HM0360 (night/IR)'. Auto-switch: off`. Cross-check `AI camera` and `ver` (build variant/version).
6. **Negative test.** `switchslot` with an erased/empty other slot must refuse: `Slot switch failed (-2): other slot has no image` (it checks the secure-boot magic word before touching the selector).
7. **Auto-switch, dark direction.** On the RP3 image: `setop 26 1` (replies `Set OpParam 26 = 1`, and saves CONFIG.TXT immediately). Put the unit in a dark box, trigger a capture (or wait for the op24 RTC check, default 15 min). Expect the AE burst to decide dark, then `Auto camera switch: light is DARK -> slot N ('HM0360 (night/IR)'). Reset scheduled.` plus the BLE notice `Auto camera switch: light level wants the night (HM0360) camera - switching at next sleep`. At next sleep: watchdog reset into the HM0360 image.
8. **No switch-back loop.** After the reboot the device is on HM0360 with op25 (the persisted dark/bright decision) still = dark, so it must **stay** on HM0360 while dark. Unbox it in room light, trigger a check → the reverse announcement, and back to RP3 at next sleep. One switch per light change, never a flip-flop — the hysteresis (dead band 12 around threshold op23=65) plus persisted op25 is what you're verifying.
9. **Mismatch guard.** With op26=1 and the other slot deliberately left `'unknown'` (reflash it and don't boot it), a dark decision must log `Auto camera switch: light wants 'HM0360 (night/IR)' but other slot holds 'unknown' - staying` and not switch.
10. **From the app**, confirm the same states: `AI slots`, `AI getop 26` / `AI setop 26 0`, `AI switchslot`, and the auto-switch announcement arriving as a spontaneous BLE message.

Two doc/code drift notes to carry into your review while you're in these files: the AE roadmap §8.5 says `AE_SAMPLE_COUNT = 8` / `AE_SAMPLE_GAP_MS = 40`, but `ledFlash.h:56–57` on the branch says **16 / 120 ms** — the header is authoritative; and the op26 comments at `fatfs_task.h:89` / `fatfs_task.c:234` still say "PLANNED" (see D5).

## Part F — Candidate findings from the cross-check (verify, then log on the PRs)

These fell out of cross-checking the docs against the code on `feat/camera-features-combined`. Each has file:line so it's quick to confirm. Ordered by how much they matter.

1. **`firmware` filename-length bug.** The CLI accepts names up to 63 chars (`CLI-FATFS-commands.c:768,780`) but `xip_update_firmware_from_sd()` builds its path in a buffer sized by `MAX_FIRMWARE_NAME_LEN` = 13 (`xip_manager.c:1577`, via `image_task.h:48`), so longer names are silently truncated and fail with "not found". Worse: with a CRC argument, the CRC pass reads the *real* file (75-byte path buffer, `CLI-FATFS-commands.c:810`) — so a long name **passes the CRC gate, prints `Firmware CRC 0x… matched.`, then fails**. Separately `ffconf.h:116` (`FF_USE_LFN 0`) makes 8.3 a hard FatFs requirement that no doc states. Suggested fixes: reject >13-char names in `prvFirmwareCommand`, and document 8.3 in `firmware_update_and_recovery.md` + `MANIFEST/README.TXT`.
2. **PR-gate CI can't catch an HM0360-only compile break.** `.github/workflows/check-ww500_md-app.yml` builds a single variant (the makefile default, currently `cis_imx708`) with toolchain **14.2** (line 18), while the release workflow pins **14.3.rel1** and builds both variants. A change that breaks only the `cis_hm0360` build sails through PR CI.
3. **`camera-phase0-bench-runbook.md:23` states the wrong default variant** — says the repo default is `cis_hm0360`; it's `cis_imx708` (`ww500_md.mk:91`). Following the runbook without an override builds the wrong image.
4. **`building_firmware.md` omits the rename-between-runs step** — both variants land on `output_case1_sec_wlcsp/output.img` (also a git-tracked "golden" file, so local builds dirty the tree). The doc should say: build RP3 → generate → copy out → `make clean` → build HM0360 → generate.
5. **`_Tools/build_ww500.sh` stops at the ELF** (never invokes `we2_local_image_gen`) though the PR presents it as the bench build tool — and three different toolchain locations are in play (script: `$HOME/arm-gnu-toolchain-14.3…`, doc: `$PWD/…`, CI: `$HOME/arm-gnu-toolchain/arm-gnu-toolchain-14.3…`). If the script's guess misses, it silently falls back to whatever `arm-none-eabi-gcc` is on PATH.
6. **Stale op26 "PLANNED" comments** — `fatfs_task.h:89`, `fatfs_task.c:234` (see D5; the feature is implemented).
7. **Message inconsistencies:** the CLI success response says `Firmware update OK. Executes at next reset.` (`CLI-FATFS-commands.c:837`) while the xprintf trace says `…Type 'reset' to boot the new image.` (`xip_manager.c:1635`); `doc/WW500_crc_checks.md:270` quotes a third, stale variant. Also `switchslot` resets at next *sleep* while `firmware` waits for an explicit `reset` — worth one clarifying sentence in `firmware_update_and_recovery.md` so nobody expects `switchslot` to reboot immediately.
8. **Selector sector erased twice per `firmware` update** (once flipping the selector, once clearing the new slot's label — see D2). Trivial wear/latency, but a one-line combined write would halve it.
9. **`xip_copy_metadata_to_flash()` undocumented erase-first precondition** and unimplemented `ModelMetaData.crc` (see D1).
10. **`cameraSwitch_labelBootSlot()` ignores `xip_set_slot_variant()`'s return** (`camera_switch.c:143`) — partially addressed by PR #142's retry+logging; check that change closes it to your satisfaction.
11. Minor: `cis_ov5647` is listed among legal `CIS_SUPPORT_INAPP_MODEL` values but has no `ifeq` branch (builds with no `USE_*` define → `Camera: Unknown`, excluded from switching); the AE roadmap §8.5 sample constants (8/40 ms) lag `ledFlash.h:56–57` (16/120 ms); `WW500_notes_for_users.md:207` still points at the superseded `Compile_and_flash.md`; the WSL `2>NUL` stray-file issue is a known follow-up in the PR description.

## Part G — Where to go after topic 1

Within the three-PR ask:

- **Topics 2–9 of PR #141** are each self-contained in `REVIEW_PR141.md` with their own doc links; topic 3 (AE light sensor) is the one that feeds topic 1's auto-switch decision, so it's the natural next read (start with `AE_Light_Sensor_Roadmap.md` §8.5).
- **Then PR #142** — small, and it touches files you'll have just read (`camera_switch.c` label retry, `fatfs_task.c` SD write path, `if_task.c` I2C re-arm). Remember it flips op26's default to 1 and op22 to 50%.
- **Then PR #140** — the image-quality work; its own `_Documentation/live_preview.md` + `rp3-image-quality-plan.md` are the guides. The `preview` command + `_Tools/live_view.py` from #140 is also the easiest way to *see* what you're testing when you get to hardware, even while reviewing the first two PRs.

Not yours to chase, just so you know they're in hand on our side: the `AI_PROCESSOR_MSG_FILE_LOOPBACK` regression pass on the new FIFO/ACK scheme (promised in the BLE #27 thread), iOS hardware validation of #146/#30, and the op26 box/unbox auto-switch cycle on hardware — though if you do program a board per Part E, those serial logs would close that last gap for free.
