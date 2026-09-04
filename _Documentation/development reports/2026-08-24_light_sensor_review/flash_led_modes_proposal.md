# Proposal: reinstate ALWAYS_ON / TIME_OF_DAY flash modes

Written for the "Add other options for enabling the flash LED" task in
[CLAUDE_light_sensor_review.md](CLAUDE_light_sensor_review.md).

**Status: final design, ready to implement.** Charles has resolved every open
question from the previous draft (4 Sept 2026) - this revision replaces that
draft's options/questions with the concrete decisions taken, and is written as
an implementation plan rather than a design discussion. Still no code changed
yet - this is the last review pass requested before coding starts.

## 1. What's being asked

Reinstate two capture-flash modes that used to exist alongside `FLASH_MODE_AE`
(removed by commit `d9d9d253`, "feat: simplify flash to AE-only mode"; the
`git log`-able diff was used as a starting point for this design - see the
previous revision of this doc, kept in git history, for that trace):

- **Always on** - flash fires on every capture, unconditionally.
- **Time of day** - flash fires only within a configured UTC time window.

## 2. Decisions taken (supersede the previous draft's open questions)

1. **Backward compatibility is not a concern** - few devices are deployed and
   they will all be updated. No migration/default-safety scheme needed.
2. **`FLASH_MODE_OFF = 0`** - the obvious, no-effect value. Enum numbering is
   otherwise unconstrained.
3. **Two new op-parameters go after Charles's own `OP_PARAMETER_RFU_1`/`_2`
   placeholders** (indices 32/33 in `fatfs_task.h`) - i.e. starting at 34.
   Single, simple time-of-day window: `OP_PARAMETER_FLASH_TOD_START` +
   `OP_PARAMETER_FLASH_TOD_DURATION` only - no date/season/sunrise-sunset
   adjustment.
4. **`OP_PARAMETER_AE_CHECK_INTERVAL` (op24) is renamed to
   `OP_PARAMETER_FLASH_EVALUATE_INTERVAL`** - same index, broadened meaning:
   it now paces periodic re-evaluation for AE *and* time-of-day, not just AE.
5. **Time-of-day is also re-evaluated immediately whenever the RTC is set**
   (`prvSetUtc()`), in addition to the periodic per-wake evaluation - matching
   the old `ledFlashNewTime()`'s trigger, but only setting the `flashActive`
   flag (not driving hardware directly - see §4).
6. **No precision engineering** - "the flash does not have to turn on and off
   at precise times." Periodic re-evaluation on whatever cadence
   `OP_PARAMETER_FLASH_EVALUATE_INTERVAL` is set to is entirely sufficient;
   no RTC-alarm-exact scheduling for the window boundary itself.
7. **`lightSensor.c` stays exclusively about light sensing** - none of the new
   mode logic (time-of-day math, RTC access, periodic-wake-for-TOD condition)
   goes in that file. It lives in `ledFlash.c` (which already owns
   `FlashLedMode_t`) and `image_task.c` (which already owns the periodic-wake
   scheduling and already reads `ledFlashGetFlashMode()` elsewhere).
8. **The two DARK/LIGHT console lines become mode-agnostic** - see §6.

## 3. New op-parameters

Added to `OP_PARAMETERS_E` in `fatfs_task.h`, immediately after
`OP_PARAMETER_RFU_2` (33):

```c
OP_PARAMETER_RFU_1,                    // 32 RFU
OP_PARAMETER_RFU_2,                    // 33 RFU
OP_PARAMETER_FLASH_MODE,               // 34 Capture flash mode: 0=off, 1=AE, 2=always-on, 3=time-of-day
OP_PARAMETER_FLASH_TOD_START,          // 35 FLASH_MODE_TIME_OF_DAY: minutes after midnight UTC when the flash turns on
OP_PARAMETER_FLASH_TOD_DURATION,       // 36 FLASH_MODE_TIME_OF_DAY: duration (minutes) the flash stays on, wraps past midnight
```

`OP_PARAMETER_AE_CHECK_INTERVAL` (24) renamed in place (no index change) to
`OP_PARAMETER_FLASH_EVALUATE_INTERVAL`, comment updated:

```c
OP_PARAMETER_FLASH_EVALUATE_INTERVAL,  // 24 Interval (minutes) between periodic flash-mode re-evaluations (AE light level or time-of-day window). 0 disables
```

