# Inactivity event dropped during I2C exchanges (GitHub issue #205)

#### File: README.md
#### Author: Claude Sonnet 5, reviewing a fix suggested by a different AI
#### 4 September 2026

## Status

Open - fix implemented, not yet build-verified or device-tested by Charles.

## Background

A different AI opened [GitHub issue #205](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/205),
"An inactivity event during an I2C reply leaves the device awake until it is
power-cycled": `handleEventForStateI2CTx()` (`if_task.c`) had no case for
`APP_MSG_IFTASK_INACTIVITY`, so it fell to `flagUnexpectedEvent()` and was
silently dropped with no retry - if that happened, the device's shutdown
barrier never completed and it stayed awake (no DPD, no motion detection,
draining the battery) until power-cycled. Suggested fix: defer the event via
the existing `savedMessage` mechanism, the same way three other event types
are already deferred in that state.

Charles asked for this to be verified rather than adopted on trust, and asked
a good question along the way: why would an inactivity event ever fire *during*
an I2C exchange at all - shouldn't starting I2C activity reset the inactivity
timer?

## What we found

**The reported bug is real** - confirmed by reading `handleEventForStateI2CTx()`
directly; it has no case for `APP_MSG_IFTASK_INACTIVITY` and falls to `default:`.

**Why inactivity can genuinely fire mid-exchange**: the active mechanism
(`inactivity.c`, `USEIDLETASK`) tracks *CPU idle time*, not *per-operation
activity*. `inactivity_on_task_switched_in()` resets the idle clock whenever
any real task runs; `inactivity_IdleHook()` fires the callback once the idle
task has been scheduled continuously for `OP_PARAMETER_INTERVAL_BEFORE_DPD`
(default 1000ms - short). An I2C exchange can spend real time waiting for the
*other* processor (BLE/nRF) to read the data, bounded only by the much longer
Missing Master timeout - if nothing else needs the CPU during that wait, it's
genuinely idle by this definition, even though the exchange is still logically
in progress. So the scenario in the issue isn't a contrived edge case; it's a
natural consequence of a short default inactivity timeout combined with I2C
exchanges that can legitimately take that long.

**Why it's a permanent lockup, not a one-cycle glitch**: the dropped message
has no retry. Since the "Sleep" message never gets sent, the device never
enters DPD - it stays awake and reachable, which likely means it keeps
receiving further I2C/BLE traffic, continuously resetting the idle clock. A
full clean idle stretch may then never reaccumulate - a self-sustaining
stuck-awake livelock.

**The suggested fix is correct** - traced the `savedMessage` replay mechanism:
a deferred message is re-queued on return to `APP_IF_STATE_IDLE`, and `IDLE`'s
handler does correctly act on `APP_MSG_IFTASK_INACTIVITY` (builds and sends
the "Sleep" message, sets `lastMessageSent`, transitions to `I2C_TX`).

**The reported bug was only one instance of a systemic gap.** Checking all 8
IF-task states found `APP_MSG_IFTASK_INACTIVITY` was only ever wired up for
`IDLE`:

| State | Handled it? |
|---|---|
| `UNINIT` | N/A - drops everything by design (pre-task-start) |
| `IDLE` | Yes - the only correct handler |
| `I2C_RX` | No - dropped |
| `I2C_TX` | No - dropped (the one #205 reported) |
| `I2C_SLAVE_TX` | No - dropped, with the original author's own `// TODO think abot what is expected!` sitting right above a commented-out deferral block for this exact gap |
| `I2C_SLAVE_RX` | No - dropped |
| `PA0` | No - dropped (test-only state, `TEST_INT_PULSE`, low real-world impact) |
| `DISK_OP` | Already safe - its `default:` defers *any* unrecognised event, not specific ones, so it caught this incidentally |

## Fix applied

Added `case APP_MSG_IFTASK_INACTIVITY:` to all 5 affected states
(`handleEventForStateI2CRx/I2CTx/I2CSlaveTx/I2CSlaveRx/PA0` in `if_task.c`),
deferring via the existing `savedMessage = rxMessage;` pattern each state
already uses for other events. `I2C_TX` carries the full explanation (root
cause, why it's genuinely reachable, why deferral works); the other four
reference it rather than repeating the comment. `UNINIT` and `DISK_OP` were
left alone - not applicable / already safe respectively.

One caught-and-fixed mistake along the way: the first `I2C_RX` edit put a
"fall through deliberately" comment on the `INACTIVITY` case itself, but that
case is the one that actually executes and `break`s - it's the *preceding*
`PA0_INT_OUT` case that falls through into it. Comment reworded to attribute
the fallthrough to the correct case; the C logic itself was always correct in
every state, this was a documentation-only error caught by Charles reading
the diff.

## Residual note (not fixed, pre-existing, out of scope for this thread)

`savedMessage` is a single-slot buffer, not a queue. If two different
deferrable events arrive during the same busy window in the same state
(e.g. `APP_MSG_IFTASK_INACTIVITY` and one of the other already-deferred event
types), one would silently overwrite the other. This limitation already
existed for the other deferred event types before this fix; not introduced by
it, and not addressed here.

## Outcome

The other AI's diagnosis and suggested fix for `I2C_TX` were both correct,
but scoped to the one instance it happened to reproduce. The actual pattern
was systemic (5 of 8 states affected). Fixed all 5. Not yet build-verified or
device-tested.

## Open items

- [GitHub issue #205](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/205) -
  still open; close once Charles has built and confirmed the fix on device.
- The `savedMessage` single-slot limitation above is not tracked as a separate
  issue - flagging here in case it's worth filing later if it's ever observed
  in practice.
