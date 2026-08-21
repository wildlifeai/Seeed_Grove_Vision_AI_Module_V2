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
* Image generation uses `we2_image_gen_local_dpd` with the **RC24M** profile; both
  variants emit the same `output.img` path — rename between runs.
* SD-card firmware files: **8.3 filenames** in `/MANIFEST` (FatFS has no LFN support).
* Device consoles: two USB serial ports — the Himax console is the one printing clean
  text at **921600 baud**; the other is the BLE debug UART.

# 4. Hardware behaviour that will trap you

Verified on the bench (details + serial evidence in
`_Documentation/development reports/2026-07_pr141-review-cgp/`):

* **Slot labels self-heal at first boot** — flashing clears the target slot's label to
  `unknown`; each image labels its own slot on every boot. `slots` showing `unknown` for
  a never-booted slot is designed behaviour. Never gate the labelling call on cold boot.
* **Deliberate reboots are deferred watchdog resets** (`reset`, `switchslot`,
  auto-switch): they execute at the next sleep and the following boot classifies as a
  **cold** boot (PMU wakeup registers read zero).
* **Cold-boot IMX708 first captures are flaky** (instant retries all fail); DPD-wake
  captures are reliable — prefer wake-path captures for bench validation.
* **Console sessions**: an untouched boot sleeps after ~1 s; most commands hold the
  device awake ~60 s; the `reset` command deliberately does not.
* **camreg staged registers** (`RPV3_EX.BIN` etc.) are re-applied after the init tables
  at every sensor init — they override defaults, persist on SD, and survive DPD.

# 5. Cross-repo contracts

EXIF fields (Model, MakerNote), op-parameter indices/defaults, and BLE command syntax are
consumed by ww-mobile-app, ww-website and ww-backend, and mirrored in ww-hardware's
`aiProcessor.h`. Never change one unilaterally: file a `Decide:` issue, agree the
contract, and land the change with the consumers in view.
