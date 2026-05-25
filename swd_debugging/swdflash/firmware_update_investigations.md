# SWD Flash Read/Write — Claude Task Context
#### CGP / Claude, May 2025
#### Status: preliminary — experiments ongoing

## Hardware setup

Target: Himax HX6538 (WW500 board), with an external XIP SPI flash chip (W25Q128JW).
Programmer: MAX32625PICO (CMSIS-DAP v1), connected via a cable assembly through a WWIF100
routing board and a 6-way Molex to TC1 (TC2030 footprint) on the WW500.

TC1 pin connections (confirmed by board designer):
- PB7 → SWCLK
- PB8 → SWDIO
- PB6 → nRST (connected to the MAX32625PICO nRST output)

The HX6538 has a dedicated hardware RESETN pin (separate from PB6). The WW500 reset button
connects to RESETN. PB6 is a secondary reset input (SRSTN) that is only active as a reset
source when the firmware configures it as SCU_PB6_PINMUX_SRSTN.

The MAX32625PICO connects USB to the laptop and also provides board power.
pyOCD (Himax custom build `pyocd-hx 0.34.3.dev0+dirty`) drives it.

## Flash memory map (W25Q128JW)

| Region          | Address     | Size  | Description                  |
|-----------------|-------------|-------|------------------------------|
| Slot A firmware | 0x00000000  | 1 MB  | Primary firmware image       |
| Slot B firmware | 0x00100000  | 1 MB  | Secondary (A/B update) image |
| Slot selector   | 0x00FFF000  | 4 KB  | Boot slot descriptor         |

(These are flash-chip addresses, not CPU address-space addresses.)

## Key addresses in swdflash.py

| Symbol                     | Value        | Description                          |
|----------------------------|--------------|--------------------------------------|
| SPI_ISP_FLASH_CTRL_ALIAS   | 0x51010000   | ISP controller registers             |
| BASE_ADDR_FLASH1_R_ALIAS   | 0x3A000000   | XIP read window (ISP_MEM_IN_ADDR)    |
| BASE_ADDR_FLASH1_W_ALIAS   | 0x38000000   | XIP write window (ISP_MEM_OUT_ADDR)  |
| SCU_ISP_XIP_SPICACHE_ADDR  | 0x53070B00   | XIP/cache enable register            |
| SCU_HSC_ADDR               | 0x53070000   | System Control Unit (HSC) base       |
| SCU_ISP_PPC_CFG_APNS       | HSC + 0x848  | ISP Peripheral Protection Controller |

## What works (as of May 2025)

Both `burn.bat` and `dump_flash.bat` work correctly on a board with firmware installed,
using the operating procedure below.

- `burn.bat output.img` — programs a firmware image into flash at 0x00000000.
  Uses the `FlashAlgoDB` method (ARM flash algorithm downloaded into RAM).
- `dump_flash.bat` — reads Slot A (1 MB), Slot B (1 MB), selector (4 KB) into .bin files.
  Uses the `Direct` method (reads via ISP memory-mapped read alias at 0x3A000000).

## Root cause of previous failures (resolved)

**Problem — PB7 GPIO conflict (now fixed in firmware):**
Application firmware was reconfiguring PB7 as a GPIO after the bootloader had set it as
SWCLK. This silently disabled SWCLK, making SWD inaccessible after firmware booted.
Fix: removed the conflicting GPIO assignment for PB7 in application init code.

**Confirmed facts about HX6538 SWD pinmux:**
- The HX6538 bootloader configures PB7=SWCLK and PB8=SWDIO as part of its startup sequence.
- Application firmware does NOT need to call `swd_pinmux_cfg()` — the bootloader has already
  done this. The function is redundant and can be omitted.
- Application firmware MUST NOT reassign PB7 to any other function. Doing so breaks SWD.
- PB7 and PB8 are NOT in SWD function at power-on reset (before the bootloader runs).

**Why RESETN-held connection never worked:**
Holding RESETN keeps the chip in reset so the bootloader never runs and never configures
the SWD pinmux. The ARM DAP exists inside the HX6538 but is physically isolated from the
pins. This is the opposite of standard Cortex-M behaviour and is specific to this chip's
pinmux architecture.

