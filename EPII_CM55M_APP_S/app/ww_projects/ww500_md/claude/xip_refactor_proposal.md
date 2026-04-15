# XIP Refactoring Proposal
#### Claude analysis — 16 April 2026

## Executive summary

The XIP flash management code in `cvapp.cpp` can be cleanly separated into
`xip_manager.c` + `xip_manager.h`.  One complication needs a decision: two
functions (`xip_load_model_from_sd` and `xip_load_model_from_flash`) return a
`const tflite::Model *`, which is a C++ type and cannot live in a `.c` file.
**The recommended solution** is to leave those two thin C++ wrapper functions in
`cvapp.cpp` and move everything else to `xip_manager.c`.  This keeps the new
file pure C, which is exactly what future firmware-update code will need.

There is also a secondary finding: two functions that do almost the same job
(`xip_load_model_from_sd_to_flash` and `xip_copy_model_from_sd_to_flash`)
currently coexist.  This is a Phase 2 clean-up item.

---

## Analysis of the existing code

### Source of XIP code

All XIP code lives in `cvapp.cpp`.  No `xip_*` functions are declared in any
header; their forward declarations are all at the top of `cvapp.cpp`.

### Flash constants — currently in `cvapp.h`

These belong in `xip_manager.h`:

| Macro | Value | Comment |
|---|---|---|
| `FLASH_START_SAFE_ADDR` | `0x00200000` | Physical flash start after 2 MB firmware area |
| `FLASH_MODEL_AREA_SIZE` | `14 * 1024 * 1024` | Space available for NN models |
| `FLASH_SECTOR_SIZE` | `4096` | Flash sector size |
| `MODEL_FLASH_ADDR` | `FLASH_START_SAFE_ADDR` | Physical base for model writes |
| `FLASH_VIRTUAL_BASE` | `0x3A000000` | CPU XIP window base |
| `FLASH_PHYSICAL_BASE` | `0x00000000` | SPI EEPROM physical base |
| `MODEL_XIP_ADDR` | `0x3A200000` | Virtual address for model metadata start |
| `MODEL_XIP_INFO_SIZE` | `16` | Old-style filename header size |
| `MAX_CLASSES` | `16` | Maximum number of NN output classes |
| `MAX_LABEL_LEN` | `20` | Maximum bytes per class label |
| `LABEL_MAGIC` | `0x4C41424C` | Magic word for metadata validation |
| `MAX_MODEL_NAME_LEN` | `13` | 8.3 format filename + NUL |

`PROJECT_ID`, `PROJECT_VER`, `MODEL_THRESHOLD`, `ClassConfidenceData`, and the
`cv_*` function declarations stay in `cvapp.h`.

### Type definitions — currently in `cvapp.cpp`

**`ModelMetaData`** (the struct written to flash before the model) must move to
`xip_manager.h` since `cvapp.cpp` also needs it (for the `metaDataFlash`
pointer).

**`metaDataFlash`** (the macro `((const ModelMetaData *)MODEL_XIP_ADDR)`) also
moves to `xip_manager.h` since it only depends on `ModelMetaData` and
`MODEL_XIP_ADDR`.

### State variables — move to `xip_manager.c` as `static`

| Variable | Type | Purpose |
|---|---|---|
| `spi_inst` | `USE_DW_SPI_MST_E` | SPI peripheral instance selector |
| `flash_initialized` | `bool` | One-time init guard |
| `xSPIMutex` | `SemaphoreHandle_t` | FreeRTOS mutex for SPI bus |

### Functions to move to `xip_manager.c`

All of these are pure C (no TFLite types anywhere in their signatures or bodies):

**Internal helpers (keep `static`):**

| Function | Notes |
|---|---|
| `xip_align_up()` | Arithmetic helper; keep `static inline` |
| `xip_virt_to_phys()` | Address translation; keep `static inline` |
| `xip_phys_to_virt()` | Address translation; keep `static inline` |
| `file_exists()` | FATFS helper; keep `static`; name has no `xip_` prefix but is internal |
| `load_labels_from_manifest()` | Metadata helper; keep `static` |

**Public API (declare in `xip_manager.h`):**

