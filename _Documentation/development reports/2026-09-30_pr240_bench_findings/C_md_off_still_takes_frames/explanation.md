# Turning motion detection off leaves the HM0360 taking frames, which wastes power

#### File: explanation.md
#### Author: Claude (Fable 5.1), run with Victor Anton
#### 30 September 2026

When motion detection is turned off, the HM0360 keeps taking frames while the camera sleeps. Each frame uses about
9.5 mA for 35 ms (Charles's measurement,
[`power_investigation.md`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md#L67)).

- op 17 = 0 (sensitivity off): motion no longer wakes the camera, but the sensor still takes a frame every second.
  That costs about 330 uA.
- op 11 = 0 (interval off): the sensor takes a frame about every 2 seconds instead. That costs about 175 uA.

Bench, 30 Sep, WILD-7VQI, production HM0360 image: with op 17 = 0, waving at the camera never woke it in 6 sleeps; with
op 17 = 1 it woke on motion after 9 seconds. Before each sleep the firmware logs the sensor settings, and they show the
frames still on: `sleepTime=1000` with op 17 = 0, `sleepCount = 0xffff` with op 11 = 0
([log](logs/md_off_console.txt)).

The code keeps the sensor taking frames on purpose. Its comment says sleep mode draws more, 700 uA against 270 uA
([`hm0360_md.c:243-252`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_md/hm0360_md.c#L243-L252)),
but Charles measured sleep mode at no more than the other modes
([`power_investigation.md:97-103`](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/ecb54946289dc1c88440a2c1e36b2491d07b4bff/EPII_CM55M_APP_S/app/ww_projects/ww500_minimal/doc/power_investigation.md#L97-L103)).

**Fix:** when motion detection is off, put the HM0360 in sleep mode (mode 0) before the camera sleeps, remove the old
comment, and check the lower current with the PPK2.
