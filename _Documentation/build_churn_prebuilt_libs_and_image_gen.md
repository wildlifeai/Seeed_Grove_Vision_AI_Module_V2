# Local builds always leave tracked files dirty (prebuilt_libs/*.a, output.img)

**What:** Three tracked static libraries under `EPII_CM55M_APP_S/prebuilt_libs/gnu/`
(`libcommon.a`, `libfatfs.a`, `libtrustzone_cfg.a`) — and `we2_image_gen_local_dpd/
output_case1_sec_wlcsp/output.img` — get rewritten by every normal local build, so
`git status` always shows them as modified. `*.a` is already in the root `.gitignore`
(and `output.img` has an explicit "keep the golden reference image" carve-out there),
but neither stops the diff from reappearing, because `.gitignore` only affects
*untracked* files — it has no effect on files already committed to the repo.

**Where:**
- `EPII_CM55M_APP_S/prebuilt_libs/gnu/{libcommon.a,libfatfs.a,libtrustzone_cfg.a}`
- `we2_image_gen_local_dpd/output_case1_sec_wlcsp/output.img`
- Mechanism: `EPII_CM55M_APP_S/app/ww_projects/ww500_md/ww500_md.mk` (comment near
  `PREBUILT_LIB`) — every *normal* build recompiles these libraries from source and
  copies the fresh `.a` back into `prebuilt_libs/`, so that a later *forced*
  (`PREBUILT_LIB`) build can skip recompiling TFLM/CMSIS-NN from source and reuse the
  checked-in archive instead. This is deliberate: the checked-in `.a` is a build
  cache for a fresh checkout, not accidental churn — but it does mean it changes on
  every normal build.
- Existing (manual) mitigation: `AGENTS.md`'s "Never commit build churn" rule already
  covers this — the expected practice is to `git checkout -- <path>` before
  committing, not to untrack the files.

**Evidence / context:** Found 26 August 2026 while building both camera variants
(`cis_imx708`/`cis_hm0360`) to verify the light-sensor refactor
(`_Documentation/development reports/2026-08-24_light_sensor_review/`). The manual
"discard before commit" workaround is easy to miss — these files blend into a large
unrelated `git status` listing (the repo is mid a separate CRLF→LF line-ending
normalization pass), and there's no automated guard against accidentally staging
them.

**Options considered (not yet decided):**
1. **Extend the existing `.husky/pre-commit` hook** to automatically
   `git restore --staged --worktree` these specific paths before every commit.
   Team-wide and automatic — the repo already has hook infrastructure
   (`.husky/pre-commit` currently only blocks commits to protected branches), so this
   would be a natural, low-risk addition there.
2. **`git update-index --skip-worktree`** on the 3 `.a` files, per developer clone —
   simple, but local-only (not shared via the repo) and a known git footgun (easy to
   forget it's set; can behave oddly if the tracked content ever needs a real,
   intentional change).
3. **Untrack them** (`git rm --cached`) and let the existing `.gitignore` `*.a` rule
   take over permanently — the cleanest long-term fix, but a bigger call: it removes
   the fresh-checkout build-acceleration cache `ww500_md.mk` relies on for the forced/
   `PREBUILT_LIB` build path. A fresh clone would then need one normal build before
   that fast-build path works. Needs confirming nothing (e.g. CI) depends on the
   checked-in `.a` being present without a prior build.

**Suggested fix:** Option 1 (hook) is the least disruptive and doesn't require a
decision about the `PREBUILT_LIB` fresh-checkout behaviour — recommended as the
default unless there's appetite to make the bigger call in option 3.
