# Firmware Image Slot Selector
#### CGP - 17 April 2026

## Top-level design

Himax writes:

When performing a firmware upgrade using XMODEM, it is necessary to consider the
possibility of an update failure. In such a case, the system must still be able to boot and
operate normally using the previous firmware version.

The implementation approach is to divide the flash memory into two partitions:
Slot A and Slot B . 

The bootloader determines whether to execute the Slot A or Slot B
application at boot time based on a pointer stored in the last sector of the flash memory

## Contents of the selector sector

A new CLI command `write-sel` reads the final sector and writes it as a binary file to the SD card.
This has allowed me to check what is in it, and how it changes as different images are written.

Most of the sector contains 0xFF (the erased state). Only the first 20 bytes contain data.

The bootloader gives an indication of which slot is in use. The following contains the bootloader 
text and the selector sector data.

The `write-sel` command and the function it called has been replaced by `dump-sel` and the function has been modified, 
to simply print the first 32 bytes to the console.

I programmed the firmware images into the XIP memory using the existing XMODEM mechanism, 
then recorded the bootloader output and the contents of the slot selector sector. The results are shown below.

#### Slot A

```
1st BL Modem Build DATE=Jan 17 2025, Version: 2.12
Please input any key to enter X-Modem mode in 30 ms
waiting input key...
slot flash_offset 0x00000000
New MemDesp himax_sec_SB_image_process PASS
set_memory_s_ns
HX_DSP_FLAG 1
jump_addr=0x10000000
Compiler Version: ARM GNU, 14.3.1 20250623
```

Hex dump of slot selector sector

```
Slot selector (0x00fff000, first 32 bytes):
000: 48 49 4d 41 58 57 45 32  00 00 00 00 02 00 00 00 HIMAXWE2........
010: 01 00 04 4d ff ff ff ff  ff ff ff ff ff ff ff ff ...M............
```

The first 8 bytes are ASCII `HIMAXWE2`

Subsequent values are possibly (in LE format):
* slot flash_offset 0x00000000
* some value 0x00000002
* HX_DSP_FLAG 1 (0x0001)
* Checksum 0x4d04

#### Slot B

```
1st BL Modem Build DATE=Jan 17 2025, Version: 2.12
Please input any key to enter X-Modem mode in 30 ms
waiting input key...
slot flash_offset 0x00100000
New MemDesp himax_sec_SB_image_process PASS
set_memory_s_ns
HX_DSP_FLAG 1
jump_addr=0x10000000
Compiler Version: ARM GNU, 14.3.1 20250623
```

Hex dump of slot selector sector

```
Slot selector (0x00fff000, first 32 bytes):
000: 48 49 4d 41 58 57 45 32  00 00 10 00 02 00 00 00 HIMAXWE2........
010: 01 00 7c 16 ff ff ff ff  ff ff ff ff ff ff ff ff ..|.............
```

The first 8 bytes are ASCII `HIMAXWE2`

Subsequent values are possibly (in LE format):
* slot flash_offset 0x00100000
* some value 0x00000002
* HX_DSP_FLAG 1 (0x0001)
* Checksum 0x167C

## Wildlife Watcher slot metadata record (added with camera switching)

The dual-image camera switching feature (`camera_switch.c`, `xip_manager.c`) stores a small
metadata record in the spare (0xFF) bytes of this same sector, at byte offset 32 - after the
bootloader's 20-byte header, which the bootloader ignores:

```
offset 32:  'W' 'W' 'S' 'M'   magic
offset 36:  variant of Slot A (1 byte)
offset 37:  variant of Slot B (1 byte)
offset 38:  reserved (2 bytes, 0xFF)
```

Variant values: 0 = unknown / just rewritten, 1 = HM0360 (night/IR image), 2 = RP3 (day/colour
image). Each firmware image labels its own slot at boot (`cameraSwitch_labelBootSlot()`), and
`write_slot_selector()` preserves the record across sector rewrites. The `slots` CLI command
reports it; the automatic switching logic will only flip to a slot whose recorded variant
matches what is wanted.
