# Agent guide, Wildlife Watcher WW500 firmware

AI-processor (Himax HX6538 / Seeed Grove Vision AI V2) firmware for the Wildlife Watcher
WW500 camera. Production app: `EPII_CM55M_APP_S/app/ww_projects/ww500_md`, built in two
camera variants that occupy the device's A/B flash slots.

**Before changing code, building, flashing or writing docs, read
[`.agents/skills/SKILL.md`](.agents/skills/SKILL.md)**: the workflow rules, the five that
apply to almost any change, and a map to the reference files that carry the detail (git and
build, hardware traps, the bench, cross-repo contracts, documentation). This file is only
the quickstart.

## Build (WSL/Linux, toolchain pinned to Arm GNU 14.3.rel1)

```bash
cd EPII_CM55M_APP_S
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_imx708   # RP3 day/colour
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_hm0360   # HM0360 night/IR
```

`make clean` between variants is mandatory; **both variants must build** (CI enforces the
matrix). Flashable image: `we2_image_gen_local_dpd` with the RC24M profile, full recipe
in `_Documentation/building_firmware.md`; flashing and recovery in
`_Documentation/firmware_update_and_recovery.md`.

## Non-negotiables

- **Check the agent layer before each commit.** Ask whether the change makes anything in this
  file, the skill or its reference files wrong, missing or redundant, then add, edit or delete
  in the same commit. The three questions are in
  [`.agents/skills/references/documentation.md`](.agents/skills/references/documentation.md).
  A confidently wrong skill costs more than a thin one.
- **No em dashes** in documents or anything else that gets pasted elsewhere. Commas, or a new
  sentence.
- Never rebase or force-push the stacked feature branches; review work goes on
  `review/<name>-<topic>` branches.
- Ask the maintainer before pushing to any shared branch.
- Never commit build churn (`prebuilt_libs/**/*.a`, image-generator outputs, `NUL`,
  `obj_*`).
- Docs are the record, GitHub issues are the tracker: substantive findings go in
  `_Documentation/development reports/`, open items become issues (they auto-add to the
  [project board](https://github.com/orgs/wildlifeai/projects/3)). Rules for starting and
  closing a thread: [`development reports/README.md`](_Documentation/development%20reports/README.md).
- EXIF fields, op-parameters and BLE commands are cross-repo contracts (app, website,
  backend, ww-hardware), never change unilaterally.

## Where things are

| | |
|---|---|
| Durable docs | `_Documentation/`, `EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/` |
| How the code got this way | `_Documentation/development reports/` |
| Op parameters | `EPII_CM55M_APP_S/app/ww_projects/ww500_md/MANIFEST/config_file.md` |
| Console/BLE commands | `_Documentation/ble_commands.md` (`help` on the console lists all) |
| PR summary at a branch tip | `REVIEW_PR<N>.md` |
