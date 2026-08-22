# Responses to the PR #141 review

#### File: review_responses.md
#### Author: Claude (Opus 5), reviewed by Victor Anton
#### August 2026

*Answers to [`CGP_Code_Review_July26.md`](../../../EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/CGP_Code_Review_July26.md).
File:line references are against `review/cgp-141` at `55ccb85a`, and will drift as the code
moves.*

*Sections 0 to 7 are the review exchange as it stood in August, including positions later
revised. Section 8 was updated on 18 August to record what actually happened. For current
status see the [thread README](README.md).*

## 0. Bench results (6 Aug, WW500 C02, scripted serial — full detail in `bench_validation_evidence.md`)

1. **Watchdog resets classify cold** — `Wakeup_event = 0x0000/0x0000` even from a warm (RTC-wake `0x0002`) session. No AON retention; CGP's `app_getResetRequest()` → cold-boot claim confirmed. The cold-boot gate on `labelBootSlot()` still needs reverting, but for exactly one reason: an interrupted OTA makes a plain DPD wake the new image's first boot — and that's warm (selector flips at `firmware` time, `xip_manager.c:1636`). Safe latency win instead: the XIP-mapped selector read (`xip_manager.c:927` "Simplification opportunity").
2. **RTC wipe proven**: every deliberate reboot takes the cold path and `exif_utc_init("2024-01-01…")` rewinds the clock until the next time sync. Fix: keep an RTC already reading ≥2025.
3. **Pre-#141 IF-task fragility reproduced organically** (June-14 build wedged in `I2C RX State`, could never sleep) — evidence for topic 6's reason to exist.
4. **AE burst runs per image, not per wake** (no last-frame guard in `handleEventForCapturing`) — a 3-shot MD event pays 3 × ~1.9 s.

## 1. Items flagged as inaccurate — checked

- **"Bounded 3 s wait — no sign of it."** It's split across the two files the PR names: the wait is `image_task.c:2019–2025` (`sendMsgToMaster()`, `xSemaphoreTake(…, pdMS_TO_TICKS(3000))`); the drop-and-release half is `if_task.c:1403–1418`.
- **"SAVE_STATE — I see no code… plain inactivity saves anyway."** Handler `fatfs_task.c:681–710`; senders `image_task.c:699/1114/1334`. Plain inactivity does save — the gap: SAVE_STATE saves **and unmounts** (`:694`), and inactivity with fatfs unmounted sleeps with **no save** (`image_task.c:703–707`). So save+unmount → BLE keeps device awake → `setop` (RAM only) → next inactivity → silently lost. SAVE_CONFIG saves at setop time (bench: `Config saved (op params persisted).`). The comment's parenthetical will be tightened.
- **"Capture retry is ineffective — I saw the 3 attempts."** Real, and reproduced on the bench: cold-boot IMX708 captures fail all 3 instant retries then the image task goes `Uninitialised`; DPD-wake captures are reliable. This is the case for **#140's progressive-dwell retry** — resolve `WDTIMOUTFIX` there; the retry's other job is preventing the old teardown-wedge.
- **"Register files moved MANIFEST → SD root — retrograde."** Correct: main's `MANIFEST/README.TXT:9` + cwd-relative define vs the PR's `"0:/…"` (`image_task.h:56–65`). Fix: pin to `0:/MANIFEST/`.
- **"`Compile_and_flash.md` SUPERSEDED — not true."** Fair — superseded for build/image-gen only; Eclipse/TeraTerm/SWD content exists nowhere else. Header to be reworded.
- **"There's already a JPEG encoder in the SDK."** Confirmed (`library/JPEGENC`). Nuance: it takes 8/24 bpp, not YUV420 planar — a swap needs a ~900 KB RGB buffer or strip-wise feeding, so it's an integration task to benchmark (183 ms baseline), best settled during #140.

## 2. Short answers

