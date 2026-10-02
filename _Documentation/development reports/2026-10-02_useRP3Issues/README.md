# Correcting  places where HM0360 code is missing

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-10-02_useRP3Issues\README.md`
#### Author: Charles Palmer
#### Date: 2 October 2026

## Purpose

Some HM0360-related code is inadvertently compiled out when the RP3 camera is selected. This is because code is
inside `#define USE_HM0360` switches instead of `#if defined(USE_HM0360) || defined(USE_HM0360_MD)`

That is, when the main camera is the RP3 and the HM0360 is still needed for motion detection, 
some commands are accidently disabled. This is fixed by the current development work.

__How?__

The ww500_md makefile (`ww500_md.mk:247-264`) gives the two builds these defines:

| Build | Defines |
|---|---|
| HM0360 (`cis_hm0360`) | `USE_HM0360` |
| RP3 (`cis_imx708`) | `CIS_IMX`, `USE_RP3`, `USE_HM0360_MD` |

In the RP3 build the HM0360 is still fitted and does the motion detection, so code that is about motion detection
and is guarded by `#ifdef USE_HM0360` alone is missing from the RP3 build.

### GitHub issues and comments

- [Issue #211](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/211) (open): motion detection
  sensitivity (op 17 and the `md` command) is compiled out of the RP3 build, so Low, Medium and High are the same
  thresholds on the colour camera. Re-benched on 2 Oct 2026: still reproduces on `dev` 5750f8b5. This is the main
  issue for this thread. Its suggested fix is the same as the one below: move `cisdp_sensor_set_md_sensitivity()` into
  `hm0360_md.c`, call it before `hm0360_md_setMode()`, and widen the `md` command's guard.
- Your comment is a review comment on
  [PR #96](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/96#discussion_r3082651511) (14 Apr 2026,
  `CLI-commands.c`), on the line where that PR narrowed the `md` command from
  `#if defined(USE_HM0360) || defined(USE_HM0360_MD)` to `#if defined(USE_HM0360)`: "Retaining this as I might need to
  re-implement this code in the case of USE_HM0360_MD". That is where the gap started: the guard was narrowed because
  the setter lives in the HM0360 sensor driver, which the RP3 build does not compile.
- [Issue #250](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/250) (open), second item: remove the
  two `TODO should be #if defined(USE_HM0360) || defined(USE_HM0360_MD)` comments in `image_task.c`. It says doing
  what they say would break the RP3 build (the function is not there), and that #211 covers the real gap. Fixing #211
  as below closes that item too.
- [Issue #153](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/153) (open): the opposite fault.
  The EXIF Model is chosen with `#if defined(USE_HM0360) || defined(USE_HM0360_MD)` before `USE_RP3`, so every RP3
  photo says `WW500 HM0360`. It is the warning for this work: the combined test means "the HM0360 is fitted", not
  "the HM0360 is the main camera", so it must only be used for motion detection code.

### What goes wrong in the RP3 build today

1. op 17 (MD sensitivity) is stored in CONFIG.TXT but never applied. The HM0360 keeps the low thresholds from its
   register file (`HM0360_OSC_Bayer_640x480_setA_VGA_setB_QVGA_md_8b_ParallelOutput_R2.i`, loaded by
   `hm0360_md_init()` at cold boot), whatever op 17 says. The website and app present Low, Medium and High as a
   deployment choice; on the day camera it has no effect.
2. op 17 = 0 (sensitivity off) is not honoured either: if op 11 (MD interval) is non-zero, motion detection stays on.
3. The `md` console command, and so `AI md <n>` from the app's motion test, is "Command not recognised". The app
   treats it as non-critical and carries on.
4. The `inithm0360` console command is also missing, although `hm0360_md_reInitialise()` already brackets its writes
   with `saveMainCameraConfig()`/`restoreMainCameraConfig()` and would work in the RP3 build as it is.
5. The message printed before DPD (`hm0360_md_prepare()`) leaves out the sensitivity in the RP3 build, which is
   correct today but would be wrong once op 17 is applied.

## Code locations requiring a fix

_This section describes where changes were required - these have now all been made._

This is the survey as it was before the changes below. Line numbers are on branch `261002_useRP3Fixes` at 1cc34fd0, paths under
`EPII_CM55M_APP_S/app/ww_projects/ww500_md/`. I have listed every camera conditional in ww500_md (outside the
`cis_hm0360` driver itself) so the "keep" decisions are visible too.

### A prerequisite: make the sensitivity setter available to both builds

Most of the changes in B depend on this one. `cisdp_sensor_set_md_sensitivity()`, its four register tables and the
`MD_SENSITIVITY_CONFIG_E` enum are in the HM0360 sensor driver (`cis_sensor/cis_hm0360/cisdp_sensor.c:160-221` and
`:768`, `cis_sensor/cis_hm0360/cisdp_sensor.h:45-51` and `:71`), which only the HM0360 build compiles. Just widening
the `#ifdef`s would give "implicit declaration" and link errors in the RP3 build (the point #250 makes).

Proposal:

- Move the tables, the enum and the function into `hm0360_md.c`/`hm0360_md.h`, which both builds compile. Move, not
  copy, or the HM0360 build gets duplicate symbols. A new name such as `hm0360_md_setSensitivity()` would match the
  rest of `hm0360_md.c`; the callers then change too.
- Wrap the register writes in `saveMainCameraConfig()`/`restoreMainCameraConfig()`, as the other `hm0360_md_*`
  functions do. In the RP3 build the bus is pointing at the IMX708 (0x1A) at 400 kHz; these switch it to the HM0360
  (0x24) at 100 kHz and back. The `idSaveDepth` counter makes this safe to call from inside another `hm0360_md_*`
  function. In the HM0360 build the wrap is harmless (the slave ID is already 0x24 and the speed is not switched).
- `MD_SENSITIVITY_OFF` then becomes visible in both builds, so the literal `0` in `hm0360_md.c:814` can use it.

### B. Change to `#if defined(USE_HM0360) || defined(USE_HM0360_MD)` (motion detection code)

| # | Location | What it guards | Note |
|---|---|---|---|
| B1 | `CLI-commands.c:297-301` | Prototypes of `prvMd()` and `prvReinitHM0360()` | The `#endif` comment already says `|| defined(USE_HM0360_MD)` |
| B2 | `CLI-commands.c:702-718` | The `md` and `inithm0360` command definitions | |
| B3 | `CLI-commands.c:2343-2405` | `prvMd()` and `prvReinitHM0360()` | Remove the commented-out `//#if` and the TODO at `:2343-2344`. `prvMd()` calls the setter (needs A) |
| B4 | `CLI-commands.c:2889-2892` | Registering `md` and `inithm0360` | |
| B5 | `image_task.c:2747-2752` (`image_sleepNow()`) | Applying op 17 before `hm0360_md_prepare()` | The key change for #211: the outer block (`:2744`) is already the combined test, so the inner `#ifdef USE_HM0360` can simply go. This runs before every DPD in both builds, so op 17 is in force while the HM0360 watches for motion |
| B6 | `hm0360_md.c:804-808` (`hm0360_md_prepare()`) | Reading op 17 for the message | Once B5 is done, read op 17 in both builds (the `#else sensitivity = 1` goes). `hm0360_md.c` is only used with an HM0360, so the guard can be removed rather than widened. Update the comment at `:797-803` |
| B7 | `hm0360_md.c:819-825` | Printing the sensitivity in the "Motion Detection on!" message | As B6 |

### C. Leave as `USE_HM0360` but remove the TODO

| # | Location | Why |
|---|---|---|
| C1 | `image_task.c:2015-2018` (`configure_image_sensor()`, `CAMERA_CONFIG_INIT_COLD`) | In the HM0360 build this re-applies op 17 after `cisdp_sensor_init()` reloads the main register table. In the RP3 build this is the IMX708 init, and op 17 is applied before DPD by B5 instead. Widening it would add an HM0360 write to every RP3 wake for no gain. Remove the TODO (#250) |
| C2 | `image_task.c:2049-2053` (`CAMERA_CONFIG_INIT_WARM`) | The case is only reached in the HM0360 build ("Called at warm boot, only for HM0360"). Remove both TODOs (#250) |

If you prefer the RP3 build to apply op 17 at wake as well (for example so `getop`/`md` and the registers agree
straight after boot), C1 could be widened once A is done; it would cost one short burst of 100 kHz I2C per wake.

### D. Keep as `USE_HM0360` only (HM0360 as the main camera)

| Location | What it guards |
|---|---|
| `hm0360_md.c:32-36` | `SWITCH_I2C_SPEED_FOR_HM0360`: the I2C speed is only switched in builds where the HM0360 is not the main camera. Deliberately compile-time |
| `image_task.c:133` | `USE_HM0360_CAPTURE_TIMER` and `STROBE_CONTROLS_FLASH`: capture timing and flash for the HM0360 main camera |
| `image_task.c:693` | Tone mapping of the HM0360 main camera |
| `image_task.c:1417` | Capture timer forcing an HM0360 capture |
| `image_task.c:1694-1708` | Main camera init; the `#else` branch is the RP camera init |
| `image_task.c:1720-1721` | `#ifndef USE_HM0360` then `#ifdef USE_HM0360_MD`: the HM0360's MD-only init in the RP builds. Correct as it is |
| `image_task.c:1799`, `:1815` | Console report of the camera name, and flash duration (not used with the HM0360 main camera) |
| `image_task.c:2074` (`CAMERA_CONFIG_RUN`) | Starting HM0360 main-camera captures |
| `image_task.h:59` | `CAMERA_EXTRA_FILE` name per main camera |
| `ww500_md.c:489` | `app_get_camera_string()` |
| `ww500_md.c:601-603` | The whole I2C bus at 100 kHz when the HM0360 is the main camera |
| `ww500_md.c:605` | Build message and console camera name |
| `camera_switch.c:53-55` | Tests `USE_RP3` first, as a defence against both being defined. Correct |

### E. Already the combined test, and correct (motion detection or HM0360 present)

`image_task.c:346` (AE register struct), `:841` (clear MD interrupt after first frame), `:909` (HM0360 gain registers
for telemetry), `:2744` (prepare HM0360 for MD before DPD); `ww500_md.c:247` (HM0360 present check), `:558`,
`:665` (read the MD interrupt status at wake).

### F. The combined test used where it should not be

| Location | Fix |
|---|---|
| `image_task.c:2275-2283` | EXIF Model, issue #153. Test `USE_RP3` (and `USE_RP2`) before the HM0360, or use `#if defined(USE_HM0360)` alone. EXIF fields are a cross-repo contract (website `camera_variant`), but this is a bug fix to the documented values, not a change to them |


## Changes made (2 October 2026)

Issues #211 and #153 are fixed together on branch `261002_useRP3Fixes`.

| File | Change | Survey item |
|---|---|---|
| `hm0360_md.c`, `hm0360_md.h` | New `hm0360_md_setSensitivity()`, with the four sensitivity tables and `MD_SENSITIVITY_CONFIG_E` moved here. The writes are bracketed by `saveMainCameraConfig()`/`restoreMainCameraConfig()` | A |
| `cis_sensor/cis_hm0360/cisdp_sensor.c`, `.h` | `cisdp_sensor_set_md_sensitivity()`, its tables and the enum removed, with a comment saying where they went | A |
| `image_task.c` `image_sleepNow()` | op 17 applied before `hm0360_md_prepare()` in every build (inner `#ifdef USE_HM0360` removed) | B5 |
| `hm0360_md.c` `hm0360_md_prepare()` | op 17 read and reported in every build; `MD_SENSITIVITY_OFF` used instead of the literal 0 | B6, B7 |
| `CLI-commands.c` | `md` and `inithm0360` in both builds. `md` now says if the HM0360 did not respond (op 17 is still saved) | B1 to B4 |
| `image_task.c` `configure_image_sensor()` | Cold and warm init call the new function, still HM0360 build only; TODOs replaced by a comment (#250 item 2) | C1, C2 |
| `image_task.c` EXIF Model | Tests `USE_RP3`, then `USE_RP2`, then `USE_HM0360` | F (#153) |
| `.agents/skills/references/git-and-build.md` | Build invariant: what the defines mean and which test to use | |

The op 17 and EXIF Model values themselves are unchanged (cross-repo contracts): the RP3 build now does what
`config_file.md` and the website already say.

## Testing

_These are tests that I asked for to confirm that the code changes have worked._

Console tests, on the Himax console (921600 baud). From the app's Engineer Console, put `AI ` in front of each
command. Run them on the RP3 image first, because that is where behaviour changes. Then run them on the HM0360
image to check nothing has broken there.

| # | Do | Before (RP3 image) | Now (RP3 image) |
|---|---|---|---|
| T1 | `md 2` | `Command not recognised. ...` (app: `Unrecognised`) | `HM0360 MD sensitivity 2`, then `MD sensitivity set to 2` |
| T2 | `getop 17` after T1 | Not changed by T1 | `2` |
| T3 | `md 5` | `Command not recognised. ...` | `Error: Sensitivity must be an integer between 0 and 3.` |
| T4 | `inithm0360` | `Command not recognised. ...` | `OK` |
| T5 | `help` | No `md` or `inithm0360` | Both listed |
| T6 | `md 2`, then `capture 1 1000` | (`md` was rejected) | The photo is taken and saved as normal: the bus went back to the IMX708 after `md` |
| T7 | `setop 17 3`, then leave the device to sleep. Read the lines just before it sleeps | `HM0360 Motion Detection on! <n>ms frame interval` (no sensitivity) | `HM0360 MD sensitivity 3`, `Preparing HM0360 for MD:` and `HM0360 Motion Detection on! <n>ms frame interval, sensitivity 3` |
| T8 | `setop 17 0` (op 11 not 0), let it sleep, then wave a hand in front of the camera | The device wakes with motion: op 17 was ignored | `HM0360 Motion Detection off (the sensitivity, op 17, is 0) ...` before sleep, and no motion wake. Afterwards `setop 17 1` to put it back |
| T9 | Take a photo (`capture 1 1000`, or from the app). Copy the JPG to a PC and look at the camera model (Windows: Properties, Details, "Camera model") | `WW500 HM0360` | `WW500 RP3` |

On the HM0360 image, T1 to T5 and T7 should be the same as before (they already worked), T8 should give no wake as
before, and T9 should still say `WW500 HM0360`.

Optional, from the app: in the motion test, change the sensitivity. On the RP3 image the Himax console should show
`MD sensitivity set to <n>` where it used to show `Unrecognised`. With the device on a bench, High should pick up a
smaller or more distant movement than Low.

## Text for the GitHub issues

_This work will be submitted as PR #259. It should resolve (fully or partly) some isses, 
and I will be pasting the following comments into these issues:_

### Issue #211

> Fixed in #PR259 (branch `261002_useRP3Fixes`).
>
> The sensitivity setter was in the HM0360 sensor driver, which the RP3 build does not compile. It is now
> `hm0360_md_setSensitivity()` in `hm0360_md.c`, which both builds compile, together with its four register tables
> and `MD_SENSITIVITY_CONFIG_E`. The writes are bracketed by `saveMainCameraConfig()`/`restoreMainCameraConfig()`, so on
> the RP3 build they go to the HM0360 at 100 kHz and the bus is then put back to the IMX708.
>
> - op 17 is applied in `image_sleepNow()`, just before `hm0360_md_prepare()`, in both builds. So Low, Medium and High
>   now take effect on the RP3 camera, and 0 turns motion detection off there too.
> - The `md` and `inithm0360` console commands are in both builds (`USE_HM0360 || USE_HM0360_MD`), so `AI md <n>` from
>   the app's motion test is no longer `Unrecognised`.
> - The message before DPD reports the sensitivity in both builds.
>
> op 17's values are unchanged: the RP3 build now does what `config_file.md` and the app already say. Bench tests T1
> to T9 (console and app) are in the
> [thread README](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/261002_useRP3Fixes/_Documentation/development%20reports/2026-10-02_useRP3Issues/README.md).
>
> App side: finding E (the app should say the sensitivity has no effect on the RP3 camera) is no longer needed once
> devices have this firmware.

### Issue #153

> Fixed in #PR259 (branch `261002_useRP3Fixes`).
>
> The EXIF Model block in `image_task.c` now tests `USE_RP3` first, then `USE_RP2`, then `USE_HM0360`. The RP builds
> also define `USE_HM0360_MD`, for motion detection, and the old test for it came first. The strings are unchanged, so
> the website's `camera_variant` mapping needs no change. Photos from the RP3 image now say `WW500 RP3` (test T9 in
> the [thread README](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/261002_useRP3Fixes/_Documentation/development%20reports/2026-10-02_useRP3Issues/README.md)).
> Photos taken by the RP3 image before this firmware still say `WW500 HM0360`.

### Issue #250 (second item only)

> The two `TODO should be #if defined(USE_HM0360) || defined(USE_HM0360_MD)` comments in `image_task.c` are removed in
> #PR259. As this issue says, doing what they said would have broken the RP3 build. Instead, the setter moved to
> `hm0360_md.c` (#211), and op 17 is applied before each DPD in both builds. The two calls in
> `configure_image_sensor()` stay HM0360-build only, with a comment saying why. Ticking this item; the rest of #250
> is unchanged.
