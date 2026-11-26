# BLE processor changes for the wake sequence and photo times

#### File: `CLAUDE_BLE_processor_brief.md`
#### Written in: Seeed_Grove_Vision_AI_Module_V2, `_Documentation\development reports\2026-11-01_AI_RTC\`
#### For: ww-hardware, `MokoTech/Workspace/WildlifeWatcher_1`, branch `261101_AI_RTC`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 1 November 2026

Two jobs: a record for people of what changes and why, and instructions for the Claude session that
codes the BLE processor side. The AI processor side is done and benched.

Names: this document says "BLE processor" and "AI processor" (the HX6538, called "Himax" in parts of
ww-hardware).

## Instructions for Claude

1. Read ww-hardware's `AGENTS.md` and `.agents/skills/SKILL.md` first, and follow them.
2. Charles builds, flashes and commits. Do not run the build, do not change git state, do not offer to.
3. Wait for Charles's "go" before editing code. Propose first if anything here is unclear or wrong
   against the code.
4. Keep the changes small: two changes, all in `aiStateMachine.c` (and `aiProcessor.h` if
   `PERMITTEDTIMEERROR` changes use). No new commands, no new states.
5. Both processors are always updated together. Do not add code for mixed versions.
6. Update `.agents/skills/references/traps.md`, section "Two processors, two clocks, two versions":
   it says `setutc` is sent only when the clocks differ by more than 300 s, which becomes wrong.

## Why

1. **Photo times.** The AI processor stamps each photo from its own clock (RTC). That clock runs about
   4.6 % fast, and its first read after a wake can return the time it went to sleep. The BLE
   processor's clock is right once the app or LoRaWAN has set it. Today the BLE processor only sends
   `setutc` when the two differ by more than 300 s, so photos are often minutes out.
2. **Messages were lost.** Since PR #283 (Seeed repo) the AI processor no longer waits for the SD card,
   so `NN-` and other messages could reach the BLE processor before the wake message, while it was
   still in SLEEP (`UNHANDLED event NN- in SLEEP`): no LED flash, nothing to the app. The AI processor
   now sends the wake message first, which fixes the order. The BLE processor should still handle
   these messages in every awake state.
3. **`setutc` used to block the AI processor for about 1.4 s** while it wrote its RTC, during the
   photo. That is why it was rationed. The AI processor no longer writes the RTC on `setutc`, so it
   can be sent every wake.

## What the AI processor now does (done, benched 1 Nov)

1. Sends the wake message (`Wake|Timer|Motion <AI time>`) first, about 10 to 100 ms after boot. Its
   time can still be stale: it is only used for the BLE processor's `differ by` log line.
2. On `setutc <time>`: keeps the time in RAM and replies at once with `RTC set to <time>`, sometimes
   followed by ` (was out by N s)`. The reply still starts with `RTC set `, which `ble_commands.c`
   matches as `Ai_Event_AiUtcReceived`, so the SETUTC state works unchanged.
3. Photo times, the file date and `getutc` use that time plus the ticks since it arrived.
4. The first photo of a wake waits up to 300 ms for `setutc`, then falls back to its own RTC and prints
   `No setutc after 300ms: photo time from the AI processor's RTC`. Later photos never wait.
5. Writes its own RTC just before sleep, after the `Sleep` message, only when more than 10 minutes out
   (or never set). That takes 1.3 to 1.9 s.
6. Holds settings commands (`setop`, `capture` and the like) until its settings are loaded, about
   200 to 250 ms after boot, then replies. `selftest`, `setutc`, `getutc` and `ver` run at once.

Bench, 1 Nov: `setutc` from the BLE processor arrived 160 ms after the wake message and the photo was
stamped with its time. Anything that delays `setutc` by more than about 400 ms after the wake message
makes photos use the fallback.

## The sequence after the change

```
AI processor                                   BLE processor
boot
wake msg "Motion 2026-11-01T05:45:09Z" ---->   SLEEP -> WOKEN, logs "differ by"
                                       <----   "selftest"
reply "selfTest 0000"                  ---->   SELFTEST
                                       <----   "setutc 2026-11-01T05:45:12Z"   (every wake, if it has the time)
reply "RTC set to ..."                 ---->   SETUTC -> IDLE, then the queued app command
NN-, AE regs, grid, "Captured ..."     ---->   handled in every state
```

The order is today's order. Only the condition for `setutc` and the handling of messages change.

## The changes (all in `aiStateMachine.c`)

