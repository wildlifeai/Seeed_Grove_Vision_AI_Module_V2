# CI environment variables and secrets

What the GitHub Actions workflows need, and where each value comes from. Set up guide:
[`ci_github_setup.md`](ci_github_setup.md).

## Secrets

Repository Settings, Secrets and variables, Actions.

| Secret | Used by | Purpose |
|---|---|---|
| `SUPABASE_URL` | `build_and_upload_firmware.yml` | Supabase project API URL |
| `SUPABASE_SERVICE_ROLE_KEY` | `build_and_upload_firmware.yml` | Service-role key for admin writes |
| `SYSTEM_EMAIL` | `build_and_upload_firmware.yml` | Email used to look up the system user |
| `FALLBACK_USER_ID` | `build_and_upload_firmware.yml` | User ID if the lookup fails |
| `GEMINI_API_KEY` | `pr-agent-review.yml` | AI PR review |
| `GITHUB_TOKEN` | several | Provided automatically by Actions; nothing to configure |

## Environments

`build_and_upload_firmware.yml` publishes to one of two GitHub environments, and the
`upload-to-supabase` job declares `environment:`, so their protection rules apply.

| Environment | Protection |
|---|---|
| `dev` | none |
| `production` | requires a review, and only the `main` branch may deploy |

That gating lives in the environment's settings, not in the workflow. See
`_Documentation/firmware_update_and_recovery.md` for what the published images are used
for.

## Values the workflow computes

Passed to `scripts/upload_firmware.js`; nothing to configure.

`FIRMWARE_VERSION`, `FIRMWARE_BUILD_DATE`, `FIRMWARE_CRC`, `FIRMWARE_VARIANT` (RP3 or
HM0360), `FIRMWARE_PATH`, `RELEASE_NOTES`, `GITHUB_SHA`.

## Optional overrides

`scripts/upload_firmware.js` reads these, and no workflow sets them. Defaults are correct
for this repo; they exist for reuse elsewhere.

| Variable | Default | Notes |
|---|---|---|
| `FIRMWARE_TYPE` | `himax` | `ble` for the nRF firmware in ww-hardware |
| `BUCKET_NAME` | `firmware` | Supabase Storage bucket |
| `SYSTEM_USER_ID` | auto-detected | Skips the lookup below if set |

## How the system user is resolved

Used for the `modified_by` field. `scripts/upload_firmware.js` tries, in order:

1. `SYSTEM_USER_ID`, if set
2. the user whose email matches `SYSTEM_EMAIL`
3. any user with the `ww_admin` role
4. `FALLBACK_USER_ID`, with a warning

Steps 2 to 4 are why `SYSTEM_EMAIL` and `FALLBACK_USER_ID` are secrets: without them the
upload still succeeds but attributes the record to nobody in particular.
