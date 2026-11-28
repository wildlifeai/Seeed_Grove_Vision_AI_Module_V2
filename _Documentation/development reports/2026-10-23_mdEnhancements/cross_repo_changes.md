# Changes the BLE processor and app need to know about

#### File: `cross_repo_changes.md`
#### Author: Claude, for Charles Palmer
#### Date: 25 October 2026

AI processor changes on branch `1261023_mdEnhancements` that affect ww-hardware (BLE
processor), ww-mobile-app, ww-website or ww-backend. For the PR description and the `Decide:`
issue. Add to this list as further changes are made.

## Op parameters

| Op | Change | Who needs to act |
|---|---|---|
| 33 | `OP_PARAMETER_RFU_2` becomes `OP_PARAMETER_MD_BLOCK_THRESHOLD`: the device wakes when **more than** this many HM0360 MD cells show motion. Range 1-255, default 4 | ww-hardware: rename in `aiProcessor.h`. App, website, backend: name, range and default wherever op params are listed, shown or set |
| 17 | `OP_PARAMETER_MD_SENSITIVITY` is 1-3 only (1 = low, 2 = medium, 3 = high). 0 ("off") is no longer accepted: op 11 = 0 is the one way to switch MD off | App: remove any "off" sensitivity option, use op 11 = 0 instead. Website, backend: range |
| 17, 33 | Out-of-range values on a card are replaced at load (op 17 by 1, op 33 by 4). A card with op 17 = 0 and op 11 > 0 used to mean "MD off" and now wakes on motion | Anyone with deployed cards that use op 17 = 0 |
| 17, 33 | `setop 17` and `setop 33` now take effect on the HM0360 at once, not only at the next sleep | None: behaviour only |
| 11 | No change, but documented: the HM0360 sleep register stops at about 1997 ms, so a longer op 11 runs at that | App, website: consider limiting the input to 1997 |
| 22 | Firmware default (used when CONFIG.TXT has no op 22) back from 50 to 5, to match the shipped CONFIG.TXT (25 Oct) | Website: any default it shows or writes into the Setup_Package CONFIG.TXT |
| 30, 33 | Shipped `MANIFEST/CONFIG.TXT` now holds the firmware defaults: op 30 = 110 (was 0), op 33 = 4 (was 0) | Website: the "Prepare SD Card" CONFIG.TXT should match |

## Commands and replies (console and BLE `AI ...`)

| Command | Change | Who needs to act |
|---|---|---|
| `setop` | Rejects an out-of-range value for ops 17 and 33, with the reply `Error: <value> is not a valid value for op <n>.` and nothing saved. All other ops unchanged | App: handle the error reply when setting ops 17 or 33 |
| `md` | Accepts 1-3 only; the error reply is now `Error: Sensitivity must be 1, 2 or 3 (op 11 = 0 switches MD off).` | App, if it uses `AI md` |
| `interval` (new) | `interval <ms>` changes the interval of the capture burst in progress from its next image (1 ms to 15 min). Replies `Interval N ms from the next image`, or `No capture in progress: the next 'capture' sets the interval`. Not saved. Made for the live viewer | None required; available to the app as `AI interval` if wanted |

## Messages from the AI processor

| Message | Change | Who needs to act |
|---|---|---|
| `HM0360 woken by motion in N blocks:` + 32 hex bytes (new, 26 Oct) | Sent once after a motion wake, after the first image: the MD cells that woke the device. Same format as the existing `HM0360 motion in N blocks:` line | App: none required; it can show the wake cells if wanted |
| `HM0360 motion in N blocks:` (changed, 26 Oct) | No longer sent for the first image of a burst (always 0 there). A single-image capture now sends none | App: only if it expects one per image |

## Not relevant to other repos

- `mdstat` field in the live-preview frame JSON, and the `preview` stream generally: used only
  by `_Tools/live_view.py`.
- `mdth` console command: added and removed again on this branch.
