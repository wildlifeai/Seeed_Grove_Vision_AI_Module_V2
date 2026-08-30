# Bench: mobile app to device message exchange on current dev

#### File: README.md
#### Author: Claude (Opus 5), reviewed by Victor Anton
#### 30 to 31 August 2026

**Status:** closed, 31 August 2026.

Bench session capturing the full BLE command exchange between the mobile app, the nRF52
relay and the Himax, against a device running current `dev`. Started from a reported
symptom: a camera set to monitor was recording no motion detections at all, and appeared
to be doing nothing except a light sensor check every 15 minutes.

## Outcome

**The reported symptom was correct behaviour, and there is no defect behind it.**

Motion detection is inhibited when `OP_PARAMETER_MD_INTERVAL` (op11) is 0. On the device
under test it was 0 because a previous deployment had been **ended** on it. Ending a
deployment sets op11 to 0 deliberately, via `quiesceDevice({ isEndDeployment: true })` in
the app. Starting a deployment sets it back to 1000. Both directions were observed on
hardware in this session.

The 15 minute light check is `OP_PARAMETER_AE_CHECK_INTERVAL` (op24) at its default of 15,
which is what a device with no other work to do should be doing.

### The app's command sequence is correct

A deployment with capture method `activity` produced exactly the documented sequence, with
the bulk-fetch skip optimisation suppressing the writes that were already correct:

```
[DeployConfig] Configuring capture method: 'activity'
[DeployConfig] Motion detection mode - interval 1000ms, timeout 30s
[DeployConfig] Skipping parameter 17 (already 1)
[DeployConfig] Setting parameter 11 to 1000
[DeployConfig] Skipping parameter 7 (already 0)
[DeployConfig] Skipping parameter 8 (already 1000)
[DeployConfig] Skipping parameter 10 (already 1)
[DeployConfig] Deployment configuration complete (Atomic)
```

and it landed through all three processors and persisted across sleep:

```
app   | Written AI setop 11 1000 to the device WILD-CNKW
nrf   | Sending 'setop 11 1000' to AI processor (4 ctrl bytes, 13 payload, 2 CRC)
himax | Set OpParam 11 = 1000
himax | Sleep 6 0 0 1 6 2 500 0 1000 5 1 1000 100 ...
```

Motion detection then fired repeatedly, waking the device and capturing images. Evidence in
[`logs/deployment_start.log`](logs/deployment_start.log) and
[`logs/motion_detection.log`](logs/motion_detection.log).

### Ending a deployment disarms it, by design

```
[EndDeployment] Pre-fetched bulk ops for end-deployment
[DeploymentService] Ending deployment: a8e5a82d-...
[DeviceSettings] Quiescing device (EndDeployment: true)...
[DeviceSettings] 2. Clearing Motion & Timelapse intervals...
Written AI setop 11 0 to the device WILD-CNKW
[DeviceSettings] Device quiesced successfully.
```

Full exchange in [`logs/deployment_end.log`](logs/deployment_end.log).

**Operational consequence worth knowing:** a device that has had a deployment ended looks
identical to a broken one. It wakes on the AE timer, captures nothing, and reports no
motion. The only way to tell from the outside is to read op11. This is the second time that
has cost someone an afternoon, so it is worth checking op11 first whenever a camera is
reported as "not detecting anything".

### One defect found

`TEST_MODE_BITS` (op18) is left set to 2 after a deployment completes. `setop 18 2` is sent
**after** `[DeployConfig] Deployment configuration complete`, and nothing clears it, so bit
1 (`TEST_BIT_SAVE_BMP`) stays on and every subsequent capture writes a BMP alongside the
JPG. Observed file write time rose from 48ms to 112ms, and every captured filename in the
later part of the session ends `.BMP`. Filed as an issue, see Open items.

### Findings that did not survive checking

Recorded because each one looked real at the time and someone will see the same evidence
again:

