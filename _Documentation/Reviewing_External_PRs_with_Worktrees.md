# Reviewing an External PR: Worktrees + Meld + Eclipse/VSCode
#### CGP — 8 August 2026

## When to use this

Someone else (Victor, or anyone) has pushed a branch/PR with a lot of changes and you want to:

1. See exactly what changed, using [Meld](https://gnome.pages.gitlab.gnome.org/meld/) to diff two real local folders.
2. Dig into the code structure with an IDE (Eclipse or VSCode), and possibly edit it.
3. Record your own edits and observations without touching the author's branch, then push them somewhere safe.

This doc is the generic recipe. See also:
- [`Git_Branch_and_PR_Workflow.md`](Git_Branch_and_PR_Workflow.md) — the team's branch/PR conventions (`main` / `dev` / `feature-*`, rebasing, PRs). This doc follows those conventions; it doesn't replace them.
- [`Reviewing_Stacked_PRs.md`](Reviewing_Stacked_PRs.md) — the variant of this recipe for when several PRs build on each other, as with PR #141/#142/#140.

## The shape of the workflow

One `Seeed_Checking` folder holds **one real clone** (`Seeed_Grove_Vision_AI_Module_V2`) plus several `git worktree` folders alongside it — extra, real, independent working folders that all share the same underlying repository. No re-cloning, one `git fetch` updates everything, and Meld/Eclipse just see ordinary folders on disk.

```
Seeed_Checking\
  Seeed_Grove_Vision_AI_Module_V2\   <- the real clone, your everyday working copy
  pr<N>\                             <- worktree: your review of PR #<N>, on branch review/cgp-<N>
  compare\                           <- worktree: throwaway, read-only, for Meld's "other side"
  workspace_pr<N>\                   <- Eclipse workspace pointed at pr<N> (not part of git)
```

At any moment you should hold: your normal folder, at most one active review folder, and (briefly, while diffing) `compare`. Recycle rather than accumulate — see Step 7.

## Step 0 — protect any uncommitted work in your main working copy first

If `Seeed_Grove_Vision_AI_Module_V2` has uncommitted edits you're not ready to throw away, park them on their own branch before doing anything else:

```
cd D:\Development\wildlife.ai\Seeed_Checking\Seeed_Grove_Vision_AI_Module_V2
git switch -c review/cgp-<topic>
git add -A
git commit -m "review: CGP edits before discussion"
git push -u origin review/cgp-<topic>
```

Committing here doesn't decide anything — it just makes the edits safe, diffable, and pushable. `review: ...` / `WIP: ...` commit messages are fine for this; it's better than stashing because stashes are easy to lose track of.

## Step 1 — create your review worktree

```
cd D:\Development\wildlife.ai\Seeed_Checking\Seeed_Grove_Vision_AI_Module_V2
git fetch origin
git worktree add -b review/cgp-<N> ..\pr<N> origin/<feature-branch>
```

- `-b review/cgp-<N>` creates a **new branch of your own**, starting at GitHub's exact tip of `<feature-branch>` (PR #`<N>`), and checks it out into the new `..\pr<N>` folder.
- Because it's your own branch, you can commit freely without ever touching the shared `feature/...` branch, and it's always obvious later which commits are yours.
- `REVIEW_<N>.md` at the tip (if the author included one) is an offline copy of the PR description — a useful scaffold for your own report.

## Step 2 — create a throwaway "compare" folder

To see PR #`<N>`'s changes in isolation, Meld needs something to diff it against — usually the branch it's based on:

```
git worktree add ..\compare --detach origin/<base-branch>
```

`--detach` means no branch is created, just that exact commit checked out read-only ("detached HEAD") — appropriate since this folder only exists for reading, not editing.

*(Which branch counts as `<base-branch>` isn't always `dev` — see [`Reviewing_Stacked_PRs.md`](Reviewing_Stacked_PRs.md) when PRs are stacked on each other.)*

## Step 3 — diff with Meld

Compare `..\compare` ↔ `..\pr<N>`.

**Meld → Edit → File Filters** — add filters for `.git`, `obj_*`, `NUL`. A worktree's `.git` is a small text file, not a folder, and would otherwise show as a spurious diff; `obj_*` is build output; `NUL` is a Windows artifact some tools leave behind.

When you're done comparing:

```
git worktree remove ..\compare
```

Thirty seconds to recreate whenever you need it again — no reason to leave it lying around.

## Step 4 — open in an IDE

### Eclipse

1. Make a workspace **outside** the repository: `mkdir workspace_pr<N>`.
2. Launch Eclipse, switch to that workspace.
3. **File → Import → General → Existing Projects into Workspace**, browse to `..\pr<N>`.
4. Select the relevant folders (e.g. `EPII_CM55M_APP_S`, `_Documentation`, `_Tools`) — you don't need to import everything.
5. **Uncheck** "Copy projects into workspace" (you want Eclipse editing the worktree folder in place, not a copy).
6. Finish, then Clean + Build as normal.

### VSCode

No separate workspace-preparation step needed:

1. **File → Open Folder…**, point it at `..\pr<N>` directly (or use a multi-root workspace with `..\compare` and `..\pr<N>` both added, if you want to browse both side by side inside the editor).
2. VSCode's built-in Source Control view is already git-aware — it'll show the worktree's branch (`review/cgp-<N>`) and any changes.
3. Add the relevant build/IntelliSense extension (e.g. C/C++ or Makefile Tools) if you need to build from inside VSCode; otherwise it's fine as a pure code-reading/editing environment.

## Step 5 — make and commit changes

Edit in the worktree as needed. Commit small, and prefix messages per the team convention (`fix:`, `feat:`, `docs:` — see `Git_Branch_and_PR_Workflow.md`):

```
git add -A
git commit -m "fix: short description of what and why"
```

These commits land on your `review/cgp-<N>` branch only.

## Step 6 — finish: commit and push your review branch

Once your report/edits are ready:

```
git status
git add -A
git status
git commit -m "docs: add CGP code review report for PR #<N>"
git push -u origin review/cgp-<N>
```

Check `git status` before *and* after `git add -A` — confirm it only contains the files you intend (your report, and any deliberate source edits), not build artifacts. This pushes your review as its own branch for visibility/safekeeping. It does **not** touch `dev`, `main`, or the author's `feature/...` branch, and does not open a PR by itself. If you want the changes considered for merging, open a PR from `review/cgp-<N>` into the feature branch it was reviewing (see `Git_Branch_and_PR_Workflow.md` §5).

## Step 7 — housekeeping / recycling

- `git worktree list` — shows every worktree and what branch/commit it's on. Useful to sanity-check things weeks later.
- `git worktree remove ..\pr<N>` once a review is finished and pushed (or the PR has merged) — no reason to keep a stale review folder around. Follow with `git worktree prune` to clean up any leftover admin records.
- Git refuses to let the same branch be checked out in two worktrees at once — this is *why* the recipe uses your own `review/cgp-<N>` branch rather than checking out the author's `feature/...` branch directly: you get a real, editable folder without ever needing exclusive use of their branch.
- When moving to the next PR, remove the finished review worktree and repeat Steps 1–6 with the new PR number/branch.

## Appendix A — what `git worktree` actually does

- The real repository (`Seeed_Grove_Vision_AI_Module_V2\.git`) holds the full object database, all refs, and config — this is the one copy of everything.
- Each worktree folder (`pr142`, `compare`, …) contains only a tiny **`.git` file** (not a folder!) with one line: `gitdir: <path to an admin subfolder>`. That admin subfolder lives inside the real repo at `.git\worktrees\<name>\` and holds just that worktree's own `HEAD` and index — everything else is shared.
- This is why creating a worktree is fast and cheap (no second copy of the object database), and why a single `git fetch` in the main repo makes new commits visible to every worktree immediately.
- `git worktree add -b <branch> <path> <start-point>` — create a new branch at `<start-point>`, check it out into a new folder at `<path>`, wire it up as above.
- `git worktree add <path> --detach <start-point>` — same, but no branch; just that commit, read-only.
- `git worktree remove <path>` — properly tears down both the folder and its admin subfolder. **Never delete a worktree folder manually in Explorer** — that leaves orphaned admin records behind; always use `git worktree remove`.

## Appendix B — two gotchas found while setting this up (7–8 Aug 2026)

**1. Worktree paths registered by Windows git break when accessed from WSL.**
If a worktree is created using native Windows git (e.g. from Eclipse, cmd, or PowerShell), the `gitdir:` pointer files are written using Windows-style paths (`D:\...`). A WSL `git` binary can't resolve those as filesystem paths, so `git` commands run *from a WSL shell* inside that worktree folder fail (`fatal: not a git repository: ...`), and `git worktree list` run from the main repo will mark those worktrees **"prunable"** even though the folders genuinely exist — it's just checking a path format it can't interpret, not confirming absence. **Fix if you need it:** `git worktree repair` from the main repo re-synchronises the recorded paths. Otherwise, simplest is to stay consistent — do worktree git operations either always from Windows-native tools, or always from WSL, not mixed.

**2. `git status` can be very slow (or hang) on this tree over a WSL/`\\mnt\\d` mount.**
The firmware tree is large (build objects, external libs, etc.), and status/diff operations that need to stat every file get noticeably slow when the filesystem is accessed via WSL's `drvfs` layer rather than natively. Not a git bug — if you're checking status of a worktree from WSL and it seems to hang, try again from a native Windows shell, or scope the command to a specific path (`git status -- <path>`) rather than the whole tree.