- **Your dual-image build/flash steps (from your 4 Aug email)** — all six essentially right; the details now live in the standalone [`_Documentation/dual_image_build_and_flash.md`](../../dual_image_build_and_flash.md). The three corrections: (1) both variant runs write the same `output.img` — rename between runs; (2) SD filenames must be 8.3 in `/MANIFEST` (FatFS has no LFN, and `firmware` truncates >13 chars — a long name can pass the CRC gate then fail "not found"); (3) step 4 is the other way round — flashing *clears* the slot label and each image labels its own slot at **first boot**, so it's flash → `reset` → flash → `reset`, and only then does `slots` show both variants. Your step-1 values (`cis_hm0360`/`cis_imx708`) were exact.
- **Dark-box tests: who/how good?** Victor, WW500 C02: DARK 18/18 (box), BRIGHT 15/15 (room), hysteresis held at 67/65 and released ≥77, threshold change followed + survived DPD, gain-railed override fired (4/192). The bench cycle (§0) adds: release at mean 79, dead-band hold at 66.
- **"Did in-sensor WB fail?"** Never concluded — phase-0 was never run (runbook results still blank); the software path was chosen and #140 completes it (auto-AWB).
- **`wb_apply_yuv420` magic numbers** — BT.601 fixed-point coefficients (1.402·Cr, 0.344·Cb+0.714·Cr, 1.772·Cb); to become named `#define`s. `_Tools/wb_configs/` = three A/B CONFIG.TXT sets (tuned / uncorrected / no-WB).
- **`ww_serial.py`** — shared serial helper the other tools import.
- **Models to test with** — `model_zoo/` at repo root; installed via `/MANIFEST` + `cv_newModel`.
- **`VYMDDHMM.IMG` naming** — applied by the website's MANIFEST bundling (CI names `WW500_C02_<VARIANT>_<ts>.img`; the generator always writes `output.img`). A local rename script is an easy add.
- **"Ask Victor's Claude for its memory"** — the assistant's memory doesn't hold the original development transcripts; the durable record is the repo docs/PRs, and where evidence was needed we verified from code (above). This folder is the fix going forward.
- **`xip_copy_metadata_to_flash()` TODO** — different metadata (NN `"LABL"` record at 0x00200000, not the selector sector). Real latent hazard confirmed: `write_metadata_to_flash()` does a bare word-write, **no erase** (`xip_manager.c:682–710`) — safe only because `cvapp.cpp:407` runs right after the region erase; precondition undocumented; `ModelMetaData.crc` unimplemented. The WWSM labels, by contrast, are preserved across selector rewrites (`xip_manager.c:1495–1498`).
- **`APP_MSG_CLITASK_DISK_WRITE_COMPLETE`** — fatfs_task's async completion reply to the CLI task (`fatfs_task.c:482/605/648`), echoing the `fileOperation_t*` so outstanding ops can be told apart; consumed at `CLI-commands.c:2562–2581`. Predates the PR; the pointer echo is new.

## 3. `cameraSwitch_autoSwitchCheck()` — the walkthrough you asked for

Seven cheap early-outs, then the switch (`camera_switch.c:73–127`): (1) function-static `switchScheduled` latch (no double-switch before the reboot); (2) op26 ≠ 1 → false; (3) build variant UNKNOWN (RP2/IMX477) → false; (4) `dark = (op25 == 1)`, wanted = dark ? HM0360 : RP3, wanted == self → false — **it never reads the sensor**: op25 is the persisted, hysteresis-filtered decision from `ledFlashNewAEStats()` (`ledFlash.c:425–480`; 16-sample burst, gain-railed → dark, dead band 12 over op23), and because op25 survives DPD in CONFIG.TXT it also survives the reboot — that is what prevents a switch-back loop; (5) selector unreadable → false; (6) other slot not labelled with the wanted variant → log `…- staying`, false; (7) `xip_switch_slot()` (refuses an empty slot), set latch, `app_setResetRequest(true)`, BLE announcement, reboot at next sleep (`>>> Reset by watchdog`) — deferred so the announcement reaches the app first.

Called at `image_task.c:877` after each AE decision; the sampling block runs only when a consumer is enabled (`:863–864`). Into DPD, op26 arms an RTC wake every op24 minutes (`:2654–2667`). Asymmetry: manual `switchslot` needs no label — only a programmed image (`CLI-commands.c:874–876`) — which is how labels bootstrap.

## 4. Topic-2 (camreg / vcm) — verdicts on your comments

