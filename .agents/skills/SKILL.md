---
name: ww-firmware-agent
description: >
  Essential workflow, repository awareness and guardrails for any agent working on the
  Wildlife Watcher WW500 firmware (Himax WiseEye2 / Seeed Grove Vision AI V2). Read and
  follow this skill before changing code, building, flashing hardware, or writing
  documentation in this repository.
---

# Wildlife Watcher Firmware Agent

This repository holds the AI-processor (HX6538) firmware. The production application is
`EPII_CM55M_APP_S/app/ww_projects/ww500_md`, built in **two camera variants** that live in
the device's A/B flash slots: `cis_imx708` (RP3 day/colour) and `cis_hm0360` (HM0360
night/IR). The BLE relay processor lives in the separate `ww-hardware` repo; the app,
website and backend consume this firmware's EXIF fields, op-parameters and BLE commands.

> **Canonical documentation** — consult before assuming:
>
> * `_Documentation/building_firmware.md` — toolchain (Arm GNU 14.3.rel1), two-variant build, image generation
> * `_Documentation/firmware_update_and_recovery.md` — update paths, XMODEM recovery, slot model
> * `_Documentation/Operational_Parameters.md` + `MANIFEST/config_file.md` — op-parameter meanings and defaults
> * `_Documentation/ble_commands.md` — console/BLE command surface (the app speaks `AI <cmd>`)
> * `EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/` — subsystem notes (slot_selector.md, FatFS behaviour, …)
> * `REVIEW_PR<N>.md` at a branch tip — offline copy of that PR's description
> * `_Documentation/development reports/` — **how the code got this way**; check the relevant
>   thread before re-deriving or re-litigating a past decision

# 1. Development conversations and documentation

**The rules live in [`_Documentation/development reports/README.md`](../../_Documentation/development%20reports/README.md).
Read it before starting or closing a thread.** In short: docs are the record, GitHub
issues are the tracker (project board: `https://github.com/orgs/wildlifeai/projects/3`,
auto-add is enabled for this repo); every thread README carries Status, Outcome and Open
items; and threads record *how the work happened*, not how the code works.

What that means for an agent, beyond reading the rules:

* **Never leave substantive material only in a chat transcript, email or PR comment.** An
  investigation, review exchange or design discussion belongs in a dated thread under
  `_Documentation/development reports/YYYY-MM-DD_short-description/`. This is the failure
  mode to watch for: the work is done, the finding is real, and it evaporates because it
  only ever existed in a conversation.
* **Two homes, and do not confuse them.** How the firmware behaves now goes in the durable
  docs (`_Documentation/*.md`, `ww500_md/doc/*.md`); how it got that way goes in the
  thread. When something is agreed, update both in the same change: the topic doc gets the
  "what", the thread's Outcome gets the "why".
* **Never edit a thread to keep it true.** Threads are an append-only audit trail. If
  behaviour changes, the durable doc changes; the thread stays as the record of what was
  believed and decided at the time.
* **Open items are GitHub issue links, nothing else.** A document must never be the only
  place an open item lives. File with the `review-finding` template. Before closing a
  thread, check its issues are actually still open work: an issue already fixed and merged
  reads as available work and wastes someone's afternoon.
* **Keep hardware evidence.** Bench and serial logs supporting a claim belong in the
  thread's `logs/` folder, referenced from the write-up.

# 2. Git guardrails

* **Never rebase or force-push** the stacked feature branches — the stack breaks and
  reviewers' diff bases move.
* Review edits go on `review/<name>-<topic>` branches, not directly on shared feature
  branches; they are cherry-picked across after discussion.
* Ask the maintainer before pushing to any shared branch. Commit messages use
  conventional prefixes (`feat:`, `fix:`, `docs:`, `ci:`).
* **Never commit build churn**: `prebuilt_libs/**/*.a` deltas, `we2_image_gen_local*/`
  outputs, stray `NUL` files, `obj_*` trees. Check `git status` before staging.

# 3. Build and flash invariants

* Toolchain is pinned: **Arm GNU 14.3.rel1**. Build under WSL/Linux;
  `make clean` between camera variants is **mandatory** (objects don't encode the `-D`
  flags). Both variants must build — a change that compiles for one only is broken.