Once pyOCD establishes a connection (after the bootloader has configured the pins), that
connection survives a subsequent `sw_sysresetreq` software reset because the SWD DAP
hardware stays powered from its own domain and SWCLK continues to be driven by the probe.

## Correct operating procedure

The script has a 5-second countdown followed by up to 3 connection attempts with 0.5-second
retries. The correct procedure for each invocation of swdflash.py (i.e. each section of
burn.bat or each of the three calls in dump_flash.bat):

1. **Hold RESET** (the black button, or TC1 nRST via programmer) during the countdown.
2. **Attempt 1 fails** with "No ACK received" — this is expected. The chip is in reset,
   SWD pins are not yet configured.
3. **Release RESET** when prompted — the bootloader runs and configures PB7/PB8 as SWD.
4. **Attempt 2 succeeds** (0.5 seconds later) — pyOCD connects to the running bootloader,
   halts it, and proceeds.

**Why 0.5s (not 2s):** if the running firmware reassigns PB7 after boot (as the `cc2d49eb`
build does), a 2-second wait gives the firmware time to execute that reassignment before
the retry, causing the retry to fail as well.  A 0.5-second retry catches the board while
the bootloader is still running and before the application has touched PB7.

In practice (confirmed by user): for `dump_flash.bat` (three sequential swdflash.py calls),
it is sufficient to hold and release RESET only for the **first** call. Subsequent calls
connect successfully without needing a reset, because the firmware has SWD enabled
throughout its operation (PB7 GPIO conflict is fixed).

The 5-second countdown (`--delay 5`, default) gives time to reach the reset button.
Pass `--delay 0` to skip the countdown if reset is already being held.

## Script extensions added beyond the Himax original (version 1.0.6)

- `--operation dump` — read flash to file instead of writing
- `--dump_size N` — number of bytes to read (default 0x1000)
- `FlashProgramming.flash_dump(addr, sz)` — direct ISP read via the 0x3A000000 alias
- `--delay N` — countdown before connecting (default 5s)
- `--retries N` — connection retry count (default 3)
- Updated user-facing messages to explain the hold/release reset procedure

`dump_flash.bat` was created to dump the three flash regions in sequence.

## Attempted: detecting probe presence via SWCLK (not implemented)

The goal was to allow application firmware to detect at startup whether the SWD probe
is physically connected, so it could conditionally keep PB7 as SWCLK or reassign it
to another GPIO function.

**Approach tried:** read PB7 as a GPIO input with internal pull-up enabled early in
application startup. If the probe drives SWCLK low when idle, the pin would read LOW
(probe present); if floating, it would read HIGH (no probe).

**Result: not viable with this hardware.**
The MAX32625PICO tri-states SWCLK when not actively clocking — it does not hold it
low at idle. With the pull-up on the WW500 SWCLK line, the pin reads HIGH whether or
not the probe is connected. Oscilloscope confirmed: SWCLK is high at idle, and only
toggles during active SWD transactions. After a session closes the probe tri-states
the pin and the pull-up returns it to HIGH.

**Approaches that were explored and rejected:**
- Reading SWDIO (PB8): pull-up makes it HIGH always; probe also idles SWDIO high
- Reading nRST (PB6): DAPLink open-drains nRST; only LOW when actively asserting reset
- Driving SWCLK low via pyOCD `DAP_SWJ_Pins` command: not exposed in pyocd-hx 0.34.3
- Driving SWCLK low via `probe.swj_sequence()`: no effect on physical pin at idle
- Doing a full SWD session then closing: SWCLK returns HIGH (probe tri-states on close)

**Possible future approach (not implemented):**
Add a `SWD_ENABLE` key to CONFIG.TXT on the SD card. Firmware reads this at startup
and decides whether to keep or reassign PB7. This would be explicit, reliable, and
require no hardware changes. The existing CONFIG.TXT infrastructure makes this
straightforward to add when needed.

