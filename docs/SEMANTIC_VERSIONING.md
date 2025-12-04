# Semantic Versioning Implementation Summary

## Overview
Successfully implemented semantic versioning using Conventional Commits and automatic release note generation for the firmware build pipeline.

## Changes Made

### 1. Workflow Updates (`.github/workflows/build_and_release.yml`)

#### Added Semantic Versioning Step
- Uses `mathieudutour/github-tag-action@v6.2` to calculate next version
- Analyzes commit messages following Conventional Commits format:
  - `feat:` → Minor version bump (0.0.0 → 0.1.0)
  - `fix:` → Patch version bump (0.0.0 → 0.0.1)
  - `BREAKING CHANGE:` → Major version bump (0.0.0 → 1.0.0)
- Starting version: `v0.0.0`
- Only calculates version (dry run), doesn't create tag yet

#### Added Release Notes Generation
- Collects all commit messages since last tag
- Format: `- <commit message> (<short hash>)`
- Stores in `RELEASE_NOTES` environment variable
- Used in database `release_notes` field

#### Added Git Tagging Step
- Automatically creates and pushes Git tag after successful upload
- Tag format: `v0.1.0`, `v0.2.0`, etc.
- Only runs if upload succeeds

### 2. Upload Script Updates (`scripts/upload_firmware.js`)

#### Environment Variables
- **New**: `FIRMWARE_VERSION` - Semantic version (e.g., `v0.1.0`)
- **New**: `RELEASE_NOTES` - Commit messages since last release
- **Removed**: `GITHUB_SHA` (no longer required)
- **Fallback**: If `FIRMWARE_VERSION` not set, uses `commit-<sha>` format

#### Storage Path
- **Old**: `himax/<commit-hash>_output.img`
- **New**: `himax/<version>_output.img` (e.g., `himax/0.1.0_output.img`)

#### Database Record
- **version**: Now uses semantic version (e.g., `v0.1.0`) instead of `commit-abc1234`
- **name**: `Himax Firmware v0.1.0` instead of `Himax Firmware (commit-abc1234)`
- **release_notes**: Contains actual commit messages instead of generic message

## How It Works

### On Push to Main:
1. **Checkout** - Fetches full git history
2. **Calculate Version** - Analyzes commits using Conventional Commits
3. **Generate Release Notes** - Collects commit messages since last tag
4. **Build** - Compiles firmware
5. **Generate Image** - Creates .img file
6. **Upload** - Uploads to Supabase with semantic version
7. **Tag** - Creates git tag for the release

### First Release Example:
Starting from v0.0.0, with commit: `feat: add automated firmware pipeline`
- Calculated version: `v0.1.0` (minor bump due to `feat:`)
- Release notes: `- feat: add automated firmware pipeline (a738ab3)`
- Storage path: `himax/0.1.0_output.img`
- Git tag created: `v0.1.0`

### Subsequent Releases:
With commit: `fix: correct GCC path detection`
- Calculated version: `v0.1.1` (patch bump due to `fix:`)
- Release notes: `- fix: correct GCC path detection (xyz9876)`
- Automatically increments from previous tag

## Conventional Commit Format

To ensure correct version bumping, use these commit message formats:

```
feat: add new feature          → Minor bump (0.1.0 → 0.2.0)
fix: fix bug                   → Patch bump (0.1.0 → 0.1.1)
docs: update documentation     → No bump (documentation change)
chore: update dependencies     → No bump (maintenance)

feat!: breaking change         → Major bump (0.1.0 → 1.0.0)
OR
feat: something
BREAKING CHANGE: explanation   → Major bump (0.1.0 → 1.0.0)
```

## Testing

The implementation has been tested with:
- ✅ Semantic version calculation (dry run mode)
- ✅ Release notes generation
- ✅ Upload script with version and release notes
- ⚠️ Full pipeline not yet run (requires push to main)

## Next Steps

1. **Commit these changes** to the feature branch
2. **Merge to main** to trigger first automated release
3. **Verify** that tag `v0.1.0` (or calculated version) is created
4. **Check** Supabase:
   - Storage: `firmware/himax/0.1.0_output.img`
   - Database: Version `v0.1.0` with release notes

## Future Improvements

- Add GitHub Releases with release notes
- Include changelog generation
- Add notifications on release
- Support pre-release versions (alpha, beta, rc)
