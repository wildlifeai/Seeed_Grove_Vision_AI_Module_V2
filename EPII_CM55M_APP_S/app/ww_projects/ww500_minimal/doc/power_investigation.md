# ww500_minimal: awake-idle current and tickless idle

#### File: power_investigation.md
#### Author: Claude (Sonnet 5), from measurements by Charles Palmer
#### 21 September 2026

`ww500_minimal` was built to measure the HX6538 (AI processor) on its own. This document
records what was found about the current the processor draws while it is awake and idle, how
FreeRTOS tickless idle behaves, and the clock-gating experiment. Measurements are Charles's,
made with a multimeter on a WW500_C00 board that carries only the HX6538. The interpretation
is Claude's and is marked as inference where it is not measured.

How the work happened is in `_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/`.
How the app works is in [README.md](README.md).

## Short summary: what we have learnt

The aim was to find out where the HX6538's current goes on a board that carries little else, and whether a low-current
state that does not need a reboot exists. All figures are Charles's multimeter readings on 21 September 2026 (a WW500_C02
carrying only the HX6538, LEDs off, nothing on the I/O).

**The states, from lowest current to highest:**

| State | Current | Notes |
|---|---|---|
| **DPD** (deep power down, `dpd`), bare board | **10 uA** | The chip reboots on a timer or WAKE-pin wake. |
| DPD, PCA9574 fitted (23 September 2026) | **13.8 uA** | See "PCA9574 and the extra DPD current" below - cause not found. |
| **Power-down with retention** (`sleep 30 1`) | **1.5 mA** | RAM is kept. The app's banner appears 4 to 5 ms after the first bootloader line, against 13 to 16 ms after a DPD wake. |
| Power-down, no retention (`sleep 30 0`) | 1.5 mA | RAM lost. Behaves like DPD (banner 16 ms after the first bootloader line). Same current as with retention. |
| Awake and idle, best state found (24 MHz RC oscillator, PLL off, unused clock enables off, UART moved to RC, crystal off) | 4.8 mA | Console and DPD still work. Fully reversible. |
| Awake and idle, 24 MHz RC oscillator, PLL off | 8.5 mA | |
| Awake and idle, 24 MHz RC oscillator, PLL on | 10.1 mA | |
| **Awake and idle as it was (400 MHz, tickless idle sleeping)** | **17.3 to 17.9 mA** | |

**What we learnt, in short:**

1. **DPD is the low-power state.** 10 uA, and both wake sources work. Reboot cost after a wake: from the first bootloader line to a running
   CLI task takes 24 to 30 ms. The time before that line is not visible to the host.
2. **FreeRTOS tickless idle works but does not give low current.** The CPU does sleep in WFI (24 wake-ups a second at 400 MHz), yet the
   processor draws 17 to 18 mA because WFI stops only the CPU clock: the PLL, buses, SRAMs and every peripheral clock stay on.
3. **Awake-idle current can be cut by 72 %, to 4.8 mA, with run-time changes only**: run from the 24 MHz RC oscillator (-7.2 mA), switch the PLL
   off (-1.6), switch off the unused clock enables (-2.9), and move the UART to the RC oscillator so the 24 MHz crystal can go off (-0.8).
   Going slower than 24 MHz saves at most another 1 mA. A floor of about 4 to 5 mA remains (cores, U55, SRAM leakage, DC-DC).
4. **Under 1 mA is not available while running code.** The datasheet offers sub-mA only in Power-down with retention and DPD, neither of
   which runs code, and nothing in the SDK or the examples does better.
5. **Power-down with retention works** after one fix (below), keeps the RAM (a value in RAM survived four wakes in a row), and starts the
   application about 10 ms sooner than a DPD wake. The sleep current is 1.5 mA, 150 times DPD's. Whether that 10 ms matters is not shown: the
   full wake latency has not been measured.
6. **The application note's rule matters.** It says the clock must be the 24 MHz RC oscillator before entering Power-down or DPD. The Himax
   example that our Power-down function was copied from woke the boot ROM at the 400 MHz PLL setting and the wake hung (8.9 mA, no
   console). Doing what our DPD code does (wake on the RC oscillator, PLL disabled) fixed it.
7. **If clocks are gated they must be restored before entering DPD.** Gating the `hsc` clock group and entering DPD did not resume. With the
   restore (now in the code) it does.
8. **Timing is wrong by known amounts.** The RTC and the sleep timers run from the internal 32 kHz RC oscillator (no 32 kHz crystal fitted) and
   are about **4.1 % fast**: a 30 s alarm is really about 28.8 s. At 400 MHz the FreeRTOS tick is exact (-10 ppm). At 24 MHz on the RC
   oscillator the tick is 0.96 % slow.
9. **Oscillators.** The 24 MHz crystal is running and feeds the PLL and, through the LSC reference clock, the console UART. The 32.768 kHz
   crystal oscillator is enabled at a cold boot with no crystal fitted, and disabled after any DPD wake. Neither matters for current.
10. **Energy view (arithmetic with stated assumptions).** Power-down draws about 1.5 mA more than DPD while asleep. It only pays if the
    energy it saves at each wake exceeds that. If the 10 ms it saves were spent at about 30 mA (an assumption, not measured) that is about 0.3 mA
    seconds, which is 0.2 s of the extra sleep current. So unless events arrive less than about 0.2 s apart, DPD uses less energy.
    Power-down with retention is a latency option, not an energy one.

**What to use, on what we know:** DPD for low power. Power-down with retention only if the wake latency has to be as short as possible and 1.5 mA while
waiting is acceptable. Staying awake in the 4.8 mA state does not avoid a cost: it is 480 times DPD's current and leaving it for full speed takes at
least tens of ms (the delays in `clkfast` are guesses). Nothing found reaches a low current without either rebooting or restarting the application.

## What we know

Each item says how it was established. "Measured" is a multimeter or console reading. "Read" is from the SDK source, the datasheet or the
application note. "Inferred" is a conclusion not directly tested.

**Tickless idle and the CPU**
- FreeRTOS tickless idle is enabled and working (measured: the `idle` command counted 49 idle-hook calls in 2 s at 400 MHz, 3 in 2 s at 24 MHz, 1 in 2 s
  at 1.5 MHz, each matching the sleep limit the 24-bit SysTick allows).
- The kernel idle loop does not execute WFI itself; tickless idle is what puts the CPU to sleep (read, from the kernel source; inferred that turning
  it off would leave the CPU running).
- Nothing in the other apps in this repository changes tickless idle. The SDK's low-power examples use the PMU sleep modes (read).
- Clock start-up state: PLL 400 MHz from the 24 MHz crystal, HSC clock = PLL, LSC clock = PLL / 4, every clock enable set, both crystal oscillators
  enabled (measured with `clocks`).

**Awake-idle current** (measured unless noted)
- Baseline 17.3 to 17.9 mA. Gating unused clock enables alone at 400 MHz: 11.2 to 11.6 mA (repeatable from a power-cycle).
- 24 MHz RC oscillator: 10.1 mA. PLL off: 8.5 mA. Unused enables off: 5.6 mA. UART on RC then crystal off: 4.8 mA. Clock enables cost less at
  24 MHz (2.9 mA) than at 400 MHz (about 6 mA); the `flash` group's clocks cost nothing at 24 MHz.
- Below 24 MHz: 12 MHz 8.1 mA, 6 MHz 7.9, 3 MHz 7.8, 1.5 MHz 7.7 (with the PLL off and the enables on). So the dynamic part is small and
  the rest is static.
- The 24 MHz crystal oscillator costs about 0.8 mA; the 32.768 kHz oscillator costs nothing measurable.
- All the run-time experiments are reversible: `clkfast` and `clkon` returned the current to 17.4 mA.
- Entering DPD from the slow, gated state works on both wake sources, and `clocks` afterwards shows everything back to normal.

**How the clocks are wired** (read, and measured where stated)
- The console UART clock is the LSC reference clock divided by `uart_div`. Its source was the crystal; switching the crystal off stopped the console
  until the reference clock was moved to the RC oscillator (measured). The RC oscillator's error (about 1 %) does not upset the console at 921600 baud.
- The SDK has functions to enable and disable each crystal oscillator, choose the 32 kHz source, and trim the RC32K oscillator. Nothing in the
  application uses them (read, by searching the code).

