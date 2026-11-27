# Counters lost, and other fixes

#### File: `README.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/README.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 25 November 2026, updated 26 November 2026

## In short

- **Goal.** Fix AI processor faults found while testing other work. No new features.
- **Fixed: three faults.**
  - [#296](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/296), commands
    lost between "Sleep" and DPD. Committed `ae19db33`, issue closed. The same change stopped the RTC
    calibration repeating on most sleeps.
  - [#297](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/297), operational
    parameter counters lost from CONFIG.TXT. Benched on both images, committed `52e69958`.
  - [#298](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/298), deployment ID and GPS position lost if CONFIG.TXT
    is damaged. Benched on both images, committed `e483dc7b`, issue closed.
- **Found, not yet resolved:** six more (table below): #207 (next), #295, #299, #300,
  ww-hardware #83, and one /RST that was ignored.
- **Sources.** The BLE processor's LoRaWAN work
  ([CLAUDE_note_AI_counters_lost.md](CLAUDE_note_AI_counters_lost.md)) and Victor's email of
  21 November ([Victors_Issues.eml](Victors_Issues.eml)).
- **This branch is also the Seeed PR for the lorawanPing work** (Charles, 25 November).
  `261106_lorawanPing` gets no more commits.

Branch `261125_countersLost`, from `261106_lorawanPing` at `d8690d72`.

## Documents

Read the record of a fix for its details. The drafts are for Charles to send. The rest is background.

| Document | What it is | State |
|---|---|---|
| [CLAUDE_296_sleep_order.md](CLAUDE_296_sleep_order.md) | **#296**: commands lost between "Sleep" and DPD. The fault, the options, the change, the bench results | Fixed, committed `ae19db33` |
| [CLAUDE_297_counters_lost.md](CLAUDE_297_counters_lost.md) | **#297**: operational parameter counters lost from CONFIG.TXT. The cause, the fix, self-test bit 12, the new image folder, the bench results | Fixed, committed `52e69958` |
| [CLAUDE_298_deployment_id_flash.md](CLAUDE_298_deployment_id_flash.md) | **#298**: deployment ID and GPS position are lost if CONFIG.TXT is damaged. Found with #297. The issue text and the fix | Fixed, committed `e483dc7b`, closed |
| [CLAUDE_issue_83_BLE_selftest_bit12.md](CLAUDE_issue_83_BLE_selftest_bit12.md) | **ww-hardware #83**: bit 12's new meaning, and keeping AI event bits until reported. Follows #297 | Draft, to raise |
| [CLAUDE_note_for_Victor_bit12.md](CLAUDE_note_for_Victor_bit12.md) | Note to Victor: bit 12's new meaning for the app and website | Draft, to send |
| [CLAUDE_note_AI_counters_lost.md](CLAUDE_note_AI_counters_lost.md) | From the BLE processor Claude, 23 November: the counter loss (#297), the ignored /RST, the clock, `setop` checks. Copied unchanged from ww-hardware | Background |
| [Victors_Issues.eml](Victors_Issues.eml) | Victor's email of 21 November: #296, #295 | Background |
| [logs/](logs) | `ai1` to `ai6`, `ble1` to `ble5`: #296. `ai7` to `ai10`, `ble7` to `ble10`: #297. `ai11` to `ai17`, `ble11` to `ble15`: #298. Each record describes its logs | Evidence |

## Found, not yet resolved

| Fault | Source | Issue | State |
|---|---|---|---|
| A `setop` after Save State replies OK but is not saved. Made more likely by #296 (the RTC work now sits after Save State) | Found checking `hardware-traps.md` | [#207](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/207) | Next. Plan: RTC work before Save State; refuse `setop` once Save State has started |
| BLE processor: rename bit 12, keep AI event bits (12, 14) until reported | Follows #297; Victor's bit 14 question | [ww-hardware #83](https://github.com/wildlifeai/ww-hardware/issues/83) | Not started |
| No EXIF date when the camera does not know the time | Victor | [#295](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/295) | Rule decided, not coded |
| The AI processor's RTC runs about 5 % fast in DPD | BLE note, section 6 | [#299](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/299) | Not started |
| One /RST ignored | BLE note, section 5 | None | Not explained |
| `setop` accepts any value for most op parameters | BLE note, section 7 | [#300](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/300) | Decisions needed (ranges; can counters be set) |

**#295, decided 25 November (Charles).** No new flag:

- `setutc` this wake: date from it, as now.
- No `setutc`, RTC year before `RTC_FIRST_VALID_YEAR` (still 2024 after a reset): no
  `DateTimeOriginal` or `CreateDate`, and a console line saying why.
- No `setutc`, RTC year valid: date from the RTC, as #295 asks.
- Also: after a reset with no time, 2024 photo times can repeat, and an image file with the same
  name is overwritten (`FA_CREATE_ALWAYS`). To fix with #295: open with `FA_CREATE_NEW` and step
  the sub-second digit until the name is free (comment added to #295, 26 November).

**[#299](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/299), the clock, found so far.** The RTC runs 4.6 to 5 % fast in DPD (59 minutes after about 21 hours;
7 min 49 s after 2.5 hours in `logs/ai9.txt`), and a cold boot or /RST restarts it from 2024. The
BLE processor's `setutc` at each wake corrects photos; the RTC itself is rewritten only when more
than 10 minutes out.

## Not for this branch

- **Victor wants PRs in both repos,** BLE version 0.30.62, to merge with his four tested PRs.
- **ww-hardware #77** was rebuilt by Victor as 0.30.61: pull before committing to it.
- **#247 and #288** are parked by Victor.

**Question for Victor:** "merge all six together": presumably Seeed #284, #286, #289,
ww-hardware #77 and the two lorawanPing PRs. Tell him the Seeed one is `261125_countersLost`,
including #296, #297 and #298.

## Open items

- [#297](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/297), [#298](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/298): fixed and committed, to merge.
- [#207](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/207): next.
- [#295](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/295): to code, with the overwrite guard.
- [ww-hardware #83](https://github.com/wildlifeai/ww-hardware/issues/83): for the BLE processor side.
- [#299](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/299): the RTC drift, to decide the approach.
- [#300](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/300): `setop` range checks, decisions needed.
  The app relies on setting ops 19 and 20 with a new deployment ID (27 November), so they must stay
  settable.
- Not an issue: the ignored /RST (not explained, not seen again).

## Outcome

Not yet written.
