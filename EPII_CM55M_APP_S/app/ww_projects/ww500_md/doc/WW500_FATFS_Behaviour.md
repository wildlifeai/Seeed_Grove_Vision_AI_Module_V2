# WW500 FatFS Behaviour Analysis
#### CGP / Claude — 16 May 2026

## Scope

This document covers two related issues in the FatFS SPI-mode SD card stack on the WW500
board.  All line numbers refer to:

```
middleware/fatfs/port/mmc_spi/mmc_we2_spi.c
```

The FatFS configuration for the active application is:

```
app/ww_projects/ww500_md/ffconf.h
```

---

## 1. Assertion failure — `mmc_disk_initialize` line 515

### Symptom

Mounting a 64 GB card returns error 3 (`FR_NOT_READY`).  A subsequent `format` command
(which calls `f_mkfs`) immediately asserts:

```
assertion "ret == ARM_DRIVER_OK" failed:
  file "middleware/fatfs/port/mmc_spi/mmc_we2_spi.c", line 515,
  function: mmc_disk_initialize
```

### Root cause — SPI driver state after `power_off()`

The assertion is the check on the return value of `Driver_SPI0.Control(...)` (lines
508–515).  This is a pure SPI-controller configuration call; it does not touch the SD card,
so it should always succeed — *unless* the driver is in an unexpected state.

The problem arises from a sequence of two calls to `mmc_disk_initialize`:

**First call (from `f_mount`):**

1. `Driver_SPI0.Initialize(NULL)` — OK  
2. `Driver_SPI0.PowerControl(ARM_POWER_FULL)` — OK  
3. `Driver_SPI0.Control(mode | SPI_CLOCK_SLOW)` — OK (past line 515)  
4. Card fails to come ready — `ty` remains 0  
5. `power_off()` is called: only `Driver_SPI0.PowerControl(ARM_POWER_OFF)` — **no
   `Uninitialize()`**  
6. `Stat = STA_NOINIT` is set; `f_mount` returns `FR_NOT_READY`

**Second call (from `f_mkfs` → `disk_initialize`):**

1. `Stat & STA_NOINIT` is still set, so the early-return guard (line 480) does not fire  
2. `Driver_SPI0.Initialize(NULL)` — called on a driver that is initialised-but-powered-off  
3. `Driver_SPI0.PowerControl(ARM_POWER_FULL)` — called  
4. `Driver_SPI0.Control(mode | SPI_CLOCK_SLOW)` → **returns non-OK → assertion fires**

The Himax CMSIS SPI driver apparently does not tolerate `Initialize()` being called again
after `PowerControl(ARM_POWER_OFF)` without an intervening `Uninitialize()`.  The CMSIS
specification (Driver_SPI.h) allows drivers to return `ARM_DRIVER_ERROR` in this case.

### Fix

Add `Driver_SPI0.Uninitialize()` to `power_off()`:

```c
static void power_off(void)
{
    Driver_SPI0.PowerControl(ARM_POWER_OFF);
    Driver_SPI0.Uninitialize();
}
```

This brings the driver back to a clean state so that the next call to `Initialize()` starts
from scratch.

### Why does the 64 GB card fail initialisation? — Confirmed diagnosis

Diagnostic tracing was added to print each `send_cmd` response.  The traces from a failing
64 GB card and a working 32 GB card were compared directly.

**Working 32 GB card (SanDisk / Samsung class):**

```
CMD0  → 0x01  (idle)
CMD8  → 0x01  (SDv2 confirmed)
CMD55 → 0x01, ACMD41 → 0x01   (iteration 1, still initialising)
CMD55 → 0x01, ACMD41 → 0x01   (iteration 2)
CMD55 → 0x01, ACMD41 → 0x01   (iteration 3)
CMD55 → 0x01, ACMD41 → 0x01   (iteration 4)
CMD55 → 0x01, ACMD41 → 0x00   (iteration 5 — init complete)
CMD58 → 0x00  (OCR read OK)
CardType = 0x18 (CT_SDC2 | CT_BLOCK — SDHC/SDXC block-addressed)
→ mounts successfully
```

**Failing 64 GB card:**

```
CMD0  → 0x01  (idle)
CMD8  → 0x01  (SDv2 confirmed)
CMD55 → 0x01, ACMD41 → 0x01   (iteration 1, still initialising)
CMD55 → 0xFF  (no response — card has stopped driving MISO)
CMD55 → 0xFF  (no response — repeats for all 1000 iterations)
→ timeout, FR_NOT_READY
```

