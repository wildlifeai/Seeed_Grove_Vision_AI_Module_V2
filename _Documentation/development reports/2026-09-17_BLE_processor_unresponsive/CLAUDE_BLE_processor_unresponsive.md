# Task: Understand and fix Unresponsive BLE processor

#### File: CLAUDE_BLE_processor_unresponsive.md
#### Author: Charles Palmer
#### Date: 17-19 September 2026

## Background

Victor emailed on 16/9/26 as follows:

```
One of the three camera Nick assembled kept saying AI NACK in the engineer console. 

I am not sure how it got there but I have managed to connect the camera via USB and have 
flashed the firmware to the latest dev branch firmware images. Unfortunately, 
I still keep on seeing the himax processor caught in a never-ending cycle. 
```

Victor provided the log file from the AI processor: [teraterm160926.txt](teraterm160926.txt).
Then later logs from both [teraterm_ble.txt](teraterm_ble.txt) and [teraterm_ai.txt](teraterm_ai.txt) taken simultatnespusly - these will have timestamps to see
what the BLE processor is doing while the AI processor times out.

I then ran a cold boot sequence on my desktop unit to produce [ble_log_1.txt](ble_log_1.txt) and [ai_log_1.txt](ai_log_1.txt)
as the reference for normal behaviour.

## Evidence of Failure

The log file shows a cold boot and the first message sent by the AI processor at around line 110 
does not receive a response.

Three 1000ms inactivity timeouts occur before the 4000ms MISSINGMASTERTIME timer expires at around
line 145.

In the mean time the single `savedMessage` variable (which is supposed to retain messages
that need to be deferred) is corrupted by more than 1 message needing deferral.

## Possible Cause

* Is the BLE processor compeletely dead?
* Does the timing of the two processors waking up cause the AI processor to send a message before the 
BLE processor is ready?

I am waiting to see if Victor can provide the console log for the BLE processor so we can see that as well.
__LATER__ these arrived and are analysed below:

## Analysis of simultaneous logs

#### Correct behaviour

In  [ble_log_1.txt](ble_log_1.txt) and [ai_log_1.txt](ai_log_1.txt):
1. AI processor asserts its first interrupt to BLE processor at `10:44:00.250` (the pin chnages before the message is printed).
2. BLE processor receives this at `10:44:00.197` - therefore OK.

#### Incorrect behaviour