`fatfs_task.c`'s default-value table (`op_parameter[]`) gets three new trailing
entries (32/33's `OP_PARAMETER_RFU_1/2` need no entry - unlisted trailing
array elements default to 0 in C, which is exactly "RFU" already):

```c
0,      // 34 OP_PARAMETER_FLASH_MODE (0 = off - matches today's default behaviour)
0,      // 35 OP_PARAMETER_FLASH_TOD_START
0,      // 36 OP_PARAMETER_FLASH_TOD_DURATION
```

Per `fatfs_task.h`'s own standing warning comment, `aiProcessor.h` (MKL62BA /
`ww-hardware` repo) needs the matching update - Charles's own doing, since he
pre-planned the RFU slots for this; flagging so it isn't forgotten, not as a
blocker on this repo's implementation.

## 4. `ledFlash.h`/`ledFlash.c` changes

`FlashLedMode_t`:

```c
typedef enum flashLedMode {
    FLASH_MODE_OFF = 0,        // Off all the time
    FLASH_MODE_AE,              // Determined by light levels
    FLASH_MODE_ALWAYS_ON,       // On all the time
    FLASH_MODE_TIME_OF_DAY,     // On within a configured UTC time window
} FlashLedMode_t;
```

`OP_PARAMETER_FLASH_MODE`'s value maps directly onto `FlashLedMode_t` - one
source of truth, no separate inference table (possible now that §2.1 removes
the need to keep `ledInUse == 0` doing double duty as a mode override).
`OP_PARAMETER_FLASH_LED` (op13) keeps its existing, narrower meaning: which
LED colour(s) to use *when the flash is active* - orthogonal to *why* it's
active. If op13 = 0 (no LED selected), `ledFlashIsActive()` already returns 0
regardless of `flashActive`, so nothing fires - no special-case code needed
for that combination.

`ledFlashSetFlashModeFromOpParam()` gains a second parameter and a mode
`switch`, called from `setupLEDFlash()` (`image_task.c`) at every wake, same
as today:

```c
void ledFlashSetFlashModeFromOpParam(uint16_t ledInUse, uint16_t flashModeParam) {
    ledFlashSelectLED(ledInUse);
    flashMode = (FlashLedMode_t) flashModeParam;

    switch (flashMode) {
    case FLASH_MODE_OFF:
        flashActive = false;
        break;
    case FLASH_MODE_ALWAYS_ON:
        flashActive = true;
        break;
    case FLASH_MODE_TIME_OF_DAY:
        evaluateTimeOfDay();
        break;
    case FLASH_MODE_AE:
    default:
        // Restore the last AE light decision - persisted because RAM is lost
        // in DPD, and the first capture after a motion-detect wake happens
        // before any fresh AE reading exists.
        flashActive = lightSensor_isDark();
        break;
    }

    // debug
    XP_CYAN xprintf("[LS] In ledFlashSetFlashModeFromOpParam with %d Mode %d\n",
            ledInUse, flashMode); XP_WHITE
}
```

New local helper (static, `ledFlash.c`) - the only place RTC-based time-of-day
math lives:

```c
#define MINUTES_PER_DAY (24 * 60)

// Sets flashActive from the current UTC time and OP_PARAMETER_FLASH_TOD_START/
// OP_PARAMETER_FLASH_TOD_DURATION - a single wrap-around window, deliberately
// no sunrise/sunset or seasonal adjustment (the flash does not need to switch
// at precise times - see decision §2.6). A GPS-based sunrise/sunset refinement
// was discussed separately and deferred:
// https://chatgpt.com/share/6a97cda3-32f0-83ec-ad6f-bee5b1845321
static void evaluateTimeOfDay(void) {
    rtc_time now;
    uint16_t minutesAfterMidnight;
    uint16_t start;
    uint16_t duration;

    if (exif_utc_get_rtc_as_time(&now) != RTC_NO_ERROR) {
        return;   // no fresh time available - leave flashActive as it was
    }

    minutesAfterMidnight = (uint16_t)((now.tm_hour * 60) + now.tm_min);
    start    = (uint16_t) fatfs_getOperationalParameter(OP_PARAMETER_FLASH_TOD_START);
    duration = (uint16_t) fatfs_getOperationalParameter(OP_PARAMETER_FLASH_TOD_DURATION);

    flashActive = ((minutesAfterMidnight - start + MINUTES_PER_DAY) % MINUTES_PER_DAY) < duration;
}
```