**Sleep modes** (measured unless noted)
- DPD: 10 uA. Both wake sources work. After every DPD wake the RAM is not kept.
- Power-down: 1.5 mA with or without retention. With retention the RAM is kept (a `.noinit` counter counted 1, 2, 3, 4 wakes in a row); without it, it is
  not. The timer wake is reported as `SB_timer_int` (0x0040).
- Retention shortens the start: banner 4 to 5 ms after the first bootloader line (four wakes), against 13 to 16 ms for DPD (two) and 16 ms with no retention
  (one). The first bootloader line to the CLI task: 16 to 20 ms with retention (an outlier of 5 ms), 24 to 30 ms for DPD, 37 ms with no retention. These are
  host timestamps with a few ms of jitter.
- After a Power-down wake the bootloader restores the PLL and clocks, as after a DPD wake (`clocks` afterwards).
- The wake hung until the boot-ROM wake clock was set to the RC oscillator with the PLL disabled, as in the DPD code and as the application note asks (measured
  before and after the change; the cause is inferred).

**Timing accuracy** (measured with host timestamps)
- 400 MHz tick: 0.99999 s per 1.000 s (-10 ppm). 24 MHz tick: 1.0096 s (0.96 % slow).
- RTC: 4.05 % fast at 400 MHz, 3.96 % fast at 24 MHz (counting skipped seconds over about 100 s).
- The Power-down timer: a programmed 30 s took 28.81 s, four times in a row (30 / 28.81 = 4.1 % fast), and 28.83 s once without retention.

**The datasheet and application note** (read; the figures are not reproduced here because both are marked Himax Confidential)
- Sub-mA states are Power-down with retention and DPD only. Sleep (WFI) mode has no current figure. The lowest running state has the Big core and
  the U55 power shut off, which this application cannot do.
- Section 9 of the application note: switch to the 24 MHz RC oscillator before entering Power-down or DPD.

## What we do not know

**Wake latency and the reboot cost**
- The time from the WAKE edge or the timer alarm to the application running, for DPD, Power-down with retention and Power-down without. Only the
  part after the first bootloader line is visible in the console log. It needs an oscilloscope or logic analyser on the WAKE pin and on PB10 (the blue
  LED pin, which the app sets high early in `app_main()`). Not done.
- How long the boot ROM and the first bootloader take before their first message.
- Whether the Power-down wake by the WAKE switch works: it was tested only on the build that hung, before the fix.
- How long the crystal and the PLL really take to start. `clkfast` waits 20 ms and 5 ms, both guesses. So the time to get from the slow idle state
  back to full speed is not known and may be much shorter than 25 ms.

**Current**
- Why Power-down draws 1.5 mA, several times the datasheet's typical for the chip alone, and why that is the same with and without retention (so
  retaining the RAM costs nothing measurable). Candidates, none tried: the PMU settings copied from the Himax example (the internal DC-DC left on
  in Power-down, the I/O retention setting, the external-supply pin left off), states of the pins, other blocks kept powered in the stand-by
  domain, or the board.
- Why DPD is 10 uA when the datasheet's typical is about ten times lower (the board, most likely).
- Why DPD rose to 13.8 uA once the PCA9574 was fitted (HM0360 and SD card removed) - see "PCA9574 and the extra DPD current" below. The camera and FatFS application code were both ruled out.
- The full combined floor: the 4.8 mA state plus `clkdiv 16`. The `u55`, `i3c`, `puf`, `dma` and `sdio` parts of `clkoff` were never used, so which
  block of the `hsc` group stops a DPD resume when left off is not known.
- Whether dynamic voltage and frequency scaling (the datasheet mentions a 0.8/0.9 V scheme) or powering down unused SRAM banks can lower the static
  floor. Not looked at in the SDK.

**Behaviour**
- Whether the restore before DPD is what made the `hsc`-gated DPD resume work: the earlier failed test also differed in other ways.
- How much RAM must be retained. The linker script puts the driver library's code and `.noinit` in the system SRAM, so retaining less than the
  Himax example does was not tried.
- What the retention flags in the PMU configuration do exactly. The power-management library is a binary.
- Whether the RTC error could be trimmed out with the RC32K trim register, or would need the time from the BLE processor. Not tried.
- Whether any of this holds in `ww500_md`, with its camera, SD card, BLE link and neural network. The experiments ran only in `ww500_minimal`.

---

## Where is the HM0360 power going?

Sleep measurements (DPD):

I meaured the volatges across R28 (XSHDN) and R31 (XSLEEP). In both cases the voltage at the HM0360 is 1.65V vs 1.81V at the 1V8 rail.
Since the reistors a 1M this means 160nA flows into these pins.

Voltages at some pins (VSYNC, HSYNC, SEN_PCLK, STROBE) at in the range 10-150mV and seem to be floating (Hi-Z). SEN_INT (the MD interrupt)
is essential 0V. 

There are 2 power supply rails with 0R resistors. I remove these and measure the currents in turn:

* __2V8__ (R45) 73uA (pulses higher to c. 400uA periodically - perhaps when the counter expires?)
* __1V8__ (R47) 138uA (pulses higher to c. 600uA periodically)
* __Sum__ 200uA
* __Whole board__ 260uA (pulses higher to c. 600uA periodically)

__Different modes__

the MODE_SELECT register is 0x0100 and documented in the data sheet section 6.1. 

Table 6.1 (p31) and section 10.2 (p48) document modes 0, 1, 2, 3, 4, 6, 7.

I can type `cam 0` (to put the camera in mode 0) and the same with other modes. 
Results follow (other than made 2 which is reported above):

* __2V8__ Mode 1 (continuous) = 1.4mA. All other modes constant at 73uA
* __1V8__ Mode 1 (continuous) = 4.8mA. All other modes constant at 138uA.
* __Whole Board__ modes other than 1: All other modes constant at 138uA.

__Interesting:__ I think the ww500_md project uses mode 2 rather than mode 0 when MD is diabled
because mode 0 seemed to have much higher power. This is not what we see here (tentative - check). 

__HM0360 data sheet__

Section 2 Sensor Overview says:

```
...target current consumption of 256uA in AoS monitor mode and 8.6mA in VGS 60 fps read out mode.
```

DC characteristcs are in section 11.3, p 78.

---

## Detailed record

The sections below are the working record in the order the work happened, with the measurements and logs. Where a later section corrects an
earlier one, the correction is marked. Two corrections worth knowing: (a) the idea that oscillator settings persist across DPD was wrong (see Part D),
and (b) the suggestion that the application lives only in TCM, so less RAM could be retained, was wrong (see Part E).

## Conditions for the awake-idle measurements

- Console `blink off`, `led 9 0`, `led 10 0`, then `inactivity 3600`, with nothing typed while
  the reading was taken (about 10 s after each command).
- LEDs off, so they add no current. All tasks blocked, so the idle task is running.
- `SystemCoreClock` 400 MHz. Tickless idle enabled.

## Why the idle "ticks" behave as they do

An ordinary FreeRTOS kernel wakes the CPU on every tick (1000 times a second here), and its
idle task does not sleep by itself: it just loops. With `configUSE_TICKLESS_IDLE` set, the
idle task instead does this on every pass:

1. Calls `vApplicationIdleHook()`. In this app that is `inactivity_IdleHook()`, which counts how
   long every task has been idle. The hook must not block.
2. Asks the kernel how long it will be until a task must run (the expected idle time). If that
   is at least 2 ticks, it calls the port's `vPortSuppressTicksAndSleep()`.
3. The port stops the tick, sets the SysTick reload to cover the whole idle period and executes
   `WFI`. The CPU clock stops until an interrupt arrives.
4. When it wakes, the port works out how many tick periods have really passed and tells the
   kernel to step its tick count forward by that many, so `xTaskGetTickCount()` stays correct.
   The idle task then loops back to step 1.

So during idle the tick interrupt is not firing 1000 times a second. The tick count still
advances, but in a jump when the CPU wakes.

### Why 41 ms and about 24 wakeups a second

SysTick runs from the CPU clock (`configSYSTICK_CLOCK_HZ` is `configCPU_CLOCK_HZ`, which is
`SystemCoreClock`). At 400 MHz and a 1000 Hz tick one tick is 400,000 counts. SysTick is a
24-bit counter, so its longest reload is 0xFFFFFF = 16,777,215 counts. The port therefore caps
one sleep at 16,777,215 / 400,000 = **41 ticks = 41 ms**, even when no task is due for much
longer (for example every task is waiting `portMAX_DELAY`). At the end of each 41 ms sleep the
SysTick interrupt wakes the CPU, the kernel catches up the 41 ticks, the idle hook runs, and the
CPU sleeps again. That is 1000 / 41 = about **24.4 idle-hook calls per second**.

