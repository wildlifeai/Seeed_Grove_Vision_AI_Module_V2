# Mobile app bench: three-way BLE logs behind #202, #203 and #204

#### File: README.md
#### Author: Claude (Opus 5), reviewed by Victor Anton
#### 2 September 2026, logs filtered 3 September

Evidence captured while wiring the mobile app to `AI light`, kept because three firmware issues
rest on it and the logs are not reproducible without the same hardware and app build.

**Device:** `ae_review` at `4dc43587`, HM0360 and RP3 slots.
**App:** wildlifeai/ww-mobile-app `0.0.64`, PR #254, 1 to 2 September 2026.
**Decision line wording:** these captures predate e8b7feb5. Here the line reads
`AE light check: mean AE = 24 ... -> DARK (flash wanted)`; the firmware now prints the shorter
gain-based form, `AE light check: AGain = 2, conv=Y -> BRIGHT (change)`. Both are documented in
`ble_commands.md`.

## Why three-way

The app talks to the nRF52 over BLE, the nRF52 relays to the Himax over I2C. Each leg is only
visible from its own console, so no single log shows a command being sent, relayed and acted on.
Every finding here needed all three at once: each console on its own looks correct.

```
[06:14.266] app   | Written AI light to the device WILD-CNKW
[06:14.469] nrf   | Sending 'light' to AI processor (4 ctrl bytes, ...)
[06:14.578] himax | AE light check: mean AE = 71 ... -> DARK (flash wanted)
```

## The files

The two logs are filtered to the lines that carry the story (hex dumps, I2C state chatter, boot
banners and the app's sync noise removed; about a quarter of each raw capture remains). Every
line the issues quote is in them.

| File | What it shows |
|---|---|
| [`logs/ai_light_and_camera_disabled.log`](logs/ai_light_and_camera_disabled.log) | `AI light` end to end, the truncated decision line at all three stages, the `AI disable` test that produced both silent-drop paths, and one wake where the sensor failed to initialise (`Disabling camera functions because there is no camera!`, reported as self-test bit 8) |
| [`logs/connect_flow_and_camera_switch.log`](logs/connect_flow_and_camera_switch.log) | What connecting actually sends (16 commands, including the deliberate pre- and post-wake `selftest` pair), and camera slot switches in both directions |
| [`logs/ae_light_check_decisions.txt`](logs/ae_light_check_decisions.txt) | Every unique `AE light check` line captured, deduplicated. The dataset behind #204 |
| [`bench_log.py`](bench_log.py) | The logger that produced them |

## What each issue draws from these

**[#202](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/202), a dropped
`light` request is never reported.** Two distinct paths, both in
`ai_light_and_camera_disabled.log`. The known one prints `Can't capture - camera system not
enabled`; the new one prints `IMAGE task unhandled event 'Image Event Start Capture' in
'Capturing'` with the camera **enabled**, and never reaches that branch at all.

**[#203](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/203), the decision
line is truncated.** The same message at each stage in the same log: 150-byte payload at the Himax,
151 bytes out of the nRF, and the app receiving `-> DARK (flash wante`. The e8b7feb5 wording fits
under the clip, so this no longer shows; the clip itself and the pointer lifetime remain.

**[#204](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/204), the op23
decision.** 17 unique decisions in `ae_light_check_decisions.txt`. `gain railed` classified all 17
correctly; mean AE could not separate the classes, reaching 74 for DARK while BRIGHT started at 70.
One indoor scene, so this corroborates the 303-frame time-lapse analysis rather than replacing it.

## Running the logger

```bash
python bench_log.py                          # capture until Ctrl+C
python bench_log.py --duration 1800 -o run.log
python bench_log.py --himax COM6 --nrf COM5  # skip port detection
```

Needs `pyserial`, and `adb` on PATH for the app leg; close TeraTerm first, COM ports are exclusive.
It identifies the two consoles by the FTDI adapters' serial numbers and refuses to guess when it
cannot tell them apart. Timestamps are read-time, so a burst of buffered serial output shares one
stamp; ordering within one console is reliable. The docstring has the rest.
