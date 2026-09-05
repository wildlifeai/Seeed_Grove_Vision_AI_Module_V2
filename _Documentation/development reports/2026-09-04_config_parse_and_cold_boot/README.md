# CONFIG.TXT parsing and the RP3 cold-boot capture

#### File: README.md
#### Author: Claude (Opus 5), reviewed by Victor Anton
#### 4 to 5 September 2026

One bench session verifying Charles's `ae_review` fixes on hardware before merging them to
`dev`, which turned up a regression in one of them and two findings of its own.

**Status:** closed. `ae_review` merged to `dev` as #215.

## Outcome

**Charles's fixes are confirmed on hardware.** #202, #203 and #205 each reproduced their
original failure and then behaved correctly on the fixed build. The op0 fix was checked
both ways: a card carrying five of the real over-long comment lines kept `op0 = 42` instead
of being zeroed, and on an empty card the counter stepped one per image written and
survived two deep-sleep power cycles.

**A regression was found in the op0 fix and fixed before the merge.** `isNumericToken()`
requires every character to be a digit, but FatFS here is built with `FF_USE_STRFUNC 1`,
"without LF-CRLF conversion", so `f_gets()` hands the CR to the parser and `"2\r"` is
rejected. Every parameter on a CRLF card fell back to its compiled default. The shipped
`MANIFEST/CONFIG.TXT` is CRLF, so this hit freshly prepared cards, the ones carrying the
user's own configuration. Fixed by stripping the CR as well as the LF.

**Two build inputs had been removed from `ae_review`.** `cert/cfg/ICVSBContent.cfg` and
`OEMSBContent.cfg` are hand-authored and nothing regenerates them, so merging as it stood
would have deleted them from `dev` and every clean clone would have built unsigned images
with nothing reporting a failure. Restored in the same PR.

**The method note worth keeping.** The device rewrites `CONFIG.TXT` on its first sleep, so
a test card stops being the card you wrote before you can read the result. Setting the FAT
read-only attribute on the file stops the device overwriting it and makes the parse
observable. Two runs were wasted before this was understood.

## Open items

- [#217](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/217) Remove comment support from CONFIG.TXT, agreed with Charles
- [#218](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/218) The device writes LF, everything that prepares a card writes CRLF
- [#219](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/219) A stuck device relays the same unhandled event over and over
- [#220](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/220) RP3 cold-boot capture always fails; the same capture works after a wake

## Logs

Both consoles on one timeline, Himax at 921600 and nRF at 115200.

| File | What it shows |
|---|---|
| [`crlf_readonly_card.log`](logs/crlf_readonly_card.log) | A CRLF card made read-only. `op13` and `op22` read the card's values, not the defaults |
| [`op0_parse.log`](logs/op0_parse.log) | `op0 = 42` surviving five over-long comment lines whose tails used to zero it |
| [`op0_persistence.log`](logs/op0_persistence.log) | The sequence number stepping 0, 1, 2, 3 against four written JPGs, across two DPD cycles |
| [`fresh_card.log`](logs/fresh_card.log) | An empty card: `'CONFIG.TXT' NOT found`, the directories created, and the file the firmware writes from defaults |
| [`cold_boot_and_relay.log`](logs/cold_boot_and_relay.log) | #220 and #219 in one run on `dev`: five failed restarts, then the same capture in 52 ms after the wake |
| [`device_written_CONFIG.TXT`](logs/device_written_CONFIG.TXT) | What the device writes: 261 bytes, no comments, LF endings, RTC unset |

Device: WW500, `dev` at `a8d5a2a8` for the final runs, `ae_review` revisions `ee65771f`,
`e8b7feb5` and `4bcb722c` earlier.