The `idle` CLI command measures this: in 2000 ms with everything idle it counted 49 hook calls
(24 per second), which matches. If the idle task were spinning instead of sleeping the count
would be tens of thousands per second.

Consequences:

- **Inactivity timing:** the 1 s inactivity test in `inactivity.c` is checked about every 41 ms,
  so DPD starts at most about 41 ms late.
- **The sleep limit depends on the clock.** At a slower CPU clock the counter takes longer to
  run out: at 24 MHz the limit would be 0xFFFFFF / 24,000 = 699 ms, so far fewer wakeups.
- **The tick does not drift the RTC.** The RTC is separate hardware. Kernel time can drift by a
  tiny amount because SysTick is stopped briefly at each end of a sleep (the port compensates
  as best it can).
- **Anything else that interrupts ends the sleep early** (for example a character on the
  console UART), and the same catch-up applies.
- **While blinking** the blinky task wakes every 500 ms, so the idle sleeps are about 41 ms
  each and the kernel runs the idle loop about 12 times between blinks.
- **Tickless idle is what puts the CPU to sleep at all.** The kernel idle task itself never
  executes WFI, so turning `configUSE_TICKLESS_IDLE` off would leave the CPU running flat out
  while idle. (Inferred from the kernel source: the idle loop has no WFI.)

Where this comes from in the source: `os/freertos_10_5_1/NTZ/config/FreeRTOSConfig.h`
(`configUSE_TICKLESS_IDLE 1`, used by the secure-only build), `.../NTZ/freertos_kernel/tasks.c`
(`prvIdleTask`, `prvGetExpectedIdleTime`, `eTaskConfirmSleepModeStatus`) and
`.../portable/GCC/ARM_CM55_NTZ/non_secure/port.c` (`vPortSuppressTicksAndSleep`, weak; nothing
in this repository overrides it, and no pre-sleep or post-sleep hooks are defined).

No other example in `app/scenario_app` or `app/ww_projects` changes tickless idle. The SDK's
low-power examples instead use the PMU sleep modes (`hx_lib_pm_cfg_set` and
`hx_lib_pm_trigger`), and `hello_world_cmsis_cv` has a `..._24M` variant that switches the HSC
and LSC clock source to the 24 MHz crystal before sleeping.

## What the datasheet says about the power modes

Source: `HX6538-A_DS_preliminary_v02.pdf` (Himax, July 2024), section 5.2 "Power and clock
management" (page 31) and section 6.3 "Power consumption" (page 50). The datasheet is marked
Himax Confidential, so the current figures are **not** reproduced here: look them up in section 6.3.
The same folder holds an application note, `HX6538-A_AN_preliminary_v02.pdf`, which has not been read yet.

The chip has these power modes:

| Mode | What is running | Resume |
|---|---|---|
| Active, dual core | Both Cortex-M55 cores, the U55 and peripherals powered and clocked | |
| Active, single core | Only the Little Cortex-M55 (CM55S) runs; the Big core (CM55M, which runs this app) and the U55 are power shut off, as are some peripherals | |
| Sleep | The Arm-defined WFI mode: the CPU clock is gated, DC-DC on, everything else powered. This is what tickless idle uses | Immediate |
| Power-down | Both cores and the U55 powered off, TCM and system SRAM can be kept in retention, DC-DC in light-load mode | Short: the code and data are still in RAM, "no re-load programming and re-booting latency" |
| Deep-power-down (DPD) | Main core power removed except the PMU, DC-DC off | The program is re-loaded from external flash and the chip re-boots |

Table 6.3 gives one typical current per mode, and in round terms:

- The dual-core active figure is tens of mA, for a heavy neural-network workload.
- The lowest state that still runs code, single-core active with the PLL and crystal off and the
  Little core at 24 MHz from the RC oscillator, is a low single-digit mA.
- Power-down with retention is a fraction of a mA (sub-mA).
- DPD is about one microamp.
- Sleep mode (WFI) has **no figure** in the table.

What this means for our measurements:

- **Awake idle is close to the active figure.** Our 17 to 18 mA (11.6 mA with clock enables gated)
  is Sleep mode of the dual-core active state: both cores are powered, the PLL and DC-DC are on
  and 400 MHz clocks are running. WFI stops only the CPU clock, so it cannot give the "low power"
  of the other modes.
- **Sub-1 mA is only offered by Power-down (with retention) and DPD**, never while code is
  running. So the tickless-idle goal of under 1 mA is not reachable on this chip. The nearest thing
  is Power-down with retention.
- **Our DPD current (10 uA) is about ten times the datasheet's typical.** The datasheet figure is
  for the chip alone, and typical values are "not guaranteed". The difference is most likely the
  board (leakage through pins, pull-ups, regulators). It is worth understanding but is small in
  absolute terms.
- **Other levers the datasheet mentions:** an internal 0.8/0.9 V dynamic voltage and frequency
  scaling (DVFS) scheme (section 1), and powering down unused SRAM, TCM and AHB memory space at
  initialisation (section 5.1). Neither has been looked at yet in the SDK.

## What the application note says about power modes

Source: `HX6538-A_AN_preliminary_v02.pdf` (Himax, September 2024), sections 8 and 9. It is marked Himax
Confidential; the points are paraphrased. The rest of the note is a hardware reference design (circuits,
PCB layout, power-up sequence).

- **Section 8, power architecture.** In Power-down (PD) the external supplies stay on and only the AON and
  SB domains are powered inside the chip. In DPD only the AON domain is powered, and external supplies can
  be cut by an external power switch controlled from PA1.
- **Section 9, clock sources for PD and DPD.** The clock source must be switched to the 24 MHz **RC**
  oscillator before the system enters PD or DPD. After waking and finishing the boot the PLL source can
  be switched back to the 24 MHz crystal, if a crystal is fitted.

How the code compares (Claude's reading; the Himax power-management library is a binary, so its internals
could not be checked):

- **Section 8:** consistent. `sleep_mode_enter_dpd()` sets the DC-DC output pin to VMUTE mode (the PA1
  power-switch control), and `sleep_mode_enter_sleep()` leaves it off, so external supplies stay on in PD.
- **Section 9, before sleeping:** the code does not switch the clock source itself. It passes zeroed clock
  settings to `hx_lib_pm_trigger()` with `PM_CLK_PARA_CTRL_BYPMLIB`, which means the library controls the
  HSC and LSC clocks. The unmodified DPD path has worked reliably, which suggests the library does what the
  note requires. The SDK's own example (`hello_world_cmsis_cv`) instead passes the 24 MHz **crystal** as the
  source, which does not match the note. The `clkslow rc` experiment puts the system in the state the note
  asks for before PD or DPD.
- **Section 9, after waking:** before entering sleep the code records the running PLL and divider settings for
  the bootloader (`hx_drv_swreg_aon_set_bl_pmuwakeup_freq`) and sets the boot-ROM speed for the wake to "PLL
  disabled", which is the RC oscillator. The image is built with the RC24M bootloader profile
  (`building_firmware.md`). The application never changes the PLL source itself.
- **Not known:** whether the WW500 has a 24 MHz crystal fitted, and what the PLL is fed from. The `clocks`
  command now prints the PLL source and the HSC and LSC sources and dividers, so it can be read from the
  board. If there is no crystal, `clkslow xtal` must not be used.

## The crystal oscillators

The WW500 board has a 24 MHz crystal and **no** 32.768 kHz crystal (Charles, 21 September 2026).

- **What the datasheet says** (section 5.2, paraphrased): after reset the chip runs from the internal 24 MHz
  RC oscillator until software switches; both crystal oscillators (24 MHz and 32.768 kHz) are **enabled by
  default** and can be disabled by software; the PLL is disabled by default and can take its input from an RC
  oscillator or the crystal. The datasheet's lowest running state has the 24 MHz crystal disabled.
