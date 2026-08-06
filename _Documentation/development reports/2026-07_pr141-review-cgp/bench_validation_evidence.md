# WW500 #141 bench validation — dual-slot labelling + op26 auto-switch cycle

*6 Aug 2026, WW500 C02 on the bench, driven entirely over the Himax console USB (COM5 @ 921600). All quotes are verbatim serial-log lines; `[ time ]` prefixes are seconds into the logging session (`console_session.log`, attached). This run closes both items PR #141's verification section listed as "pending on hardware": the **op26 box/unbox auto-switch cycle** and the **MODE_SLEEP sensor-wake light-check path**.*

## Setup

- Both #141 variants built from `feat/camera-features-combined` (tip `a70f016`) with the pinned Arm GNU 14.3.rel1 (GCC 14.3.1 20250623); RC24M image profile.
- Both flashed via the bootloader X-Modem route (`xmodem_send.py`, two burns, `result = True`, 0 block errors): consecutive burns alternate slots, ending with **Slot A = RP3 (built 15:22:59), Slot B = HM0360 (built 15:15:50)**.
- The board has both sensors: `HM0360 present at 0x24`, `Main camera present at 0x1a` (IMX708), so the full automatic cycle was testable.

## Part E ladder — every checkpoint, verbatim

| Checkpoint | Evidence |
|---|---|
| X-Modem burn wipes labels (documented behaviour, seen live) | After burn 2, `slots` → `Slot A: 'RP3 (day/colour)', Slot B: 'unknown'` — burn 2's selector rewrite erased slot B's label from burn 1 |
| Half-labelled state reported honestly | same line — `'unknown'` for the never-since-booted slot, not an error |
| Mismatch guard refuses an unlabelled slot | `[4143.4]` `AE light check: mean AE = 0 (min 0, max 8) over 16 frames, threshold = 65, gain railed = yes -> DARK (flash wanted)` → `Auto camera switch: light wants 'HM0360 (night/IR)' but other slot holds 'unknown' - staying` |
| Manual `switchslot` needs no label, reports honestly | `[4177.2]` `Switched to slot 1 ('unknown'). Reset scheduled.` |
| Deferred reset at sleep | `[4177.3]` `>>> Reset by watchdog` |
| First boot self-labels its slot | `[4180.7]` `Slot B labelled variant 1` (right after the `Camera: HM0360` banner) |
| Fully labelled state | `[4211.5]` `Active slot 1 running 'HM0360 (night/IR)'. Slot A: 'RP3 (day/colour)', Slot B: 'HM0360 (night/IR)'. Auto-switch: on` |

## The op26 automatic cycle — four wakes, both directions

All checks ran on the **deployed-firmware wake path** (op24 set to 1 min for the test): DPD → RTC wake → one-frame AE check (`Timer wake for AE light check` / `Skipping NN processing`), not bench-forced captures.

| Wake | Image | Scene | Reading | Decision | Action |
|---|---|---|---|---|---|
| `[4143]` | HM0360* | covered | mean 0 (0–8), gain railed | DARK | guard: **staying** (slot B then unlabelled) |
| `[4331]` | HM0360 | covered | mean 0 (0–0), gain railed | DARK | wanted == self → **stays silently** ✓ |
| `[4452]` | HM0360 | lamp on | **mean 79**, railed no | **BRIGHT (changed)** | `Auto camera switch: light is BRIGHT -> slot 0 ('RP3 (day/colour)'). Reset scheduled.` + BLE notice sent **before** the reboot → `[4512]` watchdog reset → RP3 boots |
| `[4637]` | **RP3** | lamp on | **mean 66** (dead band 65–77) | BRIGHT — **held by hysteresis**, no "(changed)" | no switch line → **no flip-flop** ✓ |
| `[4758]` | RP3 | re-covered | **mean 48** (45–72), gain railed | **DARK (changed)** | `Auto camera switch: light is DARK -> slot 1 ('HM0360 (night/IR)'). Reset scheduled.` → `[4818]` watchdog → back on HM0360 |

*\*first row ran on the RP3 image (before the switchslot) — see the log; table lists the running image per wake.*

Numbers worth noting: the bright flip released at mean 79 — just past the 65+12 hysteresis release the AE roadmap specifies; the dead-band hold at 66 and the min/max spreads match the multi-frame-aggregate design (§8.5). The RP3-image wakes exercised the **§8.5.1 MODE_SLEEP path** (MD off on that image, HM0360 woken for the sampling window) — the second "pending" item.

## Findings beyond the checklist