- **"Big, inefficient… `#ifdef` it for development."** Measured: ~495 lines, ~143 B static RAM (`stagedSettings[24]` = 96 B of packed 4-byte records); no duplicate table (`camRegSaveTable` is a *function*); extra boot read ≈ 1–5 ms. Counter-argument: roadmap Phase 2 builds the **app tuning card on these commands** — field adjustment without reflashing is a product goal (and the staged-exposure mitigation in §7 rides on it). Position: keep in production; a `FIELD_TUNING_CLI` gate is cheap if still wanted, but `cis_file_process()` predates the PR and must stay.
- **`camRegFileName` not required** — agreed it carries no information; it exists because `fileOperation_t.fileName` is non-const (`fatfs_task.h:144`). Fix the field, then delete it.
- **File read twice** — confirmed, strictly sequential, two different consumers (staged list vs `hx_drv_cis_setRegTable`); the second *file* read is avoidable (bytes already in RAM; one guard needed for >24-entry hand-authored files). Nearby easy cleanups: dead `f_getcwd` debug (`cis_file.c:191–197`), per-boot heap alloc (`:225`).
- **Reads and write in one file** — agreed: `cis_file_saveStagedToFile()` in `cis_file.c`; camreg calls it.
- **`camRegWritePending` "can only have been added by AI… no chance of typing fast enough"** — it's load-bearing: the save is **async** (queued to fatfs_task), the buffer is a **live pointer into `stagedSettings[]`** (`CLI-commands.c:1321`, no snapshot), and the CLI task **out-prioritises fatfs_task** (`ww500_md.c:809–854`) — so a queued second command runs before the write starts. Input isn't just typing: BLE feeds `AI camreg` lines into the same parser, the console runs at 921600 (a pasted block ≈ 90 KB/s), and the runbook prescribes back-to-back sequences. Without it: `FA_CREATE_ALWAYS` truncate-then-write → torn `RPV3_EX.BIN`. Removing it honestly costs a 96-byte snapshot — more RAM than the flag.
- **Provenance** — your best finding, one premise fixed: `scan_cis_settings.md` **and** `.py` both exist in `_Tools/` (pointed at by `cis_file.c:6–7`), but the doc is stale (HM0360-era, no camreg) and the pipeline is **one-way** — no `.BIN`→text decoder, so field `camreg` strands tuning state in comment-less binary; re-saves also force delay records to writes (`cis_file.c:155`) and drop entries past 24. Fixes: `--decode` mode (~20 lines), doc refresh, optional `camreg export`; your text-file-as-source-of-truth workflow is the call decision.
- **Register table — "does it exist?"** (i) Baseline: the driver includes `IMX708_common_setting.i` (~46 entries) and `IMX708_mipi_2lane_2304x1296.i` (~89; exposure `0x0202/03`, gain `0x0204/05`, per-channel digital `0x020E–0x0215`) + the exposure override in `cisdp_sensor.c:89–92`. (ii) Tuning targets: `camera-phase0-bench-runbook.md:33–41,70`. (iii) A record of deltas *found during tuning* doesn't exist — the phase-0 campaign was never run; what happened instead is §6.
- **`vcm`** — DW9817-type focus actuator at I2C `0x0C`, inline in `CLI-commands.c:1471–1553` (`#ifdef USE_RP3`); `probe` + `<0–1023>`; **not persistent** across a camera power cycle (roadmap Phase 3.1) — matching your bench observation of the lens releasing at DPD.

## 5. Battery / DPD impact

DPD floor current: unchanged by everything in these PRs. What changes is the wake economy: with op26=1 (or op13 AE) and timelapse 0, an RTC wake every op24 min (default 15) — each check ~5–10 s awake (SD init incl. 1 s delay, sensor, model load, ~1.9 s burst) ≈ **8–16 min extra awake/day** idle; plus the per-capture burst (~1.9 s — currently per *image*, §0.4). #141 ships battery-neutral defaults; **#142's op26→1 flip is the decision** — options: app-enables per install / op24=60 default / arm the wake only when the opposite slot is labelled. Your LoRaWAN-alignment idea (nRF wakes the Himax at heartbeat) folds in here.

## 6. Where the RP3 image-quality work ended up (the "position")