| Observation | Why it was not a defect |
|---|---|
| Motion detection triggering every 6.7s, extrapolated to 523 images/hour | Measured over a 7.4 minute window that happened to contain a person moving. The device had been monitoring for over 12 hours, giving about 14 images/hour. Rate also fell to zero for 3 minutes when the room went quiet |
| Battery at 3112mV, 3% | The device was USB powered. That is the rail voltage, not a battery state |
| `Error opening '0:/RPV3_EX.BIN': 4` on every wake | `cis_file_process()` runs at every sensor init and logs this whenever no camera registers have been staged, which is the default state. Noisy, not broken |
| Operational parameters appearing to reset after an XMODEM flash | Partly explained by the app's `[ResetDefaults]` writing op0 and op26 on connect. Not fully chased; see Open items |
| `AE light check ... mean AE = 72 ... threshold = 65 -> DARK` reading as an inverted comparison | Three way comparison with a hysteresis band: dark below 65, bright above 65 + `AE_HYSTERESIS` (12), hold the previous decision between. Confirmed live when the value crossed 77 and flipped with a `(changed)` suffix |

## Bench setup

Two USB serial ports and one adb connection, all merged onto one timeline by
[`_Tools/tri_log.py`](../../../_Tools/tri_log.py):

| Leg | Port | Baud | Carries |
|---|---|---|---|
| `himax` | COM6 | 921600 | FreeRTOS CLI, `Set OpParam`, `Sleep`, MD and AE lines |
| `nrf` | COM5 | 115200 | `BLE in`/`BLE out`, I2C relay to the Himax, LoRa |
| `app` | adb logcat | | `[DeployConfig]`, `[DeviceSettings]`, `Written AI ...` |

Note the port assignment: COM5 is the nRF and COM6 is the Himax, which is the opposite of
what some older notes assume. Identify them by baud rate and content rather than number.

Firmware built from `da79fcb9` and flashed over XMODEM as `R6830F23.IMG`. The device had
been running a build from 24 July, 62 commits behind.

[`_Tools/decode_ops.py`](../../../_Tools/decode_ops.py) decodes a `Sleep` or `OpParams`
dump into named parameters with defaults and change markers. Both of those messages are a
bare positional list of every parameter from index 0, so counting by eye silently shifts
every reading, which is worth avoiding.

### Two traps in the tooling

Both cost real capture time and are fixed in the committed version:

- **`adb logcat --pid=N` does not exit when process N dies.** It goes quiet forever. A
  reader that waits for the pipe to close therefore blocks, and an app restart silently
  kills the app leg for the rest of the session. `tri_log.py` now polls for the pid
  changing and terminates the reader itself.
- **The Himax console sleeps about 1 second after boot chatter stops and does not wake on
  UART.** A byte has to go in the moment `Starting CLI Task` appears, before any other
  waiting, or every subsequent command lands on a sleeping device.

## Open items

| Issue | Description | State |
|---|---|---|
| [ww-mobile-app#249] | Deployment leaves `TEST_MODE_BITS` set to 2, so every capture writes a BMP as well as a JPG | open, filed from this thread |

Filed in `ww-mobile-app` rather than here: the offending `setop 18 2` is sent by the app
after its deployment configuration completes, so the fix belongs there. The evidence and
this thread live in the firmware repo because that is where the bench session ran.

Not filed, recorded here only:

- Leftover debug logging at `cis_file.c:88`, commented `// DEBUG - find out where we are!`,
  prints `CWD is '...'` at every sensor init. 63 occurrences in one bench session. Cosmetic.
- Firmware built inside a git worktree bakes in `GIT_BRANCH="nogit"` and
  `GIT_COMMIT="nogit"`, because the worktree's `.git` file points at a Windows path WSL
  cannot resolve. The banner then reads `Git branch: 'nogit' nogit-dirty` and cannot
  identify the commit. Build from a normal clone when the banner matters.
- The apparent operational parameter reset after an XMODEM flash was not fully explained.
  Worth a look if it recurs, since a firmware update that discards deployment configuration
  would matter in the field.

## Related threads

- [PR #140 review](../2026-08-10_pr140-rp3-image-quality-review/README.md), for the AE and
  exposure background
- [Ready-now fixes, batch 1](../2026-08-18_ready-now-fixes-batch1/README.md), for the
  build and flash tooling used here

[ww-mobile-app#249]: https://github.com/wildlifeai/ww-mobile-app/issues/249
