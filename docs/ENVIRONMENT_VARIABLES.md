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
| `SYSTEM_USER_ID` | *auto-detect* | User ID for `modified_by` field | Explicitly set for different environments if system user differs |
| `SYSTEM_EMAIL` | `system@wildlife.ai` | Email to query for system user | Override system user email for different environments |
| `FALLBACK_USER_ID` | `00000000-0000-0000-0000-000000000000` | Fallback user ID if queries fail | Set environment-specific fallback user |

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
| `GITHUB_SHA` | Git commit hash (used as fallback for version/notes if semantic versioning unavailable) | GitHub Actions |

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
     SYSTEM_USER_ID: your-system-user-id
   ```

2. **Repository variables**: Settings → Secrets and variables → Actions → Variables tab
   - Useful for non-sensitive configuration

3. **Environment-specific**: Settings → Environments → Create environment
   - Define different variables for staging vs production
   - Requires approval rules for production deployments

## System User ID Resolution

The `SYSTEM_USER_ID` is used for the `modified_by` field in database records. The script uses a multi-tier fallback strategy:

### Resolution Order:
1. **Environment Variable**: If `SYSTEM_USER_ID` is set, use it directly
2. **Database Query (Email)**: Query for user with email from `SYSTEM_EMAIL` (default: `system@wildlife.ai`)
3. **Database Query (Role)**: Query for any user with `ww_admin` system role
4. **Configured Fallback**: Use `FALLBACK_USER_ID` (default: `00000000-0000-0000-0000-000000000000`) with warning

### Security Best Practices:
- **Secrets**: Store `SYSTEM_EMAIL` and `FALLBACK_USER_ID` as GitHub Secrets for production
- **Production**: Set `SYSTEM_USER_ID` explicitly as environment variable
- **Development**: Let auto-detection find the system user
- **Multiple Environments**: Use different system users/emails for staging/production

### Example (Using Secrets):
```yaml
# In GitHub Secrets, add:
# SYSTEM_EMAIL_PROD: admin@your-production-domain.com
# FALLBACK_USER_ID_PROD: your-production-fallback-uuid

# In workflow:
env:
  SYSTEM_EMAIL: ${{ secrets.SYSTEM_EMAIL_PROD }}
  FALLBACK_USER_ID: ${{ secrets.FALLBACK_USER_ID_PROD }}
```

### Example (Environment-Specific):
```yaml
# Staging environment
env:
  SYSTEM_USER_ID: aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa
  SYSTEM_EMAIL: system@staging.wildlife.ai
  FALLBACK_USER_ID: staging-fallback-user-id

# Production environment  
env:
  SYSTEM_USER_ID: bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb
  SYSTEM_EMAIL: system@production.wildlife.ai
  FALLBACK_USER_ID: production-fallback-user-id
```