Method: instrumentation first — live preview (#140) + `live_view.py` made a ~1 fps tuning loop; `tune_stats.py` scored frames against phone reference (G/R 1.37 → target 0.94); bench experiments over datasheet faith (AE limit-cycle, `MAX_AGAIN` decode, MODE_SLEEP zero-read — roadmap §8.5/8.5.1); dead ends documented (`RP3_white_balance_reencode_issue.md`). Position: **#141's op27/28 fixed gains = interim fix; #140 = the endpoint** (highlight-metered AE op29/30, auto-WB op31 with op27/28 fallback, full RPi-order pipeline; bench G/R 0.97–1.01). Topic 2's role shifts to investigation/field-tuning transport. Field addendum (6 Aug): deployed integration-preview photos show the remaining gap — `ae.c` state is RAM-only + 3 steps/wake from the 0x0940 default, so bright scenes never converge on the wake path; camreg-staged exposure validated on the bench as the zero-code mitigation; the durable fix is persisting converged exposure across DPD (~15 lines in `ae.c`, a #140 review note).

## 7. Triage of your changes

| Change | Proposal |
|---|---|
| Comment rewrites, defines-to-top, `ae_monitor.py` fix | **Done** — committed in `55ccb85a`. |
| Cold-boot gate on `labelBootSlot()` (on this branch: the `woken == APP_WAKE_REASON_COLD` guard at the `image_task.c` call site, commit 55ccb85a) | **Revert** — §0.1. |
| MD flash at brightness 0 ("0 = dim not off") | **Verify then keep** — mapping supports you (`ledFlashBrightness()` → 4-bit PSU code, 0 is a valid level; "off" is the enable bit); decide together with #142's op22 5→50 default (your `Operational_Parameters.md` update already documents the "0 means 'dim', not 'off'" semantics). |
| Flash EXIF 0/1/2 | **Adjust** — 0x9209 must stay EXIF-spec; LED type into MakerNote. Note the related Model-tag bug: RP3 photos say `WW500 HM0360` (`image_task.c` tests `USE_HM0360‖USE_HM0360_MD` first — the both-defined trap your camera_switch.c comment warns about). |
| Restore logits / drop `USE_PERCENTAGE` | **Coordinate first** — website ingestion consumes label+confidence; decide the contract, then also drop `NN_confidence_EXIF_not_written.md` if logits win. |
| `#ifdef WDTIMOUTFIX` | **Park** — your commit keeps the retry enabled behind the flag, which is right; resolve when reviewing #140's dwell version (§1). |
| `xSPIMutex` creation location | **Either** — pre-scheduler is the property that matters (`ww500_md.c:801`). |
| Committed build outputs (`objs.in`, five `prebuilt_libs/gnu/*.a` — TFLM lib 23→25.8 MB) | **Please restore** — these are canonical pinned-toolchain (14.3.rel1) binaries the local build overwrites; merging your versions would change what every build links with no source diff. One command pair puts them back without touching any of your changes (commands below). |
| Renames | `variantName→getCameraName` fine; `autoSwitchCheck→considerSwitching` better — take it; `thisVariant→getCurrentCamera` and `labelBootSlot→setCurrentCamera` mislead (compile-time build variant; records identity, sets nothing) — suggest `getBuildVariant` / `recordBootSlotVariant`. Batch renames in one commit (#142 touches `camera_switch.c`). |

### Restoring the committed build outputs

`objs.in` and the five `prebuilt_libs/gnu/*.a` files are outputs the local build rewrites
(ours does it too — the PR notes flag it). The `.a` binaries checked into the repo are
canonical: compiled once with the pinned Arm GNU 14.3.rel1 toolchain, and every firmware
build links against them. Your commit replaced them with your machine's builds (the TFLM
lib grew 23 -> 25.8 MB — a different-toolchain signature), so a merge would silently
change what everyone links, with no visible source diff. Restoring touches none of your
code or docs — your originals stay in the branch history:

```bash
git pull        # first: we have added the review-discussion docs to this branch
git checkout a70f016a -- EPII_CM55M_APP_S/objs.in EPII_CM55M_APP_S/prebuilt_libs/gnu/
git commit -m "chore: restore prebuilt libs and objs.in to pinned-toolchain versions"
git push
```

For next time: a quick `git status` before `git add -A` — anything under
`prebuilt_libs/`, `obj_*`, or named `objs.in`/`NUL` stays out of commits.

## 8. What happened

*Updated 18 August 2026, replacing the "next steps" list this section originally held.*

**Merged.** All three PRs are in `dev`: [#141] as `1936ce37`, [#142] as `59ced6fd`,
[#140] as `8cbb6a2b`, with the review changes applied.

**Decided.** op26 (automatic camera switching) now defaults to 0, off, so the simplest
configuration is the default; the reasoning is in [#165]. op22 (MD flash brightness) stays
at 50%, since 5 was too dim for night motion detection. On-device image correction is
retained.

**Not decided.** The EXIF NN output contract, logits versus percentages, is still open. It
moved to [#189], which consolidates Charles's August input with two findings that came
later: Camtrap DP does not actually require percentages in EXIF, and the output tensor's
quantization scale never leaves `cvapp.cpp`, so the server cannot currently derive
probabilities from the logits it is sent.

**Filed.** The remaining findings became 41 GitHub issues labelled `review-finding`, grouped
in the [thread README](README.md). The two named here have numbers: the MakerNote telemetry
gap is [#159], and updating the deployed integration-preview cameras is [#164].

**Still open from §7's triage.** The cold-boot gate on `labelBootSlot()` is [#151]; the EXIF
Model tag on RP3 photos is [#153].

**What the merge itself exposed**, none of it caught by this review, all in build and CI
rather than firmware behaviour: [#188] (the `D:/hxbuild` workaround broke every Linux build,
and the image-generation binaries were not executable), [#190] (`device_image` deletes the
other camera variant's image), [#156] (the PR gate built one variant on the wrong
toolchain, which is why the first two reached `dev`) and [#155] (the `firmware` command
truncated long filenames). [#156], [#155] and [#179] are fixed in [#191].

Several findings here were indeed answered by [#140] as expected, including auto-AWB over
fixed gains and the progressive-dwell capture retry. AE persistence across DPD was not:
that remains [#158].

[#140]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/140
[#141]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/141
[#142]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/142
[#151]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/151
[#153]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/153
[#155]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/155
[#156]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/156
[#158]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/158
[#159]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/159
[#164]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/164
[#165]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/165
[#179]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/179
[#188]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/188
[#189]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/189
[#190]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/190
[#191]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/191
