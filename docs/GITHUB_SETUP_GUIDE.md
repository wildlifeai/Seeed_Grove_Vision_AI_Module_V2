# GitHub Setup Guide for Firmware Build Pipeline

This guide walks you through setting up GitHub for the automated firmware build and release pipeline.

## Prerequisites

- Admin access to the GitHub repository
- Supabase project credentials

## Step-by-Step Setup

### Step 1: Add Required GitHub Secrets

These secrets are **required** for the pipeline to work:

1. **Navigate to Repository Settings**
   - Go to your GitHub repository
   - Click **Settings** (top menu)
   - In left sidebar, click **Secrets and variables** → **Actions**

2. **Add SUPABASE_URL**
   - Click **New repository secret**
   - **Name**: `SUPABASE_URL`
   - **Value**: `https://nuhwmubvygxyddkycmpa.supabase.co`
   - Click **Add secret**

3. **Add SUPABASE_SERVICE_ROLE_KEY**
   - Click **New repository secret**
   - **Name**: `SUPABASE_SERVICE_ROLE_KEY`
   - **Value**: Your Supabase service role key (starts with `eyJ...`)
   - ⚠️ **Important**: Never commit this key to your repository!
   - Click **Add secret**

### Step 2: Optional Configuration (Recommended for Production)

These are **optional** but recommended for security and flexibility:

#### Option A: Add as Repository Secrets (Most Secure)

1. **SYSTEM_EMAIL** (Optional - for production)
   - Click **New repository secret**
   - **Name**: `SYSTEM_EMAIL`
   - **Value**: `system@wildlife.ai` (or your custom system email)
   - Click **Add secret**

2. **FALLBACK_USER_ID** (Optional - for production)
   - Click **New repository secret**
   - **Name**: `FALLBACK_USER_ID`
   - **Value**: `00000000-0000-0000-0000-000000000000` (or your system user UUID)
   - Click **Add secret**

#### Option B: Add as Repository Variables (Less Sensitive)

For non-sensitive configuration:

1. In the same **Secrets and variables** → **Actions** page
2. Click the **Variables** tab
3. Click **New repository variable**
4. Add these variables:
   - **FIRMWARE_TYPE**: `himax` (default, can change for different firmware types)
   - **BUCKET_NAME**: `firmware` (default, can change for staging: `firmware-staging`)

### Step 3: Update Workflow File (If Using Secrets/Variables)

If you added optional secrets/variables, update `.github/workflows/build_and_release.yml`:

```yaml
env:
  SUPABASE_URL: ${{ secrets.SUPABASE_URL }}
  SUPABASE_SERVICE_ROLE_KEY: ${{ secrets.SUPABASE_SERVICE_ROLE_KEY }}
  # Optional: Uncomment to use secrets
  SYSTEM_EMAIL: ${{ secrets.SYSTEM_EMAIL }}
  FALLBACK_USER_ID: ${{ secrets.FALLBACK_USER_ID }}
  # Optional: Uncomment to use variables
  # FIRMWARE_TYPE: ${{ vars.FIRMWARE_TYPE }}
  # BUCKET_NAME: ${{ vars.BUCKET_NAME }}
```

### Step 4: Verify Configuration

#### Check Required Secrets

1. Go to **Settings** → **Secrets and variables** → **Actions** → **Secrets** tab
2. You should see:
   - ✅ `SUPABASE_URL`
   - ✅ `SUPABASE_SERVICE_ROLE_KEY`

#### Optional: Check Variables/Secrets

If you added optional configuration, verify in the appropriate tab.

### Step 5: Test the Pipeline

#### Method 1: Manual Trigger (Recommended for First Test)

1. Go to **Actions** tab in your repository
2. Click **Build and Release Himax Firmware** workflow
3. Click **Run workflow** dropdown
4. Select `main` branch
5. Click **Run workflow** button
6. Watch the workflow run to ensure it completes successfully

#### Method 2: Push to Main (Automatic)

1. Merge your PR to `main` branch
2. Workflow will automatically trigger
3. Go to **Actions** tab to monitor progress

### Step 6: Verify Success

After a successful run:

1. **Check Supabase Storage**
   - Open Supabase Dashboard
   - Go to **Storage**
   - Check `firmware` bucket
   - You should see: `himax/<version>_output.img`

2. **Check Supabase Database**
   - Go to **Table Editor**
   - Open `firmware` table
   - You should see a new record with:
     - `version`: e.g., `v0.1.0`
     - `type`: `himax`
     - `location_path`: `himax/0.1.0_output.img`
     - `release_notes`: Your commit messages
     - `is_active`: `true`

3. **Check Git Tags**
   - Go to repository **Tags** or **Releases**
   - You should see new tag: `v0.1.0` (or calculated version)

## Current Development Stage Summary

### ✅ What's Configured

- [x] Workflow file created
- [x] Upload script implemented
- [x] Semantic versioning enabled
- [x] Release notes generation
- [x] Auto-tagging on success

### ⚠️ What You Need to Add

**Required (Must Add):**
- [ ] `SUPABASE_URL` secret in GitHub
- [ ] `SUPABASE_SERVICE_ROLE_KEY` secret in GitHub

**Optional (Recommended for Production):**
- [ ] `SYSTEM_EMAIL` secret (defaults to `system@wildlife.ai`)
- [ ] `FALLBACK_USER_ID` secret (defaults to `00000000-0000-0000-0000-000000000000`)

### First-Time Checklist

Before merging to `main`:

- [ ] GitHub secrets added
- [ ] Supabase `firmware` bucket exists (or script will create it)
- [ ] System user exists in database (UUID: `00000000-0000-0000-0000-000000000000`)
- [ ] Arm GNU Toolchain compatible version exists (workflow uses 13.2.Rel1)
- [ ] Manual workflow test run succeeded

## Troubleshooting

### "Missing required environment variables"
- **Cause**: GitHub secrets not configured
- **Fix**: Add `SUPABASE_URL` and `SUPABASE_SERVICE_ROLE_KEY` as secrets

### "Bucket does not exist" error
- **Cause**: First-time run, bucket not created
- **Fix**: Script auto-creates bucket. Ensure service role key has permissions.

### "Foreign key constraint violation (modified_by)"
- **Cause**: System user doesn't exist in database
- **Fix**: 
  1. Run seed data on Supabase database
  2. Or set `SYSTEM_USER_ID` to an existing user UUID

### "Build failed" errors
- **Cause**: Missing Arm toolchain or build dependencies
- **Fix**: Check workflow logs for specific error. GitHub Actions should install toolchain automatically.

## Need Help?

- **Workflow logs**: Go to Actions tab → Click failed run → View logs
- **Supabase logs**: Supabase Dashboard → Logs
- **Documentation**: See `ENVIRONMENT_VARIABLES.md` and `SEMANTIC_VERSIONING.md`

## Security Notes

🔒 **Never commit these to Git:**
- Service role key
- System user IDs
- System emails
- Any UUID fallbacks

✅ **Always use GitHub Secrets for:**
- Database credentials
- API keys
- Production configuration
