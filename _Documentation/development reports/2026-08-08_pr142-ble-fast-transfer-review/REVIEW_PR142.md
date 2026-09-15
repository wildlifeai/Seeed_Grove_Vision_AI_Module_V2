# PR #142: BLE fast file transfer (Himax side)

> Offline copy of the pull-request description (https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/142). The canonical discussion lives on GitHub; this file exists so the summary is readable from a clone without internet access.

> **Note:** re-opened copy of #139 (dev was rolled back so this can be reviewed before merging).

## Summary

Makes BLE file receive on the WW500 **fast and reliable** — the Himax half of the fast-file-transfer work. Together with the nRF side (wildlifeai/ww-hardware#27) and the mobile app changes (already merged), this takes app → SD card transfers from **~0.24 KB/s to ~6–8 KB/s in bursts (~1.5 KB/s sustained on large files)**, verified end-to-end on hardware: a 512 KB file transfers, CRC-checks, and both firmware slots update over BLE from the cloud with no cable.

**Stacked on** #141 (`feat/camera-features-combined`) — this PR's diff shows only the transfer work. Merge #141 first; GitHub retargets this PR to `dev` automatically.

## Changes

- **SD write path reliability** (`fatfs_task.c`): `f_sync` cadence (every 16 writes) instead of per-packet; `f_sync` after `f_open(CREATE_ALWAYS)` so overwriting a large file doesn't fail on packet 1 (`ftx err 7`); transient write errors retried 3× with backoff; **short writes (volume full) fail fast** — no retry, since the file pointer already advanced; file-size cap raised to 10 MB for model files.
- **I2C slave deaf-window fix** (`if_task.c`): the receive buffer is re-armed **in the TX-done interrupt callback** rather than after a task-queue hop. At streaming rates the nRF writes the next packet immediately after reading our ACK; the task-hop delay clipped the frame header, the packet failed CRC silently, and the transfer stalled ("AI processor not responding").
- **Session-aware console logging**: per-packet hex dumps / state traces / event logs (~16 lines per packet at 921600 baud) are suppressed while a transfer session is active (`g_fileRxActive`) — they measurably throttled the packet loop. Everything logs normally outside transfers.
- **Missing-master window 1 s → 4 s**: survives brief BLE parameter renegotiations mid-transfer without tearing the session down.
- **Session inactivity hold**: DPD is held off while a transfer session is open (restored on close/abort).
- **Slot label robustness** (`camera_switch.c`): boot-slot variant labelling now logs every failure path and retries the flash write once — a silently lost label left the app's day/night camera switching stuck on "unknown" (seen once in the field after a dual-image update).
- **Review fixes**: white-balance clamps R/B before applying gains (uneven highlight tinting); `init_flash` always restores XIP when the SPI EEPROM was opened, even if `read_ID` fails (previously hard-faulted the next flash fetch).

## Testing

- 512 KB transfer completes with whole-file CRC verification, repeatedly, including the overwrite case.
- Full dual-image cloud update through the mobile app verified on a WW500 C02: download → BLE transfer → `firmware` flash with CRC pre-check → reset → verify, both variants.
- Known platform limit (documented in the mobile repo): Android holds the fast connection interval for only ~25 s per transfer, so large files run fast then settle at ~1.5 KB/s. Not device-side; iOS untested so far.