1. **Cold-boot IMX708 first capture fails all 3 instant retries** — `Frame timed out - restarting sensor, retry 1/3 … 3/3 … giving up` (5 s apart, back-to-back restarts), after which the image task goes `Uninitialised` and takes no further captures that session. The same sensor delivers frames reliably on every DPD wake. This is precisely the observation Charles logged against topic 6, and the empirical case for **#140's progressive-dwell retry** — review it there rather than deleting the retry under `WDTIMOUTFIX`.
2. **Immediate op-param persistence (topic 7) seen working**: every `setop` answered with `Config saved (op params persisted).` with no unmount, including mid-session.
3. **`RPV3_EX.BIN` sought at SD root** (`Error opening '0:/RPV3_EX.BIN'`) — live confirmation of the MANIFEST→root move Charles flagged (fix agreed: pin to `0:/MANIFEST/`).
4. **Watchdog resets classify cold and restart the RTC at the 2024 epoch** on this build too (banner + `Wakeup_event = 0x0000` + epoch timestamps after every deliberate reboot) — the separately-reported RTC-preserve fix stands.
5. The **BLE announcement reaches the app before the reboot** in both switch directions — the deferred-reset design doing its job.

## Final board state

Both slots hold today's #141 builds and correct labels; config restored to branch defaults and persisted (`Set OpParam 24 = 15`, `Set OpParam 26 = 0`, each `Config saved`); closing reading:

```
Active slot 1 running 'HM0360 (night/IR)'. Slot A: 'RP3 (day/colour)', Slot B: 'HM0360 (night/IR)'. Auto-switch: off
```

Raw log: `console_session.log` (attached; ~4,800 s, includes all boots, bursts and I2C traffic). Earlier same-day experiments (watchdog cold/warm classification, IF-task wedge on the old June-14 build) are written up separately in the review-response doc §0.

## Addendum (6 Aug, evening) — staged-exposure override validated (the zero-code fix for the field white-outs)

Context: field photos from the integration-preview camera showed all RP3 (colour) captures blown white while HM0360 captures were fine. Root cause chain: the IMX708 wakes at the init-table exposure (0x0940 = 2368 lines) every DPD cycle — `ae.c` (#140) resets its RAM state per wake and allows only 3 damped steps, so bright scenes never converge on the wake path (bench convergence used live preview, which lifts the cap). Two side-findings from the same photos: the EXIF `Model` says `WW500 HM0360` on RP3 photos (`image_task.c` tests `USE_HM0360 || USE_HM0360_MD` before `USE_RP3` — the both-defined trap `camera_switch.c`'s comment warns about, present in #141 too), and the MakerNote AE fields are the HM0360 light-sensor's registers, so the IMX708's actual exposure is recorded nowhere.

Validation on the bench WW500 (#141 RP3 image, 30 s timelapse so all captures use the reliable DPD-wake path):

| Step | Log evidence | Photo |
|---|---|---|
| Baseline, no staged file | `Error opening '0:/RPV3_EX.BIN': 4` → capture 52 ms → `SW-JPEG 14428 bytes` | dim-but-detailed lamp-lit room (stock 2368 lines ≈ right for a dim scene) |
| Stage short exposure | `camreg 0202 00` / `camreg 0203 40` → `2 staged, saving to '0:/RPV3_EX.BIN'` → `Saved 8 bytes` | — |
| Every following wake | `Loaded 2 staged register(s) from '0:/RPV3_EX.BIN'` → `Processed 2 settings` → capture 21 ms → `SW-JPEG ~5,720 bytes` ×5 wakes | near-black (64 lines is a daylight value — 37× less light, exactly as commanded) |
| Natural A/B/A control | one wake ran with the card removed: no staged load → `SW-JPEG 15619 bytes` (back at baseline) | — |

**Conclusion:** the `camreg` staged-register mechanism (Charles's CAMERA_EXTRA_FILE design + #141's camreg front-end) provably controls the IMX708 exposure on every wake with zero code changes — the immediate mitigation for bright-site deployments is to stage a short exposure (tens of lines for daylight). The value is per-scene, so mixed day/night lighting still needs the durable fix: **persist `ae.c`'s converged exposure/gain across DPD** (the one missing behaviour in #140's otherwise-validated AE) — a review note for #140, not new development. Also queued from this investigation: fix the EXIF Model `#if` ordering (in the #141 batch) and add IMX708 exposure/gain + op29–31 state to the RP3 MakerNote so field photos can self-diagnose.

*(The test card was reformatted afterwards; the two key frames are preserved as screenshots in the session record, and the complete serial log is `console_session.log` / `console_session_part1.log`. Board left clean: both #141 slots labelled, compiled-default config.)*