- **What the code does:** nothing. No code in `app/`, `board/`, `device/` or `library/` calls any of the SDK
  functions that control the oscillators, so their state is whatever the reset default and the bootloaders
  (binary images, RC24M profile) leave. The SDK functions that exist are, in `hx_drv_scu.h`:
  `hx_drv_scu_set_xtal24m_en()` and `set_xtal32k_en()` (with `get_` versions), `set_xtal24m_sel()` (the
  frequency range of the crystal), `set_clksrcselen_bysyscase()` (chooses which oscillators are enabled for a
  set of clock sources, and can disable the ones not required), and `set_pdhsc_hsc32kclk_cfg()` and
  `set_pdlsc_32k_cfg()` (choose RC32K or XTAL32K as the 32 kHz clock). The only place the crystal is used in
  the repository is the SDK examples, which pass `SCU_HSCCLKSRC_XTAL24M` as the clock to sleep with.
- **Consequences:** the 32.768 kHz oscillator circuit is probably enabled with nothing connected, which may
  waste a little current (unknown how much). The 24 MHz crystal is probably running while the PLL and
  the CPU run from something else. The RTC has no crystal, so it uses the internal RC32K oscillator and is
  not accurate. Which 32 kHz source the RTC uses is not confirmed: the `clocks` command now prints it.
- **Experiment (coded, not run):** `clocks` now also prints whether each crystal oscillator is enabled, the
  crystal frequency range selection, and the HSC and LSC 32 kHz clock sources. `xtal 32 0` switches the
  unused 32 kHz oscillator off and `xtal 32 1` back on. `xtal 24 0` switches the 24 MHz one off, and is
  refused while the PLL, HSC or LSC clock is fed from it (use `clkslow rc` first). Measure the current before
  and after. Entering DPD or Power-down puts the oscillators back to how they were.
- **Caution on `clkslow xtal`:** the crystal is running (see below), so `clkslow xtal` would work, but `clkslow rc`
  is the source the application note says to use before PD or DPD, so use `rc`.

### Measured: switching the 32.768 kHz oscillator off (Charles, 21 September 2026)

- **Awake:** `xtal 32 0` made no measurable difference to the 17 mA. The oscillator circuit's current is lost in
  the total.
- **In DPD:** entering DPD with it disabled left the DPD current unchanged at 10 uA. The datasheet has both
  oscillators off in DPD anyway, so the 10 uA is not from the crystals.
- **It was still disabled after the DPD wake.** This was first read as "the setting persists across DPD". Part D below
  showed the 32.768 kHz oscillator is disabled after any DPD wake, even when we did not touch it, so this reading was
  probably that effect and not persistence.
- **Open question:** `enterDpd()` calls `power_diag_restoreClocks()`, which should re-enable the oscillator
  before DPD, so the state after the wake should have been "enabled". Either the image did not contain that
  change or the re-enable did not take effect. To check: is the `Built:` time in the banner later than the change,
  and does `clkoff hsc` then `dpd` now resume?
- **Conclusion:** switching the crystal oscillators off is not worth pursuing for current. The `xtal` command
  stays as a diagnostic. The CPU and bus clock speed is the lever still to test (`clkslow rc`, `clkpll 0`).

### What the `clocks` command showed (21 September 2026)

- The PLL is fed from the **24 MHz crystal** (`XTAL24M`); the HSC clock is the PLL divided by 1 (400 MHz) and the
  LSC clock is the PLL divided by 4 (100 MHz). The UART has its own 24 MHz clock.
- The 24 MHz crystal oscillator is **enabled**, with range selection 1 (12.1 to 24 MHz), which is right for a
  24 MHz crystal.
- The 32.768 kHz crystal oscillator is **enabled**, with no crystal fitted. Both the HSC and LSC 32 kHz clocks
  are taken from the internal RC32K oscillator, so nothing here uses the 32 kHz crystal oscillator and it is a
  candidate to switch off (`xtal 32 0`).
- Because the PLL is fed from the crystal, the crystal can only be switched off after the PLL has been
  switched off, which needs the CPU and bus clocks off the PLL first. The order for the experiment is
  `clkslow rc`, `clkpll 0`, `xtal 24 0`. `clkfast` reverses it in the right order (crystal, then PLL, then the
  clock sources), waiting for each to start.

## What was running: the `clocks` command

At start-up (all values as printed by the `clocks` command):

```
PLL 400000000 Hz, HSC_CLK 400000000, CM55M 400000000, U55 400000000, AXI 400000000
LSC_CLK 100000000, CM55S 100000000, UART 24000000
HSC: cm55m u55 axi ahb0 ahb5 ahb1 apb2 rom sram0 sram1 i3c_hc puf dma0 dma1 sdio i2c2ahb_flash_w qspi ospi spi2ahb
HSC image: xdma_w1 xdma_w2 xdma_w3 xdma_r sc inp dp 2x2 5x5 cdm jpeg tpg edm rgb2yuv csc mipirx mipitx
LSC: cm55s ahb_m ahb_2 ahb_3 apb_0 sram2 dma2 dma3 i2s_host pdm uart0 uart1 uart2 i3c_slv0 i3c_slv1
     pwm012 i2s_slv ro_pd i2c_slv0 i2c_slv1 i2c_mst i2c_mst_sen sw vad_d adcck gpio sspim sspis ckmon sc
SB:  apb1_ahb4 ts adc_lp_hv i2c2ahb_dbg wdt0 wdt1 timer0-8 sb_gpio hmxi2cm
AON: rtc0 rtc1 rtc2 pmu aon_gpio aon_swreg antitamper
```

Every clock enable in every domain is set. The app never gates any: `platform_driver_init()` only
sets up driver structures and the clocks come from the bootloader. `ww500_md` behaves the same.

## Clock-gating experiment

The `clkoff <group>` CLI command switches off groups of clock enables that this app does not use,
and `clkon` restores them. Groups (see `power_diag.c`):

| Group | What is switched off |
|---|---|
| `image` | The camera data path: XDMA, INP, DP, 2x2, 5x5, CDM, JPEG, TPG, EDM, RGB2YUV, CSC, MIPI RX/TX, SC |
| `hsc` | U55 (neural network), I3C host, PUF, DMA0, DMA1, SDIO |
| `lsc` | CM55S core, DMA2/3, I2S, PDM, UART1/2, I3C slaves, PWM, I2C slaves and masters, VAD, ADC clock, SSPI master/slave, clock monitor, SC. **Since the SD card was added (step 7) the SPI master (`sspim`) and DMA2/3 are no longer switched off**, because the SD card needs them; the measurements below were made with them off |
| `sb` | Temperature sensor, ADC, WDT1, TIMER3 to TIMER8, Himax I2C master |
| `flash` | QSPI, OSPI, SPI2AHB, I2C2AHB flash write. Not included in `all` |
| `u55`, `i3c`, `puf`, `dma`, `sdio` | The individual blocks of `hsc` (`dma` = DMA0 and DMA1). Added to find which block stops the DPD resume |

Always kept on: CM55M, AXI, AHB/APB buses, ROM, SRAM0/1/2, UART0, GPIO, debug (SW), TIMER0-2, WDT0,
SB GPIO and all AON clocks.

### Results: cumulative from a power-cycle (the reliable measurement)

Sequence: power-cycle, type a character within 3 s, `blink off`, `led 9 0`, `led 10 0`,
`inactivity 3600`, then the `clkoff` commands in this order with nothing else typed. Run twice,
with identical readings each time. Baseline **17.9 mA**.

| Step | Current (both runs) | Extra saving |
|---|---|---|
| `clkoff image` | 16.9 mA | 1.0 mA |
| `clkoff hsc` | 15.4 mA | 1.5 mA |
| `clkoff lsc` | 13.1 mA | 2.3 mA |
| `clkoff sb` | 12.7 mA | 0.4 mA |
| `clkoff flash` | 11.6 mA | 1.1 mA |
| `clkon` | 17.9 mA | |

Total saving **6.3 mA**. This agrees with an earlier, less carefully recorded run the same day
(about 17 mA down to 11.2 mA, 5.8 mA saved).

### Results: one session with many `clkon` cycles (an unreliable sequence)

Baseline 17.7 mA, and every `clkon` returned to it. In Part 1 each group was switched off on its
own, then restored with `clkon`:

| Group on its own | Current | Saving |
|---|---|---|
| `image` | 16.8 mA | 0.9 mA |
| `hsc` | 16.2 mA | 1.5 mA |
| `lsc` | 15.2 mA | 2.5 mA |
| `sb` | 17.3 mA | 0.4 mA |
| `flash` | 16.8 mA | 0.9 mA |

