# WW500 light sensor: ground truth and what needs fixing

#### File: light_sensor_ground_truth.md
#### Author: Claude (Opus 5, then Fable 5.1), with Victor Anton, for Charles Palmer
#### 2 September 2026, updated 3 September 2026 against `ae_review` e8b7feb5

Everything below comes from three-way bench captures (the app's own log, the nRF console at
115200, the Himax console at 921600, interleaved with millisecond timestamps) on two evenings,
New Zealand time. Nothing here is inferred from a code read alone unless it says so. Line
numbers are `ae_review` at e8b7feb5.

| | 2 September | 3 September |
|---|---|---|
| Firmware | `ae_review` at ee65771f, gain-based decision | `ae_review` at e8b7feb5 on both slots |
| Device | WW500 `WILD-CNKW`, HM0360 then RP3 | the same, both images |
| App | Light Sensor readings, wildlifeai/ww-mobile-app#263 | Capture Picture, wildlifeai/ww-mobile-app#264 |
| Measurements | 25 `light` checks over three runs, none dropped | about twenty captures with every flash choice, three console tests of the flash gate |
| Op parameters | op13 = 0, op11 = 0, op21 = 2, op23 = 65, op24 = 15, op26 = 1 | op13 as chosen (0, 1, 2), op9 80, op11 = 0, op25 as the checks wrote it |
| Evidence | app repo, `documentation/development reports/2026-09-02_light-sensor-readings/` | app repo, `.../2026-09-03_capture-flash-and-keep-awake/`; this repo, `2026-09-03_capture_bench_findings/` |

## Summary

| # | Finding | Status on 3 September | Where |
|---|---|---|---|
| 1 | The STROBE pin stayed armed through the gain-based light check | **Fixed at e8b7feb5.** Not yet bench-verified: op11 was 0 in every run, so the arming path never ran | `lightSensor.c:129` to `133`, `162` to `165` |
| 2 | The passive `light` command writes its verdict to op25 | **Decide.** Unchanged, and the app now leans on the same persistence (section 6) | `lightSensor.c:170` |
| 3 | The gain rule flips at a steady scene in the transition zone | **Decide, with data.** Seeed#204 | algorithm choice |
| 4 | The nRF parses the sleep stats and forwards only `Sleep` | **Fix, ww-hardware.** Not filed | nRF relay |
| 5 | Seeed#203, the wording, the ordering, the `light` ack | **Verified.** The wording changed again at e8b7feb5; both forms fit under the clip | |
| 6 | A chosen flash LED does not fire unless op25 says dark | **Verified 3 September.** The reason the flash-mode op is needed | `ledFlash.c:357`, `image_task.c:2732` |
| 7 | Flash and timer settings apply at wake, not on `setop` | **Verified 3 September.** `AI dpd` exists but is a race until Seeed#205 is fixed | `ww500_md.c`, `image_task.c` |

What changed since the 2 September version: finding 1 is fixed in code; the decision line is
now `AE light check: AGain = 2, conv=Y -> BRIGHT (change)`; findings 6 and 7 are new; and the
capture bench found a device that never sleeps again after an inactivity event lands during an
I2C reply, Seeed#205, which is not a light sensor fault but bit this work twice.

## 1. The STROBE pin stayed armed through the gain-based light check

**What it was.** `decideDarkBrightGainBased()` woke the HM0360 into `MODE_SW_CONTINUOUS`,
read the gain registers and restored the mode, and never touched the strobe. The mean-based
path did (`sampleAeStats()` saves, disables and restores it). The arming is real and
survives DPD: at sleep, `image_task.c:2732` calls `hm0360_md_configureStrobe(true)` when
`ledFlashIsActive() > 0`, `mdInterval > 0` and `OP_PARAMETER_MD_FLASH_LED != 0`, and
`ledFlashIsActive()` (`ledFlash.c:318`) returns op13 whenever `flashActive` is set, which is
restored from op25 at every wake (`ledFlash.c:357`). A DARK verdict, op13 set, motion detection
on, op21 set: that sleep armed the strobe, and the next `light` streamed about five frames at
10 fps with it armed. Five flashes in half a second, the fast flicker Charles reported.

**The fix, at e8b7feb5.** The gain-based path now saves and disables the strobe before the read
(`lightSensor.c:129` to `133`) and restores it after (`:162` to `165`), the same treatment the
mean-based path had.

**Still owed: the bench proof.** Every run so far had op11 = 0, so the console said
`No LED flashes.` at every sleep and the arming never happened. To close it: `setop 13 2`,
`setop 11 1000`, `setop 21 2`, `setop 22 50`; cover the lens; `capture 1 1` (its check writes
op25 = 1); wait for the sleep and confirm `Preparing HM0360 for MD:   LED flashes.` on the
console, which is the proof the strobe is armed; then `light` and watch the IR LED. Before the
fix: a burst. After: nothing.