## Slot selector scripts (added May 2026)

Two scripts for inspecting and reprogramming the 4 KB selector sector at 0x00FFF000.
These are stand-alone Python utilities — they do not call swdflash.py.

### parse_selector.py / parse_sel.bat

Reads a binary dump of the selector sector and validates the 20-byte
`SlotSelectorHeader` (magic, flash_offset, constant fields, checksum).

```
parse_sel.bat [file]          # default: dump_selector.bin
python parse_selector.py [file]
```

Reports which slot (A or B) is selected and whether every field is valid.

### set_selector.py / set_sel.bat

Builds a correct 4 KB sector image for the requested slot and programs it
into flash at 0x00FFF000 via swdflash.py.

```
set_sel.bat 0                 # select Slot A (flash_offset 0x00000000)
set_sel.bat 1                 # select Slot B (flash_offset 0x00100000)
python set_selector.py 0|1
```

Slot A values: flash_offset=0x00000000, checksum=0x4D04
Slot B values: flash_offset=0x00100000, checksum=0x167C

## Slot integrity analysis (added May 2026)

### What is ckBS?

`ckBS` is the 4-byte magic (`0x63 0x6B 0x42 0x53`) at the start of every
BLP (Bootloader Protected) packet in the Himax WE2 firmware image format.
The `we2_image_gen_local_dpd` tool (found as the string `BLPVM` in the
binary) wraps each firmware component — bootloaders, memory descriptors,
application — in a BLP packet before writing it to flash.  Each packet
begins with a fixed 0x52-byte (82-byte) header block starting with `ckBS`,
followed by the signed/encrypted payload.  The partition boundaries in a
slot dump are therefore identified by the positions of `ckBS` signatures.

### Firmware image layout within each 1 MB slot

Confirmed by cross-referencing `we2_image_gen_local_dpd/output_case1_sec_wlcsp/layout.json`
against the actual Slot B dump.  The two layouts differ by one 4 KB sector
throughout — see note below.

**dpd layout (we2_image_gen_local_dpd) — what is on the board:**

| Offset    | Size   | Partition                | ckBS? |
|-----------|--------|--------------------------|-------|
| 0x00000   | 4 KB   | hx_memory_descriptor     | yes   |
| 0x01000   | 72 KB  | 2nd_bootloader           | yes   |
| 0x13000   | 80 KB  | bootloader               | yes   |
| 0x27000   | 4 KB   | hx_memory_descriptor_ota | yes   |
| 0x28000   | ~276 KB| cm55m_application        | yes   |
| 0x6B000   | ~8 KB  | cm55m_application p2     | yes   |

Total used: ~445 KB out of 1 MB.  Remainder is 0xFF.

**Non-dpd layout (we2_image_gen_local) — NOT what is on the board:**
Same partitions but shifted one 4 KB sector later: bootloader at 0x14000,
OTA descriptor at 0x28000, application at 0x29000, total ~670 KB.

The "preamble" (0x00000–0x27FFF, i.e. everything before the application)
should be identical between Slot A and Slot B when both carry the same
firmware version.

### check_slots.py / check_slots.bat

Analyses dump_slot_a.bin and dump_slot_b.bin and reports:
- File size (must be exactly 1 MB)
- `ckBS` magic presence at each partition boundary
- Whether each partition region contains non-0xFF data
- Payload extent (last non-0xFF byte) and application size
- Byte-by-byte preamble comparison between the two slots

```
check_slots.bat               # defaults to dump_slot_a.bin and dump_slot_b.bin
python check_slots.py [slot_a.bin] [slot_b.bin]
```

### BLP integrity checking

The BLP format uses **cryptographic signatures** (RSA), not simple CRCs.
The layout.json `crc` field per partition (e.g. `0x2484` for the bootloader)
does not appear as a plain 16-bit value anywhere in the BLP headers — it is
part of the signature scheme.  The `att_secheadersize` (0x4AC = 1196 bytes
for most partitions, 0x57C for the application) is the full security wrapper;
the ~256-byte RSA signature block within it is what the Himax ROM bootloader
verifies when loading each partition.  Without access to the signing key or
the ROM verification code the stored signature cannot be independently checked.

