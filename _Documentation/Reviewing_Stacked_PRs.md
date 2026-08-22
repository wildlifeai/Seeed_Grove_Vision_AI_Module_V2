# Reviewing Stacked PRs
#### CGP — 8 August 2026 (updated 10 August 2026)

## What "stacked" means

Sometimes several PRs are submitted as a chain, each building on the previous one rather than each branching independently from `dev`:

```
dev → feat/A (PR #A) → feat/B (PR #B) → feat/C (PR #C)
```

Victor's PRs #141, #142 and #140 were exactly this shape:

| Order | Branch | PR | Based on |
|---|---|---|---|
| 1 | `feat/camera-features-combined` | #141 | `origin/dev` |
| 2 | `feat/ble-fast-transfer` | #142 | `origin/feat/camera-features-combined` |
| 3 | `feat/uart-live-preview` (WIP) | #140 | `origin/feat/ble-fast-transfer` |

This doc is the companion to [`Reviewing_External_PRs_with_Worktrees.md`](Reviewing_External_PRs_with_Worktrees.md) — same worktree/Meld/Eclipse recipe, with two adjustments: **review order** and **diff base**. Everything else there (folder layout, Eclipse/VSCode setup, pushing your review branch, cleanup) applies unchanged.

## Rule 1 — review oldest first

Review the stack in the order it was built, not the order you feel like tackling it. Each PR's own diff only makes sense once you already understand everything under it.

## Rule 2 — diff each PR against the branch it's stacked on, not against `dev`

If you diff PR #142 against `dev`, you'll see PR #141's changes too, mixed in with #142's — hard to review. Diff against the specific branch it's based on instead:

```
# PR #141 (base of the stack — this one does diff against dev)
git log --oneline origin/dev..origin/feat/camera-features-combined
git diff origin/dev...origin/feat/camera-features-combined

# PR #142 (based on #141, not dev)
git log --oneline origin/feat/camera-features-combined..origin/feat/ble-fast-transfer
git diff origin/feat/camera-features-combined...origin/feat/ble-fast-transfer

# PR #140 (based on #142, not dev)
git log --oneline origin/feat/ble-fast-transfer..origin/feat/uart-live-preview
git diff origin/feat/ble-fast-transfer...origin/feat/uart-live-preview
```

Each command now shows *only* that PR's own commits/diff.

## Applying this to the worktree recipe

In [`Reviewing_External_PRs_with_Worktrees.md`](Reviewing_External_PRs_with_Worktrees.md) Step 2, the `compare` worktree is checked out from `<base-branch>`. For a stacked PR, `<base-branch>` is **the previous PR's branch**, not `dev`:

```
# reviewing PR #142 — compare against #141's tip, not dev
git worktree add ..\compare --detach origin/feat/camera-features-combined
```

The `review-cgp-<N>` review worktree itself (Step 1) is still created from that PR's own branch tip as usual — you want the full file contents there, just the *comparison* narrowed to the previous branch in the stack.

## What if an earlier PR changes after you've already reviewed a later one?

This will happen — if you (or the author) fix something in #141 after you've started on #142, #142's branch doesn't automatically pick it up. Per the team's [`Git_Branch_and_PR_Workflow.md`](Git_Branch_and_PR_Workflow.md) §7 ("Staggered Development — Dependent Tasks"), the author rebases the downstream branch onto the updated upstream one, e.g.:

```
git checkout feat/ble-fast-transfer
git rebase feat/camera-features-combined
```

**Important — this is the author's/maintainer's job on the shared `feat/...` branches, not yours.** Victor's instructions (12 Jul 2026 email) were explicit: don't rebase or force-push the shared feature branches — rewriting their history breaks the chain for everyone downstream. He merges fixes upward through the stack himself so #142 and #140 pick them up.

What *you* can safely do is rebase your own `review/cgp-<N>` branches if you want your review to reflect an updated upstream — that only rewrites history you own.

## Recycling review worktrees through the stack

Following on from Step 7 of the worktree doc, moving to the next PR in the stack looks like:

```
git worktree remove ..\review-cgp-<N>
git worktree add -b review/cgp-<N+1> ..\review-cgp-<N+1> origin/<next-feature-branch>
git worktree add ..\compare --detach origin/<this-stack's-previous-branch>
```

Same routine each time: Meld `..\compare` ↔ `..\review-cgp-<N+1>`, scaffold/copy the PR description, edit and commit on `review/cgp-<N+1>`, push when done, remove `..\compare` after.

In practice it's just as reasonable to keep all the stack's review worktrees (and their Eclipse workspaces) open side by side until the whole stack is reviewed, and remove them together at the end — that's how the #141/#142/#140 round below actually went.

## Quick reference — the #141/#142/#140 worked example

- #141 is the base of the stack — review it against `dev`. ~4,900 lines across 9 topics.
- #142 is stacked on #141 — review it against `feat/camera-features-combined`. Roughly a tenth of #141's size.
- #140 is stacked on #142, still WIP — review it against `feat/ble-fast-transfer`, for direction rather than line-by-line polish (best judged by running the live preview tool on real hardware).
- Fixes you make on #141 get merged upward by Victor through #142 and #140 — you don't need to re-propagate them yourself.
- All three were reviewed with `review-cgp-140`/`review-cgp-141`/`review-cgp-142` worktrees and matching `workspace_140`/`workspace_141`/`workspace_142` Eclipse workspaces held open side by side throughout, rather than recycled one at a time — see the note above. As of 10 Aug 2026: `review/cgp-141` and `review/cgp-142` are already pushed; `review/cgp-140` is the last to go up, after which all three worktrees and workspaces get removed together (Step 7 of the main doc) — `Seeed_Grove_Vision_AI_Module_V2` is the only one of the five folders that's kept.