## 2. The passive `light` command writes its verdict to op25

**What we saw** (2 September). Nine consecutive `light` commands from the app, op13 = 0 the
whole time, and the op array the Himax sends at each sleep:

```
seq=232  op25=1   (before the first light)
seq=233  op25=0   after: AGain = 0, conv=Y -> BRIGHT (change)
seq=234  op25=1   after: AGain = 3, conv=N -> DARK (change)
seq=237  op25=0   after: AGain = 0, conv=Y -> BRIGHT (change)
```

op25 follows the verdict of every passive check. `lightSensor.c:170` (gain-based) and `:328`
(mean-based) persist it; both are reached from `lightSensor_takeReadingForced()` (`:302`),
which `prvLight()` calls as, in its own words, "a passive diagnostic, no flash arming, no
camera-switch check". The persistence is the one side effect the passive path did not skip.

**Why it matters.** Through finding 6's chain, the verdict a diagnostic wrote becomes
`flashActive` at the next wake, so it decides the flash on the next real capture before that
capture's own check runs, and whether the following sleep arms MD illumination. On a bench that
is what we want; on a deployed device an operator checking the light is setting the flash.

**Reproduce.** `setop 13 2`; bright scene; `light`, `getop 25`: 0. Cover the lens; `light`,
`getop 25`: 1. Uncover; `capture 1 1`: the flash fires on a bright scene.

**Options.** Skip the persist when `aeCheckCliTriggered` is set (`image_task.c:270`), keeping
it on the capture and timer paths, which is a two-line change matching the comment in
`prvLight()`; or keep persisting and say so, in which case the app must stop streaming `light`
outside the bench. Either way the flash-mode op (section 6) is where "flash on this capture"
should live, not in op25.

## 3. The gain rule flips at a steady scene in the transition zone

**What we saw** (2 September, run 1, HM0360, an indoor evening scene that did not change):

| check | AGain | conv | verdict | AE mean | integration |
|---|---|---|---|---|---|
| 1 | 0 | Y | BRIGHT (change) | 76 | 376 |
| 2 | 3 | N | DARK (change) | 90 | 376 |
| 3, 4 | 3 | Y | DARK | 84, 75 | 376 |
| 5 to 8 | 0 | Y | BRIGHT (change at 5) | 76 | 376 |