* **`make` runs image generation itself** (`ww500_md/mk/image_gen.mk`, RC24M profile) and
  writes both `output_case1_sec_wlcsp/output.img` and an 8.3 `VYMDDHMM.IMG` copy named for
  the variant (`R`/`H`). Do **not** run `we2_local_image_gen` by hand, it destroys the
  image make just built. No renaming between variants is needed.
* **A failed secure-boot certificate step does not fail the build**, and size only catches
  it on one variant. RP3 goes 487424 signed to 462848 certless; **HM0360 is 462848 either
  way**. So 462848 is both a good HM0360 image and an unsigned RP3 one. Check the build log
  for the `FileNotFoundError` traceback and that `secureboot_tool/cert/ICVSBContent.crt`
  exists. Never gitignore `secureboot_tool/cert/cfg/*.cfg`: they are hand-authored build
  inputs and nothing regenerates them.
* SD-card firmware files: **8.3 filenames** in `/MANIFEST` (FatFS has no LFN support).
  The `VYMDDHMM.IMG` name make emits already satisfies this.
* Device consoles: two USB serial ports — the Himax console is the one printing clean
  text at **921600 baud**; the other is the BLE debug UART. Probe for it every session
  (§5), never hard-code the COM number.
* **X-Modem is a normal bench path, not only recovery**, the way to get locally built
  images onto a device with no SD card. Each burn writes the **backup** slot and makes it
  active, so two consecutive burns fill both slots, and a final `switchslot` re-labels the
  one left behind. Runbook: `_Documentation/firmware_update_and_recovery.md`; the script
  timing that matters is in §5.

# 4. Hardware behaviour that will trap you

Verified on the bench (details + serial evidence in
`_Documentation/development reports/2026-08-06_pr141-camera-features-review/`):

* **Slot labels self-heal at first boot** — flashing clears the target slot's label to
  `unknown`; each image labels its own slot on every boot. `slots` showing `unknown` for
  a never-booted slot is designed behaviour. Never gate the labelling call on cold boot.
* **Deliberate reboots are deferred watchdog resets** (`reset`, `switchslot`,
  auto-switch): they execute at the next sleep and the following boot classifies as a
  **cold** boot (PMU wakeup registers read zero).
* **Cold-boot IMX708 first captures fail, full stop**. Every in-place retry times out
  (`Frame timed out - restarting sensor, retry n/5`) and the image task then goes
  Uninitialised. The progressive-dwell retry does not rescue it. DPD-wake captures are
  reliable and take ~52 ms, so **always get past one wake cycle before believing a capture
  or light-sensor result**. Cheapest way in: `setop 7 1` (timelapse a minute), wait for
  `Wakeup_event = 0x0002 ... RTC Timer`, test, then `setop 7 0`.
* **Console sessions**: an untouched boot sleeps after ~1 s; most commands hold the
  device awake ~60 s; the `reset` command deliberately does not. Scripting against this
  has its own rules, see §5.
* **The periodic AE light check only runs if something consumes it.**
  `aeCheckRequired = lightSensor_isRequired()`, which is true when the flash mode is
  `FLASH_MODE_AE` (**op34**, not op13) or automatic camera switching (op26) is on. With
  both off, captures come and go with no light check at all. The `light` command is the
  exception and always forces a reading. When it is on, the device also wakes every op24
  minutes (15 by default) to take a throwaway frame and read the AE registers, which is a
  battery cost worth knowing about before enabling it.
* **camreg staged registers** (`RPV3_EX.BIN` etc.) are re-applied after the init tables
  at every sensor init — they override defaults, persist on SD, and survive DPD.

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

Windows shell, unrelated to the hardware but the same class of silent failure:

* **`MSYS_NO_PATHCONV=1`** for git revspecs (`origin/dev:path`) and for `/tmp` paths passed
  to WSL. MSYS rewrites them into Windows paths and git then reports "not a valid object
  name" about a path you never typed.
* **Write multi-line WSL scripts to a file** and run `wsl bash /tmp/x.sh`. Passing them as
  `wsl -- bash -c '...'` mangles them: variables arrive empty, the script runs in the wrong
  directory, and it still exits 0.

# 6. Cross-repo contracts

EXIF fields (Model, MakerNote), op-parameter indices/defaults, and BLE command syntax are
consumed by ww-mobile-app, ww-website and ww-backend, and mirrored in ww-hardware's
`aiProcessor.h`. Never change one unilaterally: file a `Decide:` issue, agree the
contract, and land the change with the consumers in view.
