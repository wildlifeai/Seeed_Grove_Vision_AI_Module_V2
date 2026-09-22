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
  and prints `Please press reset button!!` when it wants the reset.
* **`PYTHONIOENCODING=utf-8` for any serial or flashing tool.** `xmodem_send.py`'s
  progress bar uses a block character cp1252 cannot encode, and the exception lands
  **mid-flash**. Re-running recovers, since the bootloader is in a separate flash region.
* **To send commands from a script, drive the app's Engineer Console over adb**, not the
  Himax console: `adb shell input text` (spaces as `%s`), wait about 1.5 s for the text to
  land, then tap send. It wakes a sleeping device, and its typed line bypasses the app's
  queue, so it can land mid-transfer when a test needs that. Opening the Himax port with
  pyserial's default DTR resets the board, and the device never wakes on serial input.
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
