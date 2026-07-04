# WW500 CRC Checking on SD Card Files
#### CGP / Claude — 19 May 2026

## Executive Summary

Two changes are needed, both in `CLI-FATFS-commands.c`:

1. A new `crc <filename>` CLI command that reports the CRC16-CCITT and size of any file in
   the current directory.
2. An optional second parameter on the existing `firmware` command: if `firmware <name> 0xNNNN`
   is used, the file's CRC is checked before any flash operations begin; a mismatch aborts
   with an error and leaves flash untouched.

Both changes use the existing `crc16_ccitt_stream_*` API from `crc16_ccitt.h`/`.c` — no new
CRC algorithm is needed.

---

## 1. CRC algorithm

The project already contains a CRC16-CCITT implementation (`crc16_ccitt.c`) that is shared
with the nRF52832 companion processor.  The algorithm uses initial value `0xFFFF` and
augments the final state with two zero bytes.  It is already in use in `fileRx.c` for
validating files received over I2C.

The streaming API is used whenever data must be read in chunks (as it must be for files of
arbitrary size):

```c
uint16_t crc = crc16_ccitt_stream_init();
crc = crc16_ccitt_stream_update(buf, len, crc);   // call once per chunk
uint16_t final_crc = crc16_ccitt_stream_final(crc);
```

The same functions must be used on both the WW500 and the companion app to produce matching
CRC values.

---

## 2. Shared helper function

Both the new `crc` command and the modified `firmware` command need to read a file and
compute its CRC.  A single static helper in `CLI-FATFS-commands.c` serves both:

```c
#define CRC_READ_BUF_SIZE   512u

// Computes CRC16-CCITT of the file at 'path'.
// On success: writes crc and size, returns FR_OK.
// On failure: returns a non-zero FRESULT.
static FRESULT compute_file_crc(const char *path, uint16_t *crc_out, uint32_t *size_out)
{
    FIL     file;
    FRESULT res;
    uint8_t buf[CRC_READ_BUF_SIZE];
    UINT    bytes_read;
    uint32_t total = 0;
    uint16_t crc;

    res = f_open(&file, path, FA_READ);
    if (res != FR_OK) {
        return res;
    }

    crc = crc16_ccitt_stream_init();

    for (;;) {
        res = f_read(&file, buf, sizeof(buf), &bytes_read);
        if (res != FR_OK) {
            f_close(&file);
            return res;
        }
        if (bytes_read == 0) {
            break;
        }
        crc    = crc16_ccitt_stream_update(buf, (uint16_t)bytes_read, crc);
        total += bytes_read;
    }

    f_close(&file);

    *crc_out  = crc16_ccitt_stream_final(crc);
    *size_out = total;
    return FR_OK;
}
```

**Stack cost:** 512 bytes for `buf` plus a few words of locals.  This is within the budget
of the FatFS/CLI task stack.  A heap allocation is not needed and would be wasteful for a
short-lived read.

**Loop bound:** The loop terminates when `f_read` returns `bytes_read == 0` (end of file).
FatFS guarantees this on a well-formed file; the FatFS error return provides an explicit
upper bound on failure paths.

---

## 3. New `crc` command

### CLI definition

Add to the command table in `CLI-FATFS-commands.c`:

```c
static const CLI_Command_Definition_t xCrc = {
    "crc",
    "crc <filename>:\r\n Print CRC16-CCITT and size of <filename> in current directory\r\n",
    prvCrcCommand,
    1
};
```

Register in `vRegisterCLICommands()`:

```c
FreeRTOS_CLIRegisterCommand(&xCrc);
```

### Handler

```c
static BaseType_t prvCrcCommand(char *pcWriteBuffer, size_t xWriteBufferLen,
                                const char *pcCommandString)
{
    const char *pcParameter;
    BaseType_t  lParameterStringLength;
    char        filename[64];
    uint16_t    crc;
    uint32_t    size;
    FRESULT     res;

    memset(pcWriteBuffer, 0x00, xWriteBufferLen);

    pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
    if (pcParameter == NULL) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Usage: crc <filename>");
        return pdFALSE;
    }

    if (lParameterStringLength >= (BaseType_t)sizeof(filename)) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error: filename too long");
        return pdFALSE;
    }

    memcpy(filename, pcParameter, lParameterStringLength);
    filename[lParameterStringLength] = '\0';

    res = compute_file_crc(filename, &crc, &size);
    if (res != FR_OK) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen,
                   "Error: cannot open '%s' (%d)", filename, res);
        return pdFALSE;
    }

    cli_append(&pcWriteBuffer, &xWriteBufferLen,
               "CRC 0x%04X (%lu bytes)", (unsigned)crc, (unsigned long)size);
    return pdFALSE;
}
```