Part 2 then ran the cumulative sequence in the same session: 16.7, 15.3, **15.2**, 14.9, 14.0 mA
(total 3.7 mA). Here `lsc` added only 0.1 mA after `image` and `hsc`, against 2.3 mA from a
power-cycle.

### What the results show, and what they do not

Measured:

- From a power-cycle the sequence is repeatable (two identical runs) and matches the first
  session. The awake-idle current can be reduced from about 17.9 mA to 11.6 mA by switching off
  the clock enables this app does not use.
- Each group's saving from a power-cycle: `image` 1.0, `hsc` 1.5, `lsc` 2.3 (the largest),
  `sb` 0.4, `flash` 1.1 mA. The Part 1 single-group figures (0.9, 1.5, 2.5, 0.4, 0.9 mA) are close
  to these, so the groups are largely independent, and the sum of Part 1 (6.2 mA) is close to the
  cumulative total (6.3 mA).
- The Part 2 result above (`lsc` adding only 0.1 mA) is the odd one out. It differs from both
  power-cycled runs and from Part 1, so it depended on the history of the earlier `clkoff`
  and `clkon` cycles in that session.

Inference, not tested:

- Something changed state during those cycles so that gating `lsc` no longer saved anything
  afterwards, even though `clkon` restored the baseline. The most likely suspect is the `lsc` group's
  CM55S core clock, which is gated and re-enabled while the core may be running, but this has not
  been checked. The earlier suggestion that the groups overlap through a shared sensor-control
  clock is not supported by the power-cycled runs.

Not yet tested:

- Which enables inside `lsc` give the 2.3 mA.

### DPD with clocks gated does not resume (21 September 2026)

With `clkoff all` and `clkoff flash` in force and then `dpd`, the board entered DPD but after the 30 s
alarm it did not come back: 5.4 mA and no console output. The suspicion, from Charles, is that the
flash interface is disabled when the bootloader wakes.

What is known: every DPD wake is a warm boot in which the bootloader (`1st BL ... jump_addr=0x10000000`
in the boot log) reads the application back from flash. A processor stuck in the bootloader at 5.4 mA
fits a failed flash read.

Not known: which group causes it (`flash` is the prime suspect), and whether the enables persist across
DPD or the bootloader is affected another way.

Result of the first test (Charles): `clkoff flash` then `dpd` **does** resume. So the flash interface
clocks alone are not the cause, and the flash suspicion above is not supported. The cause is one of
`image`, `hsc`, `lsc` or `sb`, or a combination. (Not recorded whether that test ran with or without
the restore-before-DPD change below.)

Second test (Charles): `clkoff hsc` then `dpd` does **not** wake properly (9.3 mA, after the alarm). The
groups `lsc`, `image`, `sb` and `flash` each resume when gated on their own. So the cause is in the `hsc`
group: U55, I3C host, PUF, DMA0/1 or SDIO. Which one is not yet known; the `clkoff` command was extended
with `u55`, `i3c`, `puf`, `dma` and `sdio` so they can be tried one at a time.

(The original plan for separating the groups, each from a power-cycle, follows. Steps (a) and the
`lsc`, `image`, `sb` part of (b) have now been done.) `blink off`, then (a) `clkoff flash` then `dpd`,
(b) `clkoff image`, `clkoff hsc`, `clkoff lsc`, `clkoff sb` (not `flash`) then `dpd`. Only a group
that stops the resume shows the 5.4 mA / no-console symptom.

Change made: `blinky_task.c` now calls `power_diag_restoreClocks()` just before entering DPD, so any
clocks switched off by `clkoff` are switched back on first. That is what a real gating scheme would need,
because gating saves current only while awake.

## Things we might try

Charles's aim is not the lowest possible sleep current (DPD already does that) but a low current
**without a reboot**, because the time to boot after a wake from DPD sets a floor on the time from a
motion detection to the first picture. In `ww500_md` that boot takes tens of ms, which is significant.
That is why staying awake in a low-current state, or waking quickly from a retention mode, is of interest.

Ordered by how much they are likely to tell us:

1. **Slower clock while awake and idle** (done: Parts A to D). Switch the HSC and LSC clock source from
   the 400 MHz PLL to a 24 MHz oscillator, then optionally power the PLL down. The datasheet's low
   "active" figure assumes the PLL and crystal are off. CLI: `clkslow <rc|xtal>`, `clkpll <0|1>`,
   `clkfast`. See "Slow-clock experiment" below.
2. **Power-down with retention** (done: Part E; about 10 ms less than DPD after the first bootloader line, total latency not measured). The datasheet's only sub-mA mode that keeps RAM
   and does not reload from flash, so its wake should be much faster than from DPD. CLI:
   `sleep <seconds> <0|1>`. The port is the unused `sleep_mode_enter_sleep()` from `ww500_md`. The wake
   is a restart at the application entry with RAM kept, not a resume inside FreeRTOS.
   To measure: the current while sleeping, and the time from the wake event to the first console output,
   compared with the same times for DPD.
3. **Measure the cost of a wake from DPD** (not done: needs an oscilloscope) (time from the alarm to the first console output, and the
   average current over that time), to compare with 2. Charles has already seen that the boot in
   `ww500_md` takes tens of ms.
4. **Oscillators** (tried for the 32.768 kHz one, no benefit: see "The crystal oscillators"). The 24 MHz
   crystal is unlikely to matter either.
5. **RTC accuracy.** The RTC is about 4 % fast on the RC32K oscillator. Try `hx_drv_scu_set_RC32K1K_trim()`, or
   correct with the time from the BLE processor. (Charles is interested in RTC accuracy.)
6. **DVFS and unused SRAM.** Look in the SDK for control of the 0.8/0.9 V scaling and for powering down
   SRAM banks the app does not use (datasheet section 5.1). The application note was thought not to be
   relevant.
6. **The 10 uA DPD floor.** Find where the difference from the datasheet's typical figure goes on the
   board.
7. **Run the application on the Little core (CM55S)** with the Big core and U55 power shut off
   (single-core active mode). This is the datasheet's lowest running state, but it would mean moving
   the whole application to the other core. Not recommended without a strong reason.
8. **Clock gating as a real feature.** Worth about 6 mA while awake, and it can be combined with 1.
   The clocks must be restored before DPD (`blinky_task.c` does this through
   `power_diag_restoreClocks()`, not yet tested on hardware). Gating the `hsc` group stops the resume
   from DPD when it is left off, and the block responsible (U55, I3C, PUF, DMA or SDIO) is unknown;
   `clkoff` can gate them one at a time.

### Would a slower clock also stop the wake-ups during tickless idle?

It reduces them but does not remove them. The longest tickless sleep is set by the 24-bit SysTick
counter: 16,777,215 counts. At 400 MHz that is 41 ms (about 24 wake-ups a second). At 24 MHz it is
699 ms (about 1.4 a second), so about 17 times fewer wake-ups. The wake-ups themselves are cheap:
each is a few microseconds of CPU time, so at 24 a second they are a tiny fraction of the time, and
they are not where the 17 mA goes. To remove them completely the port's weak
`vPortSuppressTicksAndSleep()` could be replaced with a version that wakes on a longer timer (the RTC
alarm or a stand-by timer) instead of SysTick. That is more work and is not worth doing until we know
what a slow clock saves.

### Slow-clock experiment (procedure)

From a power-cycle: type a character within 3 s, `blink off`, `led 9 0`, `led 10 0`, `inactivity 3600`.
First try `xtal 32 0` on its own and read the current (nothing uses the 32 kHz crystal oscillator), then
`xtal 32 1` to put it back. Read the current after each step, waiting about 10 s:

| Step | Type | Record |
|---|---|---|
| S0 | (nothing: baseline) | |
| S1 | `clkslow rc` | current, then run `idle` and `clocks` to see the clock, tick rate and idle-hook rate |
| S2 | `clkpll 0` | current |
| S3 | `xtal 24 0` (only allowed once the PLL is off) | current |
| S4 | `clkoff all`, then `clkoff flash` | current |
| S5 | `clkfast` | should return to S0 (it restarts the crystal, then the PLL, then the clocks) |

Use `clkslow rc`. Do **not** use `clkslow xtal` unless you know a 24 MHz crystal is fitted: without one the CPU clock stops. (The RC oscillator is also the source the application note says must be used before entering PD or DPD.) After `clkslow` the console should still work,
since the UART has its own 24 MHz clock. If it does not, power-cycle. Entering DPD or Power-down
restores the clocks first.