The 64 GB card correctly enters SPI idle state and passes the CMD8 interface condition
check, but after exactly **one** CMD55/ACMD41 exchange it stops responding entirely.
It does not drive MISO for any subsequent CMD55.  This is true regardless of:

- Whether CS is left low or raised between ACMD41 iterations
- Whether the ACMD41 argument includes voltage range bits (`0x40FF8000`) or not

**Conclusion: the 64 GB card has a firmware defect in its SPI mode implementation.**  It
does not support repeated ACMD41 polling, which is mandatory per the SD specification.
The card also presents as 16 GB FAT32 on a nominally 64 GB body — Windows cannot natively
format >32 GB as FAT32, strongly suggesting the card is counterfeit.  There is no software
workaround; the card is not usable with this SPI-mode stack.

Cards confirmed working: standard 32 GB SDHC cards (SanDisk, Samsung).  Use cards from
reputable suppliers.

---

## 2. Write speed — tens of milliseconds per operation

### Observed behaviour

File writes take tens of milliseconds, which is slow relative to what the SPI clock and
card hardware should allow.

At 12 MHz SPI, transferring 512 bytes takes approximately 0.34 ms.  If writes are taking
tens of milliseconds, the bottleneck is not the data transfer itself but the overhead
surrounding it.

### Bottleneck 1 — `wait_ready` polling interval

`wait_ready` (line 185) is called before every sector write (inside `xmit_datablock`, line
288) and also whenever a command is sent (`selectCard` → `wait_ready`).  Its loop:

```c
while (datain != 0xFF && cnt <= wt) {
    datain = xchg_spi(0xFF);
    DELAY(1);             // 1 ms per poll
    wait_spi_completed(1);  // redundant: xchg_spi already waits
    cnt++;
}
```

Issues:

- **`DELAY(1)` adds 1 ms to every poll iteration.**  A card that needs 3 ms to finish
  programming a sector will cause three 1-ms delays before it is seen as ready.
- **`wait_spi_completed(1)` after `xchg_spi` is redundant.**  `xchg_spi` already calls
  `wait_spi_completed(1)` internally (line 133).  The extra call wastes time.
- **One-byte SPI transfers for polling.**  Each `xchg_spi` call invokes
  `Driver_SPI0.Transfer(..., 1)` — a full CMSIS driver round-trip for a single byte.  At
  12 MHz, the byte itself takes 0.67 µs, but driver overhead (function call, status check
  loop) likely dominates.

### Bottleneck 2 — chip-select toggling per command

`send_cmd` (line 310) unconditionally calls `deselect()` then `selectCard()` before every
command except CMD12.  `selectCard()` in turn calls `wait_ready(500)`.  For a ready card
the first poll byte is 0xFF and `wait_ready` returns immediately, so this is normally fast.
However, each deselect/select pair still sends three dummy bytes (one in `deselect`, one
in `selectCard` before `wait_ready`, and the wait_ready poll itself) and involves three
CMSIS driver calls.

For ACMD commands (ACMD41, ACMD23, ACMD13), `send_cmd` calls itself recursively for CMD55
first and then deselects/selects again for the ACMD.  During the ACMD41 initialisation loop
(up to 1000 iterations), this is 2000 select/deselect cycles.

This mostly affects initialisation speed rather than sustained write throughput, but it
contributes to perceived latency.

### Bottleneck 3 — SPI clock limited to 12 MHz

`SPI_CLOCK_FAST` is 12 MHz (line 51).  SD cards in SPI mode support up to 25 MHz.  All
other things being equal, raising the clock to 25 MHz would approximately halve raw
transfer time.  Whether the Himax SPI peripheral and the PCB routing (trace lengths,
capacitance) support 25 MHz needs to be verified, but it is worth trying.

### Measured write path for one sector

A single-sector write (`mmc_disk_write`, count=1) executes:

| Step | Function | Approx time at 12 MHz |
|------|----------|-----------------------|
| Send CMD24 | `send_cmd` → deselect + select + 6 cmd bytes + response | ~40 µs + `wait_ready` |
| Wait card ready | `wait_ready(500)` inside `xmit_datablock` | 1 ms minimum (1 poll + DELAY) |
| Send data token | `xchg_spi(0xFE)` | ~7 µs |
| Send 512 data bytes | `xmit_spi_multi(buff, 512)` | ~340 µs |
| Send dummy CRC | 2× `xchg_spi(0xFF)` | ~14 µs |
| Read data response | `xchg_spi(0xFF)` | ~7 µs |
| Deselect | `deselect()` | ~7 µs |

The `wait_ready` call inside `xmit_datablock` (ensuring the card is not busy from a
previous write) is the dominant term.  If the application writes sectors back-to-back, the
card's internal flash programming time (typically 1–10 ms for consumer cards) accumulates
here.