### Behaviour

| Input | Output |
|-------|--------|
| `crc CONFIG.TXT` (file exists) | `CRC 0x1234 (256 bytes)` |
| `crc MISSING.TXT` (no file) | `Error: cannot open 'MISSING.TXT' (4)` — FR_NO_FILE = 4 |
| `crc` (no parameter) | `Usage: crc <filename>` |

The filename is resolved against the current FatFS working directory, consistent with all
other single-filename commands (`type`, `read`, etc.).

---

## 4. Modified `firmware` command

### Change to CLI definition

The parameter count changes from `1` (exactly one) to `-1` (variable) so that the second
parameter is optional:

```c
static const CLI_Command_Definition_t xFirmware = {
    "firmware",
    "firmware <filename> [0xCRC]:\r\n"
    " Update firmware from /MANIFEST/<filename>.\r\n"
    " Optional 0xCRC: CRC16-CCITT of file must match before flash is touched.\r\n"
    " Type 'reset' to boot after update.\r\n",
    prvFirmwareCommand,
    -1              /* variable: 1 required, 1 optional */
};
```

### Updated handler

```c
static BaseType_t prvFirmwareCommand(char *pcWriteBuffer, size_t xWriteBufferLen,
                                     const char *pcCommandString)
{
    const char *pcParameter;
    BaseType_t  lParameterStringLength;
    char        filename[64];
    char        filepath[sizeof(CONFIG_DIR) + 64 + 1];
    int         result;

    memset(pcWriteBuffer, 0x00, xWriteBufferLen);

    /* Parameter 1: filename */
    pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
    if (pcParameter == NULL) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen,
                   "Usage: firmware <filename> [0xCRC]");
        return pdFALSE;
    }

    if (lParameterStringLength >= (BaseType_t)sizeof(filename)) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error: filename too long");
        return pdFALSE;
    }

    memcpy(filename, pcParameter, lParameterStringLength);
    filename[lParameterStringLength] = '\0';

    /* Parameter 2: optional CRC */
    pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 2, &lParameterStringLength);
    if (pcParameter != NULL) {
        /* "0x1234" is always exactly 6 characters */
        if (lParameterStringLength != 6) {
            cli_append(&pcWriteBuffer, &xWriteBufferLen,
                       "Error: CRC must be in format 0xNNNN");
            return pdFALSE;
        }
        char crc_str[7];
        memcpy(crc_str, pcParameter, 6);
        crc_str[6] = '\0';

        /* Parse hex value — strtoul with base 0 accepts "0x" prefix */
        char    *end;
        unsigned long parsed = strtoul(crc_str, &end, 0);
        if (end == crc_str || *end != '\0') {
            cli_append(&pcWriteBuffer, &xWriteBufferLen,
                       "Error: invalid CRC '%s' (expected 0xNNNN)", crc_str);
            return pdFALSE;
        }
        uint16_t expected_crc = (uint16_t)(parsed & 0xFFFFu);

        /* Compute CRC of the file before touching flash */
        snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, filename);
        uint16_t actual_crc;
        uint32_t file_size;
        FRESULT res = compute_file_crc(filepath, &actual_crc, &file_size);
        if (res != FR_OK) {
            cli_append(&pcWriteBuffer, &xWriteBufferLen,
                       "Error: cannot read '%s' for CRC check (%d)", filepath, res);
            return pdFALSE;
        }

        if (actual_crc != expected_crc) {
            cli_append(&pcWriteBuffer, &xWriteBufferLen,
                       "Error: CRC mismatch — file 0x%04X, expected 0x%04X. "
                       "Flash NOT modified.",
                       (unsigned)actual_crc, (unsigned)expected_crc);
            return pdFALSE;
        }
        /* CRC matched — fall through to update */
    }

    result = xip_update_firmware_from_sd(filename);
    if (result == 0) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen,
                   "Firmware update OK. Type 'reset' to boot the new image.");
    } else {
        cli_append(&pcWriteBuffer, &xWriteBufferLen,
                   "Firmware update FAILED (error %d). Existing firmware unchanged.", result);
    }

    return pdFALSE;
}
```

### Behaviour

| Input | Result |
|-------|--------|
| `firmware output.img` | Update proceeds (no CRC check — identical to current behaviour) |
| `firmware output.img 0x1234` | File CRC computed; proceeds only if it matches `0x1234` |
| `firmware output.img 0x9999` (wrong CRC) | `Error: CRC mismatch — file 0xABCD, expected 0x9999. Flash NOT modified.` |
| `firmware output.img abc` (wrong length) | `Error: CRC must be in format 0xNNNN` |
| `firmware output.img 0xGGGG` (bad hex digits) | `Error: invalid CRC '0xGGGG' (expected 0xNNNN)` |

