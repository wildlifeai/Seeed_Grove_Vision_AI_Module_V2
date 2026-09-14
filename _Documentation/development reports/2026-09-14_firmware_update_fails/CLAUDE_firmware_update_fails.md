# Task: Fix formware update failure

#### File: CLAUDE_firmware_update_fails.md
#### Author: Charles Palmer
#### Date: 14 September 2026

## Background

The system supports updates of the AI processor firmware via the app. The app collects 
the latest firmware images (one for each camera) and attempts to install them on the 
AI processor using the 'AI firmware<filename>' command.

The process was introduced in PR #142 and is described in part in ww500_md/doc/WW500_ble_file_transfer.md

This fails, at least in some situations.

## Evidence of Failure

Two log files are placed in this folder:

* `ble_log.txt` - the console output of the BLE processor
* `ai_log.txt` - the console output of the AI processor

The SD card contains a partial file: R6905J22.IMG, 39k size.

The app shows some (expected) messages, then "Error: No transfer response for 15 seconds - device may be stuck"

## Possible Cause

Since the tranfers presumably works with Victor's setup but fails with mine, the differences could be:

1.	Different speeds of BLE transfer. I have an older phone which might have negotiated smaller BLE packet sizes.
2. 	Different SD card write speeds. Different card are known to take different times to write data.

Transfer of packets are:
```
app -> BLE processor -> AI processor -> SD  card
```
And some acknowlegements flow back in the opposite direction. 
However I suspect that at least some of these transfer steps occur without 
acknowledegments and rely on assumtions that acks are unnecessary. Such assumptions might be invalid.

## Analysis Required

1.	Claude should attempt to understand the cause of the failure, and document this.
2.	Claude should propose a fix, but not until the analysis is complete.
3.	Claude should ask any questions to help find the fault.
 
 ## Tools
 
 The code for this large file transfer was created by Claude at Victor's site. It may be that the other Claude
 was able to monitor the console output of both devices by itself. 
 Tools may exist for this - e.g. in th e`_Tools` folder.
 
 This Claude may use such tools (if they exist) - but ask first. I might need to shut down
 teh terminal emulators to facilitate this.



## Analysis (Claude, 14 September 2026)

