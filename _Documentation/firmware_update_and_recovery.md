# WW500 Firmware Update & Recovery
#### 8 July 2026

How the Himax (HX6538) firmware on the WW500 is updated, and how to recover a
device that no longer boots. (The BLE processor has its own path — Nordic DFU
from the mobile app — not covered here.)

## The dual-image model, in brief

The WW500 holds **two firmware images** in A/B flash slots — one per camera
(RP3 day/colour, HM0360 night/IR) — selected at boot by the slot selector
sector. `slots` reports the layout, `switchslot` changes camera. Full
architecture and flash layout: [slot_selector.md](../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/slot_selector.md).

Website-generated images are named `VYMDDHMM.IMG` (V = `R`/`H` variant letter,
rest = build time) and ship in the Setup Folder's MANIFEST directory — see
[MANIFEST/README.TXT](../EPII_CM55M_APP_S/app/ww_projects/ww500_md/MANIFEST/README.TXT).

## Update paths

| Path | When | How |
|---|---|---|
| **Mobile app — "Update both cameras"** | normal field/bench updates | Copy the Setup Folder's MANIFEST to the SD card → app → Firmware Update (AI processor) → one tap flashes both images in two passes and finishes on the camera in use |
| **Console** `firmware <file> [0xCRC]` | engineer/manual | Writes `/MANIFEST/<file>` to the **inactive** slot, verifies, updates the selector; `reset` boots it. With `0xCRC` (CRC16-CCITT) the file is checked **before** flash is touched |
| **Bootloader X-Modem** | recovery, factory, first update of old devices | See runbook below |
| **SWD** | development | See [bootloader.md](bootloader.md) |

Each app/console update writes the *inactive* slot: a failed write leaves the
running image untouched (the selector is only updated after a full verify).

## Safety rules

- ⚠️ **Devices built before 14 Jun 2026** have a defect in the on-device
  flasher (stale descriptor CRC → unbootable slot). Their **first** update must
  go via the bootloader X-Modem path; after that the fixed flasher is on board.
- Update with **good batteries or external power** — a brown-out mid-flash can
  strand a slot (recoverable, but avoidably).
- Always install the **pair from the same build** (the app's one-tap flow and
  the website Setup Folder guarantee this).

## Recovery runbook — device boots to the bootloader menu

**Symptom:** device does not respond to `AI ver`; on power-up the console shows
`[0] Reboot system / [1] Xmodem download and burn FW image / ...` instead of the
`**** WW500 MD ****` banner. The active slot failed secure-boot verification.

1. **Find the console port.** The bench setup exposes two "USB Serial Port"
   adapters: the Himax console prints **clean text at 921600 baud**; the other
   (BLE debug UART) prints garbage at that baud. Power-cycle while listening —
   the port showing `1st BL Modem Build ...` is the one.
2. **Burn a known-good image** (from the website Setup Folder / MANIFEST copy):

   ```
   cd xmodem
   python xmodem_send.py --port COM13 --baudrate 921600 --file <path>\R6707N35.IMG
   ```

   then power-cycle when the script says `Please press reset button!!`. The
   bootloader burns the image into the slot it was trying to boot and restarts.
3. **Verify:** the boot banner shows the image's build time and camera; `ver`
   and `slots` confirm. Repeat with the other variant's image if the second
   slot also needs restoring (the bootloader alternates to the backup slot).
4. **Labels:** X-Modem burns reset the per-slot camera labels — each slot
   re-labels itself the first time it boots, so `'unknown'` after recovery is
   normal and self-heals on the next `switchslot`.

## Console quick reference

`ver` (build + camera) · `slots` · `switchslot` · `firmware <file> [0xCRC]` ·
`crc <file>` · `dump-sel` (selector sector hex) — full list: [ble_commands.md](ble_commands.md).
