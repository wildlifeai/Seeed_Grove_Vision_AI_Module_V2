# Hardware behaviour that will trap you

The device does things that look like bugs in your code. Each of these has cost somebody a
session.

# 4. Hardware behaviour that will trap you

Verified on the bench (details and serial evidence in
`_Documentation/development reports/2026-08-06_pr141-camera-features-review/`):

* **Slot labels self-heal at first boot**, flashing clears the target slot's label to
  `unknown`; each image labels its own slot on every boot. `slots` showing `unknown` for
  a never-booted slot is designed behaviour. Never gate the labelling call on cold boot.
* **Deliberate reboots are deferred watchdog resets** (`reset`, `switchslot`,
  auto-switch): they execute at the next sleep and the following boot classifies as a
  **cold** boot (PMU wakeup registers read zero).
* **Cold-boot IMX708 first captures fail, full stop**. Every in-place retry times out
  (`Frame timed out - restarting sensor, retry n/5`) and the image task then goes
  Uninitialised. The progressive-dwell retry does not rescue it. DPD-wake captures are
  reliable and take ~52 ms, so **always get past one wake cycle before believing a capture
  or light-sensor result**. Cheapest way in: `setop 7 1`, wait for
  `Wakeup_event = 0x0002 ... RTC Timer`, test, then `setop 7 0`. **op7 is in seconds**, so
  that is a one-second timelapse: the device will capture repeatedly and faster than a
  script polling `getop` can follow, which reads as a counter jumping by two.
* **Console sessions**: an untouched boot sleeps after ~1 s; most commands hold the
  device awake ~60 s; the `reset` command deliberately does not. Scripting against this
  has its own rules, see §5.
* **`CONFIG.TXT` is also `STATE_FILE`** (`directory_manager.h`), so the device rewrites it
  on the first sleep and after every `setop`. **A card you prepared stops being that card
  before you can read the result**, and the file you then find is the device's own: all 37
  parameters, comments hoisted to the top, LF endings, RTC-unset timestamp. To test how a
  particular file parses, set the **FAT read-only attribute** on it, which the firmware
  respects, and the card survives the boot intact. Two bench runs were spent measuring the
  device's own file before this was understood.
* **The periodic AE light check only runs if something consumes it.**
  `aeCheckRequired = lightSensor_isRequired()`, which is true when the flash mode is
  `FLASH_MODE_AE` (**op34**, not op13) or automatic camera switching (op26) is on. With
  both off, captures come and go with no light check at all. The `light` command is the
  exception and always forces a reading. When it is on, the device also wakes every op24
  minutes (15 by default) to take a throwaway frame and read the AE registers, which is a
  battery cost worth knowing about before enabling it.
* **camreg staged registers** (`RPV3_EX.BIN` etc.) are re-applied after the init tables
  at every sensor init, they override defaults, persist on SD, and survive DPD.

Verified 3 and 4 September 2026 (`2026-09-03_capture_bench_findings/`, `ae_review`
e8b7feb5 and nRF 0.30.48). All were open issues when written; check the issue before
building on any of them:

* **The inactivity detector measures idle time only** (the FreeRTOS idle hook), and a capture
  waiting for a frame is idle. A multi-image capture with a gap above op8 is abandoned in
  DPD and `Captured` never comes; the IF task sends `Sleep` and completes the shutdown
  barrier on its own, because the barrier counts calls, not tasks (#208).
* **A command that reaches the Himax between Save State and DPD strands it awake** until a
  power cycle (#205): the IF task drops the inactivity event while transmitting. An ordinary
  wake-then-command can do it. In that state the nRF parks in SELFTEST and drops every app
  command. A `setop` in the same window is acknowledged and never saved (#207).
* **The nRF forwards any command mid-`txfile` and restarts its packet counter** (ww-hardware
  #33); **its console hex dump holds the download to about 1 KB/s** while its upload path is
  already gated quiet (#34); **`Failed to send` on its console is normal back-pressure**
  (#35); **the app's loopback benchmark never echoes** (#36).
* **The bench nRF runs ww-hardware `dev`, not `main`.** `ver` reports the nRF build, `AI ver`
  the Himax build; cite nRF line numbers from `dev`. **The device lags the branch**: it was
  flashed at 0.30.48 (75406df) and moves only when someone runs the app's firmware update,
  most recently to 0.30.51 on 18 September. Read the version off the device rather than
  assuming it matches the tip.
* **Releasing nRF firmware takes two workflows, in order:** **Build BLE Firmware** compiles
  and signs at whatever `version.mk` declares and opens a PR with the `.zip` and `.hex`, then
  **Upload BLE Firmware to Supabase** publishes it. Editing `version.mk` alone ships nothing:
  the upload silently falls back to the newest zip present and takes the version from *its*
  filename, so a bump with no build republishes the old image under the old number and then
  fails on a duplicate key. The version is compiled into the image and written into the DFU
  package, so renaming a zip is never a shortcut. nRF5 SDK 16.0.0 needs **GCC 10.3.1**; 12
  and newer fail on `-Werror=array-bounds` in `nrf_section.h`.
