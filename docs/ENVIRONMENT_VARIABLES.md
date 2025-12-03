# Environment Variables Configuration

This document describes the configurable environment variables for the firmware build and release pipeline.

## Required Variables

These must be configured as GitHub Secrets:

| Variable | Description | Example |
|----------|-------------|---------|
| `SUPABASE_URL` | Supabase project API URL | `https://nuhwmubvygxyddkycmpa.supabase.co` |
| `SUPABASE_SERVICE_ROLE_KEY` | Supabase service role key for admin access | `eyJhbGc...` |

## Optional Variables

These can be configured in the workflow file or as repository/environment variables:

| Variable | Default | Description | Use Case |
|----------|---------|-------------|----------|
| `FIRMWARE_TYPE` | `himax` | Type of firmware being built | Set to `ble`, `config`, etc. for different firmware types |
| `BUCKET_NAME` | `firmware` | Supabase Storage bucket name | Use different buckets for staging/production (e.g., `firmware-staging`, `firmware-prod`) |

## Usage Examples

### Different Environments

To use different buckets for staging and production:

**Staging:**
```yaml
env:
  BUCKET_NAME: firmware-staging
```

**Production:**
```yaml
env:
  BUCKET_NAME: firmware-prod
```

### Different Firmware Types

For managing multiple firmware types:

```yaml
env:
  FIRMWARE_TYPE: ble  # For BLE firmware
  BUCKET_NAME: firmware-ble
```

```yaml
env:
  FIRMWARE_TYPE: config  # For configuration firmware
  BUCKET_NAME: firmware-config
```

## Automatic Variables

These are automatically set by the workflow:

| Variable | Description | Set By |
|----------|-------------|--------|
| `FIRMWARE_VERSION` | Semantic version (e.g., `v0.1.0`) | github-tag-action |
| `RELEASE_NOTES` | Commit messages since last tag | Release notes generation step |
| `FIRMWARE_PATH` | Path to generated .img file | Find image step |
| `GITHUB_SHA` | Git commit hash | GitHub Actions (fallback for version) |

## Setting Variables

### GitHub Secrets (Required)
1. Go to repository Settings → Secrets and variables → Actions
2. Click "New repository secret"
3. Add `SUPABASE_URL` and `SUPABASE_SERVICE_ROLE_KEY`

### Environment Variables (Optional)
1. **In workflow file**: Edit `.github/workflows/build_and_release.yml`
   ```yaml
   env:
     FIRMWARE_TYPE: ble
     BUCKET_NAME: firmware-staging
   ```

2. **Repository variables**: Settings → Secrets and variables → Actions → Variables tab
   - Useful for non-sensitive configuration

3. **Environment-specific**: Settings → Environments → Create environment
   - Define different variables for staging vs production
   - Requires approval rules for production deployments
