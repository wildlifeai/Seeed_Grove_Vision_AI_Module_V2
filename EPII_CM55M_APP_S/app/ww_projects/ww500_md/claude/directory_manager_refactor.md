# `directory_manager.c` — Refactoring Analysis
#### CGP / Claude, 1 April 2026

---

## 1. Summary of what the code currently does

`directory_manager.c` has three concerns:

| Concern | Where |
|---|---|
| **Config directory** — ensure `/MANIFEST` exists, set `dirManager.current_config_dir` | `dir_mgr_init_directories()` |
| **Image directory** — decide which `/IMAGES.NNN` folder to use, create it, set `dirManager.current_capture_dir` | `dir_mgr_generateImageDirName()` + `dir_mgr_createImageDir()` |
| **Image filenames** — generate 8.3 hex names based on RTC timestamp | `dir_mgr_generateImageFilename()` |

Only concern 3 is self-contained. Concerns 1 and 2 are entangled in `dir_mgr_init_directories()`, and concern 2 requires data that is not available when `dir_mgr_init_directories()` is first called.

---

## 2. Identified problems

### 2.1 Double-call to set up the image directory

`dir_mgr_init_directories()` is called early from `vFatFsTask()` **before** `CONFIG.TXT` is read.
`dir_mgr_generateImageDirName()` queries `OP_PARAMETER_IMAGES_COUNT` and `OP_PARAMETER_IMAGES_FILE_INDEX`
from `op_parameter[]`, but those values are only populated by `load_configuration()`.

The current workaround is to call both functions **again** after `load_configuration()`:

```c
// In vFatFsTask() — fatfs_task.c lines ~1359–1364
// TODO clean this up! We have to call dir_mgr_generateImageDirName()
// a second time now that the operational parameters have been read
char path_buf[IMAGEFILENAMELEN];
dir_mgr_generateImageDirName(path_buf, IMAGEFILENAMELEN);
dir_mgr_createImageDir(path_buf);
```

The first call (inside `dir_mgr_init_directories()`) creates a directory with the wrong (default/zero)
index, which may leave a spurious `/IMAGES.000` on the SD card.

### 2.2 Dead code — `#ifdef UNZIPMANIFEST`

The large `#ifdef UNZIPMANIFEST` block in `dir_mgr_init_directories()` (lines ~197–291) is never
compiled. This includes `find_manifest_zip_path()`, `ensure_directory_exists()`, and
`ensure_default_config_file_exists()`. According to `claude.md`, this feature "is unlikely ever to
be used."

### 2.3 Dead code — commented-out functions

Large blocks of commented-out code clutter the file:
- `dir_mgr_add_capture_folder()` (~lines 355–385)
- `dir_mgr_delete_capture_folder()` (~lines 389–407)
- The original `init_directories()` (~lines 332–342)
- The `#if 0` block inside `dir_mgr_init_directories()` (~lines 297–317) — this is the old version
  before the "TODO - temporary" workaround was added.

### 2.4 Stale `extern` declarations in `fatfs_task.c`

`fatfs_task.c` (lines 138–139) still declares:
```c
extern FRESULT init_directories(directoryManager_t *dirManager);
extern FRESULT add_capture_folder(directoryManager_t *dirManager);
```
Neither function exists anywhere. These are old names superseded by `dir_mgr_init_directories()`
and `dir_mgr_add_capture_folder()`. They will cause a link error if ever called; they currently
compile only because they are never called.

### 2.5 `dir_mgr_createImageDir()` bypasses its parameter

`dir_mgr_createImageDir(char *path_buf)` takes no pointer to the manager, yet accesses the
**global** `dirManager` directly:
```c
dirManager.imagesRes = res;
strcpy(dirManager.current_capture_dir, path_buf);
```
This is inconsistent with the rest of the API, which passes `directoryManager_t *dirManager`.
It also makes the function impossible to unit-test in isolation.

### 2.6 `imagesDirIdx` and `base_dir` are unused (outside the UNZIP block)

- `dirManager.imagesDirIdx` is set to `1` in `dir_mgr_init_directories()` and never read again.
  The actual image folder index is tracked in `op_parameter[OP_PARAMETER_IMAGES_FILE_INDEX]`.
- `dirManager.base_dir` is only used inside the `#ifdef UNZIPMANIFEST` block. Outside that block
  it is set but never read.

### 2.7 `IMAGEFILENAMELEN` (13) is too small for path buffers

`IMAGEFILENAMELEN` is defined as `13` in `image_task.h` — sized for an 8.3 filename with null
terminator. It is reused as the size of `current_config_dir`, `current_capture_dir`, and
`base_dir` in the struct, and as the buffer length passed to `dir_mgr_generateImageDirName()`.

`/IMAGES.000` is 11 characters — it just fits. `/MANIFEST` is 9 characters. But this is
fragile: any small change to directory naming would overflow these 13-byte buffers silently
(stack or struct corruption). A dedicated `DIRNAMELEN` or `PATHLEN` constant (e.g. 32 or 64)
would be safer.

### 2.8 `MAXIMAGESPERDIRECTORY` is set to `4` for testing

Line 32: `#define MAXIMAGESPERDIRECTORY 4` — the production value (100) is commented out. Easy to
miss when moving from test to production.

---

## 3. Proposed refactoring options

### Option A — Minimum viable cleanup (low risk)

Remove dead code only, fix the stale externs, and move the second image-dir call into a new
function. No structural changes to the existing working logic.

**Changes:**

