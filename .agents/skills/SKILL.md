---
name: seeed-ww500-firmware
description: Working rules, build and flash invariants, hardware traps and cross-repo contracts for the Wildlife Watcher HX6538 AI-processor firmware (ww500_md, two camera variants, A/B slots). Read before changing code or docs in this repo.
---

# Wildlife Watcher firmware, working knowledge

Read [`AGENTS.md`](../../AGENTS.md) first for the quickstart. This file is the layer
underneath: the rules that apply to any change, and a map to the detail. The reference files
hold the things that have already cost someone a day, so read the one that matches your task
rather than all of them.

This repository holds the AI-processor (HX6538) firmware. The production application is
`EPII_CM55M_APP_S/app/ww_projects/ww500_md`, built in **two camera variants** that live in
the device's A/B flash slots: `cis_imx708` (RP3 day/colour) and `cis_hm0360` (HM0360
night/IR). The BLE relay processor lives in the separate `ww-hardware` repo; the app,
website and backend consume this firmware's EXIF fields, op-parameters and BLE commands.

> **Canonical documentation.** Consult before assuming:
>
> * `_Documentation/building_firmware.md`: toolchain (Arm GNU 14.3.rel1), two-variant build, image generation
> * `_Documentation/firmware_update_and_recovery.md`: update paths, XMODEM recovery, slot model
> * `_Documentation/Operational_Parameters.md` + `MANIFEST/config_file.md`: op-parameter meanings and defaults
> * `_Documentation/ble_commands.md`: console/BLE command surface (the app speaks `AI <cmd>`)
> * `EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/`: subsystem notes (slot_selector.md, FatFS behaviour, …)
> * `REVIEW_PR<N>.md` at a branch tip: offline copy of that PR's description
> * `_Documentation/development reports/`: **how the code got this way**; check the relevant
>   thread before re-deriving or re-litigating a past decision

## Read this one next

| If you are touching | Read |
|---|---|
| A branch, a worktree, the upstream, a build or a flash | [references/git-and-build.md](references/git-and-build.md) |
| Anything that behaves oddly on real hardware | [references/hardware-traps.md](references/hardware-traps.md) |
| A reproduction, a bench script, the logger or flashing a batch of boards | [references/bench.md](references/bench.md) |
| An op parameter, a command string, an EXIF field, a self-test bit | [references/cross-repo-contracts.md](references/cross-repo-contracts.md) |
| Documentation, a development report, or your own commit hygiene | [references/documentation.md](references/documentation.md) |

## Workflow

- **Never commit or push to a shared branch without asking the maintainer.**
- **Check the agent layer before each commit.** Decide whether this change makes anything in
  `AGENTS.md`, this file or a reference file wrong, missing or redundant, and fix it in the
  same commit. The three questions are in
  [references/documentation.md](references/documentation.md).
- **This is a fork.** `main` tracks upstream; our work lands on `dev`. Never push to upstream,
  and never assume a file is ours because it is in the tree.
- **Build both variants when you touch shared code.** A change that compiles for `cis_imx708`
  can fail or silently change behaviour for `cis_hm0360`, and vice versa.
- **Reproduce on hardware before you file or fix.** A console log from the device beats any
  amount of reading, and this repo's own history is full of theories that the bench disproved.

## The five that apply to almost any change

1. **Two camera variants, two flash slots.** Know which one you are building, which one is
   active on the device, and never assume the device matches your worktree. `AI ver` and
   `AI slots` first.
2. **Op parameter indices are a three-way contract** with the mobile app and the nRF firmware.
   Never renumber unilaterally.
3. **A dropped or refused request is often silent.** The firmware logs to its console and
   returns, so the app waits on something that will never arrive. If you add a failure path,
   make it tell the app.
4. **The inactivity detector counts a waiting capture as idle**, so a stalled operation puts
   the device into deep power down rather than reporting an error.
5. **Say which firmware you tested.** `AI ver` names the build; a claim without it is not
   evidence.
