const { createClient } = require('@supabase/supabase-js');
const fs = require('fs');
const path = require('path');

// Configuration
const SUPABASE_URL = process.env.SUPABASE_URL;
const SUPABASE_SERVICE_ROLE_KEY = process.env.SUPABASE_SERVICE_ROLE_KEY;
const FIRMWARE_PATH = process.env.FIRMWARE_PATH; // Path to the generated .img file
const FIRMWARE_VERSION = process.env.FIRMWARE_VERSION || `commit-${process.env.GITHUB_SHA?.substring(0, 7)}`; // Use semantic version if available, fallback to commit hash
const RELEASE_NOTES = process.env.RELEASE_NOTES || `Automated build from commit ${process.env.GITHUB_SHA}`;
const FIRMWARE_TYPE = process.env.FIRMWARE_TYPE || 'himax'; // Configurable firmware type (himax, ble, config, etc.)
const BUCKET_NAME = process.env.BUCKET_NAME || 'firmware'; // Configurable bucket name for different environments
const SYSTEM_EMAIL = process.env.SYSTEM_EMAIL || 'system@wildlife.ai'; // Email to query for system user
const FALLBACK_USER_ID = process.env.FALLBACK_USER_ID || '00000000-0000-0000-0000-000000000000'; // Fallback user ID if queries fail

if (!SUPABASE_URL || !SUPABASE_SERVICE_ROLE_KEY || !FIRMWARE_PATH) {
  console.error('Error: Missing required environment variables.');
  console.error('Required: SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, FIRMWARE_PATH');
  console.error('Optional: FIRMWARE_TYPE (default: himax), BUCKET_NAME (default: firmware)');
  console.error('Optional: SYSTEM_USER_ID, SYSTEM_EMAIL, FALLBACK_USER_ID');
  process.exit(1);
}

const supabase = createClient(SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY);

async function uploadFirmware() {
  try {
    // 1. Ensure bucket exists
    const { data: buckets, error: bucketsError } = await supabase.storage.listBuckets();
    if (bucketsError) throw bucketsError;

    const bucketExists = buckets.find(b => b.name === BUCKET_NAME);
    if (!bucketExists) {
      console.log(`Creating bucket: ${BUCKET_NAME}`);
      const { error: createError } = await supabase.storage.createBucket(BUCKET_NAME, {
        public: true, // or false, depending on access needs
      });
      if (createError) throw createError;
    }

    // 2. Upload file
    const fileName = path.basename(FIRMWARE_PATH);
    const fileContent = fs.readFileSync(FIRMWARE_PATH);
    // Use semantic version in filename for easy identification
    const versionTag = FIRMWARE_VERSION.replace(/^v/, ''); // Remove 'v' prefix if present
    const storagePath = `${FIRMWARE_TYPE}/${versionTag}_${fileName}`;

    console.log(`Uploading ${fileName} to ${storagePath}...`);
    const { data: uploadData, error: uploadError } = await supabase.storage
      .from(BUCKET_NAME)
      .upload(storagePath, fileContent, {
        contentType: 'application/octet-stream',
        upsert: true
      });

    if (uploadError) throw uploadError;

    // 3. Insert record into database
    const fileSize = fs.statSync(FIRMWARE_PATH).size;

    console.log(`Registering firmware version ${FIRMWARE_VERSION} in database...`);

    // Resolve system user ID with multiple fallback strategies
    let systemUserId = process.env.SYSTEM_USER_ID;

    if (!systemUserId) {
      console.log('SYSTEM_USER_ID not provided, querying database for system user...');

      // Strategy 1: Look for user with system email
      const { data: systemUser, error: userError } = await supabase
        .from('users')
        .select('id')
        .eq('email', 'system@wildlife.ai')
        .is('deleted_at', null)
        .limit(1)
        .single();

      if (systemUser && !userError) {
        systemUserId = systemUser.id;
        console.log(`Found system user via email: ${systemUserId}`);
      } else {
        // Strategy 2: Look for any ww_admin system role
        const { data: adminRole, error: roleError } = await supabase
          .from('user_roles')
          .select('user_id')
          .eq('role', 'ww_admin')
          .eq('scope_type', 'system')
          .is('deleted_at', null)
          .limit(1)
          .single();

        if (adminRole && !roleError) {
          systemUserId = adminRole.user_id;
          console.log(`Found system user via ww_admin role: ${systemUserId}`);
        } else {
          // Strategy 3: Fallback to hardcoded UUID (with warning)
          systemUserId = '00000000-0000-0000-0000-000000000000';
          console.warn(`⚠️  WARNING: Using hardcoded system user ID as fallback: ${systemUserId}`);
          console.warn('Consider setting SYSTEM_USER_ID environment variable or ensuring a system user exists.');
        }
      }
    } else {
      console.log(`Using provided SYSTEM_USER_ID: ${systemUserId}`);
    }

    const { data: insertData, error: insertError } = await supabase
      .from('firmware')
      .insert({
        name: `Himax Firmware ${FIRMWARE_VERSION}`,
        version: FIRMWARE_VERSION,
        type: FIRMWARE_TYPE,
        location_path: storagePath,
        file_size_bytes: fileSize,
        release_notes: RELEASE_NOTES,
        is_active: true,
        modified_by: systemUserId
      })
      .select();

    if (insertError) throw insertError;

    console.log('Firmware uploaded and registered successfully!');
    console.log(`Version: ${FIRMWARE_VERSION}`);
    console.log(`Storage Path: ${storagePath}`);
    console.log(insertData);

  } catch (error) {
    console.error('Deployment failed:', error);
    process.exit(1);
  }
}

uploadFirmware();
