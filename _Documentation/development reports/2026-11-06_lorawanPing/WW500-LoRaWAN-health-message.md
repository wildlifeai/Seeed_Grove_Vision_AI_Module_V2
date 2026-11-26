# WW500 LoRaWAN health message

29 October 2026

## Decisions

The WW500 will send a 7-byte health uplink on FPort 4 when something changes and once a day. The ping shrinks to 9 bytes so it fits at DR2.

- **What:** a fixed-layout message carrying self-test bits, SD free %, temperature, last fault reason, and reset and AI-failure counts.
- **When:** after the join on every boot, when a self-test bit changes (at most once an hour), and every 24 h.
- **Ping:** drops its 3-byte firmware-version field. The deployment record already holds both firmware versions.
- **Website:** the Realtime page shows health, firmware and fault reasons. The backend sends the existing "camera silent" alert and a new "camera fault" alert.
- **Mobile app:** a "Remote check-in" card on the camera's Monitoring Summary, and an amber or red ring on its map marker.

This answers the open questions in [ww-hardware #72](https://github.com/wildlifeai/ww-hardware/issues/72) and [Seeed #262](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/262). Downlinks ([Seeed #263](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/263)) are out of scope.

## The ping does not fit at DR2

Today's 12-byte ping cannot go out at DR2, the data rate the camera uses after every join. This is from reading the code and still needs a bench check.

- With uplink dwell time on, AS923 allows 11 bytes at DR2 ([RegionAS923.h:164](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/MokoTechSDK/src/lora/mac/region/RegionAS923.h#L164), [:301](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/MokoTechSDK/src/lora/mac/region/RegionAS923.h#L301)).
- Each ping also queues a 1-byte DeviceTimeReq MAC command, which counts against the same limit ([LoRaMac.c:1976](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/MokoTechSDK/src/lora/mac/LoRaMac.c#L1976)). That leaves 10 bytes.
- The ping is 12 bytes ([app_ww.c:231-288](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/Workspace/WildlifeWatcher_1/app_ww.c#L231-L288)). When a payload does not fit, LmHandler sends an empty unconfirmed frame instead ([LmHandler.c:539-545](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/MokoTechSDK/src/apps/lorawan/common/LmHandler/LmHandler.c#L539-L545)), and the state machine only logs "TODO - what now?" ([stateMachine.c:736](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/Workspace/WildlifeWatcher_1/stateMachine.c#L736)).
- So the server gets a frame with no content until adaptive data rate (ADR) raises the camera to DR3, which allows 53 bytes.

**New ping, FPort 2, 9 bytes:** `50 04 st NN_lo NN_hi 52 period 13 batt`. The `1c maj min` firmware field is dropped. The backend decoder reads fields by type byte, so it needs no change. The NN-positive frame (8 bytes) stays as it is.

## Health uplink on FPort 4

The health message is 7 bytes in a fixed layout on FPort 4, which no firmware or backend code uses yet. A new layout later takes a new FPort, so there is no version byte. Everything except SD free space is already on the nRF.

| Byte | Field | Encoding | Source |
| --- | --- | --- | --- |
| 0-1 | Self-test bits | u16 LE, same bit numbers as BLE `selftest` | [selfTest.h](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/Workspace/WildlifeWatcher_1/selfTest.h) |
| 2 | SD card free | 0-100 %, 0xFF = unknown | New: the Himax reports it (Seeed change) |
| 3 | Temperature | s8, °C, nRF die | BLE `temp` ([ble_commands.c](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/Workspace/WildlifeWatcher_1/ble_commands.c#L164)) |
| 4 | Last fault reason | `fault_reason_t`: 0 none, 1 test, 2 app error, 3 SDK assert, 4 SoftDevice assert, 5 hard fault, 6 watchdog | [fault.h:35-43](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/Workspace/WildlifeWatcher_1/fault.h#L35-L43) |
| 5 | Unexpected resets | count since the last health message, saturates at 255 | fault record and reset reason |
| 6 | AI processor failures | count since the last health message, saturates at 255 | [aiMonitor.h:31-37](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/Workspace/WildlifeWatcher_1/aiMonitor.h#L31-L37) |

- **Self-test bits:** send only after the Himax's first `selftest` reply. Until then the AI bits read 0xff00, which the app already treats as "not known yet".
- **SD free:** the cheapest route is a new op parameter in the Himax `Sleep` message. With the two new settings it carries op 0-38 on every sleep, so SD free would be op 39. Send 0xFF until that firmware is out.
- **Fault code and source line:** not sent. An engineer on site reads them over BLE with `fault`.
- **Firmware versions:** not sent. The deployment record already holds both (`ble_firmware_id` and `himax_firmware_id`, set at deployment start).

## When the camera sends it

The camera sends health when there is news, plus once a day so the website and app never show health older than 24 h.

- **After every join**, on a cold boot or after an unexpected reset. It goes after the first ping, once the Himax's first `selftest` reply is in.
- **When any self-test bit changes**, at most once an hour. A second change within the hour goes in the next message.
- **Every 24 h** after the last health message.

Sending rules:

- **Confirmed**, like the ping. The fault reason and the two counts reset only once the network acknowledges the message. Until then, each new message carries the running totals.
- **Queued, never dropped.** Today the uplink path returns silently when the MAC is busy ([app_ww.c:897-899](https://github.com/wildlifeai/ww-hardware/blob/5875084ffc62bcb5de2c7166f4b8ce57a01259f4/MokoTech/Workspace/WildlifeWatcher_1/app_ww.c#L897-L899)), which would lose a health message.
- **Airtime:** about 0.37 s at DR2 (SF10, 125 kHz). The 1% duty-cycle limit the firmware enforces then holds the next uplink about 37 s.

## How it is shown

The website and the mobile app share one set of status rules, so a camera reads the same everywhere. A red state also sends an alert.

| State | When | Colour |
| --- | --- | --- |
| Silent | No uplink of any kind for 3 ping periods (the period comes from the ping) | Red |
| Fault | Latest health has bit 1 (AI processor not responding), 8 (main camera), 9 (HM0360) or 11 (SD card) set | Red |
| Warning | Any other self-test bit, a fault reason, an unexpected reset, battery under 20 %, or SD free 10 % or less | Amber |
| Health unknown | No health message for 26 h | Grey |
| OK | None of the above | Green |

**Website** ([Realtime page](https://github.com/wildlifeai/ww-website/blob/cd2c6ed7a888d2d3797b1da58c0903f44b1d9233/frontend/src/pages/FieldPage.tsx)):

- New Health and Firmware columns, firmware from the deployment record. Fault reasons join the "needs attention" list.
- Silent follows each camera's ping period instead of today's fixed 24 h and 72 h.
- Battery comes from the latest ping and health from the latest health message. Today [deployment_latest_lorawan](https://github.com/wildlifeai/ww-backend/blob/a04aff4929468ccdbffb364051a93a61a59e073a/supabase/schemas/public/views/83_deployment_latest_lorawan.sql) takes the newest message of any kind, so a health uplink would blank the battery column.
- Alerts by web and email, per each user's notification rules: "camera silent" ([defined](https://github.com/wildlifeai/ww-backend/blob/a04aff4929468ccdbffb364051a93a61a59e073a/supabase/schemas/public/tables/54_notifications.sql), never sent today) and a new "camera fault". A fault alert fires when a red or amber state first appears.

**Mobile app:**

- A "Remote check-in" card on the camera's [Monitoring Summary](https://github.com/wildlifeai/ww-mobile-app/blob/a5aa6fc07b37eaf581a14b7e5185b72da9a124a8/src/screens/Devices/DeviceMonitoringSummaryScreen.tsx). Fault names use the app's existing [self-test decoder](https://github.com/wildlifeai/ww-mobile-app/blob/a5aa6fc07b37eaf581a14b7e5185b72da9a124a8/src/utils/deviceSelfTest.ts), so they read the same as on site over BLE.
- An amber or red ring on the camera's [map marker](https://github.com/wildlifeai/ww-mobile-app/blob/a5aa6fc07b37eaf581a14b7e5185b72da9a124a8/src/features/maps/components/DeploymentMarker.tsx).
- Read live from Supabase when online. It is not synced into the phone's database, and it is hidden offline.

```
Remote check-in            2 h ago
-----------------------------------
Battery        78%
SD card        41% free
Firmware       0.30.58
Health         OK

(when faulted)
Health         HM0360 camera failed
               2 unexpected resets today
```

## Work by repo

The firmware can ship first. The backend already stores every uplink on an unknown FPort as raw JSON, so no health message is lost before the decoder lands.

| Repo | Work | Waits on |
| --- | --- | --- |
| ww-hardware (nRF) | Shrink the ping to 9 bytes. Add the FPort 4 health uplink with its triggers. Queue uplinks when the MAC is busy. Keep the fault counts until acknowledged. | Nothing |
| Seeed (Himax) | Report SD free % as a new op parameter in the `Sleep` message, and add it to the shared op list. | Nothing. The nRF sends 0xFF until then |
| ww-backend | Decode FPort 4 in [lorawan-ingest](https://github.com/wildlifeai/ww-backend/blob/a04aff4929468ccdbffb364051a93a61a59e073a/supabase/functions/lorawan-ingest/parse.ts). Add typed health columns. Rework `deployment_latest_lorawan`. Add the scheduled silent check and the "camera fault" alert type. Fix `sd_card_used_capacity`, which is always null and documented as MB while the website reads it as %. These are database changes, so Victor approves them before any issue is filed. | This layout agreed |
| ww-website | Realtime columns and status rules, ping-based silence, alert settings, health fields in the public API telemetry. Correct the LoRaWAN guide, which promises a ready-made decoder and a token per organisation, neither of which exists. | ww-backend |
| ww-mobile-app | Remote check-in card, map marker ring, live read of the latest check-in. | ww-backend |

## Open questions for the ww-hardware team

- [ ] On 16 Oct the answer was "don't change the ping". Is dropping its firmware field acceptable, given the ping cannot be sent at DR2?
- [ ] Bench check: does a 12-byte ping at DR2 go out as an empty frame? Does the state machine then stay in REPORTING with no timer to leave it? Both are read from code, not seen.
- [ ] Is the nRF die temperature close enough to the enclosure temperature to be worth byte 3?
- [ ] Which resets count as unexpected: watchdog and fault only, or brown-outs too? Self-test bit 4 (brown-out) is never set today.