### Implementation — `#ifdef SPEEDIMPROVEMENTS`

The production code in `mmc_we2_spi.c` implements both paths under a compile-time switch.
Define `SPEEDIMPROVEMENTS` (e.g. `-DSPEEDIMPROVEMENTS` in the makefile) to activate the fast
path; omit it to use the safe original path.

**Default (`SPEEDIMPROVEMENTS` not defined) — original code, known reliable:**

```c
while (datain != 0xFF && cnt <= wt) {
    datain = xchg_spi(0xFF);
    DELAY(1);                 // 1 ms per poll — wt is in milliseconds
    wait_spi_completed(1);    // redundant but harmless
    cnt++;
}
```

**With `SPEEDIMPROVEMENTS` — FreeRTOS tick-count timeout, no fixed delay:**

```c
TickType_t start = xTaskGetTickCount();

datain = xchg_spi(0xFF);
while (datain != 0xFF) {
    if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(wt)) {
        return 0;   // genuine timeout after wt milliseconds
    }
    datain = xchg_spi(0xFF);
}
return 1;
```

Key differences from a naive DELAY removal:

- `wt` retains its millisecond meaning — callers do not need to change.
- The loop polls as fast as the SPI allows when the card is busy, but the timeout is
  still a real wall-clock value rather than a poll count.
- The redundant `wait_spi_completed(1)` (already done inside `xchg_spi`) is omitted.
- Requires `FreeRTOS.h` and `task.h`; these are included under `#ifdef SPEEDIMPROVEMENTS`.

### Measured write times (12 KB JPEG, 12 MHz SPI)

| Configuration | Write time |
|---|---|
| Original (`DELAY(1)` in `wait_ready`) | ~200 ms |
| `SPEEDIMPROVEMENTS` (tick-count polling) | ~40 ms |

The 200 ms baseline includes the `CTRL_SYNC` wait inside `f_close`, which correctly polls
until NAND programming completes.  The 40 ms path does the same wait but polls at SPI speed
instead of 1 ms steps.

### Further options (not yet implemented)

**Poll in 8-byte bursts** — amortises CMSIS driver call overhead across 8 bytes per
iteration.  Would slightly reduce the 40 ms further but requires more invasive changes.

**Increase SPI clock** — `SPI_CLOCK_FAST = 25000000` may approximately halve raw transfer
time.  Verify with the specific card and PCB routing before committing.

---

## 3. SPI driver layer structure

`Driver_SPI0` is a variable of type `ARM_DRIVER_SPI`, which is a struct of function
pointers (defined in `CMSIS/Driver/Include/Driver_SPI.h`).  There are three layers between
`mmc_we2_spi.c` and the hardware registers.

### Layer diagram

```
mmc_we2_spi.c          — FatFS port; SD card SPI protocol (full source)
      │
      │  Driver_SPI0.Send() / .Receive() / .Control() / …
      ▼
cmsis_drivers/SPI/Driver_SPI.c   — CMSIS wrapper (full source)
      │
      │  SPI0_Resources.dev->spi_write() / spi_read() / spi_control() / …
      ▼
drivers/inc/hx_drv_spi.h         — Himax DEV_SPI API (header only)
prebuilt_libs/gnu/libdriver.a    — dw_spi_*() implementations (binary, no source)
      │
      ▼
SPI hardware registers (DesignWare DW_apb_ssi core inside HX6538)
```

### Initialisation of Driver_SPI0

`Driver_SPI0` is initialised as a **static constant** at compile time in `Driver_SPI.c`:

```c
ARM_DRIVER_SPI Driver_SPI0 = {
    ARM_SPI_GetVersion,     // all entries are static functions in Driver_SPI.c
    ARM_SPI_GetCapabilities,
    ARM_SPI_Initialize,     // ← also calls hx_drv_spi_mst_get_dev() at runtime
    ARM_SPI_Uninitialize,
    ARM_SPI_PowerControl,
    ARM_SPI_Send,
    ARM_SPI_Receive,
    ARM_SPI_Transfer,
    ARM_SPI_GetDataCount,
    ARM_SPI_Control,
    ARM_SPI_GetStatus
};
```

The second struct of function pointers — `SPI0_Resources.dev` of type `DEV_SPI*` — is
populated at **runtime** when `Driver_SPI0.Initialize()` is first called:

```c
SPI0_Resources.dev = hx_drv_spi_mst_get_dev(USE_DW_SPI_MST_S);
```

