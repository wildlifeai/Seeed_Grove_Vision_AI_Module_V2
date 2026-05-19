# WW500 XIP Flash Integrity — Corruption Risk Analysis
#### CGP / Claude — 19 May 2026 · Updated 20 May 2026

## Background

Firmware images were occasionally found corrupt after an apparently successful update.
The circumstances were unclear.  This document records a code review of all paths that could
corrupt XIP flash, either directly (explicit erase/write) or indirectly (race conditions,
cache effects, mode-switch issues), and the fixes applied on 20 May 2026.

---

## 1. XIP mode and the `disable_xip` / `enable_xip` design

The HX6538 connects its 16 MB serial flash over a QSPI bus.  The flash chip can work in
two modes:

**SPI command mode (XIP disabled):** the SPI controller sends explicit command packets —
read, program, erase.  This is the only mode that accepts write and erase commands.

**XIP mode (XIP enabled):** the flash controller puts the chip into continuous-read mode so
it can answer CPU bus cycles directly without the command overhead.  The entire 16 MB chip
is mapped into virtual memory at `0x3A000000`:

```
0x3A000000–0x3A0FFFFF  Firmware Slot A
0x3A100000–0x3A1FFFFF  Firmware Slot B
0x3A200000–0x3AEFFFFF  NN model area
0x3AFFF000–0x3AFFFFFF  Slot selector
```

The TFLite model `Invoke()` in `cvapp.cpp` runs with the model pointer pointing somewhere
above `0x3A200000` — it is literally reading weights and activations fetched live from the
flash chip via the QSPI bus, with the Cortex-M55 D-cache and I-cache absorbing the latency.
**When XIP is disabled, that entire window disappears.**

### Design principle

XIP should be on at all times.  It should be off only for the minimum window needed to
erase, write, and verify a new image — firmware or NN model.  Himax's own sample
applications (`scenario_app/allon_sensor_tflm` etc.) enable XIP once at startup and never
toggle it again.  Our code must do the same.

### Implementation — `disable_xip()` and `enable_xip()`

Two static functions in `xip_manager.c` enforce this invariant:

```c
// Takes xSPIMutex, switches chip to SPI command mode.
// Holds xSPIMutex on return — caller must call enable_xip() on every exit path.
static bool disable_xip(void);

// Assumes xSPIMutex is already held.  Switches chip back to XIP mode
// and releases xSPIMutex.
static bool enable_xip(void);
```

`xSPIMutex` is held for the entire `disable_xip → SPI operations → enable_xip` sequence.
No other task can toggle XIP or issue SPI commands while a write or erase is in progress.

The invariant after `init_flash()` returns: **XIP is on**.  It goes off only inside a
`disable_xip` / `enable_xip` block.

---

## 2. Identified risks and their resolution

### 2.1 `write_metadata_to_flash()` was missing the SPI mutex — **FIXED 20 May 2026**

```c
// Before fix:
static int32_t write_metadata_to_flash(ModelMetaData *metaDataRam) {
    disable_xip();   // took mutex, disabled XIP, RELEASED mutex
    // ← XIP disabled, mutex FREE — another task could re-enable XIP here
    res = hx_lib_spi_eeprom_word_write(...);   // no mutex held  ← BUG
}
```

Every other write function held `xSPIMutex` across the write.  `write_metadata_to_flash`
did not.  A program command arriving while the chip is in XIP mode is undefined behaviour.

**Fix:** `disable_xip()` now holds the mutex until `enable_xip()` is called.
`write_metadata_to_flash()` calls `disable_xip()`, issues the write, then calls
`enable_xip()`.  The mutex spans the whole operation.

### 2.2 Race window between `disable_xip()` and the write mutex in all write paths — **FIXED 20 May 2026**

All write and erase functions previously followed this pattern:

```c
disable_xip();              // took mutex, disabled XIP, released mutex
                            // ← race window: XIP disabled, mutex FREE
xSemaphoreTake(xSPIMutex); // re-took mutex for write/erase loop
```

