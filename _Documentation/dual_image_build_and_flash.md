# Building and flashing the two WW500 camera images, end to end

The WW500 holds two firmware images in its A/B flash slots — RP3 day/colour
(`cis_imx708`) and HM0360 night/IR (`cis_hm0360`) — and switches between them (see
`ww500_md/doc/slot_selector.md`). This page walks the full chain: build both variants →
generate two named images → flash both slots → verify. The build and recovery details
live in [`building_firmware.md`](building_firmware.md) and
[`firmware_update_and_recovery.md`](firmware_update_and_recovery.md); this page carries
the sequence and the joins between them.

## 1. Build both variants

Per [`building_firmware.md`](building_firmware.md) (toolchain Arm GNU 14.3.rel1, WSL/Linux):

```bash
cd EPII_CM55M_APP_S
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_imx708    # RP3 day/colour
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_hm0360    # HM0360 night/IR
```

- The command-line override beats editing `ww500_md.mk` (repo default: `cis_imx708`).
- **`make clean` between variants is mandatory** — objects don't encode the `-D` flags.
- Both variants produce the same ELF path, overwritten each build.
- `_Tools/build_ww500.sh [cis_model]` wraps this under WSL, but **stops at the ELF** —
  it does not run image generation.

## 2. Generate and name the images

Use `we2_image_gen_local_dpd` with the **RC24M** profile (recipe in
[`building_firmware.md`](building_firmware.md)). Two joins the recipe doesn't state:

- Both variant runs write the **same** `output_case1_sec_wlcsp/output.img` — copy the
  first image out (e.g. to `RP3.IMG`) before generating the second, or it is silently
  overwritten.
- For the SD-card route the filename must be **8.3** (e.g. `RP3.IMG`, `HM0360.IMG`, or
  the Setup-Folder scheme `VYMDDHMM.IMG` from `MANIFEST/README.TXT` — first letter `R` or
  `H`). FatFS has no long-filename support (`ffconf.h:116`, `FF_USE_LFN 0`), and the
  `firmware` command additionally truncates names longer than 13 characters
  (`xip_manager.c:1591`) — a long name can pass the CRC check and then fail "not found".
  CI's artifact names (`WW500_C02_<VARIANT>_<timestamp>.img`) are too long for the device.

## 3. Flash from the SD card — the `firmware` command

Copy the image(s) into **`/MANIFEST`** on the SD card, then at the console (or `AI `-
prefixed from the app):

```
firmware RP3.IMG 0x1A2B
```

- One required argument (the filename), one optional CRC16-CCITT (`0xNNNN`) — with the
  CRC given, the file is verified **before flash is touched**. Get the value on-device
  with `crc <file>` (`cd MANIFEST` first — it resolves against the current directory) or
  on a PC with `scripts/compute_firmware_crc.js`.
- **There is no slot argument** — it always programs the *inactive* slot: erase, write
  with per-chunk read-back, a full second verify pass, and only then the selector flip.
  A failed update leaves the running image bootable.
- It finishes by clearing the freshly written slot's camera label to `unknown`
  (deliberate — see §5) and prints `firmware: slot N updated OK.` Type `reset` to boot it
  (the reset executes at the next sleep).
- **Loading both images is a two-pass sequence** (the app's "Update both cameras" flow
  automates exactly this):
  1. `firmware RP3.IMG 0x…` → `reset` → the RP3 image boots and labels its slot.
  2. `firmware HM0360.IMG 0x…` → `reset` → the HM0360 image boots and labels the other.
  3. Optionally `switchslot` (takes effect at next sleep) to finish on the wanted camera.
- Devices built before 14 Jun 2026 have a defective on-device slot writer — their
  *first* update must go via XMODEM; always install the pair from the same build
  ([`firmware_update_and_recovery.md`](firmware_update_and_recovery.md)).

## 4. Flash via XMODEM (bootloader)

The ROM bootloader's menu, not an app command — full runbook in
[`firmware_update_and_recovery.md`](firmware_update_and_recovery.md):

```
cd xmodem
python xmodem_send.py --port COM13 --baudrate 921600 --file RP3.IMG
```

Press the board reset when the script asks. The bootloader burns into the **backup**
slot and restarts into it; consecutive burns therefore alternate A↔B, so two burns
populate both slots. An XMODEM burn rewrites the whole selector sector and **wipes both
camera labels** — expected; they self-heal (§5).

## 5. The label lifecycle — what "unknown" means

Camera labels are **not** written when an image is flashed. Flashing clears the target
slot's label; each image labels **its own slot on first boot**
(`cameraSwitch_labelBootSlot()`), printing `Slot A labelled variant 2` (2 = RP3,
1 = HM0360) once per newly flashed image. Consequences:

- Immediately after flashing, `slots` reports `'unknown'` for the new slot — designed
  behaviour, not a fault.
- Automatic switching (op26) refuses to switch into an `'unknown'` slot, so after fresh
  programming each image must boot once (the two-pass sequence above, or one
  `switchslot`) before auto-switching can operate. Manual `switchslot` needs no label —
  only a programmed image.

## 6. Verify

| When | Console evidence |
|---|---|
| During `firmware` | `firmware:` / `erase_firmware_slot:` / `write_firmware_from_sd:` / `verify_firmware_slot:` trace ending `firmware: slot N updated OK.` |
| Every boot | Banner `**** WW500 MD. (WW500_C02) Built: <time> <date> ****`, then **`Camera: RP v3 (IMX708)`** or **`Camera: HM0360`** — the quickest confirmation of which image is running |
| First boot of a new image | `Slot A labelled variant 2` / `Slot B labelled variant 1` (once, never again) |
| Any time | `slots` → `Active slot 0 running 'RP3 (day/colour)'. Slot A: '…', Slot B: '…'. Auto-switch: on/off`; also `ver`, `camera`, `dump-sel` (raw selector-sector hex) |
| After `switchslot` | `Switched to slot N ('…'). Reset scheduled.` → at next sleep `>>> Reset by watchdog` → the other image's banner |

From the app, the same commands with the `AI ` prefix: `AI slots`, `AI switchslot`,
`AI camera`, `AI firmware <file> [0xCRC]`, `AI reset` (`_Documentation/ble_commands.md`).

## 7. Bench validation checklist

The sequence below exercises every path; it was run end-to-end on a WW500 C02 on
6 Aug 2026 — serial-log evidence in the review thread
(`development reports/2026-08-06_pr141-camera-features-review/bench_validation_evidence.md`).

1. Flash both slots (§3 two-pass, or two XMODEM burns) and confirm each image's
   first-boot label line.
2. `slots` with one never-booted slot → that slot reads `'unknown'`.
3. `switchslot` into an unlabelled-but-programmed slot → works, reports honestly,
   deferred watchdog reset at sleep.
4. `switchslot` with an empty slot → refused: `Slot switch failed (-2): other slot has no image`.
5. `setop 26 1`, dark scene (cover both lenses — the HM0360 is the light sensor) →
   AE check decides DARK; if the other slot is unlabelled expect the guard:
   `…but other slot holds 'unknown' - staying`.
6. Bright scene (lamp) on the night image → `Auto camera switch: light is BRIGHT -> …
   Reset scheduled.` + BLE announcement before the reboot.
7. Same light again after the switch → decision holds, **no** switch line (hysteresis /
   persisted op25 — no flip-flop).
8. Opposite light → the return switch. Restore `setop 26 0` (and any test op-params)
   when done.