`hx_drv_spi_mst_get_dev` returns a pointer to a static `DEV_SPI` struct whose function
pointer fields (`spi_open`, `spi_write`, `spi_read`, `spi_control`, `spi_close`) point into
`libdriver.a`.

### Function mapping table

| `mmc_we2_spi.c` call | `Driver_SPI.c` wrapper | Himax `DEV_SPI` call | In libdriver.a |
|---|---|---|---|
| `Driver_SPI0.Initialize(NULL)` | `ARM_SPI_Initialize` | `hx_drv_spi_mst_get_dev()` to get `dev` pointer; resets bookkeeping fields | `hx_drv_spi_mst_get_dev` |
| `Driver_SPI0.Uninitialize()` | `ARM_SPI_Uninitialize` | `dev->spi_close()` | `dw_spi_close` (inferred) |
| `Driver_SPI0.PowerControl(ARM_POWER_OFF)` | `ARM_SPI_PowerControl` | `dev->spi_control(SPI_CMD_DIS_DEV, NULL)` — disables SPI hardware | `dw_spi_control` |
| `Driver_SPI0.PowerControl(ARM_POWER_FULL)` | `ARM_SPI_PowerControl` | **No-op** — does not re-enable hardware | — |
| `Driver_SPI0.Control(ARM_SPI_MODE_MASTER, freq)` | `ARM_SPI_Control` | `dev->spi_control(SPI_CMD_MST_UPDATE_SYSCLK, …)` then `dev->spi_open(DEV_MASTER_MODE, freq)` then configures RX sample delay | `dw_spi_control`, `dw_spi_open` |
| `Driver_SPI0.Control(ARM_SPI_SET_BUS_SPEED, freq)` | `ARM_SPI_Control` | `dev->spi_control(SPI_CMD_MST_UPDATE_SYSCLK, …)` then `dev->spi_control(SPI_CMD_MST_SET_FREQ, freq)` | `dw_spi_control` |
| `Driver_SPI0.Control(ARM_SPI_CONTROL_SS, INACTIVE)` | `ARM_SPI_Control` | Calls `sspi_cs_gpio_output_level(true)` — a **GPIO callback** supplied by `mmc_we2_spi.c`, not a hardware driver call | GPIO driver |
| `Driver_SPI0.Control(ARM_SPI_CONTROL_SS, ACTIVE)` | `ARM_SPI_Control` | Calls `sspi_cs_gpio_output_level(false)` | GPIO driver |
| `Driver_SPI0.Send(buff, n)` | `ARM_SPI_Send` | `dev->spi_write(buff, n)` — interrupt-driven TX | `dw_spi_write_int` |
| `Driver_SPI0.Receive(buff, n)` | `ARM_SPI_Receive` | `dev->spi_read(buff, n)` — interrupt-driven RX | `dw_spi_read_int` |
| `Driver_SPI0.Transfer(out, in, n)` | `ARM_SPI_Transfer` | Sets TX/RX interrupt buffers via `spi_control`, then `dev->spi_control(SPI_CMD_TRANSFER_INT, …)` — full-duplex | `dw_spi_control` |
| `Driver_SPI0.GetDataCount()` | `ARM_SPI_GetDataCount` | Returns `SPI0_Resources.xfer_num` — set in software before each transfer, not read from hardware | — |
| `Driver_SPI0.GetStatus()` | `ARM_SPI_GetStatus` | `dev->spi_control(SPI_CMD_GET_BUSY_STATUS, &busy)` when transfer done flag is set | `dw_spi_control` |

### The no-op PowerControl(FULL) and the assertion bug

The table makes the assertion root cause explicit.  `PowerControl(ARM_POWER_OFF)` calls
`SPI_CMD_DIS_DEV` which disables the SPI hardware block.  `PowerControl(ARM_POWER_FULL)` is
a no-op and does not reverse this.  So when `mmc_disk_initialize` is called a second time
(after a failed first attempt), `Control(ARM_SPI_MODE_MASTER, …)` calls `spi_open` on a
peripheral that is still disabled; `dw_spi_open` in `libdriver.a` returns an error; the
CMSIS wrapper returns `ARM_DRIVER_ERROR`; and the `ASSERT_HIGH` macro fires.

Adding `Driver_SPI0.Uninitialize()` to `power_off()` works because `Uninitialize` calls
`spi_close()`, which brings the hardware to a well-defined closed state.  The next
`Initialize()` call then re-acquires the `dev` pointer via `hx_drv_spi_mst_get_dev()`, and
the subsequent `Control(ARM_SPI_MODE_MASTER, …)` → `spi_open()` starts from a clean slate.

### What is and is not observable

