# Ready-now fixes, batch 1: CI matrix, filename rejection, build docs

#### File: `README.md`
#### Author: Claude (Opus 5), reviewed by Victor Anton
#### 18 August 2026

**Status:** open. PR #191.

The first batch of `ready-now` findings from the July to August PR reviews: the ones that
were conflict-free and could be done without waiting for #143 or #144. Three issues fixed,
plus one documentation correction found while testing them.

## Outcome

### The PR gate was checking the wrong thing ([#156])

The gate compiled **one** camera variant on Arm GNU **14.2**, while the release workflow,
both developers and every local build use **14.3.rel1**. So a change that broke the other
variant, or that only 14.3 rejects, passed review.

Not hypothetical. Both CI breakages this month reached `dev` through that gap ([#188]):
the unguarded `D:/hxbuild` block that created a directory named `D:` on Linux, and the
image-generation binaries stored without the executable bit. Each was found only after
merging.

Now a two-leg matrix passing `CIS_SUPPORT_INAPP_MODEL`, pinned to 14.3.rel1, with
`fail-fast: false` so a break reads as variant-specific or common at a glance, toolchain
caching, and an explicit check that an `.elf` was produced.

Verified by building both legs before changing the workflow: RP3 8,424,672 bytes, HM0360
8,148,012 bytes, zero errors, each log confirming the camera actually selected. 13 warnings
each, all pre-existing (nine from vendor `Driver_SPI.c`).

### `firmware` truncated long filenames ([#155])

Two files disagreed about one limit. `prvFirmwareCommand` accepted 63 characters;
`xip_update_firmware_from_sd` builds its path in a `MAX_FIRMWARE_NAME_LEN` (13) buffer, so
`snprintf` truncated silently and the failure named a file the user never typed.

With a CRC argument it is worse: the CRC check builds its own full-length path, so it reads
the real file and reports a match, and only the update fails "not found".

**Before**, on `WW500_C02 15:22:59 Aug  6 2026`:

```
cmd> firmware WW500_RP3_20260818.IMG
firmware: /MANIFEST/WW500_RP3_202 not found on SD card
Firmware update FAILED (error -1). Existing firmware unchanged.

cmd> firmware NOSUCH.IMG
firmware: /MANIFEST/NOSUCH.IMG not found on SD card
```

`WW500_RP3_202` is exactly 13 characters, matching the arithmetic: `sizeof("/MANIFEST")`
10, plus `MAX_FIRMWARE_NAME_LEN` 13, plus 1, so `snprintf` writes `/MANIFEST/` and has room
for 13 more.

**After**, built and XMODEM-flashed as `R6818G11.IMG`, running `WW500_C02 16:11:03 Aug 18
2026`:

```
cmd> firmware WW500_RP3_20260818.IMG
Error: 'WW500_RP3_20260818.IMG' is 22 chars; firmware names are 8.3 format, max 12
(e.g. H6818C33.IMG). The SD card has no long-filename support.

cmd> firmware NOSUCH.IMG
firmware: /MANIFEST/NOSUCH.IMG not found on SD card
```

The control matters: a valid 8.3 name is still accepted for processing and fails only
because the file is absent, so nothing was over-tightened.

**The fix targets the disagreement, not the symptom.** `MAX_FIRMWARE_NAME_LEN` moves from
private in `xip_manager.c` to `xip_manager.h`, so callers can see the limit this code
enforces. Rejection rather than a longer buffer, because 8.3 is a hard FatFs constraint:
the app builds with `FF_USE_LFN 0` (`ww500_md/ffconf.h:116`), so a longer name cannot be
opened at all.

### Build documentation had fallen behind ([#179])

`dual_image_build_and_flash.md` still described running `we2_local_image_gen` by hand and
renaming the output. Image generation is part of the build now (`mk/image_gen.mk` hangs
`gen_image` and `device_image` off `all:`). Rewritten, with the manual route kept as a
fallback, and the `D:/hxbuild` and `WW500_FAST_LIBS` switches documented.

### The X-Modem runbook was wrong about which slot it burns

Found while flashing the [#155] fix, and not previously filed as an issue.
`firmware_update_and_recovery.md` said the bootloader "burns the image into the slot it was
trying to boot". It does not:

```
slot flash_offset 0x00000000        <- was booting slot A
slot FlashOffset 0x00100000         <- burns slot B
backup slot header
```

It burns the **backup** slot and restarts into it, leaving the running image intact. That
is better than documented and consistent with the rest of the update model, but during a
recovery the running image may be the only good one left, so the doc was telling an
engineer exactly the wrong thing.

The same commit adds `PYTHONIOENCODING=utf-8` to the documented command. Without it
`xmodem_send.py`'s progress bar raises `UnicodeEncodeError` on a Windows console and kills
the transfer **mid-flash**. That happened during this work: 126 packets sent, `error: 0` on
every one, killed by a cosmetic bug at the worst possible moment. Re-running recovers,
since the bootloader is in a separate flash region.

## Open items

| Issue | Description | State |
|---|---|---|
| [#156] | PR-gate CI builds only one camera variant, on the wrong toolchain | fixed here |
| [#155] | `firmware` command truncates filenames longer than 13 characters | fixed here, verified on hardware |
| [#179] | Update `dual_image_build_and_flash.md` for the new make image targets | fixed here |
| [#190] | `device_image` deletes the other camera variant's image | open, one-line fix, CGP's tooling |
| [#194] | Every Linux build writes a stray file named `NUL` | open |
| [#198] | Build outputs are tracked in git, so every build dirties the repo | open, needs a design decision |

## Questions for the reviewer

- Is rejecting an over-long name **up front** right, versus accepting and truncating with a
  warning? Rejection was chosen because 8.3 is a hard FatFs limit, not a buffer that could
  be enlarged.
- The matrix pins 14.3.rel1 and builds both variants. Should it also exercise a toolchain
  the other side of the pin?

## Related threads

- [PR #141 review](../2026-08-06_pr141-camera-features-review/README.md)
- [PR #142 review](../2026-08-08_pr142-ble-fast-transfer-review/README.md)
- [PR #140 review](../2026-08-10_pr140-rp3-image-quality-review/README.md)

[#155]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/155
[#156]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/156
[#179]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/179
[#188]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/188
[#190]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/190
[#194]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/194
[#198]: https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/issues/198
