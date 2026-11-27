# Counters lost, and other fixes

#### File: `README.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/README.md`
#### Author: Claude (Opus 5.5), for Charles Palmer
#### Date: 25 November 2026, updated 26 November 2026

## In short

- **Goal.** Fix AI processor faults found while testing other work. No new features.
- **Fixed: five faults.**
  - [#296](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/296), commands
    lost between "Sleep" and DPD. Committed `ae19db33`, issue closed. The same change stopped the RTC
    calibration repeating on most sleeps.
  - [#297](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/297), operational
    parameter counters lost from CONFIG.TXT. Benched on both images, committed `52e69958`, issue closed.
  - [#298](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/298), deployment ID and GPS position lost if CONFIG.TXT
    is damaged. Benched on both images, committed `e483dc7b`, issue closed.
  - [#207](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/207), a `setop` after Save State replied OK but was not saved (made
    likelier by #296). Benched, committed `6917be6d`, issue closed.
  - [#295](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/295), no EXIF date when the photo time is not plausible (before 2026), and
    image files never overwritten. Raised by Victor. Benched on both images, committed `da5f974b`,
    issue closed.
- **Found, not yet resolved:** four more (table below): #299, #300, ww-hardware #83, and one /RST
  that was ignored.
- **Sources.** The BLE processor's LoRaWAN work
  ([CLAUDE_note_AI_counters_lost.md](CLAUDE_note_AI_counters_lost.md)) and Victor's email of
  21 November (#296, #295).
- **This branch is also the Seeed PR for the lorawanPing work** (Charles, 25 November).
  `261106_lorawanPing` gets no more commits.

Branch `261125_countersLost`, from `261106_lorawanPing` at `d8690d72`.

## Documents

Read the record of a fix for its details. The rest is background.

| Document | What it is | State |
|---|---|---|
| [CLAUDE_296_sleep_order.md](CLAUDE_296_sleep_order.md) | **#296**: commands lost between "Sleep" and DPD. The fault, the options, the change, the bench results | Fixed, committed `ae19db33` |
| [CLAUDE_297_counters_lost.md](CLAUDE_297_counters_lost.md) | **#297**: operational parameter counters lost from CONFIG.TXT. The cause, the fix, self-test bit 12, the new image folder, the bench results | Fixed, committed `52e69958`, closed |
| [CLAUDE_295_no_exif_date.md](CLAUDE_295_no_exif_date.md) | **#295**: no EXIF date when the time is unknown, no image overwrite, and moving the photo EXIF code to `exif_photo.c` | Fixed, committed `da5f974b` |
| [CLAUDE_207_setop_after_save_state.md](CLAUDE_207_setop_after_save_state.md) | **#207**: a `setop` after Save State replies OK but is not saved. The fault, why #296 made it likelier, the fix, the bench results | Fixed, committed `6917be6d`, closed |
| [CLAUDE_298_deployment_id_flash.md](CLAUDE_298_deployment_id_flash.md) | **#298**: deployment ID and GPS position are lost if CONFIG.TXT is damaged. Found with #297. The issue text and the fix | Fixed, committed `e483dc7b`, closed |
| [CLAUDE_issue_83_BLE_selftest_bit12.md](CLAUDE_issue_83_BLE_selftest_bit12.md) | **ww-hardware #83**: bit 12's new meaning, and keeping AI event bits until reported. Follows #297 | Raised; for the BLE processor side |
| [CLAUDE_note_for_Victor_bit12.md](CLAUDE_note_for_Victor_bit12.md) | Note to Victor: bit 12's new meaning for the app and website, and ops 19 and 20 | Sent, 28 November |
| [CLAUDE_note_AI_counters_lost.md](CLAUDE_note_AI_counters_lost.md) | From the BLE processor Claude, 23 November: the counter loss (#297), the ignored /RST, the clock, `setop` checks. Copied unchanged from ww-hardware | Background |
| [logs/](logs) | `ai1` to `ai6`, `ble1` to `ble5`: #296. `ai7` to `ai10`, `ble7` to `ble10`: #297. `ai11` to `ai17`, `ble11` to `ble15`: #298. Each record describes its logs | Evidence |

## Found, not yet resolved

| Fault | Source | Issue | State |
|---|---|---|---|
| BLE processor: rename bit 12, keep AI event bits (12, 14) until reported | Follows #297; Victor's bit 14 question | [ww-hardware #83](https://github.com/wildlifeai/ww-hardware/issues/83) | Not started |
| The AI processor's RTC runs about 5 % fast in DPD | BLE note, section 6 | [#299](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/299) | Not started |
| One /RST ignored | BLE note, section 5 | None | Not explained |
| `setop` accepts any value for most op parameters | BLE note, section 7 | [#300](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/300) | Decisions needed (ranges; can counters be set) |

**[#299](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/299), the clock, found so far.** The RTC runs 4.6 to 5 % fast in DPD (59 minutes after about 21 hours;
7 min 49 s after 2.5 hours in `logs/ai9.txt`), and a cold boot or /RST restarts it from 2024. The
BLE processor's `setutc` at each wake corrects photos; the RTC itself is rewritten only when more
than 10 minutes out.

## Not for this branch

- **ww-hardware #77** was rebuilt by Victor as 0.30.61: pull before committing to it.
- **#247 and #288** are parked by Victor.

## Open items

- **[PR #304](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/304)** to `dev`,
  for #298, #207 and #295 (opened 28 November). The first, #301, was merged on 26 November with the
  lorawanPing work, #296 and #297 (Victor merged it with #284, #286 and #289).
- **The note to Victor** ([CLAUDE_note_for_Victor_bit12.md](CLAUDE_note_for_Victor_bit12.md)): bit 12,
  and ops 19 and 20 with a new deployment ID. Sent by email, 28 November.
- [ww-hardware #83](https://github.com/wildlifeai/ww-hardware/issues/83): bit 12 and AI event bits,
  for the BLE processor side.
- [#299](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/299): the RTC drift, to decide the approach.
- [#300](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/300): `setop` range checks, decisions needed. The app relies on setting ops 19
  and 20 with a new deployment ID, so they must stay settable.
- Not an issue: the ignored /RST (not explained, not seen again).

## Outcome

Five faults fixed, all benched on both camera images, all issues closed:

| Issue | Fault | Fix | Commit |
|---|---|---|---|
| [#296](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/296) | A command just after "Sleep" was lost | The RTC work now comes before "Sleep", which is the last thing before DPD. Also stopped the RTC calibration repeating on most sleeps | `ae19db33` |
| [#297](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/297) | Operational parameter counters lost from CONFIG.TXT | CONFIG.TXT written as `CONFIG.TMP` then renamed; an unreadable file not saved over; a loss reported by self-test bit 12 and new photos put in a new image folder | `52e69958` |
| [#298](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/298) | Deployment ID and GPS position lost if CONFIG.TXT is damaged | A flash copy, used for whatever CONFIG.TXT doesn't give: no card, a blank card, a damaged file, a missing `I`/`G` line | `e483dc7b` |
| [#207](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/207) | A `setop` after Save State replied OK but was not saved (made likelier by #296) | `setop`, `setdid`, `setgps` and `md` refused from Save State to DPD | `6917be6d` |
| [#295](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/295) | Photos stamped with a wrong date after a reset | No EXIF date if the photo time is before 2026, whatever its source; image files never overwritten; the photo EXIF code moved to `exif_photo.c` | `da5f974b` |

**Decisions on the way** (each explained in its record): self-test bit 12 now means "CONFIG.TXT
damaged" (was the PDM microphone, never fitted); the flash copy of the deployment ID and GPS position
applies per line; the EXIF date rule trusts no source; moving the RTC work before Save State was
dropped for #207 (it could overlap an SD card write).

**Handed on:** #299, #300 and ww-hardware #83 (above). The app should set ops 19 and 20 with a new
deployment ID (note to Victor).
