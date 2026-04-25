# Git Branch and PR Workflow
#### Victor Anton - 26 Feb 2026

This document describes the team's process for managing branches, pull requests, and code review. The goal is a **clean, linear commit history** with every change reviewed before it lands.

## Branch Strategy

| Branch | Purpose | Direct commits? |
|--------|---------|-----------------|
| `main` | Stable production releases | **Never** — only merged from `dev` via PR |
| `dev` | Integration branch (head branch for all work) | **Never** — only merged from feature branches via PR |
| `feature/*` | Individual task or feature work | Yes |

> [!IMPORTANT]
> All work starts and ends with `dev`. Never commit directly to `dev` or `main`.

---

## Setting Up Repository Protections (Admins Only)

To enforce this workflow and prevent accidental mistakes, repository administrators should configure the following settings in GitHub:

1.  **Set `dev` as the Default Branch:**
    - Go to **Settings** > **Default branch**.
    - Change the default branch from `main` to `dev`. This ensures all new Pull Requests automatically target `dev` by default.
2.  **Protect the `main` Branch:**
    - Go to **Settings** > **Branches** > **Add branch protection rule**.
    - Set the **Branch name pattern** to `main`.
    - Check **Require a pull request before merging`.
    - Uncheck or restrict who can push directly (usually, nobody should push directly; it should only be merged from `dev` PRs).
3.  **Protect the `dev` Branch:**
    - Add another rule with the pattern `dev`.
    - Check **Require a pull request before merging`.
    - Check **Require approvals** (e.g., at least 1 approval).
    - This forces all feature work to go through the PR and review process before landing in `dev`.

---

## 1. Start a New Task — Create a Feature Branch

Always branch from the latest `dev`:

```bash
# Make sure you are on dev and it is up to date
git checkout dev
git pull origin dev

# Create a new branch for your task
git checkout -b feature/my-task-description
```

Use a short, descriptive name: `feature/ble-timeout-fix`, `feature/add-lora-retry`, etc.

---

## 2. Do Your Work — Commit Often

Make small, focused commits as you work:

```bash
git add -A
git commit -m "Add retry logic for BLE command timeout"
```

Use clear commit messages that explain **what** and **why**.

---

## 3. Before Pushing — Rebase onto `dev`

Before you push (or create a PR), rebase your branch onto the latest `dev` to keep a **linear history**:

```bash
# Fetch the latest changes from the remote
# (Think of it as “refresh my view of what the remote repo looks like.”)
git fetch origin

# Rebase your branch onto the latest dev
# (Rewrites your branch so your commits are replayed on top of the latest dev branch, creating a clean, linear history.)
git rebase origin/dev
```

If there are conflicts, Git will pause and let you resolve them file by file:

```bash
# After resolving each conflict:
git add <resolved-file>
git rebase --continue

# If you want to abort and go back to where you were:
git rebase --abort
```

> [!TIP]
> Rebasing rewrites your commits on top of the latest `dev`, producing a clean, linear history instead of merge commits.
* You get all the latest changes from dev
* Your branch history becomes linear, with no merge commits
* Your commits appear “after” the latest dev commits
* It avoids messy merge bubbles in the history

---

## 4. Push Your Branch

After rebasing, push your branch to the remote. If you have already pushed before the rebase, you will need to force-push:

```bash
# First push
git push origin feature/my-task-description

# After a rebase (if you previously pushed)
git push --force-with-lease origin feature/my-task-description
```

> [!WARNING]
> Use `--force-with-lease` (not `--force`) to avoid accidentally overwriting someone else's changes.

---

## 5. Create a Pull Request

1. Go to the repository page on **GitHub.com**.
2. Click the **"Pull requests"** tab near the top.
3. Click the green **"New pull request"** button.
4. On the "Compare changes" page, choose the correct branches:
   - **base:** `dev` (This is the destination your changes will merge into)
   - **compare:** `feature/my-task-description` (This is your branch with the new changes)
   > [!IMPORTANT]
   > Do not target `main` directly. All new work should be merged into `dev`.
   
   > **Note on Stacked Branches:** If your branch explicitly depends on another un-merged branch instead of `dev`, set the **base** to that un-merged branch. (See Section 7 below).
5. Give the PR a clear, descriptive title.
6. Fill out the description explaining **what** was changed and **why**.
7. Link any related issues or tasks.
8. Select a reviewer.
9. Click the green **"Create pull request"** button to submit.

---

## 6. Code Review

### Automated Review — Gemini

When you create or update the PR, Gemini code review will automatically analyse your changes and comment on potential bugs, improvements, or style issues.

- **Address every comment** — fix the code or reply with a justification for keeping it as-is.
- Push fixes as new commits; the review will re-run automatically.

### Human Review

After addressing automated feedback:

1. Request a review from at least one team member.
2. The reviewer should check correctness, readability, and alignment with the project architecture.
3. Once approved, the reviewer (or PR author, per team agreement) merges the PR into `dev`.

### Reviewer Responsibilities

As a reviewer, your goals are to:
- **Understand the change**: Read the PR description and ensure the code matches the intent.
- **Check code quality**: Look for edge cases, performance issues, or deviations from team style.
- **Approve or Request Changes**: Leave clear, actionable comments. Approve the PR once all concerns are resolved.