| Function | Signature | Purpose |
|---|---|---|
| `xip_init_flash` | `int (void)` | Open SPI EEPROM, create mutex |
| `xip_enable_XIP` | `bool (bool enable)` | Enable/disable XIP memory-map mode |
| `xip_erase_model_flash_area` | `int (uint32_t size)` | Sector-erase area for model + metadata |
| `xip_is_model_in_flash` | `bool (char *filename, bool cold_boot)` | Check named model is in flash (**parameter change — see below**) |
| `xip_is_file_in_sd` | `bool (char *filename)` | Check named file exists on SD card |
| `xip_valid_model_in_flash` | `bool (void)` | Check TFLite magic bytes are present |
| `xip_write_meta_data_to_flash` | `int32_t (ModelMetaData *)` | Write metadata struct via SPI |
| `xip_copy_model_from_sd_to_flash` | `bool (char *filename)` | Copy model data from SD to flash |
| `xip_copy_metadata_to_flash` | `bool (char *modelName)` | Build and write metadata struct |
| `xip_load_model_from_sd_to_flash` | `int (char *filename)` | Older copy path — see note below |
| `xip_get_model_xip_address` | `uint32_t (void)` | **New helper** — returns virtual address of model in XIP (see below) |

### Functions that remain in `cvapp.cpp`

These use `const tflite::Model *` and must stay in C++ context:

| Function | Why it stays |
|---|---|
| `xip_load_model_from_flash` | Returns `const tflite::Model *`; calls `tflite::GetModel()` |
| `xip_load_model_from_sd` | Returns `const tflite::Model *`; calls `xip_load_model_from_flash()` |

After the refactor, `xip_load_model_from_flash` becomes a two-liner:
```cpp
static const tflite::Model *xip_load_model_from_flash(void) {
    uint32_t addr = xip_get_model_xip_address();   // C call into xip_manager
    return addr ? tflite::GetModel((const void *)addr) : nullptr;
}
```
And `xip_load_model_from_sd` calls `xip_copy_model_from_sd_to_flash()` +
`xip_copy_metadata_to_flash()` then delegates to `xip_load_model_from_flash()`.

---

## Key complication: `coldBoot` crossing the boundary

`xip_is_model_in_flash()` currently reads the static variable `coldBoot` (defined
in `cvapp.cpp`) to decide whether to print verbose debug output.  Once the function
moves to `xip_manager.c`, it cannot see that variable.

**Recommended fix:** Add a `bool cold_boot` parameter to `xip_is_model_in_flash()`:

```c
bool xip_is_model_in_flash(char *filename, bool cold_boot);
```

`cvapp.cpp` already has `coldBoot` and simply passes it through.

---

## Secondary finding: duplicate model-copy functions

Two functions do almost the same job:

| Function | What it does differently |
|---|---|
| `xip_copy_model_from_sd_to_flash()` (line 1398) | **Current path.** Writes model at `MODEL_XIP_ADDR + align_up(sizeof(ModelMetaData), 16)`. Separate metadata written by `xip_copy_metadata_to_flash()`. |
| `xip_load_model_from_sd_to_flash()` (line 893) | **Older path.** Writes a 16-byte filename header immediately before `MODEL_XIP_ADDR`, then writes model at `MODEL_XIP_ADDR`. Does not use `ModelMetaData`. |

`xip_load_model_from_sd_to_flash` is declared non-static and in the forward
declarations, suggesting it was once called from outside `cvapp.cpp`, but grep
confirms it is **not currently called from any other file**.  It is dead code in
the current call graph (nothing in `cvapp.cpp` calls it either — the actual flow
goes through `xip_load_model_from_sd` → `xip_copy_model_from_sd_to_flash`).

**Phase 1 recommendation:** Move it to `xip_manager.c` unchanged (dead code is
harmless and the move is the safest first step).  
**Phase 2 recommendation:** Delete it, or confirm whether it serves a CLI
testing purpose and keep it only if needed.

---

## Build system impact

The build system uses `get_csrcs`/`get_cxxsrcs` to collect all `.c`/`.cpp` files
from the `ww500_md` directory automatically.  A new `xip_manager.c` in that
directory will be picked up with **no makefile change needed**.

---

## Proposed file structure

### `xip_manager.h`

```
/*************************************** Includes *******************************************/
(FreeRTOS, FATFS, spi_eeprom_comm.h, hx_drv_spi.h — whatever is needed for types in the API)

/*************************************** Definitions *******************************************/
FLASH_START_SAFE_ADDR, FLASH_MODEL_AREA_SIZE, FLASH_SECTOR_SIZE ...
MODEL_XIP_ADDR, MAX_CLASSES, MAX_LABEL_LEN, LABEL_MAGIC, MAX_MODEL_NAME_LEN

/*************************************** Type definitions *************************************/
ModelMetaData struct
metaDataFlash macro

/*************************************** Public API declarations ******************************/
(extern "C" block for C++ compatibility)
xip_init_flash()
xip_enable_XIP()
xip_erase_model_flash_area()
xip_is_model_in_flash()
xip_is_file_in_sd()
xip_valid_model_in_flash()
xip_get_model_xip_address()
xip_write_meta_data_to_flash()
xip_copy_model_from_sd_to_flash()
xip_copy_metadata_to_flash()
xip_load_model_from_sd_to_flash()   ← Phase 2: candidate for removal
```