### HX6538 boot chain architecture (confirmed by experiment, May 2026)

**Confirmed:** the ROM bootloader always loads the boot chain from Slot A
(flash base 0x00000000), regardless of the slot selector.  Only the
cm55m_application is loaded from the selected slot.

Boot sequence:
1. ROM reads `hx_memory_descriptor` from 0x00000 (Slot A, fixed)
2. ROM loads and RSA-verifies `2nd_bootloader` from 0x01000 (Slot A, fixed)
3. 2nd_bootloader reads selector sector at 0x00FFF000
4. 2nd_bootloader loads `cm55m_application` from the selected slot
   (Slot A: 0x28000, Slot B: 0x128000)

**Experimental evidence (five cases tested, May 2026):**

| Slot A        | Slot B                     | Selector | Result                    |
|---------------|----------------------------|----------|---------------------------|
| valid         | valid                      | B        | boots — runs Slot B app   |
| empty         | valid                      | B        | **does not boot**         |
| valid         | valid                      | A        | boots — runs Slot A app   |
| valid         | empty                      | A        | boots — runs Slot A app   |
| valid         | app only (0x28000+), no boot chain | B | **does not boot**   |

The fifth case is critical: Slot A intact, Slot B has application data at 0x128000
but its 0x100000–0x127FFF is all 0xFF (boot chain absent) — board does not boot
from Slot B even though Slot A is valid and selected.

**Refined understanding:**
- ROM always loads 2nd_bootloader from Slot A (unchanged)
- 2nd_bootloader reads `hx_mem_descriptor_ota` from the **selected slot** (slot_base
  + 0x27000) to locate that slot's application
- If the selected slot's `hx_mem_descriptor_ota` is absent (0xFF), boot fails
- Therefore Slot B must receive a complete image (including boot chain descriptors)
  whenever it is written

Slot A's boot chain must never be erased while the board runs from Slot B.
Slot B's boot chain can be fully erased and rewritten at any time (safe).

**Why both slots contain bootloader components:**
Both slots carry `hx_memory_descriptor`, `2nd_bootloader`, `bootloader`, and
`hx_mem_descriptor_ota`.  Slot B's copies are never executed in normal operation.
They exist so that a complete, self-contained image can be written to either slot
atomically.

**OTA safety implication:**
The original board failure was an OTA update that wrote to Slot A while the board
was running from Slot B.  Slot A passes through an empty/corrupt intermediate state
during an erase-then-write sequence.  Any interruption (power loss, crash) during
that window leaves the board permanently unbootable.

Writing to Slot B is always safe — Slot B is never load-bearing for the boot chain.
Writing to Slot A while booted from Slot B is unsafe by design: a partial write
bricks the board with no recovery path short of SWD reprogramming.

**Confirmed safe partial-update pattern (May 2026):**
With both slots valid and selector on Slot B, erasing only the application portion
of Slot A (0x28000–0xFFFFF, leaving 0x00000–0x27FFF intact) does not prevent boot.
The board continues to boot Slot B's application via Slot A's intact boot chain.

This was confirmed with `erase_app.bat 0` while booted from Slot 1:
- `dump_flash.bat` + `check_slots.bat` verified the boot chain was preserved
- Board booted normally after the erase

**Fix implemented in `firmware` CLI command (May 2026):**

Key constant in `xip_manager.c`:
```c
#define FLASH_APP_OFFSET  0x28000   // cm55m_application start (dpd layout)
```

The erase/write/verify behaviour depends on which slot is the target:

**Target = Slot A** (board running from Slot B):
- Erase: from `FLASH_APP_OFFSET` only — boot chain at 0x00000–0x27FFF preserved
- Erase uses two phases (0x28000 is not 64 KB-aligned):
  - Phase 1: 8 × 4 KB sector erases  (0x28000–0x2FFFF)
  - Phase 2: 13 × 64 KB block erases (0x30000–0xFFFFF)