---

## 7. Switching to a Different Task (Staggered Development)

If you finish a task, open a PR for it, and want to keep progressing without waiting for it to be reviewed and merged, you have two options depending on whether your next task **depends** on the previous one.

### Option A: Independent Tasks (Parallel Branches)
If your new task is **completely unrelated** to the un-merged PR, branch off `dev`:

```bash
# 1. Commit or stash your current work
git add -A && git commit -m "WIP/Finish task 1"

# 2. Go back to dev. (Your HEAD now points to your local dev branch)
git checkout dev

# 3. Fetch from remote and pull the latest
# `git fetch` updates your local knowledge of 'origin' (the remote repo)
# `git pull` then updates your local branch to match the remote.
git pull origin dev

# 4. Create a new branch for the independent task
# Your HEAD moves to this new branch.
git checkout -b feature/independent-task
```

### Option B: Dependent Tasks (Stacked Branches)
If your new task **depends** on the changes in your un-merged PR (e.g., you did a fix, and now want to build a feature relying on that fix), branch off your current un-merged branch instead of `dev`.

> **Understanding HEAD:**
> In Git, `HEAD` is a pointer to the commit you are currently working on. When you run `git checkout [branch]`, `HEAD` moves to the tip of that branch. If you branch off `feature/task-1`, your new branch's `HEAD` starts exactly where `task-1` left off.

```bash
# 1. Ensure your HEAD is on the completed, un-merged branch
git checkout feature/task-1-fix

# 2. Create a new local branch directly from this one
git checkout -b feature/task-2-new-feature

# 3. Work on task 2, commit, and push
git add -A && git commit -m "Add new feature based on task 1 fix"
git push origin feature/task-2-new-feature
```

**Iterating Further (Staggered PRs)**:
- You can iterate further to create `feature/task-3`, `feature/task-4`, etc., each branching off the previous one.
- When creating the **Pull Request** for `task-2` on GitHub, make sure to change the **"base" branch** dropdown from `dev` to `feature/task-1-fix`. This ensures the PR only shows your new additions for `task-2`.
- If `task-1-fix` receives feedback and is updated during code review, its `HEAD` will move. You will need to **rebase** `task-2` onto it so you inherit those updates: 
  ```bash
  # Check out task 2 (move HEAD to task 2)
  git checkout feature/task-2-new-feature
  # Replay task 2's commits on top of the new task 1 HEAD
  git rebase feature/task-1-fix
  ```

---

## 8. Post-Merge Cleanup

After your pull request is merged into `dev`, you should delete your feature branch both remotely (on GitHub) and locally to keep the repository clean.

**Remotely (on GitHub):**
Delete via the UI (on the merged PR page) or run `git push origin --delete feature/my-task-description`.

**Locally:**
```bash
# Switch back to dev and pull the latest changes
git checkout dev
git pull origin dev

# Delete your local feature branch
git branch -d feature/my-task-description
```
If Git complains that the branch is not fully merged (because of a squash merge on GitHub), you can force delete it with:
```bash
git branch -D feature/my-task-description
```

### Cleanup for Stacked Branches
If you used the **Dependent Tasks** approach (e.g., `task-2` built on top of `task-1`), and `task-1` gets merged into `dev`:
1. Clean up `task-1` locally as shown above, and pull the latest `dev`.
2. Edit your GitHub PR for `task-2` and change its **base branch** from `task-1` to `dev`.
3. Rebase `task-2` locally onto the newly updated `dev` so it aligns properly with the rest of the team:

> **How Fetch and Rebase Work Here:**
> - `git fetch origin`: Updates your local index of the remote repo (so your computer knows where `origin/dev`'s `HEAD` actually is), without modifying your current files.
> - `git rebase origin/dev`: Takes the commits that are unique to your `task-2` branch, sets them aside, moves your branch's base pointer to the new `origin/dev` `HEAD`, and then re-applies your `task-2` commits one by one on top.

   ```bash
   # Move HEAD to task 2
   git checkout feature/task-2-new-feature

   # Update remote tracking branches
   git fetch origin

   # Replay task 2's commits on top of dev's new HEAD
   git rebase origin/dev

   # Force push the rewritten history
   git push --force-with-lease origin feature/task-2-new-feature
   ```
   *(Note: If `task-1` was squash-merged into `dev` on GitHub, git might not realize its commits are already in `dev`. You might see merge conflicts during this rebase. Resolve them normally, or ask for help, or read up on `git rebase --onto`).*

---

## Quick Reference

```text
         main ← (release PRs only)
          ↑
         dev  ← (all feature PRs target here)
        / | \
 feature/ feature/ feature/
 task-a   task-b   task-c
```

### Cheat-sheet

```bash
# New task
git checkout dev && git pull origin dev
git checkout -b feature/my-task

# Work & commit
git add -A && git commit -m "Describe what you did"

# Ready to push — rebase first
git fetch origin && git rebase origin/dev
git push origin feature/my-task          # first time
git push --force-with-lease origin feature/my-task  # after rebase

# Create PR → dev, get Gemini + human review, merge
# Post-merge cleanup
git checkout dev && git pull origin dev
git branch -d feature/my-task # Use -D if needed
git push origin --delete feature/my-task
```
