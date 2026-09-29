# The sleep timer runs about 5 % fast, so every timelapse interval is short

#### File: explanation.md
#### Author: Claude (Fable 5.1), run with Victor Anton
#### 30 September 2026

The AI processor's clock and its sleep timer both run from the chip's internal 32 kHz oscillator (the board has no
32 kHz crystal), and that oscillator runs about 5 % fast. So a timelapse set to 90 seconds really sleeps about 85, and
the AI processor's clock gains about 3 minutes an hour until the BLE processor next corrects it.

Bench, 30 Sep, WILD-7VQI, `dev` ecb54946 (HM0360 build), motion detection off (op 11 = 0) so every wake was a timer
wake. Time measured on the PC, from `>>> Entering DPD` to the next boot banner:

| op 7 | Wakes | Real sleep, seconds |
|---|---|---|
| 30 s | 4 | 28.15, 27.73, 27.75, 27.75 |
| 90 s | 3 | 85.41, 84.73, 84.72 |

Adding 60 s to op 7 added 57.0 s of real sleep, so the timer runs about 5 % fast. The alarm is set in whole seconds,
so each sleep is only good to about half a second: somewhere between 4 and 6 %. Charles measured the same thing in
`ww500_minimal` on 21 Sep, 4.05 % and 3.96 % at the two clock speeds
([`power_investigation.md:888-890`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md#L888-L890),
[`:995`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md#L995)).

Suggested fix: try trimming the oscillator with `hx_drv_scu_set_RC32K1K_trim()`, which nothing calls today, then repeat
the run above. If trimming is not enough, scale op 7 by a measured correction before setting the alarm.

## Evidence

| What | Where |
|---|---|
| The seven timed sleeps | [`logs/timer_results.csv`](logs/timer_results.csv) |
| The AI console for the run: every wake is `RTC Timer`, with the alarm times | [`logs/timer_console.txt`](logs/timer_console.txt) |

Lines starting `#####` are the bench script's notes: the PC time and what it typed. One more sleep was timed and left
out: the one across the change from 30 to 90 s, which included typing.