(`exif_utc.h` needs adding to `ledFlash.c`'s includes - a generic time-utility
header, no circularity risk checked.)

New exported function, declared in `ledFlash.h`, for the "RTC just changed"
trigger (§2.5):

```c
// Re-evaluate FLASH_MODE_TIME_OF_DAY immediately after the RTC is set (e.g.
// prvSetUtc()) - no-op in any other mode. Only updates the flashActive flag,
// same as ledFlash_setActive() - does not drive hardware directly (that
// pattern caused a real bug earlier in this review: flash staying solid on
// for ~1s with nothing needing it lit yet - see the "Completed tasks" log).
void ledFlash_reevaluateTimeOfDay(void) {
    if (flashMode == FLASH_MODE_TIME_OF_DAY) {
        evaluateTimeOfDay();
    }
}
```

## 5. `CLI-commands.c` change

`prvSetUtc()`, right after a successful RTC set:

```c
if (ret == RTC_NO_ERROR) {
    ledFlash_reevaluateTimeOfDay();
    snprintf(pcWriteBuffer, xWriteBufferLen, "RTC set to %s (this took %dms)", pcParameter, (int) elapsedMs);
}
```

## 6. `image_task.c` changes

**`setupLEDFlash()`** - pass the new op-parameter through:

```c
ledFlashSetFlashModeFromOpParam(
        fatfs_getOperationalParameter(OP_PARAMETER_FLASH_LED),
        fatfs_getOperationalParameter(OP_PARAMETER_FLASH_MODE));
```

**Periodic-wake-required condition** - currently `aeCheckRequired =
lightSensor_isRequired();`. Per decision §2.7, the TIME_OF_DAY consideration
is added here, in `image_task.c`, not inside `lightSensor_isRequired()`:

```c
aeCheckRequired = lightSensor_isRequired()
        || (ledFlashGetFlashMode() == FLASH_MODE_TIME_OF_DAY);
```

This is the only code-level change needed to make the periodic wake happen at
all under `FLASH_MODE_TIME_OF_DAY` with no other consumer (AE/slot-switch)
active. Once that forced wake happens (for *any* reason - motion, timer, or
BLE), `setupLEDFlash()` already runs as part of ordinary wake/init and already
calls `evaluateTimeOfDay()` via §4's `switch` - no new capture-path logic is
needed for time-of-day specifically. The existing `aeCheckOnlyWake` throwaway-
capture mechanics (skip NN, skip file save) are reused unchanged for the
periodic-wake case in general; the post-capture light-check block itself needs
no change either, since `lightSensor_takeReading()` and the
`FLASH_MODE_AE`-gated `ledFlash_setActive()` call are already no-ops when
their own mode isn't selected.

**Naming**: `aeCheckRequired`/`aeCheckOnlyWake`/`aeCheckDelay`/
`aeCheckCliTriggered` keep working exactly as today; renaming them to
something like `flashEvaluate*` for clarity (matching the op-parameter rename)
is a straightforward mechanical rename to do while implementing, not a design
decision - flagging so the diff isn't a surprise, not asking about it further.

**Console line 1** (before entering DPD) - reworded to be mode-agnostic and to
stop calling it a "light level" check (misleading under `TIME_OF_DAY`, which
checks nothing about light):

```c
XP_CYAN xprintf("[LS] Will wake to re-evaluate the flash in %d seconds. Flash is currently %s.\n",
        aeCheckDelay, ledFlashIsActive() ? "armed" : "not armed"); XP_WHITE
```

