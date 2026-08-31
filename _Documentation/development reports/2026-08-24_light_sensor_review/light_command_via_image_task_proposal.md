# The 'light' CLI command routes through the image task, via the real capture path

**Status: implemented 31 August 2026 (revised from the original design below after
Charles asked for one further change). Both `cis_imx708` and `cis_hm0360` build clean.
Not yet device-tested.**

## Problem

The `light` CLI command (`prvLight()`, `CLI-commands.c`) originally called
`lightSensor_takeReadingForced()` directly from whichever task processes console
commands (the "CLI" task). This was inconsistent with every other light check in the
firmware: a real capture's post-capture check, and the periodic
`OP_PARAMETER_AE_CHECK_INTERVAL` timer wake (`aeCheckOnlyWake`), both go through the
**image task**'s message queue and state machine (`APP_MSG_IMAGETASK_STARTCAPTURE` →
`handleEventForInit()`).

It was also a real concurrency hazard, not just an inconsistency: `light` called the
same `hm0360_md.c` I2C functions (which swap the I2C slave ID to address the HM0360,
then swap it back) that a real image-task-driven capture also calls. `hm0360_md.c`'s
`saveMainCameraConfig()` has a standing comment: `// TODO do we need some critical
section or semaphore code here, in case we change task mid-stream?` - i.e. this was
already a known, unaddressed gap. If someone ran `light` from the console at the same
moment the image task was mid-capture, the two tasks could race on the shared I2C
slave-ID register.

## First version (superseded)

The first implementation routed `light` through the image task's queue, but as a
lightweight shortcut: `APP_MSG_IMAGETASK_STARTCAPTURE` with `msg_data == 0` went
straight to `lightSensor_takeReadingForced()` inside `handleEventForInit()`, with no
real image capture at all - closing the I2C-race gap, but still a *different* code
path through the state machine than `aeCheckOnlyWake`'s real (throwaway) single-frame
capture.

Charles asked for one further change: **use the exact same path through the state
machine as the periodic timer wake, not just the same entry point.**

## Final design

`msg_data == 0` is still `prvLight()`'s sentinel (unambiguous - `prvCapture()`'s own
validation already rejects `0` for a real `capture` command, so it can never collide
with a genuine user request), but `handleEventForInit()` now treats it as "run a real,
throwaway single-frame capture, exactly like `aeCheckOnlyWake`" rather than a shortcut.

### 1. New flag: `aeCheckCliTriggered` (`image_task.c`)

```c
static bool aeCheckCliTriggered = false;
```

Distinguishes a CLI-triggered `aeCheckOnlyWake` capture from a timer-triggered one,
since both now share the exact same capture mechanics but must keep different
consequences (see below).

### 2. `handleEventForInit()`'s `APP_MSG_IMAGETASK_STARTCAPTURE` case

`requested_captures == 0` now means "translate this into a single throwaway capture,
the same way `aeCheckOnlyWake` already works for the timer wake":

```c
if (requested_captures == 0) {
    aeCheckCliTriggered = true;
    requested_captures = 1;
}

if (!cameraSystemEnabled) {
    if (aeCheckCliTriggered) {
        // No capture will run to report completion - signal directly.
        aeCheckCliTriggered = false;
        xSemaphoreGive(xLightCheckDoneSemaphore);
    }
    else {
        // ... existing "Can't capture" telemetry, unchanged
    }
}
else if (/* existing MIN/MAX_IMAGE_CAPTURES / MIN/MAX_IMAGE_INTERVAL range check */) {
    if (aeCheckCliTriggered) {
        aeCheckCliTriggered = false;
        xSemaphoreGive(xLightCheckDoneSemaphore);
    }
}
else {
    if (aeCheckCliTriggered) {
        aeCheckOnlyWake = true;
    }
    // ... existing real-capture setup (configure_image_sensor(CAMERA_CONFIG_RUN)
    // etc.), completely unchanged - this now runs for a CLI-triggered light
    // check exactly as it already did for a timer-triggered one.
}
```

`aeCheckOnlyWake` is only set here, inside the success branch - not eagerly at the
top - so an early rejection (camera disabled, bad params) can't leave it lingering
`true` with no capture ever running to clear it.

### 3. The post-capture light-check block

