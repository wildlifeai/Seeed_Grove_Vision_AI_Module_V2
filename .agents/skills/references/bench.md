# Driving the bench from a script

How to reproduce on real hardware rather than reasoning about it, and the traps in the
instrumentation itself.

# 5. Driving the bench from a script

Section 4 is what the hardware does; this is what your script must do about it. Each of
these has cost a session, and a human at a terminal meets almost none of them. Knowing
the fact is not enough, the timing has to be built in.

* **Send the first byte the instant you see `Starting CLI Task`**, before any drain,
  sleep, or banner parsing. The console sleeps ~1 s after the boot chatter stops (§4) and
  one keystroke raises it to the ~60 s CLI window. Miss it and every command returns
  nothing, which looks exactly like a dead port rather than a sleeping device.
* **Arm the script first, then ask for the reset.** The device does not wake on serial
  input, so opening the port and sending has already lost. Wait for the boot banner.
* **Probe for the console port every session; never hard-code it.** It moves between
  adapters. The wrong one is the BLE UART at a different baud, which returns plausible
  garbage rather than silence, so you cannot tell by whether bytes arrive.
* **Log "no response" explicitly** after each command, so a sleeping device is
  distinguishable from a quiet one when you read the transcript back.
* **Send a `\r\n` keepalive between commands** in long sequences.
* **X-Modem: start `xmodem_send.py` first, then reset.** It drives the handshake itself
  and prints `Please press reset button!!` when it wants the reset. The ROM bootloader
  listens for only 30 ms, so a script must be sending already: stream `1` continuously,
  as `_Tools/ww500_ship_check.py` does. After a burn, answer `Do you want to end file
  transmission and reboot system? (y)` with `y` while still streaming and the next window
  is caught too, so two images need one RESET press.
* **For a batch of boards, use `_Tools/ww500_ship_check.py`** (runbook
  `_Documentation/pcb_batch_flashing.md`): both images, a photo from each camera, one
  button per board.
* **`PYTHONIOENCODING=utf-8` for any serial or flashing tool.** `xmodem_send.py`'s
  progress bar uses a block character cp1252 cannot encode, and the exception lands
  **mid-flash**. Re-running recovers, since the bootloader is in a separate flash region.
* **To command a sleeping device from a script, drive the app's Engineer Console over
  adb**: `adb shell input text` (spaces as `%s`), wait about 1.5 s for the text to land,
  then tap send. It wakes the device, and its typed line bypasses the app's queue, so it
  can land mid-transfer when a test needs that. The device never wakes on serial input.
* **While the device is awake, the Himax console works from a script**, with three rules:
  one character every 30 ms (the receive buffer holds one), end with `\r\n` because the CLI
  runs a command on `\n` only (a bare `\r` does nothing, which looks like a dead console),
  and send Ctrl-C first to clear stray characters. Open the port with DTR and RTS held low:
  pyserial's default DTR reset the bench board's adapter, although neither line reset the
  PCBs flashed on 25 September 2026.
* **Preview frames (`preview 1`) arrive with other prints inside them.** On the HM0360
  image the report to the BLE processor lands in every frame, so parsing each line as JSON
  drops them all. Rebuild the JPEG from the base64 runs of 100+ characters between
  `"image": "` and `"}}` and keep it only if it fully decodes (`ww500_ship_check.py`).
* **Three-way logging** (`bench_log.py`, light sensor thread) is what makes a cross-processor
  finding provable: app over `adb logcat`, nRF and Himax consoles in one file. Its stamps are
  read time and the nRF flushes its deferred log in bursts, so order events by the Himax
  lines. Strip NULs (`tr -d '\000'`) from any excerpt before committing it, or git stores it
  as binary.

Windows shell, unrelated to the hardware but the same class of silent failure:

* **`MSYS_NO_PATHCONV=1`** for git revspecs (`origin/dev:path`) and for `/tmp` paths passed
  to WSL. MSYS rewrites them into Windows paths and git then reports "not a valid object
  name" about a path you never typed.
* **Write multi-line WSL scripts to a file** and run `wsl bash /tmp/x.sh`. Passing them as
  `wsl -- bash -c '...'` mangles them: variables arrive empty, the script runs in the wrong
  directory, and it still exits 0.