Read both logs against the current `fileRx`/`if_task.c`/`inactivity.c` source (branch
`firmwaretx`) and `ww500_md/doc/WW500_ble_file_transfer.md` (the PR #142 write-up). No code
changed yet — analysis only, per the brief.

### Timeline reconstruction

**`ble_log.txt`** — one attempt to send `R6905J22.IMG` (487424 bytes). `FILE_START` at
154 ms setup, then a burst/steady stream of `FILE_DATA` packets (241 B payload each,
`ftx ack N` confirming each). Four `Response timeout` -> `AI processor not responding` ->
`recovering to IDLE` episodes occur along the way but each time the app resumes and
acks keep climbing — these are recoverable stalls, not the failure.

The fatal event is at the end: after `ftx ack 161`, the connection interval degrades
(`Conn params applied: interval 32 units (40ms)`, a re-request back to fast, then
`18 units (22ms)`), two more `Response timeout` recoveries happen, and then — with no
further `ftx ack` — **the AI processor spontaneously sends its stats snapshot and a
`Sleep` message**, and the app's `fileTx` session times out and resets.

161 acked packets x 241 B ~ **38.8 KB** — matches the **39 KB partial file** on the SD
card almost exactly. So `ble_log.txt` is the run that produced the file in evidence.

**`ai_log.txt`** — a *separate, much shorter* episode: `FILE_START` for the same
filename arrives fresh, the file opens fine, and then **nothing else ever arrives**.
The AI processor tries to send its `ftx ack 0` back and the "Missing Master" watchdog
fires five times ("I2C master did not read our I2C message") before the session's own
15 s inactivity hold expires and the transfer is abandoned. No `FILE_DATA` packet is
ever received in this capture.

**These read as two different attempts**, not one continuous session — `ai_log.txt`'s
`FILE_START`/`Opened … for writing` pair only happens once, at genuine session start, so
it can't be a truncated middle slice of the same run as `ble_log.txt`. But nothing in
either file proves that (see open questions below).

### What the code says about the failure mode

- `AI_PROCESSOR_MSG_FILE_START` handling in `if_task.c` extends the DPD-inactivity
  period to `FILERX_SESSION_INACTIVITY_MS` (15000 ms) for the duration of the session
  (`if_task.c:536-544`), restored on every close path via `restoreInactivityPeriod()`.
- The Himax's inactivity detector (`inactivity.c`, `USEIDLETASK` build) is **pure CPU
  idle time**: `idle_start_tick` resets to 0 on *any* non-idle FreeRTOS task switch
  (`inactivity_on_task_switched_in()`), and `app_onInactivityDetection()` — the function
  that prints `Inactive for %dms` and drives Save-State -> `Sleep` — only fires after a
  **continuous, unbroken** 15 s span with no task switch at all. This is the same
  mechanism flagged in the skill file for issue #208 ("measures idle time only").
- Consequence: for `app_onInactivityDetection()` to fire mid-transfer the way
  `ble_log.txt` shows, the Himax must have seen **zero I2C activity for a full,
  unbroken 15 seconds** — not "acks were slow", but genuinely nothing arrived at the
  I2C slave interface. That's a much bigger gap than anything in the app-side doc's
  known renegotiation stalls (~950 ms Android, ~5 s iOS) and points at the **nRF side
  going quiet**, not the Himax dropping anything.
- Symmetrically, in `ai_log.txt`, each "Missing Master" retry (`MISSINGMASTERTIME` =
  4000 ms, `if_task.c:74`) requires its own task switch to fire and print — which
  itself resets the idle clock. So the printed "Inactive for 15000ms" is only the
  *final* idle stretch; the five MM-timer cycles before it add roughly another 15-20 s
  on top. This session likely ran 30-35 s before giving up, not 15 s.
- I could not find any code path that retries a dropped `ftx ack 0` — `i2ccomm_write_enable()`
  is a single attempt with one associated Missing-Master timer, no auto-resend. So for
  the timer to fire **five times** in `ai_log.txt`, something must be re-invoking
  `sendI2CMessage()` five separate times; I have not located that caller from the Himax
  side alone (no further `Received I2C message` lines appear in the capture, and no
  second `FILE_START`/`Opened` pair). This needs either a timestamped capture or the
  nRF's own console to resolve — see open questions.

### Working hypothesis

The Himax side (`fileRx.c`/`if_task.c`) is behaving exactly as designed once a packet
reaches it — sequence check, CRC, ack, done. The failure is upstream: **the nRF stops
driving I2C to the Himax for a long enough stretch (~15 s+) that the Himax's own
inactivity/DPD logic decides the session is abandoned and tears it down while the
phone/nRF still believe a transfer is in progress.** Candidates for *why* the nRF goes
quiet that long, from the skill file's own recorded ww-hardware issues (not yet checked
against this bench's nRF build):

- **#34** "its console hex dump holds the download to about 1 KB/s" — if the nRF's
  verbose per-packet console logging was left on for this bench run, a burst that
  should complete in ~200 ms could instead spend many seconds serialising log lines,
  which would look exactly like a Himax-side idle gap.
- **#33** "forwards any command mid-`txfile` and restarts its packet counter" — if
  anything (a console keepalive, a status poll) reached the nRF mid-transfer.
- The connection-interval churn visible right before the failure in `ble_log.txt`
  (12 -> 18 -> 32 -> 18 units) suggests the link was already under stress at exactly the
  point things went quiet — consistent with, but not proof of, an Android BLE
  renegotiation interacting badly with the nRF's buffering (the doc's §4 "what did NOT
  work" section describes a related but supposedly-fixed failure mode at ~25 s).

I don't yet have enough evidence to pick between these, or to rule out a difference in
nRF firmware build/branch, without visibility into the nRF's own console for this run.

### Discrepancy to confirm

The task doc names the partial file `R6905I22.IMG`; the actual I2C payload in
`ai_log.txt`'s hex dump decodes to `R6905J22.IMG` (byte `4a` = `'J'`). Almost certainly
a transcription slip, but worth a quick confirm in case it actually points at a third,
undocumented attempt.

### Open questions before I go further

1. **Are `ble_log.txt` and `ai_log.txt` from the same bench run, or two separate
   attempts** (e.g. the app auto-retried, or you manually re-ran `firmware <file>`
   after the first "No transfer response" error)? This changes how I read `ai_log.txt`
   entirely.
2. I found `_Documentation/development reports/2026-08-24_light_sensor_review/mobile_app_three_way_bench/bench_log.py`,
   which captures app (adb logcat) + nRF console + Himax console together in one
   timestamped file (per the skill file's Section 5 three-way logging note). **May I use
   or adapt it to capture a repro run?** I understand this may mean closing your current
   terminal emulators first — let me know when that's convenient, or if you'd rather
   capture it yourself and hand me the log.
3. What phone model/OS and app build produced this failure (to check against the
   documented Android-vs-iOS renegotiation difference)?
4. What nRF firmware build was on the bench for this run (`ver` on its console) —
   the skill file notes the bench nRF normally runs ww-hardware `dev` (0.30.48,
   `75406df`), not `main`; worth confirming it's still that build and not regressed.
5. Was the nRF's verbose per-packet console logging enabled during this run?
6. Can you confirm the partial file's actual name on the card (`R6905I22.IMG` vs
   `R6905J22.IMG`)?

No fix proposed yet, per the brief — want to close on the above first, ideally with a
timestamped/three-way capture, before pointing at a specific mechanism.

## Correction (Claude, 14 September 2026)

Charles confirmed `ble_log.txt` and `ai_log.txt` are **the same run**. That invalidates
the "two separate attempts" reading above — re-examined with that constraint and it
resolves cleanly, including the "why does the Missing-Master timer fire five times"
question left open above.

**`ai_log.txt` is not a second session — it's the same session's failure spikes only.**
During an active transfer, per-packet success is almost entirely silent on the Himax
console:

- `fileRx_data()`'s per-packet log is gated on `!g_fileRxActive` (`fileRx.c:184`).
- The successful-write ACK path (`i2ccomm_write_enable`'s own trace, and
  `i2cTransmissionComplete()`'s `dbg_evt_iics_cmd`) is likewise gated on
  `!g_fileRxActive` (`if_task.c:78-80`, `:417`).
- The **failure** path — `xprintf("I2C master did not read our I2C message\n")` in both
  `handleEventForStateI2CTx` and `handleEventForStateI2CSlaveTx` — has **no such gate**.

So while a transfer is in progress, a Himax console capture shows only: the one-time
`FILE_START`/`Opened … for writing` pair, then **exclusively the failures**, then the
final shutdown sequence. Nothing else prints. That's exactly the shape of `ai_log.txt`.

**The two logs line up 1:1.** `ble_log.txt` contains exactly **five** `Response timeout`
-> `AI processor not responding` episodes (after acks ~16, ~78, ~151, and two in a
tight cluster right after ack 161). `ai_log.txt` contains exactly **five** "Missing
Master" (`I2C master did not read our I2C message`) failures. These are the same five
events seen from each side of the link — not five retries of one dropped ack, but five
distinct dropped acks over the course of the transfer.

**Why the first four recovered but the fifth didn't:** the first three "Response
timeout" episodes are isolated and the app resumes cleanly each time (acks keep
climbing: 16→21, 78→87, 151→157). The fourth and fifth happen **back-to-back**, right
where the connection interval is visibly unstable — `ble_log.txt` shows
`interval 32 units (40ms)` (regression from the fast 15-30 ms path), a
`re-requesting fast set` retry, then it settles at `18 units (22ms)` — and it's in that
same window that the Himax stops getting *anything* through for good. That's when both
independent 15 s budgets expire: the Himax's true-CPU-idle inactivity timer (which, per
the analysis above, requires an unbroken 15 s idle span, plausible here since real
recovery genuinely stopped) triggers Save-State/Sleep, and the app's own 15 s
"no transfer response" watchdog fires at the same time from its side.

