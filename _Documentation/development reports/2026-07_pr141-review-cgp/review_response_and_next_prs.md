# Responses to your #141 review + what to focus on in #142 and #140

*6 Aug 2026. Checked against the code, file:line throughout. Companion to the earlier pre-read (topics 1–2); setup for the next reviews is in the attached worktree note.*

---

## 0. Bench results (6 Aug, WW500 C02 on the console — your `firmware_updates_CGP3` build of 14 Jun)

Scripted serial experiment: button reset → `setop 7 30` → DPD → RTC timer wake (warm session, raw regs `Wakeup_event = 0x0002 RTC Timer`) → `reset` → deferred watchdog → verdict boot → `setop 7 0` restored. Findings:

1. **Your watchdog claim is confirmed on hardware:** the boot after a watchdog reset reads `Wakeup_event = 0x0000, WakeupEvt1 = 0x0000 Cold boot` — even when the pre-reset session was a genuine RTC wake. No AON retention, no stale-warm misclassification. So the only remaining reason to revert the cold-boot gate on `labelBootSlot()` is the drop-before-`reset` path (§3 below) — a DPD wake can be a new image's first boot, and that boot is warm.
2. **The RTC wipe is real and fires on every deliberate reboot:** because watchdog resets take the cold path, each `reset`/`switchslot`/auto-switch runs `exif_utc_init("2024-01-01…")` and rewinds the clock (observed live: woke at 00:01:04, post-watchdog boot restarted from the epoch). Deployed consequence: an auto-switch at dusk wrongly timestamps all photos until the next BLE/LoRa time sync. Proposed one-line-class fix for the #141 batch: keep the RTC when it already reads ≥ 2025.
3. **The pre-#141 IF-task fragility reproduced organically:** during one run, your June-14 build wedged its IF task in `I2C RX State` after an incomplete `selftest` exchange — every later inactivity logged `IF Task unhandled event 'Inactivity' in 'I2C RX State'` and the device could never sleep (which also blocks the deferred reset). Logged evidence for topic 6's reason to exist; worth checking the #141 fixes cover this exact ordering.

Also confirmed from source while setting this up: the ~1.9 s AE sampling burst currently runs **per image** of a multi-shot sequence, not once per wake (`handleEventForCapturing` has no last-frame guard) — a 3-shot MD event pays ~5.7 s. Small guard proposed for the batch.

## 1. The items you flagged as inaccurate — checked

**"Bounded 3 s wait — I see no sign of this."** It's split across the two files the PR names: the wait is in `sendMsgToMaster()` — `image_task.c:1995–2001`, `xSemaphoreTake(xI2CTxSemaphore, pdMS_TO_TICKS(3000))`; the drop-and-release half is `if_task.c:1403–1418`. Searching if_task.c alone misses it.

**Topic 7 — "SAVE_STATE: I see no code for this… a plain inactivity sleep saves the state file."** The code exists: handler `fatfs_task.c:681–710`, senders `image_task.c:691/1098/1318`. And you're right that the plain inactivity path saves — for the simple ordering. The provable gap: SAVE_STATE saves **and unmounts** (`fatfs_task.c:694`), and the inactivity handler only saves `if (fatfs_mounted())` — otherwise it sleeps with no save (`image_task.c:695–699`). So: inactivity → save + unmount → BLE activity keeps the device awake → `setop` (RAM only) → next inactivity finds fatfs unmounted → **value silently lost**. SAVE_CONFIG saves at setop time instead, and warns if it can't. Agreed the comment's rationale sentence is loose — we'll tighten it.

**Topic 6 — "the capture retry is ineffective; I saw the 3 attempts."** Your observation is real, and it's exactly why #140 replaces the mechanism: *"retries now dwell progressively (100–500 ms, 5 retries) — instant back-to-back restarts were observed failing where a delayed restart succeeds."* Please don't remove it under `WDTIMOUTFIX` — its other job is preventing the old teardown-then-wedge failure — and review #140's dwell version as the fix for what you saw.

**Topic 8 — "register files moved from MANIFEST to SD root — retrograde."** You're right: main listed `HM0360EX.BIN` under MANIFEST (`MANIFEST/README.TXT:9`) with a cwd-relative define (`image_task.c:163`); the PR pinned it to the root. Pinning was the right instinct, the root the wrong place. Proposed fix: `"0:/MANIFEST/…"` + doc update — keeps one-folder provisioning.

**Topic 9 — "Compile_and_flash.md 'SUPERSEDED' — not true."** Fair: `building_firmware.md` replaces the toolchain/build/image-gen part only; the Eclipse, TeraTerm and SWD walkthroughs exist nowhere else. We'll reword the header.

**Topic 4 — "there's already a JPEG encoder in the SDK."** Confirmed: `EPII_CM55M_APP_S/library/JPEGENC/`. Good catch — to be benchmarked against `sw_jpeg.c` (your 183 ms). It's C++, but the app already links C++ via `cvapp.cpp`. Settle it while reviewing #140.

## 2. Your questions, short answers