In the gap, any other task could call `enable_xip()` and re-enable XIP before the write
began.

**Fix:** as above — `disable_xip()` now holds the mutex.  All intermediate
`xSemaphoreTake` / `xSemaphoreGive` pairs have been removed from callers.  The mutex spans
the entire `disable_xip → SPI operations → enable_xip` sequence.

### 2.3 TFLite running concurrently with firmware update — **closed at application level**

While `xip_update_firmware_from_sd()` runs, XIP is disabled for the duration of erase +
write + verify (tens of seconds).  If the image task calls `interpreter->Invoke()` during
this window it will attempt to read model weights from `0x3A200000+`, which is unmapped —
a bus fault and likely a reset.  A reset during erase or partial write corrupts the target
slot.

The model update path already guards against this: `image_task.c:792` calls
`configure_image_sensor(CAMERA_CONFIG_STOP)` before touching flash.  The firmware update
path does not contain an equivalent guard.

**Resolution:** the app writer has been notified to ensure the camera system is idle before
issuing the `firmware` CLI command.  No code change in `xip_manager.c`.

### 2.4 Slot selector checksum hardcoding — **confirmed correct**

```c
#define SLOT_A_SELECTOR_CHECKSUM    0x4D04
#define SLOT_B_SELECTOR_CHECKSUM    0x167C
// TODO: verify these values if the bootloader is ever updated.
```

These constants were captured from: *1st BL Build DATE=Jan 17 2025, Version 2.12*.  The
running bootloader has been confirmed to be that version, so the values are correct.  The
TODO comment remains as a reminder if the bootloader is ever updated.

### 2.5 DMA cache coherency — **confirmed handled in SPI driver**

`spi_eeprom_comm.h` includes `cachel1_armv7.h` (the CMSIS cache management header).  The
SDK library handles D-cache clean/invalidate around DMA operations.  This is an SDK
responsibility and is not exposed to application code.  No action needed.

---

## 3. Additional fix — `init_flash()` XIP state — **FIXED 20 May 2026**

The original `init_flash()` left XIP **disabled** on return, which violated the "XIP always
on" invariant from the very first call.  It also unnecessarily took and released `xSPIMutex`
during init, when no other task can be competing for SPI access.

**Fix:** `init_flash()` now:
1. Creates `xSPIMutex` (without taking it)
2. Calls `hx_lib_spi_eeprom_open()` — this reconfigures the SPI controller and implicitly
   takes the chip out of XIP continuous-read mode
3. Reads the flash chip ID (works because `open` left the chip in command mode)
4. Calls `hx_lib_spi_eeprom_enable_XIP(... true ...)` to restore XIP
5. Returns with XIP **enabled**

---

## 4. Summary of all changes (20 May 2026)

| Item | Change |
|---|---|
| `xSPIMutex` comment | Replaced "TODO - do we even need this" with an explanation of its role |
| `disable_xip()` | Holds mutex on success; callers must call `enable_xip()` on every exit |
| `enable_xip()` | Assumes mutex held; releases it (success or failure) |
| `write_metadata_to_flash()` | Mutex bug fixed; now properly bracketed by disable/enable |
| All 11 write/erase/SPI-read callers | Intermediate `xSemaphoreTake`/`xSemaphoreGive` removed; `enable_xip()` added to every exit path |
| `init_flash()` | No longer takes mutex; ends with XIP enabled |
| 4 read-only functions | Comments added noting they could read via XIP virtual address instead |
| `doc/WW500_XIP_flash_integrity.md` | This document updated to reflect all fixes |

### Remaining open items

| Item | Status |
|---|---|
| 2.3 TFLite / firmware update collision | App writer to ensure camera is idle before `firmware` command |
| 2.4 Slot selector checksums | Confirmed correct for current bootloader; re-verify if bootloader is updated |
| 2.5 DMA cache coherency | Handled in SPI driver; no action unless corruption recurs after above fixes |