### 1. Send `setutc` every wake when the BLE processor has the time

- In the SLEEP handler, case `Ai_Event_AiWakeReceived`, `mustSendUtc` is set only when
  `delta > PERMITTEDTIMEERROR` (300 s, `aiProcessor.h`). Make it `true` on every wake, including when
  the wake message has no time or it does not parse.
- Keep the `differ by` log line. `PERMITTEDTIMEERROR` then only chooses its colour and the
  `- too much!` text. Consider dropping the `\a` (bell), which would otherwise ring on most wakes when
  the clocks are far apart.
- Leave the SELFTEST handler as it is: it already checks `iso_time_isSet()` and goes to IDLE with
  `Can't send UTC` when the BLE processor has no time. The AI processor then uses its fallback.

### 2. Handle messages from the AI processor in WOKEN and SLEEP

- `handleEventsForWoken()` ignores `Ai_Event_AiNNPlusReceived`, `Ai_Event_AiNNMinusReceived` and
  `Ai_Event_MessageFromAi` (`DEBUG: Ignore this in WOKEN for now`). Handle them as SELFTEST, SETUTC
  and IDLE do: `NN+` green flash, `NN-` red flash, both set `aiSmMessage`; `bleMsg_push()` for other
  messages.
- `handleEventsForSleep()` sends them to `default:` (`UNHANDLED`). Handle them the same way. They can
  only arrive in SLEEP if the wake message was lost. Do not change state for them.
- Best done with one small helper used by all five states, rather than five copies.

## What does not change

- No new command, no reply format change the BLE processor depends on, no new state.
- The app sees the same messages as before, and now also gets `NN+`/`NN-` and the motion messages on
  every wake. No EXIF change.
- `AI_RESPONSE_TIMEOUT_MS` (10 s) is ample for the AI processor's held commands.
- App commands that arrive during the 100 to 200 ms wake handshake (WOKEN, SELFTEST, SETUTC) are still
  dropped (`DEBUG: Ignore this in ... for now`). Rare, and left as it is (Charles, 1 Nov).

## Bench tests (after both processors are built and flashed)

Board with the HM0360 image. Start two Tera Term logs, one per processor. Tera Term's timestamps
are NZDT (UTC+13); the devices use UTC. Note the current ops first with `AI getop 5`, `AI getop 6`
and `AI getop 7`, so they can be restored (on 1 Nov they were 1, 500 and 0).

1. **Set the BLE processor's time.** Connect the app, which sets it. Disconnect if you want the camera
   to sleep normally.
2. **`setutc` every wake.** Wave at the camera 3 times, a few seconds apart.
   - BLE log, each wake: `Send 'setutc 2026-...'`, then `AI state changed from SETUTC to IDLE`.
   - AI log, each wake: `RTC set to 2026-...`, and no `No setutc after 300ms`.
   - AI log: the `Wrote ... at h:m:s` time matches the BLE log's `setutc` time to within 1 s.
3. **Messages handled.** In the same logs:
   - BLE log: `NN-` (or `NN+`) is never followed by `Ignore this in WOKEN` or `UNHANDLED`.
   - The board's red (or green) LED flashes once per wake, and the app receives `NN-` when connected.
4. **Timer wake.** Type `AI setop 7 60` in the app. Wait for a wake about a minute later, check as in
   step 2, then type `AI setop 7 0`.
5. **BLE processor with no time.** Reset the BLE processor (power cycle or its reset) and do not
   connect the app. Wave at the camera.
   - BLE log: `Can't send UTC`, then IDLE.
   - AI log: `No setutc after 300ms: photo time from the AI processor's RTC`, and the photo is saved.
6. **Burst.** Connect the app (to set the time again), type `AI setop 5 5` and `AI setop 6 200`, then
   wave once.
   - AI log: one `RTC set to`, 5 `Wrote ... at` lines about 0.2 s apart in time order, no
     `No setutc` line.
   - Restore: `AI setop 5 1` and `AI setop 6 500` (or the values noted at the start).
7. Stop both logs and keep them with the thread (Seeed repo, `2026-11-01_AI_RTC/logs/`).

## References (Seeed repo, `_Documentation/development reports/2026-11-01_AI_RTC/`)

- `CLAUDE_wake_sequence_spec.md`: the spec for both processors, with the AI processor bench results.
- `CLAUDE_BLE_time_proposal.md`: the background and the RTC findings.
- `_Documentation/ble_commands.md`: the `setutc` reply.