### Slow-clock result: `clkslow rc` (Charles, 21 September 2026)

- `clkslow rc` alone (PLL still running) took the awake-idle current from **17 mA to 10 mA**. This is the largest
  single saving found so far, larger than all the clock-enable gating together (about 6 mA).
- `idle` afterwards: `SystemCoreClock` 24 MHz, longest tickless sleep 699 ticks, and 3 idle-hook calls in 2000 ms
  (about 1 a second), matching the expected 1.4 a second. So the tick was retuned correctly and the CPU still
  sleeps in WFI between events, with about 17 times fewer wake-ups than at 400 MHz.
- **Not yet checked:** that FreeRTOS time really is correct at 24 MHz. Compare a tick-based interval with the RTC,
  for example `getutc`, wait about 30 s by the clock on the wall, `getutc`, and the blinking period.
- **Then `clkpll 0`** (PLL off) took it from 10 mA to **8.5 mA**.
- **Still to try from here:** a lower clock (`clkdiv 2`, `4`, `8`, `16`: the RC oscillator divided down, added after this
  result to see whether the remaining current still falls with frequency), then `xtal 24 0` if that matters, then `clkoff all` and
  `clkoff flash`. The clock-enable savings should now be smaller, since dynamic power scales with clock speed.
  Anything left at 24 MHz is mostly static: leakage of the powered cores, U55 and SRAMs, the DC-DC and the PLL.
  The datasheet's lowest running state (Little core only at 24 MHz) is far lower because the Big core and U55 are
  power gated, which this application cannot do.

### Slow-clock sequence, all steps (Charles, 21 September 2026, Part A)

Awake and idle, LEDs off, from a power-cycle, each step in addition to the previous ones:

| Step | Current | Change |
|---|---|---|
| Baseline (400 MHz PLL, all enables on) | 17.3 mA | |
| `clkslow rc` (CPU and buses from the 24 MHz RC oscillator) | 10.1 mA | 7.2 mA |
| `clkpll 0` (PLL off) | 8.5 mA | 1.6 mA |
| `clkoff all` (unused clock enables off) | 5.6 mA | 2.9 mA |
| `clkoff flash` | 5.6 mA | 0 |
| `xtal 32 0` (32.768 kHz oscillator off) | 5.6 mA | 0 |
| `xtal 24 0` (24 MHz crystal off) | 4.8 mA | 0.8 mA, **and the console stopped responding** |

Total 12.5 mA saved, from 17.3 to 4.8 mA, about 72 %.

- **The clock-enable gating still helps at 24 MHz** (2.9 mA), though less than at 400 MHz, but the `flash` group
  saves nothing any more (it was 1.1 mA at 400 MHz), so its clocks only cost current when fast.
- **The 24 MHz crystal oscillator costs about 0.8 mA**, which is significant at this level (unlike the 32.768 kHz
  one, which costs nothing measurable).
- **The console died when the crystal was switched off.** The likely reason: the UART clock is the LSC reference
  clock divided by `uart_div`, and the LSC reference clock has its own source selection (RC24M1M, RC96M48M, XTAL24M
  or PLL). It is almost certainly on the crystal, and `clocks` showed the UART at 24 MHz separately from the
  LSC clock. This has not been confirmed: `clocks` now prints the LSC reference clock source.
- The CPU was probably still running from the RC oscillator, so 4.8 mA is probably a valid reading, but with no
  console it could not be confirmed. A power-cycle restores everything.
- **Change made:** `clkuart <rc|xtal>` moves the UART's reference clock to the RC oscillator, and `xtal 24 0` is now
  refused while the PLL, the CPU or bus clocks, or the UART reference clock still use the crystal. The
  RC oscillator is less accurate than a crystal, so the console may print garbage at 921600 baud: power-cycle if so.

### Slow-clock sequence with the UART moved to the RC oscillator (Charles, 21 September 2026)

Same sequence again with `clkuart rc` added before switching the crystal off. Same readings as before (17.3, 10.1, 8.5,
5.6, 5.6, 5.6 mA), then:

| Step | Current | Notes |
|---|---|---|
| `clkuart rc` | 5.6 mA | The console still works on the RC oscillator, with no garbling at 921600 baud. |
| `xtal 24 0` | **4.8 mA** | Allowed now, and the console survived. |
| `clkfast` | 11.1 mA | Not 17: `clkfast` restores the clock speed, PLL, crystals and UART clock but **not** the clock enables that `clkoff` switched off (that is `clkon`). 400 MHz with the enables still off matches the earlier 11.2 to 11.6 mA. |

`clocks` at 4.8 mA showed: everything at 24 MHz, the PLL still fed from `XTAL24M` (but off), HSC and LSC from `RC24M1M`
with divider 1, the LSC reference clock (UART) from `RC24M1M`, both crystal oscillators disabled, and only the essential
clock enables left (HSC: cm55m axi ahb0 ahb5 ahb1 apb2 rom sram0 sram1; LSC: ahb_m ahb_2 ahb_3 apb_0 sram2 uart0 ro_pd sw
gpio; SB: apb1_ahb4 i2c2ahb_dbg wdt0 timer0-2 sb_gpio; AON all).

`clkon` then returned the current to 17.4 mA (baseline 17.3 mA), so the whole set of experiments is reversible.

The restart of the crystal, the PLL and the clock sources by `clkfast` worked (the system was stable at 400 MHz
afterwards). The delays it uses (20 ms for the crystal, 5 ms for the PLL) are guesses; the real start-up and lock times
are not known. They matter for latency: going from the slow state back to full speed costs at least those 25 ms, which
is comparable to the boot time from DPD that this work is trying to avoid. To shorten it the crystal could be left on
(the slow state is then 5.6 mA instead of 4.8 mA), and the delays could be reduced by testing.

### Part B: FreeRTOS time and RTC at 24 MHz (Charles, 21 September 2026)

Log: `_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/part_b_log.txt` (Tera Term host timestamps).

- **The FreeRTOS tick is right at 24 MHz, to about 1 %.** The blinky task prints the time every 1000 ms of ticks.
  Two clean intervals between consecutive prints were 1.010 s and 1.002 s on the host clock, the difference being host
  and USB timestamp jitter. So the tick retune after `clkslow` works.
- **The RTC ran fast against the host clock.** Two `getutc` responses arrived 60.22 s apart on the host clock; the RTC
  advanced 62 s (00:01:29 to 00:02:31). It reports whole seconds, so the true RTC interval was 61 to 63 s, which is
  1.3 % to 4.6 % fast. The RTC runs from the internal 32.768 kHz RC oscillator (no crystal fitted, and `clocks` showed both
  the HSC and LSC 32 kHz clocks on RC32K1K), which is not accurate, and should not depend on the CPU clock. Whether it
  is also fast at 400 MHz has not been measured.
- Only three time prints appeared after `blink 500`: the blinking stops after its run time, 3 s after a cold boot
  (`WW500_MINIMAL_RUN_TIME_COLD_MS`). Use `awake 120` first for a longer run.

### Part B, long runs (Charles, 21 September 2026)

Logs in the thread folder (Tera Term host timestamps): `part_b_slow_24MHz_log.txt` (`clkslow rc`, `clocks` shows 24 MHz)
and `part_b_fast_400MHz_log.txt` (no `clkslow`, `clocks` shows 400 MHz). Each has about 105 to 113 time prints, one every
1000 ms of FreeRTOS ticks. Earlier, less useful logs: `part_b_log.txt` (a short slow run), `part_b_run1_log.txt`
(truncated, no `clkslow` in the saved file) and `part_b_run2_log.txt` (a 400 MHz run).

| | 400 MHz (PLL from the crystal) | 24 MHz (RC oscillator) |
|---|---|---|
| Longest clean run of prints | 99 intervals in 98.999 s | 100 intervals in 100.960 s |
| **Tick period** (nominal 1.000 s) | 0.99999 s (-10 ppm) | **1.0096 s (+0.96 %, ticks are slow)** |
| Spread of the print interval (host jitter) | 2.2 ms | 2.8 ms |
| RTC seconds skipped, average gap | 24.67 s (host) | 19.99 s (host) |
| **RTC rate against the host clock** | **+4.05 % fast** | **+3.96 % fast** |

