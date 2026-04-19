# Updating the Firmware on the WW500 Board

This guide explains how to install a new firmware image onto the WW500 board using
the built-in CLI commands.

## Overview

The board's external flash chip has two firmware slots, **Slot A** and **Slot B**,
each 1 MB in size.  The bootloader reads a small selector record to decide which slot
to execute at power-on.

Using two slots provides a safety net: the update process always writes to the
**inactive** slot and only switches the selector once the new image has been written
and verified.  If anything goes wrong during the update, the board will continue to
boot the existing firmware from the other slot.

## Preparing the Firmware File

The firmware image is produced by the compiler build process.  After a successful
build, the output file is typically named **`output.img`** and is located in the
build output directory of the Eclipse/GNU toolchain project.

## Installing the Update

1. Copy `output.img` (or rename it to any short filename ≤ 31 characters) onto the
   SD card inside the **`/MANIFEST`** folder.

2. Insert the SD card into the board and power it on (or reset it).

3. At the CLI prompt, type the `firmware` command followed by the filename:

   ```
   firmware output.img
   ```

   The command will:
   - Locate the current active slot
   - Erase the inactive slot
   - Write the new image chunk by chunk, verifying each chunk as it is written
   - Perform a full read-back verification of the entire slot
   - Update the boot selector to point to the newly written slot

   Progress messages are printed to the console throughout.  The command returns
   an error message and leaves the existing firmware untouched if any step fails.

4. Once the `firmware` command reports success, reboot the board with:

   ```
   reset
   ```

   The bootloader will read the updated selector and execute the new firmware from
   the slot that was just written.

## Notes

- The image file must fit within 1 MB (the slot size).
- The `/MANIFEST` folder is the same directory used for NN model files.
- If the update is interrupted before the selector is written (e.g. power loss
  during the write), the board will continue to boot the previous firmware.  Simply
  repeat the `firmware` command to retry.
- To inspect the raw slot selector record currently stored in flash, use the
  diagnostic command:

  ```
  dump-sel
  ```

  This prints the first 32 bytes of the selector sector to the console.  The
  `flash_offset` field shows which slot the bootloader will execute next:
  `0x00000000` is Slot A, `0x00100000` is Slot B.