**Console line 2** (wake info block, "Image sensor and data path
initialised...") - same rewording:

```c
xprintf("  Flash is currently %s.\n", ledFlashIsActive() ? "armed" : "not armed");
```

Both switch from `lightSensor_isDark()` (AE-specific, would go stale under the
new modes - the bug Charles caught in the previous review pass) to
`ledFlashIsActive()` (`ledFlash.c`, already mode-agnostic - reads the
`flashActive` flag that every mode now sets correctly). AE-specific detail
(mean AE, threshold, DARK/BRIGHT) remains available unchanged on the
`[LS] AE light check: ...` line and via the `light` CLI command - not
duplicated into these two general-status lines.

## 7. The CLI `light` command and `decideDarkBright()`/`decideDarkBrightGainBased()`

No change. As established when Charles asked about this directly: these stay
relevant whenever `FLASH_MODE_AE` *or* `OP_PARAMETER_SLOT_SWITCH` (automatic
camera switching, independent of flash mode) is active, and the CLI `light`
command keeps working under any mode (`lightSensor_takeReadingForced()`
deliberately bypasses `lightSensor_isRequired()`). Its DARK/BRIGHT verdict may
not correspond to what's currently controlling the flash under `ALWAYS_ON`/
`TIME_OF_DAY` - worth a one-line note in `light_sensor.md` so that isn't
mistaken for a bug, not a code change.

## 8. Documentation updates

**`MANIFEST/config_file.md`**: add three table rows (34/35/36), update row 24's
name/description to `OP_PARAMETER_FLASH_EVALUATE_INTERVAL`, and rewrite the
"LED Flash operation" section (currently states the time-of-day/always-on
modes "have been removed" - no longer true) to document all four modes, e.g.:

```markdown
## LED Flash operation

The capture flash has four modes, set by OP_PARAMETER_FLASH_MODE:

| Mode | Value | Behaviour |
|------|-------|-----------|
| Off            | 0 | Never fires |
| AE-driven      | 1 | Fires when the AE light sensor judges the scene dark - see OP_PARAMETER_AE_DARK_THRESHOLD |
| Always on      | 2 | Fires on every capture |
| Time of day    | 3 | Fires within a UTC window: OP_PARAMETER_FLASH_TOD_START (minutes after midnight) for OP_PARAMETER_FLASH_TOD_DURATION minutes (wraps past midnight) |

OP_PARAMETER_FLASH_LED selects which LED colour(s) are used when the flash is
active (0 = none, 1 = visible, 2 = IR) - independent of which mode is
selected. OP_PARAMETER_FLASH_EVALUATE_INTERVAL (minutes) paces how often the
mode is periodically re-evaluated while asleep with nothing else waking the
device (AE light level, or the time-of-day window) - 0 disables the periodic
wake. Motion-detection illumination (OP_PARAMETER_MD_FLASH_LED /
OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT) is independent of all of this - see
_Documentation/AE_Light_Sensor_Roadmap.md.
```

**`MANIFEST/CONFIG.TXT`** (the shipped default config): add, after line 32
(`23 65`)/before the current op24 line:

```
# 24: flash re-evaluation interval (minutes, 0 disables) - AE light level or time-of-day window
24 15
...
# 34: capture flash mode: 0=off, 1=AE, 2=always-on, 3=time-of-day
34 0
# 35: time-of-day window start (minutes after midnight UTC) - only used in mode 3
35 0
# 36: time-of-day window duration (minutes, wraps past midnight) - only used in mode 3
36 0
```

(`OP_PARAMETER_FLASH_MODE` defaults to 0/off, matching today's out-of-the-box
behaviour with `OP_PARAMETER_FLASH_LED` also defaulting to 0 in this file.)

**`_Documentation/AE_Light_Sensor_Roadmap.md` §8.1** ("Time-of-day mode
removed") gets a short follow-up note pointing at this doc, since it's no
longer accurate as the final word on the subject.

**ChatGPT link** (sunrise/sunset from GPS): <https://chatgpt.com/share/6a97cda3-32f0-83ec-ad6f-bee5b1845321>,
placed per Charles's instruction in the `evaluateTimeOfDay()` doc comment (§4
above) as the permanent code-side reference, and here in the doc for
discoverability - relevant since GPS location is already stored on-device
(`exif_gps_deviceLat`/`Lon`, parsed from `CONFIG.TXT`), so a future
sunrise/sunset-relative mode would only need the time computation itself, not
new location storage. Deliberately not implemented now - see decision §2.6.

## 9. Summary of files touched

`fatfs_task.h`, `fatfs_task.c`, `ledFlash.h`, `ledFlash.c`, `image_task.c`,
`CLI-commands.c`, `MANIFEST/config_file.md`, `MANIFEST/CONFIG.TXT`,
`_Documentation/AE_Light_Sensor_Roadmap.md`. `lightSensor.c`/`.h` untouched
(per decision §2.7). Ready to implement on your go-ahead.
