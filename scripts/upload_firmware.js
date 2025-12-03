const { createClient } = require('@supabase/supabase-js');
const fs = require('fs');
const path = require('path');

// Configuration
const SUPABASE_URL = process.env.SUPABASE_URL;
const SUPABASE_SERVICE_ROLE_KEY = process.env.SUPABASE_SERVICE_ROLE_KEY;
const FIRMWARE_PATH = process.env.FIRMWARE_PATH; // Path to the generated .img file
const FIRMWARE_VERSION = process.env.FIRMWARE_VERSION || `commit-${process.env.GITHUB_SHA?.substring(0, 7)}`; // Use semantic version if available, fallback to commit hash
const RELEASE_NOTES = process.env.RELEASE_NOTES || `Automated build from commit ${process.env.GITHUB_SHA}`;
const FIRMWARE_TYPE = 'himax';
const BUCKET_NAME = 'firmware';

if (!SUPABASE_URL || !SUPABASE_SERVICE_ROLE_KEY || !FIRMWARE_PATH) {
  console.error('Error: Missing required environment variables.');
  console.error('Required: SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, FIRMWARE_PATH');
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

    // We need a user ID for 'modified_by'. 
    // Ideally, this should be a system user or the service role user.
    // For now, we'll try to find a system user or use a placeholder if allowed.
    // Since we are using service role key, RLS is bypassed, but the NOT NULL constraint exists.
    // Let's fetch a system user ID (e.g., the first admin found or a specific system account).

    // Strategy: Look for a user with 'ww_admin' role or create a dummy UUID if acceptable?
    // Better: Use a known system UUID if one exists in your seed data. 
    // Based on previous context, '00000000-0000-0000-0000-000000000000' is the system user created in seed.
    const SYSTEM_USER_ID = '00000000-0000-0000-0000-000000000000';

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
        modified_by: SYSTEM_USER_ID
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