How the RTC figure is worked out: a print appears every tick-second and the RTC normally advances one second between
prints. Because the RTC runs fast, it advances two seconds now and then. Between two such skips there are n prints and
the RTC advanced n + 1 seconds, in n tick-seconds of host time, so the RTC gain is 1 / n. At 24 MHz a print is 1.0096 s
of host time, so 20.0 prints (19.99 s) and 21 RTC seconds give +3.96 %.

- **The 400 MHz tick is accurate to about 10 ppm**, which is what a crystal-derived clock should give.
- **At 24 MHz the FreeRTOS tick is 0.96 % slow.** The tick is retuned for 24.000 MHz (24000 counts per ms), so the RC
  oscillator is about 23.77 MHz, 0.96 % low. That is the accuracy of the RC oscillator, not a bug in the retune. All
  tick-based times at 24 MHz (the blink period, run times, the inactivity period) are about 1 % long. The RTC and DPD
  alarms are not affected. It also shows that the console UART, which we moved to the RC oscillator, has a baud rate
  error of about 1 %, which the terminal tolerated.
- **The RTC is about 4 % fast, at either clock speed** (4.05 % and 3.96 %, with a measurement uncertainty of a few
  tenths of a percent). So a DPD alarm of 30 s of RTC time is really about 28.8 s, and RTC timestamps drift by about one
  minute in 25 minutes. The RTC runs from the internal RC32K oscillator (no 32 kHz crystal), and 4 % is a lot for what the
  datasheet calls a factory-trimmed oscillator. The SDK has `hx_drv_scu_get_RC32K1K_trim()` and
  `hx_drv_scu_set_RC32K1K_trim()`, which nothing in the application uses, so the trim could perhaps be adjusted (not
  tried). The BLE processor has an accurate crystal and could supply the time (see `ble_commands.md`).
- The blinky task prints the time only while blinking, and the blinking stops after its run time, so `awake 120` was used.

### Part C: how low can the clock go? (Charles, 21 September 2026)

From a power-cycle, awake and idle, LEDs off, all clock enables and the crystal still on. Each step is on top of the previous
ones:

| Step | Clock | Current | Change |
|---|---|---|---|
| `clkslow rc` | 24 MHz | 10.2 mA | |
| `clkpll 0` | 24 MHz, PLL off | 8.6 mA | 1.6 mA |
| `clkdiv 2` | 12 MHz | 8.1 mA | 0.5 mA |
| `clkdiv 4` | 6 MHz | 7.9 mA | 0.2 mA |
| `clkdiv 8` | 3 MHz | 7.8 mA | 0.1 mA |
| `clkdiv 16` | 1.5 MHz | 7.7 mA | 0.1 mA |
| `clkfast` | 400 MHz | 17.5 mA | back to the baseline (17.3 to 17.9) |

`idle` at 1.5 MHz: `SystemCoreClock` 1,500,000 Hz, longest tickless sleep 11,184 ticks (about 11 s), and 1 idle-hook call in
2000 ms, as expected.

- **Below 24 MHz there is little left to save.** From 12 MHz to 1.5 MHz the current falls by only 0.4 mA. The
  frequency-dependent (dynamic) part is now small, so the remaining 7.7 mA is mostly **static**: leakage of the powered cores,
  U55 and SRAMs, the DC-DC and the analog blocks, plus the crystal oscillator (0.8 mA) and the idle clock enables that
  `clkoff all` removes (2.9 mA). It is also a poor trade: the CPU becomes 16 times slower, and at 1.5 MHz each 1 ms tick
  interrupt is only 1500 cycles.
- **The useful slow state is 24 MHz on the RC oscillator with the PLL off**, plus the gating and the crystal off:
  4.8 mA (see the sequence above), against 17.3 mA at 400 MHz. A further divider might take off a few tenths of a mA on top
  of that, which has not been measured.
- The static floor of about 4 to 5 mA is what the Big core, the U55 and the SRAMs cost while powered. The datasheet's much
  lower "single-core active" state needs the Big core and U55 power gated, which this application cannot do.

### Part D: DPD from the slow state (Charles, 21 September 2026)

Log: `_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/partd_log.txt`. From a power-cycle: `clkslow rc`,
`clkpll 0`, `clkoff all`, `clkoff flash`, then `dpd`. (The restore before DPD in `blinky_task.c` was in this build.)

- Current before `dpd`: 5.8 mA. In DPD: **10.1 uA**.
- **Both wake sources work from the slow, gated state**: the 30 s timer alarm (`Wakeup_event = 0x0002 ... RTC Timer`) and the
  WAKE switch (`WakeupEvt1 = 0x0001 ... WAKE signal`).
- **`clocks` after the wake shows the normal state**: PLL 400 MHz fed from the crystal, HSC on the PLL divider 1, LSC on the PLL
  divider 4, the UART reference on `XTAL24M`, the 24 MHz crystal enabled and every clock enable set. So the restore before DPD
  works after `clkslow rc`, `clkpll 0`, `clkoff all` and `clkoff flash`. Gating `hsc` and entering DPD without the restore
  did not resume earlier, so the restore is what a gating scheme needs, though the earlier test also differed in other
  ways.
- **After the wake the 32.768 kHz crystal oscillator is disabled, although `xtal 32 0` was not used in this run.** At a cold
  boot it is enabled. So DPD, or the warm-boot flow, switches it off and does not switch it back on. This means the earlier
  reading that oscillator settings "persist across DPD" (see "Measured: switching the 32.768 kHz oscillator off") is not
  supported: the "disabled" seen after that wake was probably this same effect. It is harmless: the RTC runs from the internal
  RC32K oscillator.
- **Timing seen in the log.** Timer wake: the DPD message at 18:40:30.024 and the first bootloader line at 18:40:58.834,
  28.81 s later, for an alarm of 30 RTC seconds. The RTC shows whole seconds, so the true alarm was 29 to 30 RTC seconds; that is
  consistent with the RTC being about 4 % fast but does not measure it independently. From the first bootloader line to
  `Starting CLI Task`: 28 ms after the timer wake and 24 ms after the WAKE pin wake (host timestamps, a few ms of jitter).
  The time from the wake event to the first bootloader line is not visible to the host.

### Part E: Power-down with retention, first results (Charles, 21 September 2026)

Log: `_Documentation/development reports/2026-09-20_Minimal__FreeRTOS/parte_run1.txt`. From a power-cycle, `blink off`, then `sleep 30 1`.

- **Current while sleeping: 1.3 mA.** The datasheet's typical for this mode is a fraction of a mA for the chip alone, so it is higher.
  Part of the difference may be that the mode as coded retains the TCM and all four HSC SRAM banks: the application lives in
  TCM (the linker script puts the code at 0x10000000 and the data at 0x30000000, the 256 KB ITCM and DTCM), so it may be
  possible to retain less and save current.
- **The wake does not resume.** After about 30 s the current rose to 8.9 mA and **no console text appeared**, for both the
  timer wake and the WAKE switch. The bootloader lines seen in the log at 18:50:27 are Charles power-cycling the board (they say
  `Cold boot`), not a wake. 8.9 mA is about what the chip draws at 24 MHz with the PLL off and everything on, so something is running
  but not producing console output.
- **Suspected cause.** The sleep entry printed `pmuwakeup_freq_type=1 ... pmuwakeup_run_freq=400000000`: the function told
  the boot ROM to wake at the current 400 MHz PLL setting. `sleep_mode_enter_dpd()`, which works, records the running
  clocks for the bootloader and then sets the wake clock to the RC oscillator with the PLL disabled, as section 9 of the
  application note requires. The Himax example that `sleep_mode_enter_sleep()` was copied from does not do that. Change made:
  `sleep_mode_enter_sleep()` now does what the DPD function does (not yet run).
- **Still to learn:** whether `sleep 30 0` (no retention) wakes, and what the LEDs do after a retention wake (the blue LED is
  switched on early in `app_main()`, and the blinky task then alternates both), which shows whether the application starts.

### Part E, second result: Power-down with retention now wakes (Charles, 21 September 2026)

Logs (renamed): `parte_1_retention1_wake_hung_log.txt` (the hung wake, before the fix) and `parte_2_retention1_wake_ok_log.txt`
(after the fix). The new image was built at 19:07:44 and was burned to the other flash slot (`slot flash_offset 0x00100000`
in the boot text; that is only the A/B slot rotation).

