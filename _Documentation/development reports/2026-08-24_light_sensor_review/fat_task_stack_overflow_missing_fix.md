# FAT task stack overflow in save_configuration() — fix existed on another branch, missing here

**What:** `save_configuration()` in `fatfs_task.c` declared a stack-local array
`char comment_lines[MAXNUMCOMMENTS][MAXCOMMENTLENGTH]`, sized from
`MAXNUMCOMMENTS = OP_PARAMETER_NUM_ENTRIES + 5` (unparenthesised macro too). With
`OP_PARAMETER_NUM_ENTRIES` now 27, that's `32 × 80 = 2560` bytes, plus an 80-byte
`line` buffer — **~2640 bytes of stack in one function**, on the FAT task's ~4368-byte
(`1092`-word) stack. `save_configuration()` runs directly on the FAT task (called from
`handleEventForIdle()`), so this alone eats roughly 60% of that task's entire stack
budget before FatFs's own `f_open`/`f_gets`/`f_write` call chain, the SPI driver
underneath, or the caller's own frames are even counted.

**Symptom seen on device (26/27 Aug 2026):**
```
*** USAGE FAULT ***
CFSR  = 0x00020000
HFSR  = 0x00000000
SHCSR = 0x000F0008
UFSR  = 0x0002
  INVSTATE: Invalid processor state
```
(That particular capture also had a separate, unrelated `states` CLI command crash in
the same session — a NULL function-pointer call from `internalStates[]` being
under-populated relative to `NUMBEROFTASKS`. Fixed separately; not part of this issue.)

**Where:**
- `EPII_CM55M_APP_S/app/ww_projects/ww500_md/fatfs_task.c` — `MAXNUMCOMMENTS` (was
  line 102) and `save_configuration()`'s `comment_lines`/`line` locals (was ~line 1151).
- FAT task stack size: `fatfs_createTask()` →
  `3 * configMINIMAL_STACK_SIZE + CLI_CMD_LINE_BUF_SIZE + CLI_OUTPUT_BUF_SIZE` words
  = `3×256 + 80 + 244` = 1092 words = 4368 bytes.

**This was already fixed elsewhere and just never reached this branch:**
- Commit `13bda489` (Victor, 11 July 2026), `fix(ww500_md): FatFS task stack overflow
  in save_configuration` — same root cause, identified independently, fixed by making
  `comment_lines` `static` (safe: only the FAT task ever calls this function) and
  parenthesising the macro. Present on `feat/hires-capture-fixes`,
  `feat/md-instrumentation`, `integration/dev-preview-20260722` — **not** on
  `ae_review` (or, as far as checked, `main`/`dev`).
- `ae_review`'s own history independently touched the *symptom* of this same bug:
  commit `de0e28f4` ("Added more code in UsageFault_Handler") added a detailed
  CFSR/HFSR/UFSR decoder to `hardfault_handler.c` (credited to a ChatGPT session)
  right after hitting a crash whose comment references
  `"FatFS Task received event 'Save State'"` — i.e. this exact `save_configuration()`
  path — but without connecting it to Victor's already-committed root-cause fix.

**Applied on `ae_review` (27 Aug 2026):** ported the same two changes directly into
the current (diverged) `fatfs_task.c` — `comment_lines` is now `static`, and
`MAXNUMCOMMENTS` is parenthesised. Not yet build/device-verified as part of this
change; do that before considering it closed.

**Bigger-picture question worth an issue on its own:** this is a "proven fix on one
branch, silently missing on another" situation. Worth asking: are there other
branch-divergence gaps like this one across `feat/hires-capture-fixes`,
`feat/md-instrumentation`, `integration/dev-preview-20260722`, `ae_review`, `main`,
and `dev` that are worth a deliberate audit/merge-forward pass, rather than relying on
each branch's own developer (or Claude session) to independently rediscover bugs
already fixed elsewhere?

**Suggested fix:** the code fix above is already applied on `ae_review`. Separately,
consider filing the branch-divergence question as its own issue/discussion — it's a
process gap, not a code bug, and this file doesn't attempt to answer it.