Two verdict changes in eight checks at a constant scene; the mean rule at op23 = 65 would have
called all eight BRIGHT. Integration sat at the ceiling throughout, the roadmap's transition
zone: the loop has no exposure left and hunts between analog gain 0 and 3, the gain rule turns
every hunt into a verdict, and nothing holds a verdict (`AE_HYSTERESIS` is 0, Seeed#204).
Check 2 decided on `conv=N`, 500 ms after waking the sensor. With op26 = 1 on the timer or
capture paths each DARK-to-BRIGHT flip schedules a camera-slot switch and a reboot.

**How to test both rules on one run.** The app's Light Sensor screen logs every reading with
the five registers, the device's verdict and its own scoring by both rules. Dim steady scene
near the boundary (mean 60 to 90, integration at 376), stream `light` every 3 s for five
minutes and count verdict changes (a usable sensor makes none), then a slow two-minute dimming
ramp (exactly one).

**Options.** Do not decide on `conv=N`; majority of N reads, or hysteresis on gain (dark above
2, bright at 0, hold between); or the mean rule with #204's re-tuned threshold.

## 4. The nRF parses the sleep stats and forwards only `Sleep` (ww-hardware)

At every sleep the Himax sends the whole op array and the nRF forwards six bytes:

```
himax | Sending 100 bytes: ... 'Sleep 232 0 0 51 420 1 500 0 1000 50 1 0 100 0 0 0 18 1 0 97 0 2 50 65 15 1 1 286 326 1 110 1 '
nrf   | <info> app: AI processor sends stats: '232 0 0 51 ...'
nrf   | BLE out: Sent   6 bytes: 'Sleep'
```

The device changes its own parameters while the app is not looking (op25 after every check,
the active slot on an automatic switch), so the app re-reads the whole array, one DPD wake
each time. It has since grown a per-wake cache that also patches what a `setop` wrote, but the
cache is still dropped at every Wake. Forwarding the array as sent (100 bytes, inside the
244-byte payload) gives the app op25 after every check for free. The app already treats
`^Sleep(\s+.*)?` as the sleep signal, so the suffix breaks nothing today.

## 5. Verified on `ae_review`, no action

- **Seeed#203 A, truncation.** At ee65771f both wordings arrived whole on all 25 checks. At
  e8b7feb5 the compiled line is `AE light check: AGain = 2, conv=Y -> BRIGHT (change)` and the
  mean-based one `mean AE=77 (min 75, max 80, 16 frames) thr=65, AGain=0, conv=Y, gain railed =
  N -> BRIGHT`; both fit under the 150-byte clip. The clip and the truncation warning suggested
  in #203 remain worth having, so the next overflow is caught at the bench.
- **Seeed#203 B, the stack-lifetime pointer.** `lightSensor.c:84` uses a file-scope
  `static char msgToMaster[]`. Closed.
- **Ordering.** The decision line is queued inside the check (`lightSensor.c:180`) and the
  register block after it (`image_task.c:944`). Held on every check, both days.
- **The `light` contract.** `Checking light level...` unchanged; `Failed to queue light check`
  (Seeed#202) in place, not exercised; 25 requests on 2 September, none dropped.
- **The wording.** The app stopped depending on any field but `-> DARK|BRIGHT` and accepts
  both forms, so wording changes cost it nothing. `ble_commands.md` documents both.

## 6. A chosen flash LED does not fire unless op25 says dark (3 September)

**What we saw.** The app selected the white LED (op13 = 1, brightness op9 = 80) and captured
in a lit room: no flash, every time, on both images. The chain, confirmed line by line:

1. op13 only selects the LED. `ledFlashSetFlashModeFromOpParam()` sets `FLASH_MODE_AE` for
   any non-zero op13 (`ledFlash.c`, around `:341` to `:358`) and restores
   `flashActive = lightSensor_isDark()` from op25 at every wake (`:357`).
2. A capture arms the LED only when `flashActive` is set: through STROBE on the HM0360 image
   (`hm0360_md_configureStrobe(ledFlashIsActive() > 0)`), through `ledFlashActivate()` on the
   RP3 image.
3. The check after every capture rewrites op25 with its own verdict (`lightSensor.c:170`).
   In a lit room that is BRIGHT, op25 goes to 0, and nothing the app selects will flash.

**Three console commands that separate the layers**, all proven at 15:14 on 3 September:
`AI flash 50 500` lights the white LED directly (hardware and command path); `AI getop 25`
shows the gate; `AI setop 25 1` then `AI capture 1 500` makes the next capture flash, and its
own check then puts the real verdict back. A covered lens through `AI light` does the same.

**Consequence.** The app's Capture Picture screen forces a flash by writing op25 = 1 before a
capture with a flash chosen, marked `TODO(flash-mode-op)`. It works because of finding 2's
persistence and the wake restore, which is exactly why it is a stand-in. The flash-mode op in
`flash_led_modes_proposal.md` (always off, always on, light sensor, time of day) is the answer,
with its index agreed first: the app already binds 32 and 33.

## 7. Flash and timer settings apply at wake, not on `setop` (3 September)

`setupLEDFlash()` reads op9 and op13 at wake, the inactivity period is set from op8 at boot
(`Inactivity period set at Nms`), and op10 is read when the image task starts. A `setop`
changes the RAM value and CONFIG.TXT; the running configuration changes at the next wake. The
app therefore waits for a sleep before every capture that changed a setting, 3 to 5 s each
time. `AI dpd` (`prvDpd()`, sets the period to 1 ms) would give a sleep on demand and worked
once on the bench, but while Seeed#205 is open every use is a 1 ms race between its own reply
and the detector. Applying the parameters on `setop`, or blessing `AI dpd` once #205 is
closed, are the two ways out; see `2026-09-03_capture_bench_findings/`.

## Also noted

- op23 is never read by the gain-based algorithm (`DARK_ANALOG_GAIN_THRESHOLD`, `lightSensor.c:49`,
  is a compile-time constant). The app stopped exposing it.
- The digital-gain decode fix of 28 August (`hm0360_md.c`, `0xfa >> 6` to `0xfc >> 2`) means the
  digital gain column in any export before that build is wrong.
- The phone renegotiates the BLE connection interval from 15 ms to 195 ms about twenty seconds
  after connecting, after which each notification takes about 300 ms to reach the app.
  ww-hardware#30 territory, not the light sensor's.
- A `setop` that lands after Save State replies `Set OpParam` but the config save fails with
  `Error 12`, so the value is lost at the next boot (capture bench finding B).
- op7 turned to 1 on its own on 3 September, with no logger running, and the device captured
  every second until the card was wiped (capture bench finding F). Cause not established.

## Reproducing the capture

`bench_log.py` (`mobile_app_three_way_bench/` in this folder) opens both consoles by FTDI
serial number, runs `adb logcat` for the app, and writes one time-ordered file. Windows COM
ports are exclusive, so close TeraTerm first. The findings above cite that file's timestamps.