| Layer | Source available | Can add diagnostics |
|---|---|---|
| `mmc_we2_spi.c` | Yes | Yes — `xprintf` freely |
| `Driver_SPI.c` | Yes | Yes — but avoid adding state |
| `hx_drv_spi.h` | Header only | No |
| `libdriver.a` (`dw_spi_*`) | No | No |
| SPI hardware registers | N/A | Logic analyser only |

The chip-select GPIO path (`sspi_cs_gpio_output_level`) is provided by `mmc_we2_spi.c`
itself (overriding weak defaults in `Driver_SPI.c`) and is fully observable.

---

## 4. Constraint — instrumentation boundary

All diagnostic and performance work must stay within `mmc_we2_spi.c` or `Driver_SPI.c`.
The functions in `mmc_we2_spi.c` that are safe to instrument are:

- `mmc_disk_initialize` — card identification, SPI clock selection
- `wait_ready` — card-busy polling (biggest latency contributor)
- `xmit_datablock` / `rcvr_datablock` — sector-level data path
- `send_cmd` — command framing and CS control

The SPI transfer functions (`xchg_spi`, `xmit_spi_multi`, `rcvr_spi_multi`) call
`Driver_SPI0` directly; their internal timing cannot be observed without a logic analyser.

---

## 5. Bugs found in `fileWriteImage` (`fatfs_task.c`)

Removing the 1 ms delay exposed timing failures that had been masked.  These failures in turn
revealed pre-existing code bugs that had never been triggered.

### Bug A — Missing `f_close` on `f_write` failure

When `f_write` fails, `fileWriteImage` returned immediately without calling `f_close`.  The
file was left open in FatFS's lock table.  With `FF_FS_LOCK = 2`, two such leaked files
exhausted the lock table; the next `f_open` returned `FR_TOO_MANY_OPEN_FILES` (error 18).
The abandoned file appeared on disk as a 0-byte entry (directory entry created by `f_open`,
file size never updated because `f_close` was never reached).

**Fix:** call `f_close` and clear `imagesOpen` on any early-return error path.

### Bug B — Null pointer dereference on `extraBlock`

`fileWriteImage` is also reachable from the `APP_MSG_FATFSTASK_WRITE_FILE` handler, which
passes `NULL` as the `extraBlock` argument.  The code unconditionally accessed
`extraBlock->length`.  On this ARM target (no MMU), address 0 is the vector table and reads
as 0, so the condition was false and no crash occurred — but the behaviour is undefined.

**Fix:** guard with `if (extraBlock != NULL && extraBlock->length > 0)`.

### Bug C — Empty `current_capture_dir` routes files to `/MANIFEST`

`current_capture_dir` in `dirManager` is zeroed by `memset` in `dir_mgr_init_config`.
`dir_mgr_init_image_dir` creates the directory and sets `current_capture_dir` only on
success.  If `fatfs_mkdir_recursive` fails (because the card was still busy from a preceding
write and `wait_ready` timed out prematurely), `current_capture_dir` remains an empty string.

`fileWriteImage` then calls `f_chdir("")`.  FatFS treats an empty path as "stay in current
directory" and returns `FR_OK` without changing the CWD.  The CWD was last set to `/MANIFEST`
by `load_configuration`, so the image is created there.

**Fix:** check `dirManager->current_capture_dir[0] == '\0'` at the top of `fileWriteImage`
and return `FR_NO_PATH` immediately.

### Bug D — Stale open file not closed before `f_open`

If a previous write leaked an open `imagesFile` (bug A scenario), the next call to
`fileWriteImage` overwrote the `FIL` object with a new `f_open` call without releasing the
existing lock-table slot.

**Fix:** check `dirManager->imagesOpen` at entry and call `f_close` if true.

---

## 6. Summary of changes made

| Item | File | Status |
|------|------|--------|
| Add `Uninitialize()` to `power_off()` | `mmc_we2_spi.c` | Done — fixes assertion |
| `deselect()` before ACMD41 retry delay | `mmc_we2_spi.c` | Done — CS timing fix |
| ACMD41 argument includes voltage range bits | `mmc_we2_spi.c` | Done |
| `#ifdef SPEEDIMPROVEMENTS` in `wait_ready` | `mmc_we2_spi.c` | Done — 200 ms → 40 ms |
| Fix bugs A–D in `fileWriteImage` | `fatfs_task.c` | Done |
| 64 GB card — counterfeit, no fix possible | — | Documented |
| Increase SPI clock to 25 MHz | `mmc_we2_spi.c` | Not yet tried |
| Poll in 8-byte bursts in `wait_ready` | `mmc_we2_spi.c` | Not yet tried |