- **Dark-box tests: who / how good?** Victor, on a WW500 C02. DARK 18/18 in a sealed box (mean ~0–35); BRIGHT 15/15 in room light (~78); hysteresis held at 67 vs threshold 65, released ≥77; `setop 23 45` followed and survived DPD; gain-railed override fired (gains 4/192).
- **"Did in-sensor WB fail?"** Not concluded — never run (the runbook results table is blank). The software path was chosen; #140 completes it with auto-AWB. In-sensor per-channel gains remain an open avenue.
- **Magic numbers in `wb_apply_yuv420()`** — the fixed-point YUV↔RGB coefficients; agreed: named `#define`s + provenance comments. Same for documenting `_Tools/wb_configs/` (three A/B CONFIG.TXT sets: tuned / uncorrected / no-WB).
- **`ww_serial.py`** — shared serial helper the other tools import; not run standalone.
- **Models to test with** — `model_zoo/` at the repo root; installed via `/MANIFEST` + the model-update flow (`cv_newModel`).
- **`VYMDDHMM.IMG` — where applied / generate locally?** Applied by the website's MANIFEST bundling (CI uses `WW500_C02_<VARIANT>_<timestamp>.img`; the generator always writes `output.img`). A local rename script is an easy add — good first commit if you want it.
- **"Ask Victor's Claude for its memory"** — honestly: the assistant's memory doesn't hold the original development transcripts; the durable record is the repo docs + PR descriptions, and where evidence was needed we verified from code (§1). Going forward we'll adopt your `claude/` folder convention for process notes.
- **AE wake + LoRaWAN merge (your TODO)** — plausible: the nRF could wake the Himax at heartbeat time (WAKE pin) and the RTC alarm goes away. Worth designing on the call together with the op24/op26 battery question (pre-read §5).

## 3. Your code changes — proposed triage

| Change | Proposal |
|---|---|
| Comment rewrites, defines-to-top, `ae_monitor.py` fix | **Keep** — commit (now on `review/cgp-141` per the worktree note). |
| Cold-boot gate on `labelBootSlot()` | **Revert** — your watchdog-→-cold reasoning is bench-confirmed (§0.1), but the remaining hole is enough: a drop before `AI reset` makes a plain DPD wake the new image's first boot (selector flips at `firmware` time, `xip_manager.c:1621`) — and that's warm, so the slot never labels. Safe alternative in pre-read §1 (XIP-mapped read). |
| MD flash at brightness 0 ("0 = dim not off") | **Verify then keep** — one bench check of duty-0 behaviour; note #142 raises op22's default 5 → 50, so decide semantics + default together. op21=0 stays "off". |
| Flash EXIF 0/1/2 | **Adjust** — the standard Flash tag (0x9209) must stay EXIF-spec (bit 0 = fired); put the LED type in the MakerNote CSV instead. App/website heads-up either way. |
| Restore logits / drop `USE_PERCENTAGE` | **Coordinate first** — the ww-website ingestion branch consumes label+confidence, which is why it was enabled. Your points (no float, server can derive) are good inputs; it's a cross-repo contract — decide together, then also drop `NN_confidence_EXIF_not_written.md` if we go logits. |
| `#ifdef WDTIMOUTFIX` | **Park** — resolved by #140's dwell retry (§1). |
| `xSPIMutex` creation location | **Either** — the property that matters is pre-scheduler (it is: `xip_manager_preinit()`, `ww500_md.c:800`); move it to fit your convention if you like. |

## 4. Reviewing #142 — where your eye is most valuable (~a day)

Scaffold: `REVIEW_PR142.md` at the branch tip. Focus:

1. `if_task.c` — I2C slave re-arm moved **into the TX-done interrupt**: ISR-context correctness, races with task state.
2. `fatfs_task.c` — SD write path: `f_sync` every 16; `f_sync` after `f_open(CREATE_ALWAYS)` (overwrite fix); 3× retry with backoff; **short writes fail fast** (file pointer already advanced — check the reasoning).
3. Log suppression during transfers (`g_fileRxActive`) — anything silenced that matters mid-failure?
4. Missing-master window 1 s → 4 s — knock-on timeouts.
5. `camera_switch.c` label retry — closes the ignored-return-value gap from your notes.
6. The default flips **op26 → 1** and **op22 → 50** — both have open call items; we may settle them first so you review the decided state.

Hardware pass: one large app → SD transfer with the console visible, the overwrite case, then a full `AI firmware` update over BLE. FYI: #146 sits on top (one-line iOS inactivity 5 → 15 s) — glance, not in scope.

## 5. Reviewing #140 — direction, not polish

Scaffold: `REVIEW_PR140.md`, plus `live_preview.md` and `rp3-image-quality-plan.md`. Read it as **the answer to several of your #141 findings**: your capture-retry observation → progressive dwell; the fixed WB gains → auto-AWB (op31, with op27/28 as flash/dark fallback); no AE on the IMX708 → op29/30; full pipeline in RPi ISP order from libcamera's `imx708.json`.

Best entry point is to run it: flash the #140 build, `py _Tools\live_view.py --port <console COM>` → ~1 fps live video with the CLI still usable; `setop 30/31`, `camreg`, `vcm` changes visible one frame later. That loop is also the practical answer to your "how did Claude find the problems" question (`tune_stats.py` scores frames against phone reference photos; G/R 1.37 → 0.97).

Direction calls for your judgement: the op29–31 design; the pipeline order/calibration source; **`sw_jpeg.c` vs `library/JPEGENC`**; the known-flaky warm-reboot bring-up; the preview protocol choice.

One board session covers all three PRs: op26 box/unbox cycle (#141 — still the one unvalidated path, please keep serial logs), a big BLE transfer (#142), the preview loop (#140).

## 6. Call agenda

1. Cold-boot revert + XIP-read replacement (pre-read §1); your Cold/Warm observation.
2. Triage table above — especially the EXIF/logits contract and MD-brightness/op22 semantics.
3. Battery: op26/op24 defaults + your LoRaWAN-alignment idea.
4. Topic-2 items (pre-read §4): camreg verdicts, decode tooling, `#ifdef` question, where process docs live (your `claude/` folder proposal).
5. Merge plan: cherry-pick agreed commits from `review/cgp-141` → merge #141 to dev → stack retargets → you review #142, then #140.
6. Board-session logistics.
