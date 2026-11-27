# Note for Victor: self-test bit 12

#### File: `CLAUDE_note_for_Victor_bit12.md`
#### Path: `_Documentation/development reports/2026-11-25_countersLost/CLAUDE_note_for_Victor_bit12.md`
#### Author: Claude (Opus 5.5), for Charles Palmer to send to Victor
#### Date: 26 November 2026

## In short

- Text for an email or a comment. Copy what is below the line.
- Follows [#297](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/297) ([record](CLAUDE_297_counters_lost.md), thread [README](README.md)).

---

Hi Victor,

A cross-repo change to agree, from the lost counters (Seeed #297, operational parameter counters lost
from CONFIG.TXT):

**Self-test bit 12 (`0x1000`) changes meaning.** It was "PDM microphone fails", but no WW500 has a
microphone and the bit was never set. It now means **"CONFIG.TXT damaged"**: at that wake the AI
processor found CONFIG.TXT damaged or unreadable, so its counters (photo number, NN counts, boot
counts) restarted from 0, and new photos started a new image folder.

What it needs from you:

- **App and website:** change the label for bit 12. Suggested: "Camera settings file damaged:
  counters reset". A warning rather than a fault: the camera keeps working.
- **LoRaWAN decoder:** no change; it is one more bit in the existing self-test field.

Why it happened: the counters were kept only in CONFIG.TXT, and a save emptied the file before
rewriting it, so a reset at that moment lost them. That is fixed (the file is now written in full
before it replaces the old one). Bit 12 covers what is left, such as a card fault.

The bit is set for one wake only. Your bit 14 question (the BLE processor overwriting the AI bits
each wake) applies to it too; ww-hardware #83 asks the BLE processor to keep such bits until
they have been reported.

Also found: the deployment ID and GPS position are kept only in CONFIG.TXT as well, so a damaged
file loses them and new photos carry the zero deployment ID. Seeed #298 is to keep
them in flash too.

One more for the app: when it sets a new deployment ID (`setdid`), please also set ops 19 and 20
(`setop 19 0`, `setop 20 0`). They count the photos in the image folder and the folder number, and
belong to the card, not the deployment. Without that, the new deployment's photos go to
`/MEDIA/<new ID>/IMAGES.0NN` with the old deployment's folder number. The folder is chosen at boot,
so photos taken in the same wake still go to the old folder: best sent as the last commands of a
deployment session.

Charles
