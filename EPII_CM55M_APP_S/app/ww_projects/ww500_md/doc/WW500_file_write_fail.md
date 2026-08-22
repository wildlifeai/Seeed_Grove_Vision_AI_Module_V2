# WW500 JPEG File Write Failure — Root Cause and Fix
#### CGP — 18 June 2026

## Symptom

In time-lapse mode, occasional JPEG files of 0 bytes appear on the SD card with the console message:

```
Error writing file 59200840.JPG
Image not written. Error: 1
File write took 1434ms
```

FatFS error 1 is `FR_DISK_ERR` — a hard error in the low-level disk I/O layer.

The failure only occurred on the **first warm boot** of a session, when the BLE processor sent a
`setutc` command to resync the WW500's clock.

---

## Root cause

### The players

| Task     | Priority |
|----------|----------|
| CLI      | 4        |
| IFTask   | 3        |
| FatFS    | 2        |
| IMAGE    | 1        |

### The sequence

1. IMAGE task captures a frame and sends a `WRITE_IMAGE` event to FatFS task (priority 2).
2. FatFS task starts the JPEG write (f_open → f_write → f_close), interleaved with IFTask I2C
   traffic.
3. IFTask receives a `setutc` I2C command from the BLE processor and forwards it to the CLI task
   queue.
4. CLI task (priority 4) runs `prvSetUtc()` → `hx_drv_rtc_set_time()`.
5. **`hx_drv_rtc_set_time()` suppresses ARM interrupts for ~1357 ms** while it busy-polls a
   hardware status register waiting for the new time to sync to the 32.768 kHz RTC clock domain.
6. With ARM interrupts suppressed, the FreeRTOS tick ISR cannot fire. No task switch occurs.
   FatFS task is completely frozen for 1357 ms.
7. When interrupts are re-enabled and FatFS resumes, the SPI SD-card driver's `wait_ready(500)`
   function checks its wall-clock timeout:

```c
// mmc_we2_spi.c
#define MMC_GET_MS()  ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS))

static int wait_ready(UINT wt)
{
    uint32_t start = MMC_GET_MS();
    datain = xchg_spi(0xFF);
    while (datain != 0xFF) {
        if ((MMC_GET_MS() - start) >= (uint32_t)wt)
            return 0;          // ← timeout
        ...
    }
}
```

   `xTaskGetTickCount()` is driven by the FreeRTOS tick ISR. While ARM interrupts are suppressed
   the tick counter does not advance. The counter catches up in a burst when interrupts are
   re-enabled, so the elapsed time can jump directly past the 500 ms timeout. `wait_ready()`
   returns 0 → `FR_DISK_ERR`.

### Why Fix A (priority reduction) did not work

The initial attempt was to lower the CLI task priority to 1 before the blocking call, so FatFS
(priority 2) could preempt it. This had no effect because **FreeRTOS preemption requires the tick
interrupt to fire**. Since `hx_drv_rtc_set_time()` suppresses ARM interrupts, the scheduler never
gets control regardless of task priorities.

---

## Fix applied — Fix D: guard before calling `hx_drv_rtc_set_time()`

### Principle

Ensure the SD write is **complete** before the blocking RTC call starts. Do not try to run both
concurrently.

### Implementation

**`fatfs_task.c`** — a global flag is set while any `fileWriteImage()` or `fileWrite()` call is
active:

```c
/* Set non-zero while a fileWriteImage() call is in progress. */
volatile int g_sdWriteActive = 0;
```

The flag brackets all three SD write paths in the FatFS event handler:
- `APP_MSG_FATFSTASK_WRITE_IMAGE` → `fileWriteImage(fileOp, extraBlock, ...)`
- `APP_MSG_FATFSTASK_WRITE_FILE` image path → `fileWriteImage(fileOp, NULL, ...)`
- `APP_MSG_FATFSTASK_WRITE_FILE` non-image path → `fileWrite(fileOp)`

**`CLI-commands.c:prvSetUtc()`** — polls the flag with `vTaskDelay()` yields before making the
blocking call:

```c
extern volatile int g_sdWriteActive;
TickType_t waitDeadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
while (g_sdWriteActive) {
    if (xTaskGetTickCount() >= waitDeadline) {
        xprintf("setutc: timed out waiting for SD write\n");
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}
ret = exif_utc_set_rtc_from_time(&tm);
```

Each `vTaskDelay(10)` yields the CLI task, allowing the FatFS task (priority 2) to run and
advance the write. A 2000 ms safety timeout prevents the wait from hanging indefinitely if the SD
card has failed.

### Why this works

The poll loop runs **before** `hx_drv_rtc_set_time()` is called — while ARM interrupts are still
enabled and FreeRTOS is fully operational. The FatFS task can make normal progress during the
poll. Once `g_sdWriteActive` clears (write done), the RTC call proceeds with no SD I/O in flight.

---

## Operations not requiring the guard

| Operation | Why safe |
|-----------|----------|
| `load_configuration()` | Runs at boot, before BLE comms established; never concurrent with `setutc` |
| `save_configuration()` (SAVE_STATE) | Triggered by inactivity at end of wake cycle, after `setutc` completes |
| `OPEN_FILE` / `APPEND_FILE` / `CLOSE_FILE` | IFTask waits for each step before accepting next I2C message; `setutc` cannot interleave |
| `fileRead()` (READ_FILE) | Read-only, triggered by explicit CLI commands; not concurrent with time-lapse write |

---

## Timing note

On a normal warm boot, `setutc` is only sent by the BLE processor on the **first** boot of a
session (when the WW500 clock is at the epoch 2024-01-01T00:00:02Z). Subsequent boots have a
synced clock and `setutc` is not sent. This explains why the failure was intermittent and appeared
only once per physical deployment.

The `hx_drv_rtc_set_time()` delay of ~1357 ms is caused by the WE2 RTC hardware requiring the
new counter value to be captured at a 32.768 kHz clock boundary. Worst case is one full second;
the extra ~357 ms is additional polling in the prebuilt driver. This cannot be reduced without
modifying the driver.
