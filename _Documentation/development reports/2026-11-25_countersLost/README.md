# Counters lost, and other fixes

#### File: `README.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/README.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 25 November 2026

## In short

- **Goal.** Fix a set of AI processor faults found while testing other work. No new features.
- **Sources.** The BLE processor's LoRaWAN work (counters lost after resets, the clock, `setop`
  checks) and Victor's email of 21 November (#296, #295).
- **Done so far: #296**, the one blocking the merge. A command sent just after the AI processor
  said "Sleep" was lost. Fixed and benched on both images; not yet committed. With it, item 7:
  the RTC calibration no longer repeats on most sleeps.
- **Next:** item 1 (counters lost after resets), item 3 (the clock), #295 (no EXIF date when the
  time is unknown).
- **This branch is the Seeed PR** for the lorawanPing work too (Charles, 25 November).
  `261106_lorawanPing` gets no more commits.

Branch `261125_countersLost`, from `261106_lorawanPing` at `d8690d72`.

## Documents

Read the #296 record for the fix. The rest is background.

| Document | What it is | State |
|---|---|---|
| [CLAUDE_296_sleep_order.md](CLAUDE_296_sleep_order.md) | **#296**: the fault, the options, the change and the bench results | Fixed, not committed |
| [CLAUDE_note_AI_counters_lost.md](CLAUDE_note_AI_counters_lost.md) | From the BLE processor Claude: items 1 to 4 (copied unchanged from ww-hardware) | Background, items open |
| [Victors_Issues.eml](Victors_Issues.eml) | Victor's email of 21 November | Background |
| [logs/](logs) | Bench logs `ai1` to `ai6`, `ble1` to `ble5`, described in the #296 record | Evidence |

## Items

| # | Item | Source | Issue | State |
|---|---|---|---|---|
| 1 | Ops 0, 1, 2 (photo number, NN counts) lost after /RST resets | BLE note, 1 to 4 | | Not started |
| 2 | One /RST ignored | BLE note, 5 | | Not started |
| 3 | The AI processor's clock wrong | BLE note, 6 | | Not started |
| 4 | Range checks for every op in `setop` | BLE note, 7 | | To discuss |
| 5 | A command just after "Sleep" is lost. **Blocked the merge** | Victor | [#296](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/296) | Fixed, benched both images, not committed |
| 6 | No EXIF date when the camera does not know the time | Victor | [#295](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/295) | Rule decided, not coded |
| 7 | RTC calibration repeats on most sleeps | Found with #296 | | Fixed with item 5, not committed |

**Item 3, found so far.** The RTC runs 4.6 % fast in DPD (59 minutes after about 21 hours), and a
cold boot or /RST restarts it from 2024. The BLE processor's `setutc` at each wake corrects
photos; the RTC itself is rewritten only when more than 10 minutes out.

**Item 6, decided 25 November (Charles).** No new flag:

- `setutc` this wake: date from it, as now.
- No `setutc`, RTC year before `RTC_FIRST_VALID_YEAR` (still 2024 after a reset): no
  `DateTimeOriginal` or `CreateDate`, and a console line saying why.
- No `setutc`, RTC year valid: date from the RTC, as #295 asks.

## Not for this branch

- **Victor wants PRs in both repos,** BLE version 0.30.62, to merge with his four tested PRs.
- **ww-hardware #77** was rebuilt by Victor as 0.30.61: pull before committing to it.
- **Victor's bit 14 question** (should the BLE processor hold it until the app reads it) is for
  the BLE processor.
- **#247 and #288** are parked by Victor.

**Question for Victor:** "merge all six together": presumably Seeed #284, #286, #289,
ww-hardware #77 and the two lorawanPing PRs. Tell him the Seeed one is `261125_countersLost`,
including #296.

## Open items

#295, #296. Items 1 to 4 and 7 are not yet GitHub issues.

## Outcome

Not yet written.
