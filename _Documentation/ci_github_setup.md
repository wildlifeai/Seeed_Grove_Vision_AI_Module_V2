# CI setup: firmware build and upload

One-time GitHub configuration for `build_and_upload_firmware.yml`, which builds both camera
variants and publishes them to Supabase. What each variable is for:
[`ci_environment_variables.md`](ci_environment_variables.md).

Needs repository admin access and the Supabase project credentials.

## 1. Add the secrets

Settings, Secrets and variables, Actions, New repository secret:

- `SUPABASE_URL`
- `SUPABASE_SERVICE_ROLE_KEY`
- `SYSTEM_EMAIL`
- `FALLBACK_USER_ID`
- `GEMINI_API_KEY` (only for `pr-agent-review.yml`)

The service-role key bypasses row-level security, so it is a secret, never a repository
variable.

## 2. Create the environments

Settings, Environments. The `upload-to-supabase` job declares `environment:`, so these
rules are what actually gate publishing.

| Environment | Configure |
|---|---|
| `dev` | no protection |
| `production` | required reviewer, and deployment branches limited to `main` |

Without the branch restriction on `production`, a manual dispatch from any branch could
publish to the production database.

## 3. Test it

Actions, "Build and Upload Himax Firmware", Run workflow. Choose `source: build` and
`environment: dev`.

Expect four jobs: `setup`, then `build-firmware` and `upload-to-supabase` once per camera
variant (RP3 and HM0360). Check the new rows in Supabase `firmware`.

Pushing to `dev` or `main` runs the same thing automatically, targeting the matching
database.

## Troubleshooting

**"Missing required environment variables"**: a secret in step 1 is absent or misspelled.
The job log names which one.

**"Bucket does not exist"**: `scripts/upload_firmware.js` creates the `firmware` bucket on
first run, so this means the service-role key lacks storage permissions.

**"Foreign key constraint violation (modified_by)"**: the system-user lookup found nothing
and `FALLBACK_USER_ID` is unset or not a real user. See the resolution order in
[`ci_environment_variables.md`](ci_environment_variables.md).

**"duplicate key value violates unique constraint"**: that version is already published.
Version comes from `version.mk`; bump it, or the upload has already happened.

**Build failures** are firmware, not configuration. See
[`building_firmware.md`](building_firmware.md).
