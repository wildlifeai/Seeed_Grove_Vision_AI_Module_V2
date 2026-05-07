const { createClient } = require('@supabase/supabase-js');
const fs = require('fs');
const path = require('path');

// Configuration
const SUPABASE_URL = process.env.SUPABASE_URL;
const SUPABASE_SERVICE_ROLE_KEY = process.env.SUPABASE_SERVICE_ROLE_KEY;
const FIRMWARE_PATH = process.env.FIRMWARE_PATH; // Path to the generated .img file
const FIRMWARE_VERSION = process.env.FIRMWARE_VERSION || `commit-${process.env.GITHUB_SHA?.substring(0, 7)}`;
const RELEASE_NOTES = process.env.RELEASE_NOTES || `Automated build from commit ${process.env.GITHUB_SHA}`;
const FIRMWARE_TYPE = process.env.FIRMWARE_TYPE || 'himax';
const BUCKET_NAME = process.env.BUCKET_NAME || 'firmware';
const SYSTEM_EMAIL = process.env.SYSTEM_EMAIL || 'system@wildlife.ai';
const FALLBACK_USER_ID = process.env.FALLBACK_USER_ID || '00000000-0000-0000-0000-000000000000';

if (!SUPABASE_URL || !SUPABASE_SERVICE_ROLE_KEY || !FIRMWARE_PATH) {
  console.error('Error: Missing required environment variables.');
  console.error('Required: SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, FIRMWARE_PATH');
  console.error('Optional: FIRMWARE_TYPE (default: himax), BUCKET_NAME (default: firmware)');
  console.error('Optional: SYSTEM_USER_ID, SYSTEM_EMAIL, FALLBACK_USER_ID');
  process.exit(1);
}

const supabase = createClient(SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY);

/**
 * Sanitize a version string for use in storage paths.
 * Replaces spaces with underscores and colons with hyphens.
 * e.g. "WW500_C02 12:39:43 Apr 25 2026" → "WW500_C02_12-39-43_Apr_25_2026"
 */
function sanitizeForPath(version) {
  return version.replace(/:/g, '-').replace(/\s+/g, '_');
}

