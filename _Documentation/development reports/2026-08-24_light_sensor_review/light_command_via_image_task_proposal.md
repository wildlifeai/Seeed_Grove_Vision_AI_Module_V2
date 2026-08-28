# Proposal: route the 'light' CLI command through the image task, not the CLI task

**Status: proposed, NOT implemented. Awaiting Charles's review.**

## Problem

The `light` CLI command (`prvLight()`, `CLI-commands.c`) currently calls
`lightSensor_takeReadingForced()` directly from whichever task processes console
commands (the "CLI" task). This is inconsistent with every other light check in the
firmware: a real capture's post-capture check, and the periodic
`OP_PARAMETER_AE_CHECK_INTERVAL` timer wake, both go through the **image task**'s
message queue and state machine (`APP_MSG_IMAGETASK_STARTCAPTURE` →
`handleEventForInit()`).

Worse, it's a real concurrency hazard, not just an inconsistency: `light` calls the
same `hm0360_md.c` I2C functions (which swap the I2C slave ID to address the HM0360,
then swap it back) that a real image-task-driven capture also calls. `hm0360_md.c`'s
`saveMainCameraConfig()` has a standing comment: `// TODO do we need some critical
section or semaphore code here, in case we change task mid-stream?` - i.e. this was
already a known, unaddressed gap. If someone runs `light` from the console at the same
moment the image task is mid-capture, the two tasks could race on the shared I2C
slave-ID register.

Routing `light` through the image task's own queue serializes all HM0360 I2C access
through one task, closing that gap as a side effect of fixing the consistency issue
Charles asked about.

## Proposed design

Charles's own suggestion: reuse the existing `APP_MSG_IMAGETASK_STARTCAPTURE` message,
with a sentinel `msg_data == 0` meaning "light check only, no real capture" - instead
of introducing a whole new message event type.

### 1. New semaphore (`image_task.c`)

Created in `image_createTask()`, alongside the existing `xJpegBufferSemaphore`
(same file, same function, same `xSemaphoreCreateBinary()` pattern) - but *not* given
immediately after creation, unlike `xJpegBufferSemaphore`: it must start "empty" so
the first `xSemaphoreTake()` in `prvLight()` correctly blocks until the image task
signals completion.

```c
static SemaphoreHandle_t xLightCheckDoneSemaphore = NULL;
...
xLightCheckDoneSemaphore = xSemaphoreCreateBinary();
if (xLightCheckDoneSemaphore == NULL) {
    xprintf("Failed to create xLightCheckDoneSemaphore\n");
    configASSERT(0);
}
// Deliberately NOT given here - starts empty, unlike xJpegBufferSemaphore.
```

Needs an `extern SemaphoreHandle_t xLightCheckDoneSemaphore;` in `CLI-commands.c`,
matching how `xImageTaskQueue` is already externed there.

### 2. `handleEventForInit()`'s `APP_MSG_IMAGETASK_STARTCAPTURE` case (`image_task.c`)

New branch, checked *before* the existing `MIN_IMAGE_CAPTURES`/`MAX_IMAGE_CAPTURES`
range validation (so `0` doesn't fall into "Invalid parameter values"):

```c
if (requested_captures == 0) {
    // Light-check only, no real capture - see prvLight() in CLI-commands.c.
    // Deliberately just the reading: no ledFlash_setActive(), no
    // cameraSwitch_autoSwitchCheck() - 'light' stays a passive diagnostic
    // with no side effects on flash arming or camera switching, exactly as
    // already decided when it was lightSensor_takeReadingForced() called
    // directly from the CLI task. Only *where* it runs changes here.
    lightSensor_takeReadingForced();
    xSemaphoreGive(xLightCheckDoneSemaphore);
}
else if ((requested_captures < MIN_IMAGE_CAPTURES) || (requested_captures > MAX_IMAGE_CAPTURES) ||
    (requested_period < MIN_IMAGE_INTERVAL) || (requested_period > MAX_IMAGE_INTERVAL))  {
    // ... existing validation, unchanged
}
else {
    // ... existing real-capture path, unchanged
}
```

No state-machine transition needed - `image_task_state` stays
`APP_IMAGE_TASK_STATE_INIT` throughout, since no real capture happens.

### 3. `prvLight()` rewrite (`CLI-commands.c`) - blocking send-and-wait

```c
static BaseType_t prvLight(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
    APP_MSG_T send_msg;
    (void)pcCommandString;
    configASSERT(pcWriteBuffer);

    send_msg.msg_data = 0;   // 0 = light-check only (see handleEventForInit())
    send_msg.msg_parameter = 0;
    send_msg.msg_event = APP_MSG_IMAGETASK_STARTCAPTURE;

    if (xQueueSend(xImageTaskQueue, (void *)&send_msg, __QueueSendTicksToWait) != pdTRUE) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Failed to queue light check");
        return pdFALSE;
    }
    if (xSemaphoreTake(xLightCheckDoneSemaphore, pdMS_TO_TICKS(5000)) != pdTRUE) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Light check timed out");
        return pdFALSE;
    }

    cli_append(&pcWriteBuffer, &xWriteBufferLen, "Light level: %d (%s)",
            lightSensor_getReading(), lightSensor_isDark() ? "DARK" : "BRIGHT");
    return pdFALSE;
}
```

5000ms timeout gives comfortable headroom over `sampleAeStats()`'s ~2.4s worst-case
duration, while still bounding the CLI task's wait if something goes wrong.

Still prints the exact same `Light level: N (DARK|BRIGHT)` line - `_Tools/ae_stream.py`
(`LIGHT_RE`) keeps working unchanged; this refactor is invisible to it.

## Edge case, accepted rather than fixed

If `light` is run while the image task is mid-real-capture (state isn't
`APP_IMAGE_TASK_STATE_INIT`), the `msg_data == 0` message lands on whichever state
handler is currently active - none of which recognize it - and is silently dropped as
an unexpected event, the same way a `capture` command sent while already capturing
would be. `prvLight()`'s semaphore wait then simply times out after 5s and reports
"Light check timed out" - safe and bounded, just not instant, in this rare case. Not
proposing to fix this now; flagging it as a known, acceptable limitation of this
design.

## Why this wasn't implemented immediately

Charles asked for this to be written up for review rather than implemented straight
away (reviewing Monday). The design itself was talked through and looks sound, but
touches three things across two files (a new semaphore, a new branch in the image
task's core capture-request handler, and a behavioural change to `prvLight()` from
synchronous-direct-call to blocking-message-send) - enough surface area to want an
explicit go-ahead first, consistent with how every other non-trivial change this
session was handled.
