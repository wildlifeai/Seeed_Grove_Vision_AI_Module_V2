# WW500 review — pre-read for our call

*1 Aug 2026, checked against `feat/camera-features-combined` (PR #141). Every claim has a file:line so you can verify it yourself.*

---

## 1. Before you program a board: the cold-boot condition on `cameraSwitch_labelBootSlot()`

Your TODO asked us to check this. It has a hole in the normal update flow: the `firmware` command flips the slot selector **at write time** (`xip_manager.c:1621–1625`) — `reset` only makes the reboot sooner. If the app connection drops before `AI reset`, the device sleeps, and the next **DPD wake is the new image's first boot — a warm boot** (`ww500_md.c:701`). Gated on cold boot, that image never labels its slot: `slots` reads `unknown` until a battery pull, and auto-switch (which refuses unlabelled slots, `camera_switch.c:100–108`) is silently dead in that direction.

Second question mark: `reset`/`switchslot`/auto-switch all reboot via a CPU watchdog (`image_task.c:2584–2608`). Whether the PMU wakeup-event registers read NONE (cold) after that, or retain the previous DPD wake cause (warm), is AON-domain behaviour we couldn't settle from source. **Since you'll have a serial log open: after a `reset`, does it print `### Cold Boot ###` or `### Warm Boot ###`?** One observation settles it.

**Suggestion:** revert — "first boot of this image" can't be inferred from cold/warm. Your efficiency instinct still gets its win safely: the code itself flags a "Simplification opportunity" (`xip_manager.c:917–919`) — the selector sector is XIP-mapped at `0x3AFFF000`, so the 3 boot reads become plain memory reads, no XIP toggles, no mutex. (The erase+write already happens only once per newly flashed image, `xip_manager.c:1698–1700`; today's cost is 3 short reads beside a 1 s SD-init delay.)

## 2. `cameraSwitch_autoSwitchCheck()` — the walkthrough you asked for

Seven cheap early-outs, then the switch (`camera_switch.c:73–121`):

1. **Latch** — `static bool switchScheduled`: once scheduled, always false until the reboot (no double-switch).
2. **op26 ≠ 1** → false.
3. **Build is RP2/IMX477** (variant UNKNOWN) → false.
4. **Light**: `dark = (op25 == 1)`; wanted = dark ? HM0360 : RP3; wanted == running → false. **It never reads the sensor** — op25 is the persisted, hysteresis-filtered decision from `ledFlashNewAEStats()` (`ledFlash.c:420–475`; 16-sample burst, gain-railed → dark, dead band 12 around op23). op25 surviving DPD/reboot in CONFIG.TXT is what prevents a switch-back loop.
5. **Selector unreadable** → false.
6. **Other slot not labelled with the wanted variant** → log `…- staying`, false. (Never switch into an unknown image.)
7. **Switch**: `xip_switch_slot()` (refuses an empty slot), latch, `app_setResetRequest(true)`, BLE announcement, reboot at next sleep (`>>> Reset by watchdog`) — deferred so the announcement reaches the app first.

Called at `image_task.c:868` after each AE decision; the sampling block only runs when a consumer is enabled (`:853–854`). Into DPD, op26 arms an RTC wake every op24 min (`:2626–2639`) so dawn/dusk is noticed without motion. Asymmetry: manual `switchslot` needs no label, only a programmed image (`CLI-commands.c:874–876`) — that's how labels bootstrap.

One correction from your notes: NN-model metadata is **not** in the slot selector. `xip_copy_metadata_to_flash()` writes the `"LABL"` record at the **model area, 0x00200000**; the selector sector holds only the bootloader header + our 8-byte WWSM record.

## 3. Your open TODOs

**`xip_copy_metadata_to_flash()` / pre-existing metadata** — your instinct found something real. `write_metadata_to_flash()` (`xip_manager.c:676–704`) is a bare word-write, **no erase** — over an existing record it would AND bits and corrupt it. Safe today only because the single call site (`cvapp.cpp:407`) runs right after `xip_copy_model_from_sd_to_flash()`, which erases the region first (`xip_manager.c:767–777`). The precondition isn't stated in the header comment (`xip_manager.h:147–152`), and `ModelMetaData.crc` is unimplemented ("written as 0"). Both worth logging. (The WWSM labels, by contrast, are explicitly preserved across selector rewrites — `xip_manager.c:1483–1486`.)

**`APP_MSG_FATFSTASK_SAVE_CONFIG` comment** — accurate, one loose phrase. It does save CONFIG.TXT: `STATE_FILE` is `#define`d as `"CONFIG.TXT"` (`directory_manager.h:28` — historical name, rename candidate). It doesn't unmount; SAVE_STATE does (`fatfs_task.c:693–694` vs `:719–727`). Loose bit: inactivity also triggers SAVE_STATE (`image_task.c:685–691`), not just captures — tighten the parenthetical, trust the mechanism.

