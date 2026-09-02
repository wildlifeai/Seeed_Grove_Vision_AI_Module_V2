# Building the WW500 firmware (Wildlife Watcher specifics)

The upstream README's build instructions are for the Himax examples and are **out of date
for WW500 production builds** in two ways that matter. This page records what the CI
workflow (`.github/workflows/build_and_upload_firmware.yml`) and developers actually use.

## Toolchain: Arm GNU 14.3.rel1 (not 13.2.rel1)

The README says 13.2.rel1; production images are built with **arm-gnu-toolchain-14.3.rel1**
(GCC 14.3.1 20250623). The CI workflow pins this version. Building with a different major
version produces a working but different binary (differing code size and layout), which
breaks any byte-level comparison against released images.

```bash
wget https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi.tar.xz
tar -xf arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi.tar.xz
export PATH="$PWD/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin:$PATH"
```

## Build

```bash
cd EPII_CM55M_APP_S
# IMPORTANT: run 'make clean' when switching camera variants - object files do
# not encode the -D flags, so an incremental build would link stale objects
make clean && make -j"$(nproc)"                                      # camera variant from ww500_md.mk
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_imx708   # or override: RP v3 camera
make clean && make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_hm0360   # HM0360 as main camera
```

The camera variant defaults to whatever `CIS_SUPPORT_INAPP_MODEL` is set to in
`EPII_CM55M_APP_S/app/ww_projects/ww500_md/ww500_md.mk`.

ELF output: `obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf`

## Image generation: `make` already does it

**Do not run `we2_local_image_gen` by hand.** `make` stages the ELF and runs the image
generator for you (`ww500_md/mk/image_gen.mk` and the `stage_elf` target in `ww500_md.mk`),
using the RC24M profile. Running the tool again afterwards **overwrites or destroys** the
image make just produced, and its failures are quiet (see below), so a manual re-run is a
good way to end up with nothing while everything still reports success.

Each build therefore leaves two files in `we2_image_gen_local_dpd/output_case1_sec_wlcsp/`:

| File | What it is |
|---|---|
| `output.img` | the generator's fixed output path, overwritten by every build |
| `VYMDDHMM.IMG` | a copy under the device/Setup-Folder name, e.g. `R6901M24.IMG` |

`V` is the variant letter (`R` = RP3, `H` = HM0360), then year digit, month, day, an hour
letter (`A` = 10, so `M` = 22:00) and minute. It is 8.3, so it can be copied straight into
an SD card's `/MANIFEST`. **Because the variant letter differs, the two variants' images do
not collide and no manual renaming is needed**, but each build deletes the previous
`.IMG` of *its own* variant, so copy an image somewhere safe before rebuilding that variant.

Deployed WW500 units use the **RC24M bootloader set** (internal RC oscillator, required for
deep power down), which is why the makefile passes `project_case1_blp_wlcsp_rc24m.json` and
**not** the `project_case1_blp_wlcsp.json` the README mentions. That one selects the older
Dec-2023 crystal bootloaders.

### If you must run it manually

Only for debugging the generator itself. On a Windows checkout the tool needs execute bits
restoring under Linux/WSL:

```bash
cd we2_image_gen_local_dpd
cp ../EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf input_case1_secboot/
chmod +x we2_local_image_gen arm_none_eabi/arm-none-eabi-objcopy \
         arm_none_eabi/arm-none-eabi-objdump secureboot_tool/generate_secureboot_certificates
./we2_local_image_gen project_case1_blp_wlcsp_rc24m.json        # Linux
# .\we2_local_image_gen.exe project_case1_blp_wlcsp_rc24m.json  # Windows
```

### ⚠️ A failed certificate step does not fail the build

If the secure-boot content certificates cannot be generated, the tool prints a traceback,
carries on, and **make still exits 0**. You get an image that is roughly 24 KB short and
missing its certificates, with no other warning. Check the size:

| Variant | With certificates | Certificate step failed |
|---|---|---|
| RP3 (`cis_imx708`) | 487424 | 462848 |

The usual cause is a missing build *input* under
`we2_image_gen_local_dpd/secureboot_tool/cert/cfg/` (`ICVSBContent.cfg`, `OEMSBContent.cfg`),
which shows up as:

```
FileNotFoundError: [Errno 2] No such file or directory: 'cert/cfg/ICVSBContent.cfg'
[1066] Failed to execute script generate_secureboot_certificates
```

Those two files are tracked and must stay tracked. If a branch gitignores them (a `*.cfg`
pattern under `secureboot_tool/` will), every fresh clone of it silently builds unsigned
images. Restore with:

```bash
git checkout origin/dev -- we2_image_gen_local_dpd/secureboot_tool/cert/cfg
```

## Notes

- Images are **not byte-reproducible**: the secure-boot signer emits ~1.6 KB of differing
  bytes on every run, and the build embeds `__DATE__`/`__TIME__`. Same-size output with the
  correct embedded strings is the expected result.
- The build rewrites the tracked prebuilt `.a` archives in `EPII_CM55M_APP_S/prebuilt_libs/`
  (archive re-indexing). Restore them before committing: `git checkout -- EPII_CM55M_APP_S/prebuilt_libs/`
- The makefile's `2>NUL` redirections create a stray `NUL` file when building under
  Linux/WSL - harmless, delete it.
- Firmware version strings embed `GIT_BRANCH`/`GIT_COMMIT`/`GIT_DIRTY` evaluated when make
  starts. On a detached HEAD (CI), pass `GIT_BRANCH=<name>` explicitly.
