# WW500 HM0360 Light Sensor

#### CGP, drawing together work by Victor Anton and Claude — 14 Aug 2026

## Objectives of this document

1. Give a WW500 user a single, current explanation of how the HM0360-based light sensor
   decides "dark" vs "bright", in enough detail to configure it for a real deployment.
2. Provide a step-by-step recipe for **running experiments to confirm the light sensor
   behaves as expected** before trusting it in the field — console-only, and with the
   `_Tools` Python scripts.
3. Separate what has actually been **validated on hardware** from what is still
   theoretical, so a user knows how much to trust each part.
4. List known limitations and open questions.
5. Identify gaps — missing tools, missing EXIF fields, missing BLE/app visibility — that
   would make it easier to trust and tune the light sensor in the field.
6. Point at every other document that touches this topic and say plainly whether each is
   still accurate, without editing them yet (see [§8](#8-related-documents)).

This document does not replace the files it links to — in particular
`AE_Light_Sensor_Roadmap.md` has analysis detail (the original 303-image dataset,
per-register accuracy figures) not repeated here. Treat this as the current-state summary
and starting point; follow the links for depth.

---

## 1. What the light sensor is

The WW500 has no dedicated ambient-light sensor. Instead it reuses the HM0360 camera's own
**Auto-Exposure (AE) registers** — already read after every capture for other purposes — as
a software light meter. The firmware decides "dark" or "bright" from those registers, and
that decision drives two things:

- **The capture flash** (visible or IR LED) — on when dark, off when bright.
- **Automatic day/night camera switching** — swap the active firmware slot between the
  HM0360 (night/IR) and RP3 (day/colour) images.

Both are optional and independently switched on — see [§3](#3-configuring-it).

## 2a. How the decision is made

To help me understand the code I created this sequence:

1. Just before entering DPD the code checks whether it should se a timer to wake (in, say, 15 minutes)
to check the light levels. This is in `image_sleepNow()` in `image_task.c`, and described above in the 
`Wake scheduling` section.
2. When the image task resumes in `vImageTask()` the light/dark decision previously saved in 
op param `OP_PARAMETER_AE_FLASH_STATE` (and other operational parameters) is used by 
`ledFlashSetFlashModeFromOpParam()` (called via `configure_image_sensor()` → `setupLEDFlash()` 
during camera init) to determine whether to use the flash to take an image.
3. If the timer then wakes the processor then a test is made to see if the light sensor 
code should run. It sets `aeCheckOnlyWake` true and prints "Timer wake for AE light check"
4. The image task schedules a request for a single image. We soon end up in `handleEventForCapturing()`.
NN processing is inhibited.
5. If there is a reason to know the light state (Flash is determined by the light sensor or
we might want to switch cameras depending on the light) then a call is made to `hm0360_md_getAEStats(AE_SAMPLE_COUNT, AE_SAMPLE_GAP_MS, &aeStats)`.
That includes if we have woken only to do the light measurement.
AE_SAMPLE_COUNT is defined as 16 and AE_SAMPLE_GAP_MS as 120 (in `ledFlash.h` - which seems the wrong place!). 
6. `hm0360_md_getAEStats()` gets the max, min and mean of **AE_MEAN** (a scene-brightness register, not gain)
across the sampled frames into a structure of type `HM0360_AE_STATS_T`. Gain (analog/digital) is tracked
separately, only as a running maximum, and used solely to compute a `gainRailed` flag — there is no min/mean
of gain.
7. The data from the previous step is passed to `ledFlashNewAEStats()` which is to
"Decide the flash state from ...the light sensor". The mean AE value is compared
against Operational Parameter `OP_PARAMETER_AE_DARK_THRESHOLD` (and a hysteresis of AE_HYSTERESIS is applied)
to decide whether the scene is 'light' or 'dark'. A line like this is sent to the console:
```
AE light check: mean AE = 64 (min 64, max 64) over 16 frames, threshold = 65, gain railed = no -> DARK (flash wanted)
```
8. The decision is saved to op param `OP_PARAMETER_AE_FLASH_STATE`. If 'dark' the LED is enabled.
Information is printed to the console.
9. A call is then made to `cameraSwitch_autoSwitchCheck()` - if enabled this function can switch the AI processor
firmware image between one that uses the RP3 camera in daylight and one that uses the HM0360 camera in the dark.
10. The device then enters DPD. The light/dark decision just made **does** affect whether the LED is used for
motion detection — the two are not independent. In `image_sleepNow()` (`image_task.c:2574-2590`), STROBE is
armed for MD illumination only if `ledFlashIsActive() > 0` **and** MD is enabled (op11) **and**
`OP_PARAMETER_MD_FLASH_LED` (op21) is non-zero. `ledFlashIsActive()` reflects exactly the AE decision (op25,
gated by capture flash being in AE mode, op13 ≠ 0). So op21/op22 only choose *which* LED and *how bright* —
they cannot make MD illumination fire if the AE decision currently reads "bright", or if op13 = 0. If either
of those holds, `hm0360_md_configureStrobe(false)` is called and MD illumination is disabled for that sleep
regardless of op21/op22.

## 2b. The execution sequence as explained by Claude

```
HM0360 AE registers  →  hm0360_md_getAEStats()  →  ledFlashNewAEStats()  →  op25 (persisted)
  (5 regs per frame)      (16-frame aggregate)         (threshold + hysteresis)      │
                                                                                       ├─ flash (op13)
                                                                                       └─ camera switch (op26)
```

- A **single** AE_MEAN reading is not usable as a light sensor. AE_MEAN is the *output* of
  the HM0360's own exposure control loop, not a raw brightness measurement, and that loop
  limit-cycles: bench testing in a sealed dark box showed it oscillating frame-to-frame
  between ~3 and ~66, straddling any sane threshold. A single frame misreported "bright" on
  ~37% of dark-box reads.
- The fix, `hm0360_md_getAEStats()`
  (`hm0360_md.c:507`, `hm0360_md.h:98`), samples **16 frames 120 ms apart**
  (`AE_SAMPLE_COUNT` / `AE_SAMPLE_GAP_MS`, `ledFlash.h:59-60`) and reports the mean, min,
  max, and whether analog+digital gain railed at their configured maxima on most frames
  (gain-railed is an unambiguous "too dark for the sensor to expose further" signal,
  independent of AE_MEAN). If the HM0360 is asleep (`MODE_SLEEP`, seen on the RP3 image
  when motion detection is off) it reads AE_MEAN = 0 permanently "dark" — the function wakes
  it to `MODE_SW_CONTINUOUS` for the sampling window (500 ms settle first) and restores the
  prior mode afterwards.
- `ledFlashNewAEStats()` (`ledFlash.c:425`) turns those stats into a **dark/bright**
  decision: gain-railed → dark; else `meanAE < OP_PARAMETER_AE_DARK_THRESHOLD` (op23,
  default 65) → dark; `meanAE > threshold + AE_HYSTERESIS` (12) → bright; in between, keep
  the previous decision (hysteresis, so the flash doesn't chatter at dusk/dawn).
- The decision is **persisted** as `OP_PARAMETER_AE_FLASH_STATE` (op25) because RAM does not
  survive Deep Power Down (DPD) and the first capture after a motion-triggered wake happens
  before any fresh AE reading exists.
- The check runs whenever **either consumer is enabled** — flash in AE mode (op13 = 1 or 2)
  or auto camera switching (op26 = 1) — in `image_task.c` around line 862. With *both* off,
  op25 goes stale and can report "BRIGHT" forever even inside a sealed box — this is a
  documented trap, not a bug to chase.
- **Wake scheduling** (`image_task.c:2554`, `image_sleepNow()`): if timelapse captures are
  already happening, they refresh AE registers for free — no extra wake is added. If not,
  and a consumer is enabled, an RTC alarm is armed for `OP_PARAMETER_AE_CHECK_INTERVAL`
  minutes (op24, default 15) that wakes the device, captures **one frame, saves nothing**,
  runs the check, and goes back to sleep (`aeCheckOnlyWake`). This is what lets automatic
  camera switching notice dawn/dusk without waiting for motion.
- **Motion-detection illumination** is a related but separate mechanism: while asleep, the
  HM0360's STROBE pin hardware-gates the LED directly (`hm0360_md_configureStrobe()`,
  `hm0360_md.c:724`), using LED/brightness settings held in the PCA9574 I/O expander
  (op21/op22, written on the way into DPD — `image_task.c:2574`). It is armed only if the
  light sensor last judged the scene dark. See `STROBE_timing.md` for the underlying pin
  timing.


## 3. Configuring it

| Op-param | Name | Default | Purpose |
|---|---|---|---|
| 13 | `OP_PARAMETER_FLASH_LED` | 0 | Capture flash: 0 = off, 1 = visible (AE-driven), 2 = IR (AE-driven) |
| 21 | `OP_PARAMETER_MD_FLASH_LED` | 2 | LED for motion-detection illumination while asleep: 0/1/2 |
| 22 | `OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT` | 5 | MD illumination brightness (0 means *dim*, not *off* — gated by op21, not this) |
| 23 | `OP_PARAMETER_AE_DARK_THRESHOLD` | 65 | AE Mean below this → dark |
| 24 | `OP_PARAMETER_AE_CHECK_INTERVAL` | 15 min | Periodic light-check wake interval (0 disables) |
| 25 | `OP_PARAMETER_AE_FLASH_STATE` | — | Runtime: last decision (0/1), persisted. Not user-set. |
| 26 | `OP_PARAMETER_SLOT_SWITCH` | 0 | 0 = manual `switchslot` only; 1 = automatic day/night switching |

Full descriptions: `Operational_Parameters.md`, `config_file.md`.

Example configurations (from the roadmap, still valid):

- IR at 5% for both MD illumination and capture flash: `setop 21 2, setop 22 5, setop 13 2, setop 9 5`
- IR MD illumination at 5%, visible capture flash at 50%: `setop 21 2, setop 22 5, setop 13 1, setop 9 50`

If updating firmware on a device with an existing `CONFIG.TXT`, op21/22 come up as `0`
("no MD illumination") until explicitly set — push the new defaults as part of any
firmware-update flow.

## 4. Running experiments to confirm it works

Do this before relying on the light sensor in a real deployment. Two ways to watch it:
over the console (most direct), or with the `_Tools` scripts (better for logging/analysis).

### 4.1 Console, manual

1. Connect over USB-serial (921600 baud) or use `_Tools/ww_serial.py`.
2. Enable a consumer so the check actually runs — e.g. `setop 13 2` (IR capture flash).
3. `capture 1 0` to trigger a capture, or just wait for a motion/timer wake.
4. Watch for the line (only printed to console, not sent over BLE — see
   [§6](#6-gaps-and-recommendations)):
   ```
   AE light check: mean AE = 64 (min 60, max 68) over 16 frames, threshold = 65,
   gain railed = no -> BRIGHT (no flash)
   ```
5. Cover the lens (or box the device) and repeat — `mean AE` should drop, `gain railed`
   should eventually read `yes`, and the decision should flip to `DARK (flash wanted)`.

### 4.2 With the `_Tools` scripts

| Script | Purpose | Status |
|---|---|---|
| `ww_serial.py` | Send one console command / listen, from a script (no TeraTerm needed) | Works |
| `ae_monitor.py` | Holds the device awake, repeatedly captures, prints the firmware's own decision each time (parses the "AE light check" line) | Was broken against current firmware; a local Claude session fixed the parsing and expanded `--help`. **Not yet exercised/trusted by a human against real hardware — verify before relying on it.** |
| `ae_threshold_analysis.py` | Analyses AE data extracted from JPEG EXIF (via `jpegAE-batch.py`) to recommend a dark threshold | Not re-run since `hm0360_md_getAEStats()` was added — see the caveat in [§6](#6-gaps-and-recommendations) |
| `jpegAE-batch.py` (pre-existing, not part of this PR) | Extracts the MakerNote CSV from a folder of JPEGs into a CSV | Only reads single-frame AE registers from EXIF (see [§6](#6-gaps-and-recommendations)) |

`ae_monitor.py` requires a consumer enabled first (`setop 13 1`/`2` or `setop 26 1`) —
without one, captures still happen but the firmware only logs the raw register dump, which
the script cannot parse.

### 4.3 What "working correctly" looks like

From the most recent hardware validation (`bench_validation_evidence.md`, 6 Aug 2026,
WW500_C02):

| Condition | Mean AE (16-sample) | Gain railed | Decision |
|---|---|---|---|
| Sealed dark box | ~0–35 | yes | DARK, 18/18 readings |
| Normal room light | ~78–89 | no | BRIGHT, 15/15 readings |
| Near threshold (66, dead band 65–77) | 66 | no | held previous decision — **no chatter** ✓ |
| Bright release | 79 | no | flips to BRIGHT (just past threshold+hysteresis) ✓ |

The full four-wake automatic-camera-switch cycle (dark → HM0360, bright → RP3, held at 66,
dark again → HM0360) also ran cleanly on the bench — see that file for the verbatim log.

## 5. Validated vs not yet validated

**Validated on hardware** (see `bench_validation_evidence.md`):
- Multi-frame aggregate collapses the single-frame oscillation (dark ~35 vs bright ~80).
- Gain-railed override fires correctly once the `MAX_AGAIN` decode bug was fixed.
- Sleeping-sensor wake-for-sampling path (RP3 image, MD off).
- Hysteresis holds through the dead band and releases correctly.
- Full op26 automatic camera-switch cycle, both directions, including the "unlabelled other
  slot → stay" guard and BLE notice reaching the app before reboot.
- Op-param persistence (`setop` survives DPD without a capture).

**Not yet validated:**
- Field behaviour across real dusk/dawn transitions and varied deployment environments
  (the bench box is a fast, artificial dark/bright step, not a slow natural transition).
- Multi-day battery soak with the periodic AE-check wake running.
- Whether the moderate threshold (65) chosen from the *original* single-frame dataset is
  still optimal now that the decision uses a 16-frame aggregate mean (the two are numerically
  different quantities — see [§6](#6-gaps-and-recommendations)).

## 6. Gaps and recommendations

These are things a user confirming or tuning the light sensor is likely to want, that don't
exist yet.

### 6.1 EXIF — the aggregated decision isn't recorded, only a raw single frame

The MakerNote CSV written into every JPEG (`image_task.c:2199`) comes from
`hm0360_md_getGainRegs()` — a **single-frame** read, taken purely for telemetry — not from
the 16-frame aggregate that actually drove the flash/switch decision. So `jpegAE-batch.py`
and `ae_threshold_analysis.py`, which both work from EXIF, are analysing a different (and by
design, noisier) number than the one the firmware acted on. A field photo currently cannot
answer "what did the light sensor actually decide, and why" — only "what did one raw AE_MEAN
happen to read at capture time."

**Recommendation:** add the aggregate result to the MakerNote (or a second tag) — e.g.
`meanAE (16-sample), gainRailed, decision, threshold` — so field photos are self-diagnosing.
Cheap: the aggregate is already computed once per relevant wake, just not passed to the EXIF
builder. Also flagged in `bench_validation_evidence.md`'s addendum: RP3-image photos
currently carry the *HM0360's* AE registers in EXIF (via `#if defined(USE_HM0360) ||
defined(USE_HM0360_MD)`), never the RP3/IMX708's own exposure — worth fixing alongside.

Standard EXIF `Flash` (0x9209) is already handled correctly: `exif_builder.c:402` writes it
dynamically from `input->flash_fired`, set at `image_task.c:2119` as
`exif_input.flash_fired = ledFlashIsActive()` — so a photo viewer already shows whether the
flash fired for that frame, no MakerNote needed. `BrightnessValue` (0x9203) is genuinely
missing, though — no such tag constant or write exists anywhere in `exif_builder.c`/`.h`.
Worth adding while touching this file, so a photo viewer/EXIF tool shows a brightness figure
without needing the MakerNote parsed at all.

### 6.2 BLE / app — the app never sees the light-sensor decision, only raw registers

`image_task.c` sends the raw single-frame `HM0360 AE regs:` dump over BLE
(`sendMsgToMaster()`, `image_task.c:893`) and, only when a camera switch actually fires, an
`Auto camera switch: ...` notice (`:901`). The actual **"AE light check: mean=…, threshold=…,
gain railed=…, → DARK/BRIGHT"** line from `ledFlashNewAEStats()` goes to the console
(`xprintf`) only — the app cannot currently show "here's why the flash is on/off right now"
or "the light sensor is currently reading dark/bright" without a serial cable.

**Recommendation:** send the same `"AE light check: mean=…, threshold=…, gain railed=…,
→ DARK/BRIGHT"` line that `ledFlashNewAEStats()` currently only sends to the console
(`xprintf`) to the app as well, over BLE via `sendMsgToMaster()` — same text, no new format
to design. This would let a user confirm the light sensor is behaving correctly from the
phone app during the "run experiments" phase in §4, instead of needing a bench console —
directly serving this document's audience.

Also worth surfacing: the periodic AE-check-only wakes (op24) are currently silent from the
app's point of view — no capture is saved and no BLE message is sent unless a switch fires.
A user watching the app during a dusk transition sees nothing happen until the switch itself
(if any). A periodic "light check: BRIGHT (mean 74)" BLE line would close that gap too.

### 6.3 Threshold may need re-validation against the new aggregate metric

`OP_PARAMETER_AE_DARK_THRESHOLD` = 65 was derived (`AE_Light_Sensor_Roadmap.md` §3) from a
303-image dataset of **single-frame** AE_MEAN values, before `hm0360_md_getAEStats()`
existed. The bench validation in §4.3 shows 65 still working, but on a small sample (box
vs. room, not a graded sweep). Re-running `ae_threshold_analysis.py`-style analysis against
aggregate-mean data (once §6.1 is fixed and some field photos exist) would confirm — or
correct — the default.

### 6.4 On-demand "just check the light" command — done

Added: the `light` CLI/BLE command calls the new `lightSensor_takeReadingForced()` (see
`lightSensor.c`) directly, with no capture/NN/file-save side effects, and reports the AE
value and dark/bright state, e.g. `Light level: 71 (DARK)`. Unlike the normal wake-cycle
path (`lightSensor_takeReading()`, gated on `lightSensor_isRequired()`), the forced variant
always samples regardless of whether the AE flash (op13) or auto camera-switch (op26) is
currently enabled, so it works for bench tuning before either is turned on.

Note (4 Sept 2026): since `flash_led_modes_proposal.md`, the capture flash can also
run in `FLASH_MODE_ALWAYS_ON`/`FLASH_MODE_TIME_OF_DAY`, not just AE-driven. `light`
still always forces a fresh AE reading regardless of mode, so its DARK/BRIGHT verdict
may not correspond to what's actually controlling the flash on a device running one
of those other modes - that's expected, not a bug.

### 6.5 `ae_monitor.py` needs a human validation pass

It was fixed against the current firmware by a different Claude session (not this one) after
Charles found it broken, but per `CGP_Code_Review_July26.md` its output hasn't yet been
reviewed by a human for correctness. Worth a short bench session confirming its parsed
output matches the console line before relying on it for tuning.

### 6.6 Future: ask the original author's Claude session for missing context

Some of this feature (and the white-balance/JPEG work alongside it) was implemented by a
different Claude session on Victor's machine, which may hold memory/context — design
rationale, things tried and abandoned — not captured in any committed file. Worth doing
before further work in this area, not now.

---

## 7. Known limitations (carried over from the roadmap, still true)

- With both op13 = 0 and op26 = 0, the light decision (op25) is never refreshed and can go
  stale indefinitely — not a bug, but easy to trip over during testing.
- `OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT` = 0 means "dimmest hardware level", not "off" —
  MD illumination on/off is controlled by `OP_PARAMETER_MD_FLASH_LED`, not the brightness
  value.
- The RTC alarm parameter is `uint16_t` seconds, so `AE_CHECK_INTERVAL` is clamped to
  roughly 18 hours.
- Auto camera switching (op26) only ever switches into a slot that has **already booted and
  self-labelled** as the wanted variant — an unlabelled or wrong-labelled other slot means
  the device stays put and logs "staying" rather than switching blind.

## 8. Related documents

| Document | Relevance / currency |
|---|---|
| [`AE_Light_Sensor_Roadmap.md`](../../../../../_Documentation/AE_Light_Sensor_Roadmap.md) | Primary source for the original analysis (dataset, per-register accuracy, correlations) not repeated here. Mostly current; §8.5's prose still says "8 samples / 40 ms gap" — the code and its own §8.5.1 confirm the actual values are 16 / 120 ms (`ledFlash.h:59-60`). That one paragraph is stale; the rest of the doc matches the code. Not edited here per your instruction. |
| [`_Documentation/development reports/2026-08-06_pr141-camera-features-review/bench_validation_evidence.md`](../../../../../_Documentation/development%20reports/2026-08-06_pr141-camera-features-review/bench_validation_evidence.md) | Verbatim hardware validation log for the op26 cycle and the two §8.5.1 bug fixes. Current and specific, but written as a PR-review artifact (checklists, "Findings beyond the checklist") rather than reference material — treat as evidence, not as the doc to hand a new user. |
| [`_Documentation/development reports/2026-08-06_pr141-camera-features-review/review_responses.md`](../../../../../_Documentation/development%20reports/2026-08-06_pr141-camera-features-review/review_responses.md) | Point-by-point response to `CGP_Code_Review_July26.md`, only partially about the light sensor (topic 3 of 9). Useful for the reasoning behind design decisions Charles questioned; not a standalone reference. Some content (e.g. `camera_switch.c` line references) will drift as the code changes. |
| [`Operational_Parameters.md`](../../../../../_Documentation/Operational_Parameters.md) | Authoritative, current op-param table — matches the code as of this session. |
| [`../MANIFEST/config_file.md`](../MANIFEST/config_file.md) | Also current; describes `CONFIG.TXT` format and the same op-param values. |
| [`slot_selector.md`](slot_selector.md) | Background on the A/B firmware-slot mechanism that automatic camera switching (op26) drives. Describes the underlying flash layout, not the light sensor itself; still accurate as design background. |
| [`STROBE_timing.md`](STROBE_timing.md) | Charles's own Feb 2026 investigation into STROBE-vs-VSYNC pin timing. Predates this PR but the STROBE mode it settled on (`STROBE_CFG` = 3, "Dynamic 1") matches `HM0360_SENSOR_STROBE_MODE` in `hm0360_md.h:24` today — still the right background reading for how MD illumination is physically gated. |
| [`REVIEW_PR141.md`](../../../../../_Documentation/development%20reports/2026-08-06_pr141-camera-features-review/REVIEW_PR141.md) | The PR #141 description — a good one-paragraph-per-feature overview of everything in that PR, light sensor included. Current as a historical summary of what shipped; not a how-to. |
| [`CGP_Code_Review_July26.md`](../../../../../_Documentation/development%20reports/2026-08-06_pr141-camera-features-review/CGP_Code_Review_July26.md) | Charles's original review notes and questions, including the ones this document tries to answer (§6). Keep as the record of what was asked; don't treat as current status since several items are now resolved (see `review_responses.md`). |

## 9. Some extra thoughts added by Charles:

1.    What happens to the AE registers during the 16 captures which are averaged? Do they jump about randomly? Do they move smoothy towards a settled state? Would fewer than 16 captures suffice? What happens of there is a small difference from the previous 15 minutes? Or a large change? Would it be helpful to seed the averaging algorithm with a value saved at the end of the previous sequence? Can we record all values and save in a .csv or otherwise observe this?

2. Why delay for 120ms between samples? AE_SAMPLE_GAP_MS

3.    Can we extract all of the light sensor code into a single .c file together with documentation that could be sent to Himax for comment?

4.    See this ChatGPT conversation on what might be stored as EXIF: https://chatgpt.com/share/6a7f8870-33c4-83ec-a6a6-30f1e3b84db2