- Write: `f_lseek` to `FLASH_APP_OFFSET` in file, write to `0x00028000` in flash
- Verify: `f_lseek` to `FLASH_APP_OFFSET` in file, read from `0x00028000` in flash

**Target = Slot B** (board running from Slot A):
- Erase: full 1 MB (16 × 64 KB blocks from 0x00100000)
- Write: full image from file byte 0 to flash `0x00100000`
- Verify: full image from file byte 0 vs flash `0x00100000`
- Reason: the 2nd_bootloader (always from Slot A) reads Slot B's
  `hx_mem_descriptor_ota` at 0x00127000 to locate the application.
  If this is absent (0xFF), the board will not boot from Slot B even
  though Slot A and the selector are intact.

The CRC check in `prvFirmwareCommand()` (CLI-FATFS-commands.c) covers the entire
file before any flash operation — unchanged.

The Himax-supplied boot chain blobs at 0x00000–0x27FFF appear invariant across
application builds using the same SDK.  SDK updates that change the bootloader
require SWD reprogramming via `burn.bat`.

### Diagnosis of the faulty board (May 2026)

Running `check_slots.bat` on a board dump showed:

**Slot A — truncated (incomplete firmware write):**
- Only 8 KB of payload present (0x0000–0x1FFF)
- Contains only the hx_memory_descriptor (0x0–0xFFF) and the BLP header of
  the 2nd_bootloader (0x1000–0x1FFF); body of 2nd_bootloader is absent
- Bootloader, OTA descriptor, and application are entirely absent (0xFF)
- Preamble diverges from Slot B at 0x2000 (Slot A is all-0xFF from there)
- Cause: aborted firmware update — flash erased but only the first two 4 KB
  sectors written before the process stopped

**Slot B — structurally normal, older build, NOT corrupted:**
- 445 KB of payload; all six BLP packets present at correct dpd offsets
- Application p1 ~276 KB + p2 ~8 KB; board selector pointing to Slot B
- Byte-comparison against the current dpd `output.img` shows 187,242 bytes
  differ — 66% of the application payload is different
- Pre-built Himax blobs (2nd_bootloader body 73 KB, bootloader body 81 KB)
  are **byte-for-byte identical** to the reference, confirming the flash is
  readable and the SWD dump is accurate
- Slot B build identified as `autoexposure` branch, commit `cc2d49eb`,
  built 17:49:08 May 15 2026 — confirmed by `strings dump_slot_b.bin`
- Byte-comparison against `bde60f57`'s `output.img` (retrieved with
  `git show bde60f57:we2_image_gen_local_dpd/.../output.img`): **zero
  differences** — the flash content is a perfect copy of that commit
- **Root cause of apparent board death:** the `cc2d49eb` firmware
  reassigns PB7 (SWCLK) to a GPIO function after boot, breaking SWD.
  The board was running fine; it only appeared dead because SWD was the
  sole diagnostic tool.  Combined with truncated Slot A, the bootloader
  selected Slot B, booted it, PB7 was immediately reassigned, and no
  further SWD connection was possible.
- **Confirmed not a hardware fault:** the `cc2d49eb` / `bde60f57` image
  was programmed to a known-good board using `burn.bat` and booted
  correctly, proving the firmware itself is valid.

**Identifying the Slot B build from embedded strings:**

The firmware image embeds build-time strings in .rodata that can be read
with `strings dump_slot_b.bin`.  Mapping them against the source:

```c
xprintf("**** WW500 MD. (%s) Built: %s %s ****", app_get_board_name_string(), __TIME__, __DATE__);
xprintf("Git branch: '%s' %s%s\n", GIT_BRANCH, GIT_COMMIT, GIT_DIRTY);
xprintf("Compiler Version: ARM GNU, %s\n\n", __VERSION__);
```

