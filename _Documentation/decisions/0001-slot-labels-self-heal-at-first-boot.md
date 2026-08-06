# 0001. Slot camera-labels self-heal at first boot

Status: Accepted (Aug 2026, bench-validated)
Thread: [../development reports/2026-07_pr141-review-cgp/](../development%20reports/2026-07_pr141-review-cgp/README.md)

## Context

The A/B firmware slots hold different camera variants (RP3 day / HM0360 night); automatic
switching must know which variant sits in each slot. Labels could be written when an image
is *flashed*, or by the image itself when it *boots*. Flash-time labelling can lie (an
XMODEM recovery burn wipes the selector sector wholesale; a slot write can be interrupted;
the flasher can't know what a hand-supplied binary contains), and "first boot of this
image" cannot be inferred from the cold/warm boot classification (an interrupted OTA makes
a plain DPD wake the new image's first boot).

## Decision

Each firmware image records its own compile-time variant into the WWSM record on **every
boot** (`cameraSwitch_labelBootSlot()`, cheap no-write path when already correct). The
`firmware` update deliberately clears the target slot's label to *unknown*; XMODEM burns
wipe both labels; `slots` reports `unknown` honestly. Automatic switching refuses to
switch into an unlabelled slot; manual `switchslot` requires only a programmed image and
is the bootstrap path.

## Consequences

Labels are eventually correct with no trusted flasher and no boot-type inference, at a
steady-state cost of 3 short SPI reads per boot (a planned XIP-mapped read removes even
those). A freshly programmed device shows `unknown` until each slot has booted once —
designed behaviour, documented in `firmware_update_and_recovery.md`. Never gate the label
call on cold boot.