1. **Delete** everything inside `#ifdef UNZIPMANIFEST … #endif` in `directory_manager.c`
   (the three static functions and the large `#ifdef` block in `dir_mgr_init_directories()`).
   Keep only the `#else` branch.

2. **Delete** the commented-out `dir_mgr_add_capture_folder()`, `dir_mgr_delete_capture_folder()`,
   and the old `init_directories()` stub.

3. **Delete** the `#if 0 … #else … #endif` inside `dir_mgr_init_directories()` — keep only the
   `#else` body (the current "TODO - temporary" calls). Remove the TODO comment since it will no
   longer be temporary after refactoring.

4. **Delete** the stale `extern` declarations in `fatfs_task.c` (lines 138–139).

5. **Fix** `dir_mgr_createImageDir()` — add `directoryManager_t *dirManager` as a parameter and
   remove the direct access to the global. Update all callers.

6. **Add** `#define DIRNAMELEN 32` (or similar) in `directory_manager.h` and use it for the
   three path buffers in `directoryManager_t`. Keep `IMAGEFILENAMELEN` for actual filenames only.

**Result:** Same runtime behaviour; much less noise in the file.

---

### Option B — Split `dir_mgr_init_directories()` into two phases (recommended)

This resolves the root cause of the double-call pattern.

**Rename / split the init function:**

```c
// Phase 1 — called early, before CONFIG.TXT is loaded
// Sets up config directory only.
FRESULT dir_mgr_init_config(directoryManager_t *dirManager);

// Phase 2 — called after load_configuration(), once op_parameter[] is valid
// Determines and creates the correct image directory.
FRESULT dir_mgr_init_image_dir(directoryManager_t *dirManager);
```

**Changes to `vFatFsTask()` in `fatfs_task.c`:**

```c
res = dir_mgr_init_config(&dirManager);        // Phase 1
if (res == FR_OK) {
    res = load_configuration(STATE_FILE, &dirManager);
    // ...
    res = dir_mgr_init_image_dir(&dirManager); // Phase 2 — now in the right place
}
```

This eliminates the double-call and its spurious directory creation. The TODO comments disappear.

**Additional changes:** same as Option A (dead code removal, stale externs, parameter fix,
`DIRNAMELEN`).

---

### Option C — Simplify `directoryManager_t` (optional, more invasive)

After Option A or B, consider removing unused struct members:

| Field | Status | Proposed action |
|---|---|---|
| `FIL configFile` | Used in `load/save_configuration` | Keep |
| `FIL imagesFile` | Used in `fileWriteImage` | Keep |
| `FRESULT configRes` | Used in `load/save_configuration` | Keep |
| `FRESULT imagesRes` | Used in `fileWriteImage`, `dir_mgr_createImageDir` | Keep |
| `bool configOpen` | Used in `load/save_configuration` | Keep |
| `bool imagesOpen` | Used in `fileWriteImage` | Keep |
| `int imagesDirIdx` | Set to 1, never read outside init | **Remove** — state is in `op_parameter[]` |
| `char base_dir[]` | Only used inside dead `UNZIPMANIFEST` block | **Remove** after block is deleted |
| `char current_config_dir[]` | Used in `load/save_configuration` | Keep |
| `char current_capture_dir[]` | Used in `fileWriteImage`, CLI | Keep |

Removing `imagesDirIdx` and `base_dir` reduces the struct size and removes the confusion about
which variable tracks the "current index."

---

## 4. Recommendation

**Proceed with Option B + Option C.**

- Option B is the cleanest solution. The two-phase split directly mirrors the actual boot
  sequence (SD init → config dir → load config → image dir) and eliminates the TODO workaround.
- Option C removes genuinely unused fields and reduces future confusion.
- Both changes are confined to `directory_manager.c`, `directory_manager.h`, and the init
  section of `fatfs_task.c` — well-isolated, low risk.

The `DIRNAMELEN` / `IMAGEFILENAMELEN` fix and `MAXIMAGESPERDIRECTORY` restore to 100 should also
be done at the same time.

---

## 5. Files affected

| File | Changes needed |
|---|---|
| `directory_manager.c` | Remove dead code; split init into two functions; fix `dir_mgr_createImageDir` parameter |
| `directory_manager.h` | Add `DIRNAMELEN`; update struct; update function declarations |
| `fatfs_task.c` | Remove stale externs; update init call sequence |

---

## 6. Running the compiler to verify changes

The build uses `arm-none-eabi-gcc` (Arm GNU Toolchain 14.3 rel1) and `make`, invoked from
`EPII_CM55M_APP_S/makefile`. The session runs in WSL2 on Windows.

Searching the WSL environment found that neither `make` nor `arm-none-eabi-gcc` are on the
WSL PATH. The Windows toolchain is at:
`C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.3 rel1\bin`

Two options to enable compiler checks from within this Claude Code session:

**Option 1 — Install native Linux tools in WSL (recommended)**
```bash
sudo apt install make gcc-arm-none-eabi
```
This gives a self-contained Linux build; GCC version will differ from the Eclipse build but
is sufficient to catch compilation errors. Run with:
```
! sudo apt install make gcc-arm-none-eabi
```

**Option 2 — Call the existing Windows binaries from WSL**
Add the Windows paths to `~/.bashrc`:
```bash
export PATH="$PATH:/mnt/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.3 rel1/bin"
# plus the path to make.exe (e.g. from STM32CubeIDE)
```
Then WSL can invoke the Windows `.exe` files directly. Path quoting with spaces can be
awkward; Option 1 is simpler.

*Decision: deferred — will return to this after the refactoring is complete.*