Broadened to run whenever `aeCheckOnlyWake` is set too, not just `aeCheckRequired`
(so a CLI-triggered check still gets a reading even if `aeCheckRequired` itself
happens to be false - e.g. neither the AE flash nor auto camera-switch is enabled),
and split by `aeCheckCliTriggered` to keep the CLI path's existing "forced,
side-effect-free" contract even though the capture mechanics are now shared:

```c
if ((aeCheckRequired || aeCheckOnlyWake) && (g_cur_jpegenc_frame == g_captures_to_take)) {
    if (aeCheckCliTriggered) {
        // On-demand 'light': always force a reading, but stay passive - no
        // flash arming, no camera-switch check. Only the capture mechanics
        // are shared with the timer path, not those consequences.
        lightSensor_takeReadingForced();
    }
    else {
        lightSensor_takeReading();
        if (ledFlashGetFlashMode() == FLASH_MODE_AE) {
            ledFlash_setActive(lightSensor_isDark());
        }
        cameraSwitchScheduled = cameraSwitch_autoSwitchCheck();
    }
}
```

**Deliberate scope limit:** the *mechanics* (real throwaway capture, state machine,
skip-NN, skip-file-save, flash-suppression) are now fully shared between the CLI and
timer triggers. The *consequences* (flash arming, automatic camera switching) are
still CLI-triggered-only skipped, preserving the existing "light stays a passive
diagnostic" decision. Making those consequences shared too - i.e. running `light`
from the console could arm the flash or trigger an automatic camera-slot switch and
reboot - would be a materially bigger behavioural change than "use the same path"
was asking for, so this was not done without checking first.

### 4. Completion point: `handleEventForNNProcessing()`'s `APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE`

This is the single point where a capture's *entire* lifecycle (capture → NN-skip →
file-write-skip, even for a throwaway frame - the FAT task still replies
`DISK_WRITE_COMPLETE` for a null-filename "skip the write" request) is truly finished
and the state machine returns to `APP_IMAGE_TASK_STATE_INIT`. `aeCheckOnlyWake` is
read at several points spread across this whole span (skip-NN, skip-ae_process,
skip-file-save), so it cannot be cleared right after the light-check block itself -
doing so would break this *same* capture's later skip-file-save decision. Clearing it
here instead, once the whole lifecycle is over, was necessary because `light` can now
be invoked repeatedly without the device ever sleeping in between (unlike the timer
path, which almost always leads straight back to DPD, where `image_sleepNow()`
already cleared it) - without this, a CLI-triggered check's `aeCheckOnlyWake` could
leak into a genuine `capture` command run moments later in the same session, wrongly
skipping its NN processing, file save, and flash.

```c
if (g_cur_jpegenc_frame == g_captures_to_take) {
    captureSequenceComplete(img_recv_msg.msg_parameter);
    image_task_state = APP_IMAGE_TASK_STATE_INIT;

    aeCheckOnlyWake = false;
    if (aeCheckCliTriggered) {
        aeCheckCliTriggered = false;
        xSemaphoreGive(xLightCheckDoneSemaphore);
    }
}
```

### 5. `prvLight()` (`CLI-commands.c`) - unchanged from the first version

Still a blocking send-and-wait on `xLightCheckDoneSemaphore` (5000ms timeout), still
prints the same `Light level: N (DARK|BRIGHT)` line `_Tools/ae_stream.py` parses -
none of this needed to change; only what happens on the image task side of the
message did.

## Known side effect, accepted

A CLI-triggered light check now goes through the *entire* real capture pipeline
(`configure_image_sensor(CAMERA_CONFIG_RUN)`, sensor datapath init, JPEG-encoder
skip, file-write round-trip to the FAT task) rather than a lightweight direct
register read. It's slower and does more work than strictly necessary for "read some
registers," and it also emits the same `captureSequenceComplete()` BLE telemetry
("Captured 1 images...") that the timer-triggered throwaway capture already emits
today - slightly odd-looking for an on-demand diagnostic command, but this is
existing, unchanged behaviour for the *timer* path, not something new introduced
here, and now applies equally rather than being an inconsistency between the two
triggers.

## Edge case, accepted rather than fixed

If `light` is run while the image task is mid-real-capture (state isn't
`APP_IMAGE_TASK_STATE_INIT`), the `msg_data == 0` message lands on whichever state
handler is currently active - none of which recognize it - and is silently dropped as
an unexpected event, the same way a `capture` command sent while already capturing
would be. `prvLight()`'s semaphore wait then simply times out after 5s and reports
"Light check timed out" - safe and bounded, just not instant, in this rare case.
