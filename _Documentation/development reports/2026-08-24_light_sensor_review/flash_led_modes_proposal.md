# Proposal: reinstate ALWAYS_ON / TIME_OF_DAY flash modes

Written for the "Add other options for enabling the flash LED" task in
[CLAUDE_light_sensor_review.md](CLAUDE_light_sensor_review.md). No code has been
changed - this is the design report requested there, for Charles to review
before anything is implemented.

## 1. What's being asked

Reinstate two capture-flash modes that used to exist alongside `FLASH_MODE_AE`:

- **Always on** - flash fires on every capture, unconditionally.
- **Time of day** - flash fires only within a configured UTC time window.

`FlashLedMode_t` (`ledFlash.h`) already carries a breadcrumb for this:

```c
// NOTE: could consider also these:
// FLASH_MODE_ALWAYS_ON,	// On all the time
// FLASH_MODE_TIME_OF_DAY	// Determined by time of day timer
typedef enum flashLedMode {
    FLASH_MODE_OFF,			// Off all the time
    FLASH_MODE_AE,			// Determined by light levels
} FlashLedMode_t;
```

Charles's point 3 in the task asks specifically that the mode be **set directly
from a fresh operational parameter**, rather than inferred dynamically the way
`ledFlashSetFlashModeFromOpParam()` currently infers `FLASH_MODE_OFF` vs.
`FLASH_MODE_AE` from `OP_PARAMETER_FLASH_LED` alone. That instinct turns out to
matter a lot - see §3.

## 2. These modes existed before - here's exactly how

Found via `git log -G"FLASH_MODE_TIME_OF_DAY"` - commit `d9d9d253`, "feat:
simplify flash to AE-only mode; separate MD illumination settings" (Victor,
5 Jul 2026). Summarising what it removed, since it's the natural starting point
for a redesign rather than working from scratch:

- `FlashLedMode_t` had all four values: `FLASH_MODE_OFF`, `FLASH_MODE_ALWAYS_ON`,
  `FLASH_MODE_AE`, `FLASH_MODE_TIME_OF_DAY`.
