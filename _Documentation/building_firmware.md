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
make -j"$(nproc)"                                      # camera variant from ww500_md.mk
make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_imx708   # or override: RP v3 camera
make -j"$(nproc)" CIS_SUPPORT_INAPP_MODEL=cis_hm0360   # HM0360 as main camera
```

The camera variant defaults to whatever `CIS_SUPPORT_INAPP_MODEL` is set to in
`EPII_CM55M_APP_S/app/ww_projects/ww500_md/ww500_md.mk`.

ELF output: `obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf`

## Image generation: use the RC24M profile in `we2_image_gen_local_dpd`

Deployed WW500 units use the **RC24M bootloader set** (internal RC oscillator, required for
deep power down). Use `project_case1_blp_wlcsp_rc24m.json` — **not** the
`project_case1_blp_wlcsp.json` the README mentions, which selects the older Dec-2023
crystal bootloaders.

```bash
cd we2_image_gen_local_dpd
cp ../EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf input_case1_secboot/
# Linux/WSL: execute bits may need restoring after checkout
chmod +x we2_local_image_gen arm_none_eabi/arm-none-eabi-objcopy \
         arm_none_eabi/arm-none-eabi-objdump secureboot_tool/generate_secureboot_certificates
./we2_local_image_gen project_case1_blp_wlcsp_rc24m.json        # Linux
# .\we2_local_image_gen.exe project_case1_blp_wlcsp_rc24m.json  # Windows
```

Output: `output_case1_sec_wlcsp/output.img`

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