In [teraterm_ble.txt](teraterm_ble.txt) and [teraterm_ai.txt](teraterm_ai.txt)
1. AI processor asserts its first interrupt to BLE processor at `17:25:04.290` (the pin chnages before the message is printed).
2. BLE processor does not see this: no console output between `17:25:03.316` and `17:25:14.388`
3. AI processor sends several more messages to the BLE processor which are not acted upon, until the 5th 
at `17:25:20.343`.
3. The BLE processor does see this interrupt at `17:25:20.387`. It then presumably tries to read 
AI data but instead gets to `i2cError()` and reports 'AI NACK'
4. It turns out that at `17:25:20.343` `did send a message (Sleep) to the BLE processor 
with an interrupt pulse. 
5.	The AI processor sends several more 'Sleep' messages with interrupts and these did seem to arrive at the 
BLE processor at  17:25:24.389, 17:25:28.382, 17:25:32.425, 17:25:36.407

Interestingly, the BLE counts interrupts and prints the count:  `<info> app: !4` 0 since the count starts at 0
this is the 5th count, which is the same number that the AI processor has sent. The code is there:
```
void aiProcessorAiIntEvent(void) {
	static uint8_t count = 0;

	if (!g_fileTxActive) {   // silent per-packet during a transfer session
		// Just show we have received something
		NRF_LOG_INFO("!%d ", count++);
	}

	if (m_aiProcessorEnabled) {
		// Probably in the ISR context so use the scheduler.
		app_sched_event_put(NULL, 0, executeI2cRead);
	}
}
```
So one explanation is that `m_aiProcessorEnabled` was false for the first 4 interrupts. 
This is set true at `17:25:03.011` when the BLE processor prints "Enabled I2C (instance 1)".
But the AI processor did not send its first message until after this, so that should not be the problem.

The other possibility is that  executeI2cRead() is not scheduled, or fails to perform the I2C read.  



## Branch check (Claude, 17 September 2026): `dev` is missing the GPIOTE-accuracy fix

_This section is from Claude: wild goose chase I think and TL;DR_

Before speculating further, checked whether Victor's "latest dev branch firmware" actually
contains the interrupt-detection fix from the parallel `2026-09-14_firmware_update_fails`
thread. It does not:

```
$ git merge-base --is-ancestor c3f14b9 dev
NO - c3f14b9 is NOT in dev
```

`c3f14b9` ("Fixed errors on large file transfer (interrupts missed)") is the commit,
on `charles_fileTxFix`, that switches `/IP_INT` to a dedicated, hardware-latched GPIOTE
channel for the duration of a BLE connection - see that thread for the full mechanism
(the default low-power PORT-event interrupt can silently miss a transition if the pin
is back at its previous level by the time the CPU, running at a non-elevated NVIC
priority, gets around to servicing the shared event). `dev`'s last touch of
`aiProcessor.c` is `18ffc89` ("sustain file transfer streaming"), an earlier, unrelated
commit - the GPIOTE fix was never ported across.

This matters directly here: this log's dominant symptom - `MISSINGMASTERTIME` firing
repeatedly, forever, with the device never reaching DPD - is the *exact* signature the
GPIOTE fix was written to address (see [[ww500_branch_divergence_gotcha]] in memory:
a fix diagnosed and committed on one branch can remain fully present, unfixed, on
another indefinitely). Before investigating anything else, this branch should be
retested with that fix ported across - it may simply resolve this report outright.
It doesn't rule out a wake-timing race (Charles's second hypothesis above) as an
additional or alternate contributor, but it's the more likely and more direct
candidate, and is already understood and already fixed elsewhere.

## Problem of MISSINGMASTERTIME timer

_This section is from Claude: not relevant to the main problem but wirth returning to. Also TL;DR_

Charles asked (17 Sep) whether reverting `MISSINGMASTERTIME` from 4000ms back to 300ms
would help, based on this log's own arithmetic: the first message (a `Wake` message,
around line 110) gets no response, and three separate 1000ms `Inactivity` timeouts
occur before the 4000ms `MISSINGMASTERTIME` timer finally expires (~line 145) - each of
those `Inactivity` events competing for the single `savedMessage` slot in the meantime
(see the new section below).

The comment above `MISSINGMASTERTIME` in `if_task.c` (`:73-85`) records a three-step
history, and reverting to the first step risks reintroducing the two separate, already-
diagnosed problems that motivated each raise:

1. **300ms** (commented out) - too tight: during file uploads the master was still
   going to read the message, just not within 300ms under normal load. The AI gave up
   and retried, and the master then saw **two** interrupts for what it considered one
   outstanding request, confusing its state machine into a `'busy'` error. Not a
   detection failure - just insufficient headroom for the master's own I2C-read
   scheduling.
2. **1000ms** ("Raised from 1000ms") - fixed #1, but itself proved too tight for a
   second, independently measured problem:
3. **4000ms** (current) - during a sustained file transfer, Android periodically
   re-requests high connection priority to stop the interval decaying, and each
   re-request measurably stalls the link ~950ms while it renegotiates - right at the
   edge of the 1000ms budget. 4000ms was chosen to comfortably survive that stall
   while staying under two further, unrelated budgets (the file session's own 5s
   inactivity timeout, and the app's 15s silence timeout).

Both of the problems that justified raising it are **independent of the GPIOTE
detection bug**: that fix makes the master more reliably *notice* an interrupt, but
does nothing for how long its own I2C-read scheduling takes once it has noticed, and
nothing for a genuine radio-level stall during connection-parameter renegotiation.
Reverting to 300ms (or 1000ms) risks reintroducing both of those on any sufficiently
long or loaded transfer - regressions that would very likely go unnoticed until they
resurface later, exactly as this one did.

Shortening the timeout **would** help the `savedMessage` problem specifically - fewer
`Inactivity` events would pile up per failed cycle, and the device would retry roughly
13x faster - but that's harm reduction for a *symptom*, not a fix for why the first
message went unanswered at all, or for `savedMessage`'s own design flaw. Recommend:
retest with the GPIOTE fix ported across (see above) before touching this constant at
all; if `MISSINGMASTERTIME` still needs revisiting after that, test any change against
a long file transfer specifically (to check problem #2 doesn't resurface at the ~24s
mark), not just the boot/wake scenario this report is about.

__Comment from CGP__

I think Claude is unduely worried about the timeout. AFAIK the interrupt from the AI processor should
elicit an I2C read pretty much immediately. The sequence is as follows:

1. File : Line: MokoTechSDK/src/lora/boards/WW500-C02/gpio-board.c:50-56
What happens: gpiote_evt_handler() — the raw nRF52832 GPIOTE/PORT-event callback. Looks up GpioIrq[pin] and calls it.
This is registered as the SDK-level handler for the pin via nrf_drv_gpiote_in_init() at :153.

---

2. File : Line: WildlifeWatcher_1/main.c:482
What happens: onAiInt(void *context) — the actual function pointer stored in GpioIrq[IP_INT] (via
aiProcessorInitAiInt(onAiInt) at boot). Reads the pin level; on a rising edge calls button_event_handler(IP_INT,
APP_BUTTON_RELEASE) (:488).

---

3. File : Line: WildlifeWatcher_1/main.c:309-318
What happens: button_event_handler()'s case SE (i.e. rising edge = negated by the AI
processor), calls aiProcessorAiIntEvent() (:315). The push/falling-edge case is explicitly ignored.

---

4. File : Line: WildlifeWatcher_1/aiProcessor.c
What happens: aiProcessorAiIntEvent() — if m_aiProcessorEnabled, defers onto the scheduler: app_sched_event_put(NULL,
0, executeI2cRead) (:921), moving off ISR cothe ISR context so use the  scheduler").

---

5. File : Line: WildlifeWatcher_1/aiProcessor.c:465-507
What happens: executeI2cRead() — the schedul_busy() check (:469-480), calls
nrf_drv_twi_rx(&m_twi_master, AIPROCESSORADDR, i2cRxBuffer, I2CCOMM_MAX_RBUF_SIZE) at :504 — the actual I2C read.  

So: hardware edge → gpio-board.c:54 → main.c:482 (onAiInt) → main.c:315 (button_event_handler) → aiProcessor.c:921 (schedules) → aiProcessor.c:504 (executeI2cRx() call).

However, it is worth chekcing if `m_aiProcessorEnabled` is enabled in time.

## The `savedMessage` single-slot problem (Claude, 17 September 2026)

_This section is from Claude: not relevant to the main problem but wirth returning to. Also TL;DR_

Flagged briefly above; documenting properly here since this needs addressing whenever
we return to this report, independently of whatever the underlying detection issue
turns out to be.

`savedMessage` (`if_task.c:203`) is a **single static variable**, not a queue, used to
defer any event the interface state machine can't handle immediately (arrives while
busy - mid I2C-TX, mid I2C-RX, or mid a disk operation). Four separate call sites can
write to it: `if_task.c:1054` (`handleEventForStateI2CRx()`), `:1169`
(`handleEventForStateI2CTx()` - covers `INACTIVITY`, `RX_READY`, `MSG_TO_MASTER`, and
the whole `CLI_STRING_RESPONSE...CLI_BINARY_CONTINUES` range), `:1221`
(`handleEventForStatePA0()`), and `:1351` (`handleEventForStateDiskOp()`'s `default:`
case - any non-file event during a disk operation). When the state machine returns to
`IDLE` it replays whatever is sitting in `savedMessage` (`:1531`) and clears it.

**The bug**: if two *different* deferrable events arrive before the first gets
replayed, the second assignment silently overwrites the first. The first message is
gone - never sent, never retried, never logged as lost. Both the original defer and the
one that clobbers it print an identical, generic `"Deferring event 0x%04x"` line, so
nothing in the log distinguishes a clean single deferral from a collision that just
destroyed a message.

**This exact log confirms it, with a 100% loss rate for one message type over the
whole capture**: searching the full ~1:47 session,
`"Deferring event 0x070d"` (`Message to Master` - an unsolicited `sendMsgToMaster()`
call, e.g. from `image_task.c`/`lightSensor.c`) appears **26 times**.
`"Issuing deferred event 0x070d"` (that message actually being replayed) appears
**0 times**. Every single occurrence, for the entire session, was silently overwritten
by a subsequent `Inactivity` (`0x070e`) deferral landing in the same busy window before
its turn came (visible directly at lines 132-133 then 136-137: `Message to Master`
deferred, then three lines later clobbered by `Inactivity`). Whatever that recurring
message actually is (a fixed-address pointer, `0x30011108`, each time - likely the same
static buffer reused for a periodic status/telemetry send), it was never once delivered
in this session.

**Why this matters even after the detection/timeout issue above is resolved**: a
shorter `MISSINGMASTERTIME` narrows the collision window but doesn't close it - any two
qualifying deferrable events landing in the same busy window (most plausibly during a
disk operation, which can run up to ~2s) will still silently collide. The real fix is
structural: turn `savedMessage` into a small FIFO (matching the queue-based patterns
already used elsewhere in this codebase, e.g. `fileTx`'s packet FIFO on the BLE side),
or at minimum detect an incoming overwrite and log it distinctly so a collision is no
longer invisible. Not yet designed or implemented - park for when we return to this.

## File as github issue

_This section is from Claude: probably not to be filed as it diagnoses the wrong problem. Also TL;DR_

Draft text below, ready to file - not yet filed.

---

**Title:** AI processor stuck in endless retry loop, never sleeps - `dev` branch missing GPIOTE interrupt fix

**Body:**

One of the three cameras Nick assembled was reported (Victor, 16 Sep) showing "AI NACK"
in the engineer console. Reflashed to the latest `dev` branch firmware; the Himax AI
processor is still caught in a never-ending cycle. AI-processor console log attached:
see `_Documentation/development reports/2026-09-17_BLE_processor_unresponsive/teraterm160926.txt`,
full analysis in `CLAUDE_BLE_processor_unresponsive.md` in that same folder.

**Symptom**: from a cold boot, the AI processor's first message to the BLE processor
(a `Wake` message) never gets read. `MISSINGMASTERTIME` (4000ms) then fires repeatedly,
indefinitely - the device never reaches DPD for the whole ~1:47 capture, continuously
burning power.

**Root cause (verified)**: `dev` does not contain commit `c3f14b9`
("Fixed errors on large file transfer (interrupts missed)"), which fixes exactly this
signature on a different branch (`charles_fileTxFix`) - see the
`2026-09-14_firmware_update_fails` development report. The BLE processor's default
`/IP_INT` interrupt uses the nRF52832's low-power PORT-event mechanism at a
non-elevated NVIC priority, which can silently miss a transition under load rather than
merely delay noticing it. The fix switches to a dedicated, hardware-latched GPIOTE
channel for the duration of a BLE connection.

**Action**: port `c3f14b9` (and its follow-on commits) from `charles_fileTxFix` to
`dev`, then retest with this exact scenario (cold boot, watch for the first `Wake`
message being read promptly).

**Related, separate defect found while investigating this log** (not yet fixed,
tracked here so it isn't lost): `if_task.c`'s `savedMessage` variable is a single slot,
not a queue, used to defer events that arrive while the interface is busy. If two
different deferrable events arrive before the first is replayed, the second silently
overwrites it - confirmed in this exact log: an unsolicited `Message to Master`
send was deferred 26 times and successfully replayed 0 times. See the
`savedMessage` section of `CLAUDE_BLE_processor_unresponsive.md` for the full
mechanism and fix proposal (convert to a small FIFO).




