# Git guardrails, building and flashing

This is a fork with an upstream, several worktrees and two firmware slots. Most of what goes
wrong here goes wrong before any code is compiled.

# 2. Git guardrails

* **Never rebase or force-push** the stacked feature branches, the stack breaks and
  reviewers' diff bases move.
* Review edits go on `review/<name>-<topic>` branches, not directly on shared feature
  branches; they are cherry-picked across after discussion.
* Ask the maintainer before pushing to any shared branch. Commit messages use
  conventional prefixes (`feat:`, `fix:`, `docs:`, `ci:`).
* **Check whether the clone is shallow before any branch analysis.** A shallow clone makes
  `git merge-base` return *empty* rather than fail, so ahead/behind counts, fork points and
  overlap tables come out confidently wrong, and a trial merge dies with `refusing to merge
  unrelated histories`. Test with `test -f "$(git rev-parse --git-common-dir)/shallow"` and
  fix with `git fetch --unshallow`, about a minute. Two-dot `git diff A B` compares trees
  directly and stays correct either way.
* **Never commit build churn**: `prebuilt_libs/**/*.a` deltas, `we2_image_gen_local*/`
  outputs, stray `NUL` files, `obj_*` trees. Check `git status` before staging. The three
  archives a normal build recompiles (`libcommon.a`, `libfatfs.a`, `libtrustzone_cfg.a`)
  and `output.img` are untracked (#198), so a modified `.a` in `git status` now means
  something rebuilt a real input, e.g. `make WW500_FAST_LIBS=n`. Restore it, never commit
  it, unless the PR is a deliberate SDK or toolchain update labelled
  `intended-artifact-change`.

# 3. Build and flash invariants

* Toolchain is pinned: **Arm GNU 14.3.rel1**. Build under WSL/Linux;
  `make clean` between camera variants is **mandatory** (objects don't encode the `-D`
  flags). Both variants must build, a change that compiles for one only is broken.
* **`make` runs image generation itself** (`ww500_md/mk/image_gen.mk`, RC24M profile) and
  writes both `output_case1_sec_wlcsp/output.img` and an 8.3 `VYMDDHMM.IMG` copy named for
  the variant (`R`/`H`). Do **not** run `we2_local_image_gen` by hand, it destroys the
  image make just built. No renaming between variants is needed.
* **A failed secure-boot certificate step does not fail the build**, and size only catches
  it on one variant. RP3 goes 487424 signed to 462848 certless; **HM0360 is 462848 either
  way**. So 462848 is both a good HM0360 image and an unsigned RP3 one. Check the build log
  for the `FileNotFoundError` traceback and that `secureboot_tool/cert/ICVSBContent.crt`
  exists. Never gitignore `secureboot_tool/cert/cfg/*.cfg`: they are hand-authored build
  inputs and nothing regenerates them.
* SD-card firmware files: **8.3 filenames** in `/MANIFEST` (FatFS has no LFN support).
  The `VYMDDHMM.IMG` name make emits already satisfies this.
* Device consoles: two USB serial ports, the Himax console is the one printing clean
  text at **921600 baud**; the other is the BLE debug UART. Probe for it every session
  (§5), never hard-code the COM number.
* **X-Modem is a normal bench path, not only recovery**, the way to get locally built
  images onto a device with no SD card. Each burn writes the **backup** slot and makes it
  active, so two consecutive burns fill both slots, and a final `switchslot` re-labels the
  one left behind. Runbook: `_Documentation/firmware_update_and_recovery.md`; the script
  timing that matters is in §5.
