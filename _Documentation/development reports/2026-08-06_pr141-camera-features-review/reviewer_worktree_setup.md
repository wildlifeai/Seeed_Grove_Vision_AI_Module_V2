# Reviewing #142 and #140 with Meld — worktree recipe

`git worktree` gives you several *real folders* from your one Seeed_Checking repo — Meld and Eclipse work exactly as now, no extra clones, one `git fetch` updates everything. The pattern below keeps **one review folder at a time** (recycled between PRs) plus a throwaway comparison folder, so you never accumulate stale 1 GB checkouts.

## 0. First — make your uncommitted #141 changes safe

```
cd D:\Development\wildlife.ai\Seeed_Checking\Seeed_Grove_Vision_AI_Module_V2
git switch -c review/cgp-141
git add -A
git commit -m "review: CGP #141 edits before discussion"
git push -u origin review/cgp-141
```

Committing decides nothing — it just makes the edits safe, diffable and pushable. After our call we cherry-pick the agreed ones onto the feature branch; the rest stay parked here. Clearly-temporary commit messages (`review: …`, `WIP: …`) are fine; better than stashing.

## 1. Create the #142 review folder — on your own review branch

```
git fetch origin
git worktree add -b review/cgp-142 ..\pr142 origin/feat/ble-fast-transfer
```

The `-b` matters: the folder starts at GitHub's exact #142 but sits on **your** `review/cgp-142` branch — you can't accidentally commit to the shared feature branch, it's always obvious which commits are yours, and upstream stays untouched. Edit in Eclipse there, commit small and prefixed as you go, push the branch when you're ready. `REVIEW_PR142.md` at the tip is your report scaffold.

## 2. Compare with Meld via a throwaway base folder

```
git worktree add ..\compare --detach origin/feat/camera-features-combined
```

Meld: `..\compare` ↔ `..\pr142`. (Meld File Filters: add `.git`, `obj_*`, `NUL` — a worktree's `.git` is a small file that would otherwise show as a diff.) When you're done comparing:

```
git worktree remove ..\compare
```

Thirty seconds to recreate whenever needed, always unambiguous about what it points at, and no permanent reference checkout lying around.

## 3. Recycle for #140

When the #142 review is done and pushed:

```
git worktree remove ..\pr142
git worktree add -b review/cgp-140 ..\pr140 origin/feat/uart-live-preview
git worktree add ..\compare --detach origin/feat/ble-fast-transfer
```

Same routine: Meld `..\compare` ↔ `..\pr140`, scaffold `REVIEW_PR140.md`, edits on `review/cgp-140`, remove `..\compare` after.

## 4. Housekeeping

- `git worktree list` — shows every folder and what it's on; very useful a few weeks in.
- `git worktree remove <folder>` (+ `git worktree prune`) as soon as a review is finished or a PR merges — no reason to keep old trees.
- Git allows a branch checked out in only one folder at a time; the `review/*` branches sidestep that entirely.

At any moment you hold: your normal folder, your #141 folder, one review folder, and (briefly) `compare` — nothing else.