**Revised working hypothesis:** the `MISSINGMASTERTIME` = 4000 ms window (`if_task.c:74`,
already once widened from 1000 ms for exactly this class of problem — see
`WW500_ble_file_transfer.md` §4) is being exceeded by *this* phone's BLE
connection-interval renegotiation stalls more often than the single ~950 ms Android
stall the fix was validated against, and **a dropped ack is never retried** — the
Missing-Master path is a hard give-up, recovery is entirely the app's responsibility.
Four isolated stalls were within the app's own retry budget; a fifth landing right on
top of a fourth (both inside the same connection-interval hiccup) exhausted it. This
fits the task doc's own hypothesis #1 (older phone, different BLE negotiation) better
than hypothesis #2 (SD card write speed) — nothing in either log points at a slow SD
write; `fatfs_task.c`'s write times aren't even visible in `ai_log.txt` because they'd
only print outside `g_fileRxActive`, and the successful stretches show completely
normal ack cadence (11-20 ms) right up to each stall.

This does **not** yet rule out an nRF-side contributor (e.g. its console logging, or
`ver` showing a different build than the usual bench `dev`) making the *link* itself
more fragile on this run than usual — I still don't have visibility into the nRF's own
console for this session. Updated open questions, replacing the withdrawn "same run?"
one above:

1. What phone model/OS and app build was used for this failed run? (Central to
   confirming/refuting "this phone's BLE stack renegotiates more/longer than the one
   the July fix was validated against.")
2. What nRF firmware build was on the bench (`ver` on its console)?
3. Was the nRF's verbose per-packet console logging enabled during this run? (Rules out
   vs. confirms a link-fragility contributor separate from the phone itself.)
4. `R6905I22.IMG` (task doc) vs `R6905J22.IMG` (decoded from the log) — still worth a
   quick confirm.

Given the correlation above, I think the core mechanism (undersized fixed retry budget,
no ack-retry, two stalls landing back-to-back exhausting it) is reasonably well
supported by these two logs alone, even without a three-way capture. Happy to move to
proposing a fix once you've weighed in on the above, or sooner if you'd rather I just
go ahead — your call.

## New finding: second bench log shows a different bug entirely (Claude, 14 September 2026)

Charles supplied a third capture (BLE processor log with timestamps, and the Himax
console for the same run) from a repeat attempt. Phone: Galaxy A05. nRF signon:
`Wildlife Watcher 1 (WW500-C02) Ver: 00.30.48 Built: 21:58:30 Jul 21 2026` (matches the
skill file's expected bench build). Filename discrepancy was Charles's transcription
typo, now corrected to `R6905J22.IMG` throughout.

**This run's transfer completed cleanly** — 238 packets, CRC `0xb994` verified,
`ftx ack end` sent. The actual failure is the very next step: the app's follow-up
`firmware R6905J22.IMG 0xB994` command (the one that installs the update) comes back
**`Unrecognised`**, and the device goes idle 1 s later and sleeps. This is a different
failure mode from the BLE-stability one above — it turns out the SD card was absent for
this run (Charles's bench oversight), which explains it:

- `cli_fatfs_init()` (`fatfs_task.c:1608`) — which registers the `firmware`, `crc`,
  `dir`, `txfile`, `unmount`, `dump-sel` CLI commands — only runs if the boot-time
  `fatFsInit()` (`f_mount(&fs, DRV, 1)`) returns `FR_OK`. Charles's boot console for
  this run confirms the mount failed: `Mounting FatFS on SD card ... Put the card
  SPI/Idle state fail ... Failed error = 3`, i.e. `FR_NOT_READY`. So `firmware` was
  **never registered** for this entire power-up — `Unrecognised` is exactly what
  `FreeRTOS_CLIProcessCommand()` returns for a command not in its list
  (`FreeRTOS_CLI.c:225`). Confirmed, not a guess.
- The AI processor **does** correctly know about this: `fatfs_task.c:1639` sets
  `selfTest_setErrorBits(1 << SELF_TEST_AI_NO_SD_CARD)` (bit 11, `selfTest.h`) right in
  the failure branch, so `AI selftest` would report it. This matches Charles's own
  guess.
- The BLE file-**receive** path (`FILE_START`/`FILE_DATA`/`FILE_END`, handled directly
  in `if_task.c` + `fatfs_task.c`'s `OPEN_FILE`/`APPEND_FILE`/`CLOSE_FILE` messages)
  is separate plumbing that never goes through the CLI table, which is why the transfer
  itself could proceed independently of whether `firmware` was registered.

**Open gap, confirmed by inspection, not yet fully explained:** `APP_MSG_FATFSTASK_OPEN_FILE`
(`fatfs_task.c:754`) does **not** check `fatfs_mounted()` before calling `f_chdir()` /
`f_open()` — unlike `load_configuration()` (`fatfs_task.c:1063`) and the
`SAVE_STATE`/`SAVE_CONFIG` handlers (`:713`, `:740`), which all check it first. This
means a card that was never mounted should, at minimum, fail `f_open()` and produce
`ftx err N` rather than a clean `ftx ack`/CRC-verified `OK` — the FatFS mount logic
(`ff.c` `mount_volume()`) is written to force a fresh `disk_initialize()` retry whenever
`fs->fs_type == 0` (true here, since the boot mount failed and left it invalidated), so
a second, real hardware probe attempt should have been made at `f_open()` time and
should have failed again the same way (no card = no CMD0 response). The Himax console
excerpt for the transfer itself shows **no repeat of the SD-driver diagnostic lines**
(`Card Ready` / `Put the card SPI/Idle state fail`) before `Opened '...' for writing` —
which is what I'd expect if that second real probe never actually happened. I can't yet
explain from static reading alone why the file "transfer" reported clean success with
no card ever mounted; two candidate explanations:

1. The boot-failure console excerpt and the transfer excerpt are from two different
   power-ups (e.g. a reset in between), and the card was genuinely present — even if
   only loosely/intermittently seated — by the time the transfer ran.
2. There is a genuine bug further down (in `mmc_disk_write()`'s response-token
   checking, not yet inspected in depth) that lets writes silently "succeed" with no
   card attached, independent of the mount-caching question above.

Also worth noting while in this code: `mmc_we2_spi.c:132` — **`#define MMC_CD() 1`**.
There is no real card-detect signal in this driver at all; "card present" is hardcoded
true, and absence is inferred purely from the SD command sequence timing out. That's a
separate robustness gap from the one above (it means a card physically removed while
already mounted can never be detected without an actual failed read/write), worth
flagging even though it isn't necessarily what happened in this specific run.

### Recommendation (not yet applied — flagging for agreement first)

Regardless of how #1/#2 above resolves, `APP_MSG_FATFSTASK_OPEN_FILE` should check
`fatfs_mounted()` at the top and immediately fail the transfer (`ftx err`, no `f_chdir`/
`f_open` attempted) if the card was never mounted — the same guard `load_configuration()`
already uses. This closes the gap unconditionally and is cheap; I'd like to also resolve
#1 vs #2 (a continuous, single-boot repro would show which) before deciding whether
`APPEND_FILE`/the mmc driver need anything beyond that.

### Question

Were the boot-failure lines (`Mounting FatFS on SD card ... Failed error = 3`) and the
transfer excerpt (`FILE_START` through `Unrecognised`) from the **same, continuous,
unbroken power-up** — i.e. no reset/reboot between the two — or could the device have
reset in between (in which case the card may genuinely have been present by the time of
the transfer)?

## Decision: gate file access on the first mount result (Claude, 14 September 2026)

Charles confirmed the boot-failure lines and the transfer excerpt were from one
continuous, unbroken power-up — no reset in between, and a live card
insertion/removal between boots is out of scope (not a real-world case worth
engineering around). So the FatFS-internal question of exactly how a second, in-session
`f_open()` produced a false success is set aside as not worth chasing further: **there
is no hardware card-detect signal on this board** (`mmc_we2_spi.c:132`,
`#define MMC_CD() 1`), so the firmware cannot tell "card removed mid-session" from
"card fine" by asking the hardware again later anyway. The only trustworthy signal is
the **first** access attempt, at boot. Decision: treat that result as authoritative for
the whole power-up and gate every later file-access attempt on it, rather than letting
each call retry FatFS's own mount logic and risk a different answer.

`fatfs_mounted()` already exists and is already used this way by `load_configuration()`
(`fatfs_task.c:1063`) and `save_configuration()` (`:1193`). The gap is that four other
file-access entry points don't check it and go straight to `f_chdir`/`f_open`:

- `APP_MSG_FATFSTASK_OPEN_FILE` (`:754`) — the one that matters for this bug: it's the
  first step of the BLE `FILE_START`/`FILE_DATA`/`FILE_END` receive used by the app's
  firmware update.
- `fileWrite()` (`:269`) — generic file write (`writefile` CLI command path).
- `fileWriteImage()` (`:329`) — camera image writes and the image half of `WRITE_FILE`.
- `fileRead()` (`:425`) — generic file read (`readfile`/`txfile` CLI command path).

### Fix applied

Added the same guard used by `load_configuration()`/`save_configuration()` —
`if (!fatfs_mounted()) { xprintf("SD card not mounted.\n"); return FR_NO_FILESYSTEM; }`
(or the message-handler equivalent that fills in `fileOp->res` and replies) — at the
top of all four. No change needed to `APPEND_FILE`/`CLOSE_FILE`: both already check
`transferFileOpen`, which now can never become true when the card was never mounted,
so they fall out safe for free.

Confirmed this reaches the app correctly for the firmware-update case: `if_task.c`'s
`handleEventForStateDiskOp()`, `DISK_PHASE_FILE_OPEN` case, already turns a non-`FR_OK`
`fileRxOp.res` into `ftx err N` back over I2C/BLE (`if_task.c:1364-1372`) — that part of
the plumbing was already correct, it just never got a failing `res` to report.

Not changed: the `MMC_CD()` hardcoded-present driver gap. That's a separate, deeper
question (would need real card-detect hardware to fix properly) and doesn't affect this
scenario now that access is gated on the first attempt.

## Aside: stale build timestamp on incremental builds (Claude, 14 September 2026)

Unrelated to the SD-card bug above, but found while Charles was building the fix:
building without `make clean` left the console-printed build date/time stale. Charles
pointed at `ww-hardware`'s nRF Makefile
(`MokoTech/Workspace/WildlifeWatcher_1/ww500_c02/s132/armgcc/Makefile`), which forces a
rebuild of any file containing `__TIME__`/`__DATE__` via a `FILES_TO_FORCE_REBUILD` list
and a phony `FORCE` prerequisite (a phony target with no recipe is always "out of
date", so anything depending on it always rebuilds).

**This project already had the identical mechanism** (`ww500_md.mk:105-113`,
`force_rebuild_main`) for `ww500_md.c` (the only file here using `__TIME__`/`__DATE__`)
— it just had one bug: the object path it targeted
(`OBJECT_DESTINATION = obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65`) is a bare,
unprefixed path. On a **native Windows build**, the real object tree is redirected to
`D:\hxbuild` (`ww500_md.mk`'s own `HOST_OS`/`OUT_DIR_ROOT` block, added earlier for a
Windows `MAX_PATH` issue) — so the force-rebuild rule was quietly targeting a path make
never actually builds, and `ww500_md.o` fell back to normal timestamp-based
up-to-date checking, going stale on any non-clean Windows build. WSL/Linux builds were
unaffected (that redirect is Windows-only), which is presumably why this had gone
unnoticed.

**Fix:** added `FORCE_REBUILD_OBJ_DIR`, which reconstructs the `D:\hxbuild`-aware path
the same way `OUT_DIR` eventually will (`OUT_DIR` itself isn't set yet at this point in
the include chain), and switched to a `FILES_TO_FORCE_REBUILD` list +
`$(foreach)`/`$(eval)` (mirroring the nRF Makefile's style) so another file can be added
later without repeating the pattern. `OBJECT_DESTINATION` itself is untouched — it's
also used by `clean_legacy_obj` immediately below, which specifically wants the bare
pre-redirect path, not wherever the redirect currently points.

Verified with `make -n` (no toolchain needed — this only exercises Make's dependency
logic, not compilation): with a fake `.o` newer than `ww500_md.c`, `make -n` still
recompiles it, both in the plain WSL case and with `OUT_DIR_ROOT` pointing elsewhere
(simulating the Windows redirect) — the exact case that was previously broken.

## Follow-up: still no error seen at the app, after the SD-card gate fix (Claude, 14 September 2026)

Charles tried the fixed firmware with the SD card absent and still saw no error reach
the app, and asked (a) for assurance the AI processor actually tells the app to abort,
and (b) whether "quiet" transfer-mode logging could be deferred until we know the
transfer can proceed.

**Tracing the chain (code-level assurance, not yet confirmed against a fresh console
capture from this exact run):** the new `fatfs_mounted()` guard in
`APP_MSG_FATFSTASK_OPEN_FILE` sets `fileOp->res = FR_NO_FILESYSTEM` and replies with
`APP_MSG_IFTASK_DISK_WRITE_COMPLETE`. `if_task.c`'s `handleEventForStateDiskOp()`,
`DISK_PHASE_FILE_OPEN` case, already turns any non-`FR_OK` `fileRxOp.res` into
`ftx err %d` sent over I2C (`sendI2CMessage()`) — this call is unconditional, **not**
gated by `g_fileRxActive`/quiet mode. So structurally the AI processor should be
sending the app something. Whether the nRF forwards `ftx err` correctly at this
specific point (`FILE_OPEN` phase, before any data has flowed) is outside this repo
(ww-hardware) and unverified.

**However, `g_fileRxActive` ("quiet mode") was being set too early** — at
`AI_PROCESSOR_MSG_FILE_START` (`if_task.c:549`, now removed), before the OPEN_FILE
round-trip to fatfs_task even happened. That doesn't stop the `ftx err` message itself
being sent (the send call isn't gated), but it does suppress most of the surrounding
console diagnostics (the incoming `FILE_START` hex dump, the outgoing message trace),
making the console much less useful for seeing what happened on exactly the failure
Charles hit.

**Fix:** moved `g_fileRxActive = true` from `FILE_START` to right after `OPEN_FILE`
succeeds (`if_task.c`, `DISK_PHASE_FILE_OPEN` success branch, after `ftx ack 0` is
sent) — quiet mode now only starts once the transfer is actually confirmed underway.
A failure at `FILE_START`/`OPEN_FILE` (no SD card, bad filename, etc.) now always gets
full console visibility, matching Charles's request directly. `g_fileRxActive` is
purely a console-verbosity flag (confirmed by inspection - every use is
`if (!g_fileRxActive) { xprintf(...) }` or setting/clearing it; nothing branches
functionally on it), so this doesn't change any transfer behaviour, only what's
visible on the console.

**Open item:** please capture (or share) the Himax console for a repeat of this exact
test (SD card absent, attempt firmware update) with the new code, so we can see
whether `SD card not mounted - refusing '<file>'` and the `ftx err` trace now appear,
and separately whether the app/nRF actually surfaces that `ftx err` to the user. If the
console shows the error being sent but the app still shows nothing, the remaining gap
is on the nRF/app side, not here.

## Root cause found: two OPEN_FILE handlers, only one was patched (Claude, 15 September 2026)

Charles ran a genuine clean build (confirmed: object files at `D:\hxbuild` timestamped
after the source edits, and `strings` on the compiled `.o` shows the new
"SD card not mounted - refusing" etc. text is present in the binary) and captured a
timestamped log of both processors. Symptom unchanged: `ftx ack 0` sent, whole file
"received OK" with CRC verified, `firmware ...` still `Unrecognised`, no error ever
reaches the app. So yesterday's fix compiled fine but had no effect — worth recording
why, since it wasn't obvious from the source alone.

**`fatfs_task.c` has two separate `APP_MSG_FATFSTASK_OPEN_FILE` handlers**, one per
`fatFs_task_state`:

- `handleEventForIdle()` (~line 783) — the one patched 14 Sep. Only reachable once
  `fatFs_task_state == APP_FATFS_STATE_IDLE`, which the boot code only sets **after** a
  successful mount.
- `handleEventForUninit()` (line 497) — reachable while `fatFs_task_state` is still
  `APP_FATFS_STATE_UNINIT`, its state at boot. **The boot-time mount failure path never
  advances this state** (`fatfs_task.c`'s startup sequence sets
  `fatFs_task_state = APP_FATFS_STATE_IDLE` only inside the `if (res == FR_OK)` branch)
  — so with no SD card, the task stays in `UNINIT` for the entire power-up, and
  *every* file-related message, forever, is handled here instead.

So the 14 Sep fix was dead code for this exact scenario — correct, but for a state that
is never reached when the card is absent from boot.

**The real bug:** `handleEventForUninit()`'s `OPEN_FILE`/`APPEND_FILE`/`CLOSE_FILE`
case (and, found in the same sweep, its `WRITE_FILE`/`WRITE_IMAGE`/`READ_FILE` cases)
already existed and already replied with `FR_NO_FILESYSTEM` — but only in the
message's `msg_data` field. `if_task.c`'s `handleEventForStateDiskOp()`, which decides
whether to send `ftx ack` or `ftx err`, checks the **`fileOp->res` struct field**
(`fileRxOp.res != FR_OK`), not `msg_data`. That field was never written in this
handler, so it sat at its default `FR_OK` (0) — a caller checking it saw "success" no
matter what `msg_data` said. This matches the console evidence exactly: `Rx data =
0x0000000d` (13, `FR_NO_FILESYSTEM`) on the `Disk Write Complete` event, immediately
followed by `ftx ack 0` being sent — the *message* carried the right error code, the
*struct field the completion handler actually reads* did not.

**Fix:** added `fileOp->res = FR_NO_FILESYSTEM;` alongside the existing `msg_data`
assignment in all three `handleEventForUninit()` cases. Verified with `make -n` only
(parses/compiles file discovery; no toolchain available to fully build in this
session — Charles is building).

**Lesson for next time a "why doesn't my fix do anything" comes up in this task:** grep
for the *same event name* across the whole file before concluding a single edit site
covers it — `grep -n "case APP_MSG_FATFSTASK_OPEN_FILE"` would have shown both sites
immediately. Cost a full day/two round-trips here.

## Confirmed fixed (Claude, 15 September 2026)

Charles re-tested (SD card still absent) after the `handleEventForUninit()` fix.
Timestamped logs from both processors confirm the full chain now works:

- **AI log:** `OPEN_FILE` completes with `Rx data = 0x0000000d` (13, `FR_NO_FILESYSTEM`)
  as before, but this time sends `Sending 15 bytes: ... 'ftx err 6'`
  (6 = `FILERX_ERR_FILE_OPEN`, `fileRx.h`) instead of `ftx ack 0`.
- **BLE log:** `BLE out: Sent 10 bytes: 'ftx err 6'`, immediately followed by
  `AI state changed from PROCESSING to IDLE` — the app's transfer state machine drops
  out on the error instead of continuing to send the rest of the file's packets (the
  behaviour Charles described before any of this session's fixes: "the app continues
  to send all the bytes of the file").

SD-card-absent firmware-update failure: **fixed**. Three changes landed this thread,
all in `ww500_md` (uncommitted, branch `firmwaretx`):

1. `fatfs_task.c` — `fatfs_mounted()` guards in `fileWrite()`, `fileWriteImage()`,
   `fileRead()`, and `handleEventForIdle()`'s `OPEN_FILE` case (14 Sep).
2. `fatfs_task.c` — `handleEventForUninit()`'s `OPEN_FILE`/`APPEND_FILE`/`CLOSE_FILE`
   and `WRITE_FILE`/`WRITE_IMAGE`/`READ_FILE` cases now set `fileOp->res`, not just
   the message's `msg_data` (15 Sep) — **this was the actual fix**; #1 patches a state
   the task never reaches when the boot-time mount fails.
3. `if_task.c` — quiet-mode (`g_fileRxActive`) deferred until `OPEN_FILE` succeeds, so
   a failure here is never silenced (14 Sep).
4. `ww500_md.mk` — unrelated `force_rebuild_main` fix for stale `__TIME__`/`__DATE__`
   on Windows builds (14 Sep).

## Open items

- The original BLE-transfer-stall hypothesis (`ble_log.txt`/`ai_log.txt`, 14 Sep,
  before the SD-card-absent cause was found): a transfer with the card present died
  mid-stream, correlating 1:1 between 5 app-side `Response timeout` events and 5
  Himax-side `Missing Master` I2C failures, worse right where the BLE connection
  interval was visibly unstable. Working hypothesis is `MISSINGMASTERTIME` (4000 ms)
  being exceeded by this phone's (Galaxy A05) renegotiation stalls, with no ack retry.
  **Not yet investigated further or fixed** — parked once the reproducible
  SD-card-absent bug took priority. Should be revisited with a real card in the slot.
- None of this thread's fixes are committed yet.