async function uploadFirmware() {
  try {
    // 1. Ensure bucket exists
    const { data: buckets, error: bucketsError } = await supabase.storage.listBuckets();
    if (bucketsError) throw bucketsError;

    const bucketExists = buckets.find(b => b.name === BUCKET_NAME);
    if (!bucketExists) {
      console.log(`Creating bucket: ${BUCKET_NAME}`);
      const { error: createError } = await supabase.storage.createBucket(BUCKET_NAME, {
        public: true,
      });
      if (createError) throw createError;
    }

    // 2. Upload file to storage
    const fileName = path.basename(FIRMWARE_PATH);
    const fileContent = fs.readFileSync(FIRMWARE_PATH);
    const fileSize = fs.statSync(FIRMWARE_PATH).size;
    const versionTag = sanitizeForPath(FIRMWARE_VERSION);
    const storagePath = `${FIRMWARE_TYPE}/${versionTag}_${fileName}`;

    console.log(`Uploading ${fileName} to ${BUCKET_NAME}/${storagePath} (${fileSize} bytes)...`);
    const { error: uploadError } = await supabase.storage
      .from(BUCKET_NAME)
      .upload(storagePath, fileContent, {
        contentType: 'application/octet-stream',
        upsert: true
      });

    if (uploadError) throw uploadError;
    console.log(`✅ Storage upload complete: ${storagePath}`);

    // 3. Resolve system user ID
    const systemUserId = await resolveSystemUserId();

    // 4. Generate firmware name
    const firmwareName = generateFirmwareName(FIRMWARE_TYPE, FIRMWARE_VERSION);

    // 5. Upsert database record (check existing, then update or insert)
    const record = {
      name: firmwareName,
      version: FIRMWARE_VERSION,
      type: FIRMWARE_TYPE,
      location_path: storagePath,
      file_size_bytes: fileSize,
      release_notes: RELEASE_NOTES,
      is_active: true,
      modified_by: systemUserId
    };

    // Check if a record with this (type, version) already exists
    const { data: existing, error: findError } = await supabase
      .from('firmware')
      .select('id')
      .eq('type', FIRMWARE_TYPE)
      .eq('version', FIRMWARE_VERSION)
      .is('deleted_at', null)
      .limit(1)
      .maybeSingle();

    if (findError) throw findError;

    let result;
    if (existing) {
      console.log(`Firmware (${FIRMWARE_TYPE}, ${FIRMWARE_VERSION}) already exists — updating record ${existing.id}...`);
      const { data, error } = await supabase
        .from('firmware')
        .update({
          name: record.name,
          location_path: record.location_path,
          file_size_bytes: record.file_size_bytes,
          release_notes: record.release_notes,
          is_active: record.is_active,
          modified_by: record.modified_by,
          updated_at: new Date().toISOString()
        })
        .eq('id', existing.id)
        .select();

      if (error) throw error;
      result = data;
      console.log('✅ Existing firmware record updated.');
    } else {
      console.log(`Registering new firmware (${FIRMWARE_TYPE}, ${FIRMWARE_VERSION})...`);
      const { data, error } = await supabase
        .from('firmware')
        .insert(record)
        .select();

      if (error) throw error;
      result = data;
      console.log('✅ New firmware record inserted.');
    }

    console.log(`  Version:       ${FIRMWARE_VERSION}`);
    console.log(`  Name:          ${firmwareName}`);
    console.log(`  Storage Path:  ${storagePath}`);
    console.log(`  File Size:     ${fileSize} bytes`);
    console.log(`  Release Notes: ${RELEASE_NOTES.split('\n')[0]}...`);
    console.log(result);

  } catch (error) {
    console.error('Deployment failed:', error);
    process.exit(1);
  }
}

/**
 * Resolve the system user ID using multiple fallback strategies.
 */
async function resolveSystemUserId() {
  let systemUserId = process.env.SYSTEM_USER_ID;

  if (systemUserId) {
    console.log(`Using provided SYSTEM_USER_ID: ${systemUserId}`);
    return systemUserId;
  }

  console.log('SYSTEM_USER_ID not provided, querying database for system user...');

  // Strategy 1: Look for user with system email
  const { data: systemUser, error: userError } = await supabase
    .from('users')
    .select('id')
    .eq('email', SYSTEM_EMAIL)
    .is('deleted_at', null)
    .limit(1)
    .single();

  if (systemUser && !userError) {
    console.log(`Found system user via email: ${systemUser.id}`);
    return systemUser.id;
  }

  // Strategy 2: Look for any ww_admin system role
  const { data: adminRole, error: roleError } = await supabase
    .from('user_roles')
    .select('user_id')
    .eq('role', 'ww_admin')
    .eq('scope_type', 'system')
    .is('deleted_at', null)
    .order('created_at', { ascending: true })
    .limit(1)
    .single();

  if (adminRole && !roleError) {
    console.log(`Found system user via ww_admin role: ${adminRole.user_id}`);
    return adminRole.user_id;
  }

  // Strategy 3: Fallback to hardcoded UUID
  console.warn(`⚠️  WARNING: Using hardcoded system user ID as fallback: ${FALLBACK_USER_ID}`);
  console.warn('Consider setting SYSTEM_USER_ID environment variable or ensuring a system user exists.');
  return FALLBACK_USER_ID;
}

/**
 * Generate a descriptive firmware name based on type and version.
 */
function generateFirmwareName(type, version) {
  const typeLabels = {
    himax: 'Himax AI Processor',
    ble: 'BLE Nordic',
    config: 'Config',
  };
  const label = typeLabels[type] || type.toUpperCase();
  return `${label} Firmware ${version}`;
}

uploadFirmware();