**Important:** the CRC check uses the same path as `xip_update_firmware_from_sd` —
`/MANIFEST/<filename>` — so the CRC is over exactly the bytes that will be written to flash.

---

## 5. FreeRTOS yielding and inactivity during firmware update

### 5.1 FreeRTOS task yielding

`xip_update_firmware_from_sd()` delegates to three sequential sub-functions:
`erase_firmware_slot()`, `write_firmware_from_sd()`, and `verify_firmware_slot()`.  All
three contain a `vTaskDelay(1)` at the end of each inner-loop iteration:

| Sub-function | Loop body | `vTaskDelay(1)` calls |
|---|---|---|
| `erase_firmware_slot()` | one 64 KB block erase | 16 (one per block, 1 MB slot) |
| `write_firmware_from_sd()` | 4 KB file read + flash write + verify | ~256 (one per chunk) |
| `verify_firmware_slot()` | 4 KB file read + flash read + compare | ~256 (one per chunk) |

Other tasks receive the CPU at every `vTaskDelay(1)`.  The SPI mutex (`xSPIMutex`) is held
across the full write and verify loops (taken before the `while`, released after), so any
other task that tries to take `xSPIMutex` will block for the duration of that sub-function.
This is correct and expected for exclusive SPI bus ownership.

**Conclusion: no change required.** The yielding is already present and was deliberately
designed into the code (each `vTaskDelay(1)` carries the comment *"Force a task switch to
prevent the inactivity timeout firing"*).

### 5.2 Inactivity check

The inactivity system runs in `USEIDLETASK` mode (`inactivity.c`).  The mechanism is:

- `inactivity_on_task_switched_in()` — called via `traceTASK_SWITCHED_IN()` on every task
  context switch.  Resets `idle_start_tick = 0` and `inactivity_triggered = pdFALSE`
  whenever any non-idle task is switched in.
- `inactivity_IdleHook()` — called from `vApplicationIdleHook()`.  Starts the idle
  accumulator when the idle task first runs; fires `app_onInactivityDetection()` once
  `timeSinceActivity >= tasksInactiveTicks` (default `INACTIVITYTIMEOUT` = 1000 ms).

`app_onInactivityDetection()` sends `APP_MSG_IMAGETASK_INACTIVITY` (0x0A00) to the image
task queue and `APP_MSG_IFTASK_INACTIVITY` to the if-task queue, which leads to DPD.

During firmware update, `vTaskDelay(1)` is called at the end of every chunk/block.  When
the CLI task is rescheduled after each delay, `inactivity_on_task_switched_in()` fires and
resets `idle_start_tick` to zero.  The idle accumulator therefore never exceeds
approximately one chunk processing time — in practice well under 100 ms — far below the
1000 ms threshold.

**Conclusion: no change required.**  The `APP_MSG_IMAGETASK_INACTIVITY` event cannot be
triggered by a firmware update under the current design.  The `vTaskDelay(1)` pattern
handles both yielding and inactivity prevention in a single call.

---

## 6. Step-level progress messages to the companion app (optional)

This section documents a design for sending progress messages to the nRF52832 companion
during a firmware update.  It is not part of the minimum CRC implementation but is recorded
here for future reference.

### 6.1 Mechanism

The existing path for pushing a string from the AI processor to the companion app is:

```c
APP_MSG_T msg;
msg.msg_event     = APP_MSG_IFTASK_MSG_TO_MASTER;
msg.msg_data      = (uint32_t)(uintptr_t)string_ptr;
msg.msg_parameter = strlen(string_ptr);
xQueueSend(xIfTaskQueue, &msg, __QueueSendTicksToWait);
```

`if_task` receives this and calls `sendI2CMessage(..., AI_PROCESSOR_MSG_RX_STRING, ...)`,
which transmits the string to the nRF52832 over I2C.

### 6.2 Message prefix convention

All firmware-update status messages sent via this path should start with the same fixed
prefix so that both the companion app and a human reading the console can identify and
parse them unambiguously.  Proposed prefix: **`"Firmware: "`**.

Existing `xprintf` progress lines inside `xip_manager.c` use different wording and go only
to the console (UART); they are separate from these I2C messages and do not need to change.

### 6.3 Changes to `prvFirmwareCommand`

No changes to `xip_manager.c` are needed.  `prvFirmwareCommand` already calls the three
sub-operations sequentially and checks each return code.  A static helper sends the message:

```c
#define FIRMWARE_MSG_PREFIX  "Firmware: "

static void send_firmware_status(const char *text) {
    static char msg_buf[64];
    APP_MSG_T   msg;

    snprintf(msg_buf, sizeof(msg_buf), FIRMWARE_MSG_PREFIX "%s", text);
    msg.msg_event     = APP_MSG_IFTASK_MSG_TO_MASTER;
    msg.msg_data      = (uint32_t)(uintptr_t)msg_buf;
    msg.msg_parameter = strlen(msg_buf);
    xQueueSend(xIfTaskQueue, &msg, __QueueSendTicksToWait);
}
```

The updated command body (after the CRC check passes):

```c
    send_firmware_status("erasing slot...");
    result = erase_firmware_slot(target_slot);       // internal — not currently public
    if (result != 0) {
        send_firmware_status("erase FAILED");
        /* ... existing error path ... */
    }

    send_firmware_status("writing image...");
    result = write_firmware_from_sd(target_slot, filepath);
    if (result != 0) {
        send_firmware_status("write FAILED");
        /* ... */
    }

    send_firmware_status("verifying...");
    result = verify_firmware_slot(target_slot, filepath);
    if (result != 0) {
        send_firmware_status("verify FAILED");
        /* ... */
    }

    send_firmware_status("update complete — type 'reset' to boot");
```

**Note:** `erase_firmware_slot`, `write_firmware_from_sd`, and `verify_firmware_slot` are
currently `static` inside `xip_manager.c`.  To drive them from `prvFirmwareCommand` they
would need to be made non-static and declared in `xip_manager.h`, or `xip_update_firmware_from_sd`
would need a progress-callback parameter.  The cleanest approach is to expose the three
sub-functions and remove the monolithic wrapper, since the CLI command can then own the
top-level sequence and send status between each step.

### 6.4 Full message sequence

| Event | Message sent to app |
|-------|---------------------|
| CRC mismatch (if CRC param given) | `"Firmware: CRC mismatch — update aborted"` |
| Erase started | `"Firmware: erasing slot..."` |
| Erase failed | `"Firmware: erase FAILED"` |
| Write started | `"Firmware: writing image..."` |
| Write failed | `"Firmware: write FAILED"` |
| Verify started | `"Firmware: verifying..."` |
| Verify failed | `"Firmware: verify FAILED"` |
| All steps OK | `"Firmware: update complete — type 'reset' to boot"` |

### 6.5 Files changed (if implemented)

| File | Change |
|------|--------|
| `CLI-FATFS-commands.c` | Add `send_firmware_status()` helper; update `prvFirmwareCommand` to call it between steps; add `#include "if_task.h"` if not already present |
| `xip_manager.c` | Make `erase_firmware_slot`, `write_firmware_from_sd`, `verify_firmware_slot` non-static |
| `xip_manager.h` | Declare the three newly public sub-functions |

---

## 7. Files changed

| File | Change |
|------|--------|
| `CLI-FATFS-commands.c` | Add `#include "crc16_ccitt.h"`; add `compute_file_crc()` static helper; add `prvCrcCommand()` and `xCrc` definition; update `prvFirmwareCommand()` and `xFirmware` definition; register `xCrc` in `vRegisterCLICommands()` |

No changes are needed to `xip_manager.c`, `xip_manager.h`, or any header.

---

## 8. Notes and constraints

**`crc16_ccitt.h` include**: `CLI-FATFS-commands.c` does not currently include
`crc16_ccitt.h`.  Add `#include "crc16_ccitt.h"` alongside the other project includes near
the top of the file (after `#include "xip_manager.h"` is a suitable location).

**`CONFIG_DIR` path**: The `firmware` command constructs the full path as
`CONFIG_DIR "/" filename`.  `CONFIG_DIR` is defined in `image_task.h` (the macro used by
`xip_update_firmware_from_sd`).  The same macro is already used in `xip_manager.c`; use it
in `prvFirmwareCommand` via the existing `#include "image_task.h"` in
`CLI-FATFS-commands.c`.

**Integer type for file size**: FatFS `f_size()` and byte counters are `DWORD` (uint32_t).
The `%lu` format specifier is used with a cast to `unsigned long` for portability across
32-bit and 64-bit printf implementations.

**Stack depth**: `compute_file_crc` adds 512 bytes to the stack.  The existing
`prvFirmwareCommand` already calls `xip_update_firmware_from_sd` which uses 4 KB heap
buffers (not stack), so this 512-byte addition is not a concern.
