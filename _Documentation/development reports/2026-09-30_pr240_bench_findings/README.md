# PR #240 open items on the bench

#### File: README.md
#### Author: Claude (Fable 5.1), run with Victor Anton
#### 30 September 2026

## Status

Open. Bench board WILD-7VQI, AI processor on `dev` ecb54946, BLE processor 0.30.51.

## Outcome

- **A. After an unread Sleep message and `dpd`, the AI processor freezes until RESET.** Reproduced 2 times out of 2; with
  the normal inactivity period the board sleeps as intended. See
  [`A_freeze_after_unread_sleep/explanation.md`](A_freeze_after_unread_sleep/explanation.md).
- **B. The sleep timer runs about 5 % fast**, so every timelapse interval is short (a 90 s timelapse sleeps about 85 s).
  See [`B_sleep_timer_fast/explanation.md`](B_sleep_timer_fast/explanation.md).
- **C. Turning motion detection off leaves the HM0360 taking frames, which wastes power**, whether by op 17 = 0 or
  op 11 = 0. Fix: put the sensor in mode 0 when motion detection is off. See
  [`C_md_off_still_takes_frames/explanation.md`](C_md_off_still_takes_frames/explanation.md).

## Open items

- A: [#243](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/243).
- B: [#245](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/245).
- C: [#247](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/247).