- **The change to the boot-ROM wake clock fixed the wake.** `sleep 30 1` now sleeps at **1.5 mA** and wakes after about 30 s with
  the LEDs blinking and the console working. The boot text is `Wakeup_event = 0x0040 ... SB_timer_int` and the app recognises
  it as a timer wake. So the cause was the boot ROM being told to wake at the 400 MHz PLL setting; the fix, copying the DPD path (RC
  oscillator, PLL disabled), is what section 9 of the application note describes.
- **`clocks` after the wake shows the normal state**: PLL 400 MHz from the crystal, HSC and LSC on the PLL, the UART reference on the
  crystal, every clock enable set. So the boot loader restores the clocks on a Power-down wake, as on a DPD wake.
- **The bootloader runs on a Power-down wake, and so does its console banner.** The log shows `1st BL Modem Build ...`, `slot flash_offset`,
  `HX_DSP_FLAG` and `jump_addr=0x10000000` for every wake. (In the log of this first fixed run there were extra `New MemDesp ...PASS` and
  `set_memory_s_ns` lines. **That was wrong to read as a Power-down feature:** the later log shows the same four lines for every DPD and Power-down wake, and the
  extra lines only belong to the first boot of a newly burned image.)
- **Timing from this run.** First bootloader line to `Starting CLI Task`: 22 ms after this wake and 24 to 28 ms after earlier DPD wakes. See Part E, third result, for the
  measurements that separate them.
- **What was added to check it (run in the third result):** at every boot the app prints `Retention check`: it keeps a magic value in the `.noinit`
  section, which the start-up code does not clear. If a retained wake works the value survives and a wake counter counts up; after DPD
  or a power-cycle it says the RAM was not kept. The `.noinit` variables are in the system SRAM (the linker script puts `.noinit` and
  the driver library's code in `CM55M_S_SRAM`), so they check the SRAM retention.
- **A correction.** Earlier this document suggested retaining less (TCM only) because "the application lives in TCM". That is not
  supported: the linker script places the driver library's code and `.noinit` in the system SRAM, so the SRAM retention is probably
  needed as coded, and reducing it would need linker changes.
- **The programmed 30 s timer took 28.88 s of host time** (the sleep message at 19:10:46.091, the bootloader at 19:11:14.971). The Power-down
  wake timer (a stand-by timer) is clocked from the same 32 kHz RC oscillator as the RTC, so it is also about 4 % fast (30 / 28.9 = 1.04).
  All 32 kHz-derived timing, alarms and timers alike, is about 4 % fast.
- **The wake latency that matters** (from the WAKE edge to the application running) needs a hardware measurement: the blue LED pin (PB10) goes
  high early in `app_main()`, so an oscilloscope or logic analyser on the WAKE pin and PB10 would show it for DPD, `sleep 30 1` and `sleep 30 0`.

### Part E, third result: retention check and timing (Charles, 21 September 2026)

Log: `parte_3_retention_tests_log.txt` in the thread folder. Build 19:17:30, from a power-cycle, with the retention check in the boot text. In all the
`sleep 30 x` runs the current was about 1.5 mA, and in DPD 10 uA.

| Sleep | Wake | Retention check | Sleep message to first bootloader line | First BL line to app banner | First BL line to CLI task |
|---|---|---|---|---|---|
| DPD (alarm 30 RTC s) | timer | not kept | 28.40 s | 16 ms | 24 ms |
| DPD (alarm 30 RTC s) | timer | not kept | (first line of the log) | 13 ms | 30 ms |
| `sleep 30 1` | `SB_timer_int` | **kept, 1 in a row** | 28.81 s | 5 ms | 5 ms (outlier) |
| `sleep 30 1` | `SB_timer_int` | **kept, 2 in a row** | 28.81 s | 4 ms | 20 ms |
| `sleep 30 1` | `SB_timer_int` | **kept, 3 in a row** | 28.81 s | 5 ms | 17 ms |
| `sleep 30 1` | `SB_timer_int` | **kept, 4 in a row** | 28.81 s | 4 ms | 16 ms |
| `sleep 30 0` | `SB_timer_int` | not kept | 28.83 s | 16 ms | 37 ms |

- **Retention works.** With `sleep 30 1` the `.noinit` value survived four wakes in a row and the counter counted up. With `sleep 30 0` and with DPD the RAM was not kept.
- **Retention starts the application about 10 ms sooner.** The banner comes 4 to 5 ms after the first bootloader line, against 13 to 16 ms for DPD and for
  Power-down without retention. That is about the time to reload the application from flash. From the first bootloader line to the CLI task the difference is about the
  same (16 to 20 ms against 24 to 37 ms), with the caveat that the host timestamps have a few ms of jitter and there are few samples.
- **The 30 s programmed sleep took 28.81 s**, the same four times to within 4 ms. The Power-down timer is clocked from the same 32 kHz RC oscillator as the RTC,
  so that oscillator is about 4.1 % fast.
- **The sleep current was the same with and without retention** (1.5 mA), so keeping the RAM costs nothing measurable, and the 1.5 mA comes from
  elsewhere in the Power-down mode.
- The Power-down wake by the WAKE switch was not tested in this run.

### Power-down experiment (procedure)

From a power-cycle: type a character, `blink off`, `inactivity 3600`, then `sleep 30 1` (retention on)
and read the current while it sleeps. Watch when the boot messages appear after 30 s, and press the
WAKE switch on another run. Repeat with `sleep 30 0` (no retention). The boot log after a retention
wake should show no `1st BL` bootloader messages if the flash reload really is skipped. Record the
time from the wake event to the first console output, and the current during the sleep.

### PCA9574 and the extra DPD current (Charles, 23 September 2026)

Step 8 added the HM0360 camera and the PCA9574 I2C expander (used to test the sensor I2C bus - see
`ww500_minimal/doc/README.md`, "HM0360 camera"). Measuring DPD again, to check it still holds at 10 uA with
that hardware fitted, gave a surprise:

- With the HM0360 and the SD card both removed, and the PCA9574 fitted, DPD is **13.8 uA**, not 10 uA. The
  PCA9574 itself was expected to add under 1 uA.
- **Same with or without the SD card fitted.** Ruling in favour of neither being about the card itself.
- **The camera and FatFS application code were both ruled out.** `WW500_NO_CAMERA` and `WW500_NO_FATFS`
  (`ww500_minimal.mk` - build options that stop the image task and the FatFS task from ever being created;
  see `ww500_minimal/doc/README.md`, "Building without the camera or FatFS code") were tried alone and
  together. None of the four combinations changed the figure: still 13.8 uA every time, camera code present or
  absent, FatFS code present or absent.

So the extra current (13.8 - 10 = 3.8 uA) is not from either piece of new application code running. What is
left, in order of suspicion:

- **The PCA9574 itself**, drawing more than the under-1 uA expected. Not checked against its datasheet.
- **A floating pin.** The HM0360 module and the SD card may have carried pull-ups or pull-downs (for SDA/SCL,
  or the SPI MISO/DI line) that are now missing, leaving an input pin at a mid-rail voltage. 3.3 V / 3.8 uA is
  about 868 kOhm, which is the right order of magnitude for a floating CMOS input's leakage, not a real
  resistor - this is Charles's own observation. Not measured with a meter yet.

Not resolved. Next step: measure the DC voltage on SDA, SCL and the SPI MISO/DI pins with a high-impedance
meter, to see whether any sits away from a clean 0 V or 3.3 V.

## State of the code

Everything below was built and run on the bench.

- **Application changes made for the experiments** (all in `ww500_minimal`): `power_diag.c/.h` and the CLI commands `idle`, `clocks`, `clkoff`, `clkon`,
  `clkslow`, `clkpll`, `clkdiv`, `clkuart`, `clkfast`, `xtal` and `sleep`; `sleep_mode_enter_sleep()` (Power-down) brought back and changed to wake the boot ROM on the RC
  oscillator; the restore of the clocks before DPD in `blinky_task.c`; the `Retention check` and the Power-down wake decoding in `ww500_minimal.c`.
- **The experiment commands change nothing unless used.** They can stay as diagnostics or be removed.
- **Not run:** the `u55`, `i3c`, `puf`, `dma` and `sdio` parts of `clkoff`, the full combination with `clkdiv 16`, and the Power-down wake by the WAKE switch on the fixed build.
- **Not committed.** Nothing has been staged, committed or pushed. `ww.mk` still selects `ww500_minimal` and must be set back to `ww500_md` before any production
  build. `EPII_CM55M_APP_S/app/main.c` has an added `WW500_MINIMAL` block. `ww500_md` itself was not changed.