| String found in binary | Source macro / function          |
|------------------------|----------------------------------|
| `WW500_C02`            | `app_get_board_name_string()`    |
| `17:49:08`             | `__TIME__`                       |
| `May 15 2026`          | `__DATE__`                       |
| `autoexposure`         | `GIT_BRANCH`                     |
| `cc2d49eb`             | `GIT_COMMIT`                     |
| (absent)               | `GIT_DIRTY` — empty, clean tree  |
| `14.3.1 20250623`      | `__VERSION__`                    |

Confirmed against git log: `cc2d49eb` ("Fixed merge conflict to merge to dev",
2026-05-15 17:44:34) was the HEAD of the `autoexposure` branch five minutes
before the build.  The following commit on that branch (`bde60f57`,
"Rebuilt output.img", 17:52:38) came after the build.

To search for a build by timestamp or commit in future:
```
strings dump_slot_X.bin | grep -A5 "Git branch:"
git log --all --format="%h %ai %D %s" | grep 2026-05-15
git branch --all --contains <hash>
```

**Current board state (as of 2026-05-25):**
- `burn.bat output.img` completed successfully — Slot A now contains current firmware
  (the `main` branch build, boots and prints correct version string)
- Selector sector still points to **Slot B** (`cc2d49eb`, PB7-reassigning build)
- The board boots the Slot B firmware; once it reaches application code, PB7 is
  reassigned and SWD becomes inaccessible for that power cycle

**Outstanding step:** run `set_sel.bat 0` using the hold/release reset procedure
to switch the selector to Slot A.  The 0.5s retry delay (see above) is required
because `cc2d49eb` reassigns PB7 quickly after boot.  This has not yet been
successfully completed — attempt at session end failed; reduced retry delay (2s→0.5s)
was applied but not yet tested.

## Scripts and batch files

| Batch file              | Python script       | Slot | File arg | What it does                                                |
|-------------------------|---------------------|------|----------|-------------------------------------------------------------|
| `burn.bat [file]`       | —                   | A    | optional | Program file (default `output.img`) into Slot A            |
| `burn_slot.bat N file`  | `burn_slot.py`      | 0/1  | required | Program any file into Slot A or Slot B                      |
| `erase_slot.bat N`      | `erase_slot.py`     | 0/1  | —        | Erase entire 1 MB slot to 0xFF                              |
| `erase_app.bat N`       | `erase_app.py`      | 0/1  | —        | Erase only application (0x28000–end); preserves boot chain  |
| `dump_flash.bat`        | —                   | both | —        | Dump Slot A, Slot B, selector to .bin files                 |
| `set_sel.bat N`         | `set_selector.py`   | 0/1  | —        | Write selector sector: 0=Slot A, 1=Slot B                   |
| `parse_sel.bat [file]`  | `parse_selector.py` | —    | optional | Decode and validate a selector sector dump                  |
| `check_slots.bat [a] [b]` | `check_slots.py`  | both | optional | Structural check of two 1 MB slot dumps                    |

`N` is always `0` for Slot A or `1` for Slot B.

### Experiment procedure (boot-chain hypothesis test)

To reproduce the dead-board state and test the boot-chain hypothesis:

```
set_sel.bat 1            # switch selector to Slot B
erase_slot.bat 0         # erase Slot A to all 0xFF  (prediction: board still boots Slot B)
burn_slot.bat 0 dump_slot_a.bin   # write the old truncated image to Slot A
                         #                             (prediction: board now fails to boot)
```

`dump_slot_a.bin` is the truncated image read from the originally-faulty board
(8 KB of data, remainder 0xFF — hx_memory_descriptor + partial 2nd_bootloader header).

## Other files

| File                            | Description                                        |
|---------------------------------|----------------------------------------------------|
| `swdflash.py`                   | Main pyOCD script (Himax base + dump extensions)   |
| `output.img`                    | Current firmware image ready to flash              |
| `dump_slot_a.bin`               | Last dump of Slot A                                |
| `dump_slot_b.bin`               | Last dump of Slot B                                |
| `dump_selector.bin`             | Last dump of selector sector                       |
| `FlashAlgo_sck_equ_hclk_div16/` | Flash algorithm blob used by FlashAlgoDB method   |
| `_version.py`                   | Script version string                              |