- **Mode selection was inferred from two existing op-parameters, overloaded**:
  `ledFlashSetFlashModeFromOpParam(ledInUse, startTime, duration)` -
  `ledInUse == 0` → off; `duration == 0` → always on; `duration == 1` → AE;
  `duration > 1` → time-of-day, using `startTime`/`duration` as the actual
  window (so `duration` was simultaneously a mode-selector *and* a data value -
  exactly the ambiguity Charles's point 3 wants to avoid).
- **Time-of-day storage**: `OP_PARAMETER_FLASH_LED_START_TIME` (21, minutes
  after midnight UTC) and `OP_PARAMETER_FLASH_LED_DURATION` (22, minutes,
  wrapping across midnight).
- **Evaluation**: `setupLEDFlash()` (called at every wake, `image_task.c`) read
  the RTC (`exif_utc_get_rtc_as_time()`) and called `ledFlashNewTime(time)`
  right after setting the mode - so time-of-day was re-evaluated fresh on
  every wake, the same cadence the AE path uses today. `ledFlashNewTime()` was
  *also* called from `prvSetUtc()` (`CLI-commands.c`) whenever the RTC was set
  from the app/BLE, for immediate responsiveness to a clock correction.
- **`ledFlashNewTime()`'s body** computed `flashActive` from a wrap-around
  minutes-of-day comparison, then called `ledFlashActivate()` directly -
  toggling the hardware immediately, not just updating a flag.

Why it matters that I found this rather than designing fresh: the old
`duration`-as-mode-selector approach is a genuine anti-pattern worth not
repeating (confirms Charles's instinct in point 3), but the *evaluation
timing* (per-wake, via a direct RTC read) is sound and matches how the AE path
works today - worth keeping.

## 3. Complication: the freed op-parameter slots are gone

The commit's own message explains why it was removed: slots 21/22 (previously
`FLASH_LED_START_TIME`/`FLASH_LED_DURATION`) were reassigned to
`OP_PARAMETER_MD_FLASH_LED` / `OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT` (the
motion-detection illumination settings from
[AE_Light_Sensor_Roadmap.md §8](../../AE_Light_Sensor_Roadmap.md#8-flash-configuration-simplified-and-motion-detection-illumination)),
which are very much in active use. **Any new time-of-day parameters need new
indices** - the current table (`fatfs_task.h`) runs 0-31
(`OP_PARAMETER_NUM_ENTRIES` = 32), so new entries would start at 32.

This is a cross-repo contract, per `AGENTS.md`: *"EXIF fields, op-parameters
and BLE commands are cross-repo contracts (app, website, backend, ww-hardware)
- never change unilaterally."* Concretely, adding new op-parameters means:

- `fatfs_task.h`'s enum and `fatfs_task.c`'s default-value table (both flagged
  in `fatfs_task.h`'s own header comment: *"If this list is changed then it
  must be changed in the MKL62BA code also in aiProcessor.h"*).
- `_Documentation/Operational_Parameters.md` and `MANIFEST/config_file.md`.
- The BLE processor side (`aiProcessor.h`, `ww-hardware` repo) and whatever in
  the app/backend lets a user actually set these values - none of that is
  visible from this repo, so this needs the maintainer's coordination, not
  just a firmware patch.

## 4. Complication: a new mode-select parameter needs a safe default

If a new `OP_PARAMETER_FLASH_MODE` is simply appended at index 32, every
**already-deployed** device reads 0 there by default (nothing has ever written
that slot) - same situation as the 21/22 migration in §8.4 of the roadmap doc.
If 0 means `FLASH_MODE_OFF`, every device currently relying on
`OP_PARAMETER_FLASH_LED` (op13) = 1 or 2 for AE-driven flash would silently go
dark after a firmware update, until the app pushes a new default. That's a
real behaviour regression in the field, not just a config nicety - worth
deciding deliberately rather than as an afterthought:

- **Option A**: make 0 mean "not set - infer from op13 the old way (0 = off,
  else = AE)", and require an explicit non-zero value to opt into
  `ALWAYS_ON`/`TIME_OF_DAY`. Backward-compatible by construction; slightly odd
  in that 0 doesn't literally mean any one mode.
  - Given `OP_PARAMETER_NUM_ENTRIES` currently defines the number of entries
     in the array, and any device that has not been updated with a new entry
     will report zero for that Operational Parameter, the "opt-in" case
     described here is not a big diversion from the existing convention. 
- **Option B**: number the enum so `AE = 0` (the safe legacy default) and give
  `OFF`/`ALWAYS_ON`/`TIME_OF_DAY` non-zero values instead. Cleaner semantics,
  but `FLASH_MODE_OFF` no longer being 0 is a small trap for future readers
  used to "0 = off" being the norm elsewhere in this table.
- **Option C**: have the app push an explicit default (e.g. `1` = AE) as part
  of the firmware update flow, same as the §8.4 migration note recommended for
  21/22. Cleanest data model, but depends on an app-side change actually
  happening before/with the firmware rollout - a coordination risk if it
  doesn't.

I'd lean towards **A** (matches how this table has already handled two prior
migrations, needs no app-side coordination to be safe on day one), but this is
Charles's call.

## 5. Proposed design (pending the above decisions)

**`ledFlash.h`**: restore `FLASH_MODE_ALWAYS_ON` and `FLASH_MODE_TIME_OF_DAY`
to `FlashLedMode_t`, keeping `FLASH_MODE_AE` as-is.

**New op-parameters** (indices 32+, exact numbers TBD by whoever also updates
`aiProcessor.h`):

| Parameter | Meaning |
|---|---|
| `OP_PARAMETER_FLASH_MODE` | 0/1/2/3 → off / AE / always-on / time-of-day (or per §4's Option A, 0 = "infer from op13") |
| `OP_PARAMETER_FLASH_TOD_START` | Minutes after midnight UTC - reuses the old `FLASH_LED_START_TIME` semantics |
| `OP_PARAMETER_FLASH_TOD_DURATION` | Minutes, wraps past midnight - reuses the old `FLASH_LED_DURATION` semantics |

`OP_PARAMETER_FLASH_LED` (op13) keeps its current meaning unchanged - which
LED colour(s), 0/1/2 - and stays consulted under every mode, since LED colour
selection is orthogonal to *why* the flash is on. The existing short-circuit
(`ledInUse == 0` → always off, regardless of mode) is kept, so op13 = 0 still
means "off" no matter what `OP_PARAMETER_FLASH_MODE` says - one less way for a
confusing combination to arise.

**Evaluation, per mode** (all inside `ledFlashSetFlashModeFromOpParam()` /
`setupLEDFlash()`'s existing per-wake call, matching where AE mode is already
evaluated):

- `FLASH_MODE_ALWAYS_ON`: `flashActive = true`, set once at wake. No
  persisted runtime state needed (unlike AE, nothing to survive DPD - it's
  unconditionally true every time).
- `FLASH_MODE_TIME_OF_DAY`: read the RTC and compare against
  `FLASH_TOD_START`/`FLASH_TOD_DURATION` using the old wrap-around window
  math, fresh at every wake (RTC is the persisted state here - no new
  `OP_PARAMETER_*_STATE` needed, unlike AE's `OP_PARAMETER_AE_FLASH_STATE`).
  One open question: also re-evaluate from `prvSetUtc()` on an explicit clock
  correction, as the old code did? See §8.
- Unlike the removed `ledFlashNewTime()`, the new evaluation should only set
  the `flashActive` flag - not call `ledFlashActivate()` directly. This
  session's own history is exactly why: `ledFlash_setActive()` was
  deliberately made a *pure* flag setter (removing an `ledFlashActivate()`
  call it used to make) after that immediate-hardware-write pattern caused a
  real bug (flash staying solid on for ~1s after a bare light check, no
  consumer actually needing it lit at that moment yet). The same reasoning
  applies here - only arm the hardware where it's actually about to be used
  (capture time, or `image_sleepNow()`'s STROBE arming), not the moment the
  decision is computed.

**Reassuring finding - the consumption side needs no new branching.** Both
places that actually *consume* the decision already do so generically, via
`flashActive`/`ledFlashIsActive()`, not by checking which mode is active:

- Capture-time flash arming (`image_task.c`, `configure_image_sensor()`).
- `image_sleepNow()`'s STROBE-arming for motion-detection illumination -
  already gated on `ledFlashIsActive()` being nonzero, with the MD-specific
  LED/brightness settings applied independently (§8.3 of the roadmap doc).

So once `flashActive` is set correctly for the new modes, capture flash and MD
illumination both pick it up for free. But §6 and §7 below are real gaps this
alone doesn't cover.

## 6. Gap Charles spotted (3 Sept 2026): AE state/`lightSensor_isDark()` goes stale under other modes

Charles asked directly: would `OP_PARAMETER_AE_FLASH_STATE`,
`decideDarkBright()`/`decideDarkBrightGainBased()`, and the `light` CLI command
only matter when `FLASH_MODE_AE` is selected? Tracing every call site gives a
more precise answer than a flat yes:

**`decideDarkBright()`/`decideDarkBrightGainBased()` are not AE-mode-only,
today or after this change.** They run via `lightSensor_takeReading()`, gated
by `lightSensor_isRequired()`:

```c
return (ledFlashGetFlashMode() == FLASH_MODE_AE)
        || (fatfs_getOperationalParameter(OP_PARAMETER_SLOT_SWITCH) == 1);
```

`OP_PARAMETER_SLOT_SWITCH` (automatic day/night camera switching, op26) is a
second, independent consumer that doesn't care what the *flash* mode is. So
with auto camera-switching enabled, `decideDarkBright()` keeps running and
`OP_PARAMETER_AE_FLASH_STATE` keeps getting freshly written even under
`FLASH_MODE_ALWAYS_ON`/`FLASH_MODE_TIME_OF_DAY` - just no longer consumed for
the flash decision, only for the camera-switch decision.

**One call site is already correctly gated, no change needed.** The block
that actually feeds `flashActive` from the AE decision
(`image_task.c`, inside the post-capture light check) already checks the mode:

```c
if (ledFlashGetFlashMode() == FLASH_MODE_AE) {
    ledFlash_setActive(lightSensor_isDark());
}
```

**Two call sites do need attention when the new modes are implemented:**

1. `ledFlash.c`'s `ledFlashSetFlashModeFromOpParam()` currently does
   `flashActive = lightSensor_isDark();` unconditionally in its "not off"
   branch - fine today since AE is the only non-off mode, but it needs to
   become mode-conditional (`FLASH_MODE_AE` only) once `ALWAYS_ON`/
   `TIME_OF_DAY` compute `flashActive` their own way instead.
2. **The two console lines added 1-2 Sept 2026** (`image_task.c`: "Currently
   it is DARK/LIGHT" before entering DPD, and "Light is currently DARK/LIGHT"
   in the wake info block) call `lightSensor_isDark()` unconditionally,
   regardless of mode. Under `ALWAYS_ON`/`TIME_OF_DAY` with camera-switch off,
   `OP_PARAMETER_AE_FLASH_STATE` would sit frozen at whatever it last held
   (from whenever AE mode was last active, or its power-on default) - these
   two lines would report a stale, disconnected value with no relationship to
   what's actually driving the flash. This needs fixing alongside the mode
   work, not left as-is - see open question 8 below for the two ways to fix
   it.

**The CLI `light` command needs no change and stays useful under any mode** -
`lightSensor_takeReadingForced()` deliberately bypasses
`lightSensor_isRequired()` (per its own doc comment: "always samples,
ignoring `lightSensor_isRequired()`, for on-demand bench/debug use"), so it
keeps working as a diagnostic regardless of flash mode. Worth a one-line note
in its output/`light_sensor.md` that its DARK/BRIGHT verdict may not
correspond to what's actually controlling the flash under `ALWAYS_ON`/
`TIME_OF_DAY`, so a user doesn't mistake the mismatch for a bug.

## 7. Gap Charles spotted (3 Sept 2026): TIME_OF_DAY needs periodic re-checking

MD illumination is armed once, at the wake where `flashActive` was last
computed, and then just runs unattended for the whole sleep - `hm0360_md`'s
STROBE pin gates the LED in hardware, with no CPU involvement per frame. The
*only* thing that ever revises that decision is the AI processor waking up
again (motion, a timer, or the BLE processor) and re-running
`setupLEDFlash()`.

For `FLASH_MODE_AE` this is already handled: `image_task.c`'s
`aeCheckRequired` (`= lightSensor_isRequired()`) causes a periodic
`OP_PARAMETER_AE_CHECK_INTERVAL`-driven wake specifically so the light
decision can't go stale for long, regardless of whether motion happens
(`image_task.c` ~line 2793's own comment: *"wake periodically anyway to
sample the light level ... so the decision is fresh before the next
motion-detect capture"*).

**`FLASH_MODE_TIME_OF_DAY` has no equivalent, as I've proposed it so far.**
`lightSensor_isRequired()` only returns true for `FLASH_MODE_AE` or
`OP_PARAMETER_SLOT_SWITCH`, so `aeCheckRequired` would be false under
time-of-day mode - no periodic wake gets scheduled at all. If nothing else
wakes the device (no motion, BLE quiet), `flashActive` stays exactly as it was
computed at the last wake for the *entire* rest of the sleep - including
straight through the end of the configured window and on into broad
daylight, with MD illumination still firing on every motion-detection frame
the whole time. This is the concrete failure mode Charles described.

`FLASH_MODE_ALWAYS_ON` does **not** have this problem - "always on" has no
daylight exception to go stale against, so there's nothing to re-check.

**Proposed fix**, matching Charles's suggestion: broaden the condition that
schedules the periodic wake so it also covers `FLASH_MODE_TIME_OF_DAY`, e.g.
`lightSensor_isRequired()` becoming something like:

```c
return (ledFlashGetFlashMode() == FLASH_MODE_AE)
        || (ledFlashGetFlashMode() == FLASH_MODE_TIME_OF_DAY)
        || (fatfs_getOperationalParameter(OP_PARAMETER_SLOT_SWITCH) == 1);
```

(exact placement TBD - this logic may belong on the `ledFlash`/mode side
rather than `lightSensor.c`, since time-of-day isn't a light-sensor concept -
see open question 6 below.)

This reuses the *existing* `OP_PARAMETER_AE_CHECK_INTERVAL` timer/RTC-alarm
machinery rather than adding a parallel one, per Charles's suggestion - one
interval, one wake mechanism, covering "re-check whatever's making the flash
decision" in general rather than being AE-specific. The main cost is a naming
mismatch: an op-parameter literally called "AE check interval" would now also
pace time-of-day re-checks, which may read oddly to whoever configures it via
the app - see open question 7.

## 8. Open questions for Charles

1. §4's default-safety question - Option A, B, or C (or something else)?
2. Should `FLASH_MODE_TIME_OF_DAY` also re-evaluate immediately when the RTC
   is set via `prvSetUtc()` (as the old code did), or is the per-wake
   evaluation sufficient? The old immediate path called `ledFlashActivate()`
   directly, which per §5 shouldn't be repeated verbatim - if kept, it would
   just re-set `flashActive` early rather than toggle hardware.
3. Exact `OP_PARAMETER_FLASH_MODE` numbering - does it matter to you which
   integer means which mode, or should I just pick sensible ones once the
   default-safety question is settled?
4. Given the cross-repo coordination this needs (§3), do you want me to also
   draft the `aiProcessor.h`/`Operational_Parameters.md`/`MANIFEST/config_file.md`
   changes as part of this proposal (still not applying anything), or hold
   those until the numbering/defaults are agreed?
5. Is a single time-of-day window (start + duration, wrapping past midnight)
   still sufficient, or would you want something like sunrise/sunset-relative
   timing now that the light sensor itself exists? (The old implementation
   predates `lightSensor.c` entirely.)

   Note (Charles, 2 Sep 2026): a separate ChatGPT conversation about deriving
   sunrise/sunset time from GPS location is here:
   <https://chatgpt.com/share/6a97cda3-32f0-83ec-ad6f-bee5b1845321> - relevant
   since GPS location is already available on-device (`exif_gps_deviceLat`/
   `Lon`, parsed from `CONFIG.TXT` by `processGPS()` in `fatfs_task.c` - a
   separate file line, not part of the numbered `op_parameter[]` table), so a
   sunrise/sunset-relative mode would not need new location storage, only the
   time computation itself.
6. §7's periodic-wake condition (`lightSensor_isRequired()`) currently lives in
   `lightSensor.c`, which is otherwise entirely about AE/light sensing - does
   time-of-day belong there too (simplest, one function everything already
   calls), or should the periodic-wake decision move somewhere mode-agnostic
   (e.g. `ledFlash.c`, which already owns `FlashLedMode_t`) now that it needs
   to reason about a mode `lightSensor.c` doesn't otherwise know about?
7. §7's naming mismatch - keep reusing `OP_PARAMETER_AE_CHECK_INTERVAL` as a
   general "flash re-check interval" (Charles's suggestion, no new op-param
   needed), or would that be confusing in the app UI where it's presumably
   still labelled as an AE-specific setting? An in-between option: keep the
   op-parameter as-is but rename only its *label* in
   `Operational_Parameters.md`/the app to something mode-neutral, without
   touching its number or the firmware logic.
8. §6's stale-console-line problem - reword the two DARK/LIGHT lines to be
   mode-agnostic (e.g. print the actual `flashActive`/`ledFlashIsActive()`
   state generically - "Flash is currently armed/not armed" - which stays
   meaningful under every mode), or suppress/qualify them specifically when
   `FLASH_MODE_AE` isn't selected (e.g. append "(not currently in use - mode
   is X)")? I'd lean towards the generic-rewording option, since it stays
   accurate and useful under every mode without needing a special case, but
   it does lose the specific "AE judged it dark" phrasing.

No code has been changed for this task - waiting for answers before touching
`ledFlash.c/.h`, `fatfs_task.c/.h`, or `image_task.c`.