### `xip_manager.c`

```
/*************************************** Includes *******************************************/

/*************************************** Definitions *******************************************/
FILE_CHUNK_SIZE, MODEL_OLD_META_SIZE

/*************************************** Global variables *************************************/
(none — all state is static)

/*************************************** Local variables *************************************/
static USE_DW_SPI_MST_E spi_inst
static bool flash_initialized
static SemaphoreHandle_t xSPIMutex

/*************************************** Local Function Declarations **************************/
static inline xip_align_up()
static inline xip_virt_to_phys()
static inline xip_phys_to_virt()
static bool file_exists()
static uint8_t load_labels_from_manifest()

/*************************************** Local Function Definitions ***************************/
(implementations of the above)

/*************************************** Global Function Definitions **************************/
xip_init_flash()
xip_enable_XIP()
xip_erase_model_flash_area()
xip_is_model_in_flash()
xip_is_file_in_sd()
xip_valid_model_in_flash()
xip_get_model_xip_address()        ← new function
xip_write_meta_data_to_flash()
xip_copy_model_from_sd_to_flash()
xip_copy_metadata_to_flash()
xip_load_model_from_sd_to_flash()
```

### `cvapp.cpp` after refactor

Additions:
```cpp
#include "xip_manager.h"
```

Removals:
- All moved `xip_*` functions
- Moved type definitions and macros
- Moved static variables

Retained XIP functions (now thin wrappers):
- `xip_load_model_from_flash()` — calls `xip_get_model_xip_address()` then `tflite::GetModel()`
- `xip_load_model_from_sd()` — calls `xip_copy_model_from_sd_to_flash()` + `xip_copy_metadata_to_flash()` then `xip_load_model_from_flash()`

`cv_init()` change:
```cpp
// Before:
if (xip_is_model_in_flash(filename)) {
// After:
if (xip_is_model_in_flash(filename, coldBoot)) {
```

`cv_eraseModel()` and `cv_newModel()` — no change needed; they call `xip_erase_model_flash_area()` which now lives in `xip_manager.c` and is declared in `xip_manager.h`.

### `cvapp.h` after refactor

Remove: all flash constants and `ModelMetaData`/`metaDataFlash` (now in `xip_manager.h`)  
Add: `#include "xip_manager.h"` (so callers of `cvapp.h` still get access to the types)

---

## Phase 2 items (after Phase 1 is tested)

1. **Remove or retain `xip_load_model_from_sd_to_flash`** — confirm it is dead code and delete it, simplifying the file.

2. **Rationalize mutex handling** — `xip_enable_XIP()` takes the mutex internally but many callers also take it around surrounding operations; this interleaving is fragile.  A cleaner design would have an internal `_xip_enable_XIP_locked()` (no mutex) called while already holding the lock, and the public `xip_enable_XIP()` wrapping it with the mutex.

3. **Add stubs for firmware slot management** — at minimum declare:
   ```c
   int xip_erase_firmware_slot(uint8_t slot);           // slot 0=A, 1=B
   int xip_write_firmware_slot(uint8_t slot, ...);
   int xip_set_active_slot(uint8_t slot);
   ```
   with `/* Not yet implemented */` bodies, once the memory map for the selector sector is known.

4. **File-level documentation** — add a block comment to `xip_manager.c` describing the XIP memory layout (same information as in `CLAUDE_refactor_xip.md`).

5. **Remove `#ifdef SPIDEBUG` debug code** from `cvapp.cpp` — it is already guarded but takes up 160 lines; it could move to `xip_manager.c` or be removed.

---

## Questions before proceeding

1. Should `xip_load_model_from_sd_to_flash` be preserved in `xip_manager.c` or deleted outright in Phase 1?  (Recommendation: move it as-is in Phase 1, decide in Phase 2.)

2. Should `xip_get_model_xip_address()` be added as a new function (to let `cvapp.cpp` get the computed virtual address without calling back into TFLite), or is it acceptable for `xip_load_model_from_flash` to duplicate the address arithmetic?

3. Confirm the memory map values: `MODEL_XIP_ADDR = 0x3A200000` means the model metadata starts at physical `0x00200000`.  The alignment comment says model starts at `0x00200000 + align_up(sizeof(ModelMetaData), 16)`.  Is `sizeof(ModelMetaData)` known to be stable (i.e., not changing in Phase 2)?