**`APP_MSG_CLITASK_DISK_WRITE_COMPLETE`** — fatfs_task's async completion reply to the CLI task (`fatfs_task.c:482/605/648`; `:648` is the camreg path), echoing the `fileOperation_t*` in `msg_parameter` so multiple outstanding ops can be told apart. Consumed at `CLI-commands.c:2562–2581` (clears `camRegWritePending`). Predates this PR; the pointer echo is what's new.

## 4. Your topic-2 comments (camreg / vcm)

**1 — "big, inefficient… `#ifdef` for development only."** Measured: ~495 lines, **~143 bytes static RAM** (`stagedSettings[24]` = 96 B; records are packed 4-byte `{action, addr, val}`). No duplicate table — `camRegSaveTable` (`CLI-commands.c:1311`) is a *function*. Extra boot read ≈ 1–5 ms. The counter-argument to `#ifdef`: roadmap Phase 2 builds the **app tuning card on these commands** — field adjustment without reflashing is a product goal (and #143's sweep harness rides on them). Our starting position: keep in production; a `FIELD_TUNING_CLI` gate is cheap if still wanted, but `cis_file_process()` predates this PR and must stay (it's how HM0360 strobe/MD tweaks are applied). camreg is compiled into both variants (only `vcm` is `USE_RP3`) — arguably right, `HM0360EX.BIN` staging is useful on the night camera too.

**2 — `camRegFileName` not required.** Agreed it carries no information. It exists because `fileOperation_t.fileName` is a non-const `char *` (`fatfs_task.h:144`). Fix: const-ify the field, then delete the variable.

**5 — file read twice.** Confirmed, strictly sequential (fatfs_task loads staged table `fatfs_task.c:1594` → semaphore → image_task re-reads in sensor init `image_task.c:1845`). Two different consumers (CLI staging vs `hx_drv_cis_setRegTable`), but your point stands — the bytes are already in RAM; `cis_file_process()` could apply the staged table directly (guard: a hand-authored file >24 entries is applied fully but staged partially, `cis_file.c:79–83`). Two easy nearby cleanups: dead `f_getcwd` debug print every boot (`cis_file.c:186–192`) and a per-boot heap alloc for ≤96 B (`:220`).

**6 — reads and write should live in one file.** Agreed: move the save into `cis_file.c` as `cis_file_saveStagedToFile()`; camreg just calls it.

**7 — `camRegWritePending` "can only have been added by AI… no chance of typing fast enough."** We'd push back — it's load-bearing:
- The save is **async** (queued to fatfs_task, `CLI-commands.c:1327–1332` → `fatfs_task.c:624`) and the buffer is a **live pointer into `stagedSettings[]`** (`:1321`) — no snapshot.
- The CLI task **out-prioritises fatfs_task** (`ww500_md.c:808–853`), so a queued second command runs **before the write starts** — the window is "as long as the CLI task is runnable", not milliseconds of SD latency.
- Input isn't just typing: BLE feeds `AI camreg …` into the same parser (`if_task.c:420`), the console is 921600 baud (a pasted block ≈ 90 KB/s), and the runbook itself prescribes back-to-back sequences.
- Without it: second command mutates the table mid-write; the file opens `FA_CREATE_ALWAYS` (truncate-then-write, `fatfs_task.c:257`) → torn `RPV3_EX.BIN`, found at next boot.

Removing it honestly requires a 96-byte snapshot per save — more RAM than the flag. If you can construct a scenario where it's genuinely dead, that's a real finding — bring it.

**8 — provenance.** Your conclusion is right and it's the best finding in this topic — one premise to fix: `scan_cis_settings.md` **and** `scan_cis_settings.py` both exist, in **`_Tools/`** (pointed at by `cis_file.c:6–7`). The doc is stale (HM0360-era, no camreg). The real gaps, as you say: the pipeline is **one-way** (text → `.BIN`, no decoder anywhere), so the first field `camreg` strands tuning state in comment-less binary; re-saves also force every record to a write action (`cis_file.c:150` — delay records lost) and drop entries past 24 (`:246–247`). Cheap fixes to discuss: `--decode` mode in the script (~20 lines), doc refresh, optional `camreg export`. Your "text file is the source of truth" workflow is the call decision.

**Register table — "does it exist?"** Three answers: (1) baseline = the driver includes `cis_sensor/cis_imx708/IMX708_common_setting.i` (~46 entries) and `IMX708_mipi_2lane_2304x1296.i` (~89: exposure `0x0202/03`, analog gain `0x0204/05`, per-channel digital gains `0x020E–0x0215`), plus the exposure override in `cisdp_sensor.c:89–92`. (2) Tuning targets are tabulated in `camera-phase0-bench-runbook.md:33–41,70` (staged registers apply *after* the init tables). (3) A record of deltas **found during tuning doesn't exist yet** — the runbook's results table is still blank; the phase-0 campaign was never run. What happened instead is §6.

**`vcm` (your blank line).** Drives the RP3's DW9817-type focus actuator at I2C `0x0C`, inline in `CLI-commands.c:1471–1553` (`#ifdef USE_RP3`, raw `hx_drv_i2cm_*`, no driver file); `vcm probe` checks presence; `vcm <0–1023>` sets position. **Not persistent** across a camera power cycle — persistence is roadmap Phase 3.1.

## 5. Battery / DPD impact

**DPD floor current: unchanged** — nothing alters `sleep_mode_enter_dpd()`'s PMU config or leaves hardware powered; the RTC already ran. Label check = 3 short reads/boot; staged-file read ≈ 1–5 ms; both noise beside the 1 s SD-init delay.

**What changes is the wake economy**, all op-gated:
- **New periodic wake**: timelapse = 0 + (op13 AE-flash or op26 = 1) → RTC wake every op24 min, default 15 (`image_task.c:2619–2643`). Each check is lightened (1 frame, no NN run `:809`, no file save `:950`) but still boots fully (SD init incl. 1 s delay, sensor, model load, ~1.9 s AE burst). ≈ 5–10 s awake × 96/day ≈ **8–16 min extra awake/day** on an idle device — the dominant idle-battery term when enabled.
- **AE burst**: ~1.9 s per check (16 × 120 ms, self-documented `ledFlash.h:54–57`); +~0.5 s HM0360 MODE_SLEEP wake on RP3-with-MD-off. Worth one log check: does the burst run once per wake or once per image of a multi-shot sequence? (Count `HM0360 AE regs:` prints per event.)
- **Transfer sessions** hold DPD off while open + inactivity tail — bounded, user-initiated.

**The decision:** #141's defaults are battery-neutral (op13 = 0, op26 = 0). **#142 flips op26 → 1**, turning the 15-min heartbeat on for every deployment. Options for the call: leave op26 = 0 and let the app enable it on dual-camera installs; raise op24's default (dawn/dusk needs ~4 checks/day, not 96); or arm the periodic wake only when the other slot holds the opposite variant (today a single-image device wakes forever to log "staying"). Your serial timestamps ("Woke at" → ">>> Entering DPD") + one current-probe trace give hard numbers cheaply.

## 6. Where the RP3 work ended up (the "position" you asked about)

**Method**: instrumentation first — live UART preview (#140) + `_Tools/live_view.py` made a ~1 fps tuning loop; `tune_stats.py` scored frames against phone reference photos, turning the green tinge into a number (bright-quartile G/R 1.37 vs target 0.94); bench experiments over datasheet faith (the AE limit-cycle, `MAX_AGAIN` decode bug, MODE_SLEEP zero-read — `AE_Light_Sensor_Roadmap.md` §8.5/§8.5.1); dead ends documented (`RP3_white_balance_reencode_issue.md` — why the software re-encode exists).

**Position:**
- **#141's op27/28 fixed WB gains = interim fix** — removes the gross cast, illuminant-dependent.
- **#140 = the endpoint**: highlight-metered auto-exposure (op29/30) + auto WB (op31, grey-world, falls back to op27/28 for flash/dark frames) + full pipeline in RPi ISP order (black level → WB → CCM → gamma, per libcamera `imx708.json`). Bench: G/R 0.97–1.01 vs reference 0.94.
- **Topic 2's role shifts**: camreg/vcm/`.BIN` staging stop being how images get *fixed*, remain how the pipeline gets *investigated and field-tuned* (bench loop, #143 harness, app tuning card). That's the context for your `#ifdef` question.
- **Still open**: field validation of thresholds/defaults; warm-reboot sensor bring-up flakiness (#140 follow-up); CCM/lens-shading phases; your provenance gap.

## 7. Renames

| Proposal | Response |
|---|---|
| `thisVariant()` → `getCurrentCamera()` | It's the **compile-time build variant**, not runtime state — `getBuildVariant()` says that. |
| `variantName()` → `getCameraName()` | Fine. |
| `labelBootSlot()` → `setCurrentCamera()` | Misleading — it records the build's identity into the slot metadata, sets nothing. `recordBootSlotVariant()` (or keep "label"). |
| `autoSwitchCheck()` → `considerSwitching()` | Better — take it. |

Batch renames in one commit: `camera_switch.c` is also touched by #142 (label-retry — which incidentally closes the ignored-return-value gap at `camera_switch.c:143`), and one commit merges up the stack cleanly.

## 8. Suggested agenda

1. §1 — agree the revert + XIP-read replacement; your Cold/Warm observation after `reset`.
2. §4 — your findings: which become commits now (const fix, dead debug, single read, save-path move) vs follow-ups (decode tooling, doc refresh); the `#ifdef`/production question.
3. §5 — the op26/op24 defaults decision, with your bench numbers.
4. §6 — the position, and what it means for reviewing #140.
5. §7 — renames + comment ground rules.
6. Logistics for your board session (it doubles as the op26 hardware validation — please keep the serial logs).
