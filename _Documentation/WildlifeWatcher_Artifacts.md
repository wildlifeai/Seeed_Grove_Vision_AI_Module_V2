# Wildlife Watcher Artifact Management

This document outlines the various digital artifacts that comprise the Wildlife Watcher ecosystem, how they are managed, and how they interact with the mobile application and backend services.

## 1. Artifact Definitions

The Wildlife Watcher device relies on two primary categories of digital artifacts to function: Device Firmware and Machine Learning (ML) Models.

### Device Firmware Artifacts

This set of artifacts defines the software running on the two microprocessors of the Wildlife Watcher device.

#### BLE and LoRaWAN Firmware
- **Filename Format**: `ble_firmware.zip` (example: `0.11.1-feature-add-setdeploymentid-command.0_WildlifeWatcher_1_ww500_a00_nus_001101.zip`)
- **Purpose**: Runs on the MKL62BA processor, managing Bluetooth Low Energy (BLE) and LoRaWAN communications
- **Version Control**: Version number is indicated in the filename (e.g., `000804`)
- **Build Process**: Automatically generated and uploaded to Supabase Storage [WW Hardware build and release workflow](https://github.com/wildlifeai/main/ww-hardware/.github/workflows/ble_build_release.yml).
- **Update Protection**: The Device Firmware Update (DFU) process prevents the installation of earlier versions
- **Repository**: https://github.com/wildlifeai/ww-hardware/MokoTech/Workspace/WildlifeWatcher_1/ww500_c01

#### Himax AI Firmware
- **Filename**: `output.img` (example: `0.0.2-fix-firmware-dpd-build.0_output.img`)
- **Purpose**: Runs on the Himax AI processor, handling image capture, sensor management, and machine learning inference
- **Version Control**: Version number is indicated in the filename (e.g., `0.0.2`)
- **Build Process**: Manually generated during the application compilation process [Seeed_Grove_Vision_AI_Module_V2](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/_Documentation/Compile_and_flash.md) and automatically generated and uploaded to Supabase Storage [Seeed_Grove_Vision_AI_Module_V2 build and release workflow](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/main/.github/workflows/build_and_release.yml).
- **Repository**: [Seeed_Grove_Vision_AI_Module_V2](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2)


#### Configuration File
- **Filename**: `CONFIG.TXT`
- **Location**: `/MANIFEST/` directory
- **Purpose**: Defines default device parameters including:
  - Operational parameters (e.g., `Delay before DPD`, `Num pics when triggered`)
  - Sensor configuration settings
  - Device recovery data
- **Build Process**: Automatically generated and uploaded to Supabase Storage [Seeed_Grove_Vision_AI_Module_V2 Upload Config Firmware to Supabase workflow](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/main/.github/workflows/upload_config_firmware.yml).
- **Runtime Override**: The mobile app can override default parameters by sending specific BLE commands
- **Repository**: [Seeed_Grove_Vision_AI_Module_V2](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2)

### Machine Learning (ML) Artifacts

The AI capabilities of the device are defined by the ML model and its accompanying documentation.

#### ML Model Package
- **Package Name**: `Manifest.zip`
- **Contents**:
  - **ML Model**: `/MANIFEST/model_vela.tfl` - The trained neural network model that identifies species in images
  - **Model Labels**: `/MANIFEST/labels.txt` - Maps the model's output indices to human-readable species names
- **Training Guide**: https://www.notion.so/Machine-Learning-Models-2a78b68cc7b480d38b7ee00cdae251aa
- **Conversion and Upload Tool**: [WildlifeWatcher Model Conversion](https://github.com/wildlifeai/wildlife-watcher-model-conversion)
- **Build Process**: the app prepares and uploads models to supabase if: 1) the user wants, 2) is signed in and 3) has the right permissions to do so. 

---

## 2. Current State

This section describes how artifacts are currently managed and deployed. The process is largely manual.

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        CURRENT STATE                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  DEVICE FIRMWARE ARTIFACTS                                      │
│  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │
│  │ BLE & LoRaWAN    │  │ Himax AI         │  │ CONFIG.TXT    │  │
│  │ Firmware         │  │ Firmware         │  │               │  │
│  ├──────────────────┤  ├──────────────────┤  ├───────────────┤  │
│  │ Version: Git tag │  │ Version: Git tag │  │ Version: None │  │
│  │ Storage: Email   │  │ Storage: Local   │  │ Storage: Repo │  │
│  │ Deploy: DFU/BLE  │  │ Deploy: USB      │  │ Deploy: SD    │  │
│  └──────────────────┘  └──────────────────┘  └───────────────┘  │
│                                                                 │
│  ML ARTIFACTS                                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ Manifest.zip                                             │   │
│  │ ┌────────────────────┐  ┌──────────────────────────┐     │   │
│  │ │ model_vela.tfl     │  │ labels.txt               │     │   │
│  │ ├────────────────────┤  ├──────────────────────────┤     │   │
│  │ │ Version: Platform  │  │ Version: Platform        │     │   │
│  │ │ Storage: Local     │  │ Storage: Local           │     │   │
│  │ │ Deploy: SD card    │  │ Deploy: SD card          │     │   │
│  │ └────────────────────┘  └──────────────────────────┘     │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### Device Firmware Artifacts

#### BLE and LoRaWAN Firmware
- **Versioning**: Versions are tracked manually using Git tags in the firmware's GitHub repository.
- **Storage**: Built locally by developers and shared via email. No central automated registry exists
- **Deployment Method**: Installed via the Device Firmware Update (DFU) mechanism over BLE using the NRF Toolbox mobile app.

#### Himax AI Firmware
- **Versioning**: Versions are tracked manually using Git tags in the firmware's GitHub repository
- **Storage**: Built locally by developers and transferred to the device only via USB connection
- **Deployment Method**: Loaded using the device's bootloader via XMODEM transfer over a serial connection
- **Deployment Process**: See [Compile_and_flash.md](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/main/_Documentation/Compile_and_flash.md)

#### Configuration File
- **Versioning**: No versioning mechanism currently exists
- **Storage**: The `CONFIG.TXT` file is stored in the seed grove vision GitHub repository [https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/main/EPII_CM55M_APP_S/app/ww_projects/ww500_md/CONFIG](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/main/EPII_CM55M_APP_S/app/ww_projects/ww500_md/CONFIG)
- **Deployment Method**: Loaded into the device via the SD card, typically copy and paste the entire folder

### Machine Learning (ML) Artifacts

#### ML Model Package
- **Versioning**: ML models are versioned based on the scheme used in the training platform (e.g., Edge Impulse)
- **Storage**: No central automated registry. Models are prepared locally using the model formatting and conversion tool [TBC - Link to conversion tool documentation]
- **Deployment Method**: Loaded into the device via the SD card, typically included in the `Manifest.zip` folder
- **Deployment Process**: example recorded in [November Product showcase video](https://youtu.be/71M0tcAi2wo?si=iGz2dD5tA0hEu48v&t=332)

---

## 3. Target State

The Wildlife Watcher ecosystem has achieved significant automation through GitHub Actions workflows, Supabase backend, and the Streamlit model conversion tool. This section describes the current implementation and remaining work.

### Architecture Overview

```
┌───────────────────────────────────────────────────────────────────────┐
│                      CURRENT ARCHITECTURE                             │
├───────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │              GITHUB REPOSITORIES (AUTOMATED)                    │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐   │  │
│  │  │ ww-hardware  │  │ Himax        │  │ Config (MANIFEST/)   │   │  │
│  │  │ (BLE FW)     │  │ Firmware     │  │                      │   │  │
│  │  └──────┬───────┘  └──────┬───────┘  └──────────┬───────────┘   │  │
│  │         │                 │                     │               │  │
│  │         ▼                 ▼                     ▼               │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │           GITHUB ACTIONS WORKFLOWS                       │   │  │
│  │  │  • ble_build_release.yml    • build_and_release.yml      │   │  │
│  │  │  • Semantic versioning      • upload_config_firmware     │   │  │
│  │  │  • Auto upload to Supabase  • DB auto-registration       │   │  │
│  │  └──────────────────────┬───────────────────────────────────┘   │  │
│  └─────────────────────────┼───────────────────────────────────────┘  │
│                            │                                          │
│                            ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                      SUPABASE                                   │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │ Storage: firmware/ and ai-models/                        │   │  │
│  │  │  • BLE: WildlifeWatcher_1_ww500_a00_nus_XXXXXX.zip       │   │  │
│  │  │  • Himax: output.img                                     │   │  │
│  │  │  • Config: config-firmware-config-vX.Y.Z.zip             │   │  │
│  │  │  • AI Models: {org_id}/{model_name}/Manifest.zip         │   │  │
│  │  └──────────────────────────────────────────────────────────┘   │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │ Database: firmware & ai_models tables                    │   │  │
│  │  │  • Auto-registered by workflows                          │   │  │
│  │  │  • RLS: Public SELECT on firmware/ai_models              │   │  │
│  │  │  • RLS: Authenticated INSERT for models                  │   │  │
│  │  └──────────────────────────────────────────────────────────┘   │  │
│  └───────────────────────────┬─────────────────────────────────────┘  │
│                              │                                        │
│                              ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │          STREAMLIT APP (wildlife-watcher.streamlit.app)         │  │
│  │  • Public MANIFEST.zip download (Config + Default Model)        │  │
│  │  • Authenticated model conversion & upload (Vela)               │  │
│  └───────────────────────────┬─────────────────────────────────────┘  │
│                              │                                        │
│                              ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │              WILDLIFE WATCHER MOBILE APP                        │  │
│  │  • Queries firmware table on startup                            │  │
│  │  • Downloads via FirmwareService                                │  │
│  │  • BLE DFU updates (DfuService)                                 │  │
│  │  • Himax: Manual USB (OTA not yet implemented)                  │  │
│  │  • Config: Manual SD card (OTA not yet implemented)             │  │
│  │  • AI Models: Manual USB (OTA not yet implemented)              │  │
│  └─────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┘
```

### Repository Structure

1. **BLE Firmware Repository** (`ww-hardware`)
   - **Path**: `MokoTech/Workspace/WildlifeWatcher_1/ww500_a00`
   - **Workflow**: `.github/workflows/ble_build_release.yml`
   - **Trigger**: Push to `main`
   - **Output**: `WildlifeWatcher_1_ww500_a00_nus_XXXXXX.zip`

2. **Himax Firmware Repository** (`Seeed_Grove_Vision_AI_Module_V2`)
   - **Path**: `EPII_CM55M_APP_S/`
   - **Workflow**: `.github/workflows/build_and_release.yml`
   - **Trigger**: Push to `dev` or `fix/*` branches
   - **Output**: `output.img`

3. **Config Files** (in Himax repository)
   - **Path**: `EPII_CM55M_APP_S/app/ww_projects/ww500_md/MANIFEST/`
   - **Workflow**: `.github/workflows/upload_config_firmware.yml`
   - **Trigger**: Push to `main` with changes to `MANIFEST/**`
   - **Output**: `config-firmware-config-vX.Y.Z.zip`

4. **ML Model Conversion** (`wildlife-watcher-model-conversion`)
   - **Type**: Streamlit web application
   - **URL**: https://wildlife-watcher.streamlit.app
   - **Function**: Vela conversion, Supabase upload, public download

---

### Device Firmware Artifacts

#### Supabase Database Schema
```sql
CREATE TABLE public.firmware (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  name TEXT NOT NULL,
  version TEXT NOT NULL,
  type TEXT NOT NULL CHECK (type IN ('ble', 'himax', 'config')),
  location_path TEXT NOT NULL,
  file_size_bytes BIGINT,
  is_active BOOLEAN DEFAULT true,
  created_at TIMESTAMPTZ DEFAULT now(),
  modified_by uuid,
  deleted_at TIMESTAMPTZ,
  release_notes TEXT,
  metadata JSONB
);
```

#### BLE and LoRaWAN Firmware ✅ **AUTOMATED**

**Versioning**:
- **Tag Format**: `vX.Y.Z` (semantic versioning)
- **Auto-incremented**: `mathieudutour/github-tag-action` with `default_bump: minor`
- **Application Version**: 6-digit padded integer (e.g., v1.2.3 → 010203)
- **Filename**: `WildlifeWatcher_1_ww500_a00_nus_XXXXXX.zip`

**Build & Deploy**:
- **Build**: ARM GNU Toolchain 13.2, signed with `nrfutil`
- **Storage**: `firmware/ble/` bucket in Supabase
- **Registry**: Auto-registered via `upload_firmware.js`
- **Mobile App**: 
  1. Queries `firmware` table on startup
  2. Downloads via `FirmwareService.ensureFirmwareDownloaded()`
  3. User triggers update → `DfuService.startDFU()` (Nordic DFU over BLE)

**CI/CD**: `.github/workflows/ble_build_release.yml` in `ww-hardware`

---

#### Himax AI Firmware ✅ **AUTOMATED** (⚠️ OTA Deployment Pending)

**Versioning**:
- **Tag Format**: `vX.Y.Z` (semantic versioning)
- **Auto-incremented**: `mathieudutour/github-tag-action` with `default_bump: patch`
- **Filename**: `output.img`

**Build & Deploy**:
- **Build Process**:
  1. Compile with ARM GNU Toolchain 13.2
  2. Copy ELF to `we2_image_gen_local/input_case1_secboot/`
  3. Generate `.img` using `we2_local_image_gen`
- **Storage**: `firmware/himax/` bucket in Supabase
- **Registry**: Auto-registered via `upload_firmware.js`
-  **Deployment**: ⚠️ **Currently manual via USB**. OTA via mobile app not yet implemented.

**CI/CD**: `.github/workflows/build_and_release.yml` in `Seeed_Grove_Vision_AI_Module_V2`

---

#### Configuration Files ✅ **AUTOMATED**

**Versioning**:
- **Tag Format**: `config-vX.Y.Z`
- **Auto-incremented**: `mathieudutour/github-tag-action` with `tag_prefix: config-v`, `default_bump: patch`
- **Filename**: `config-firmware-config-vX.Y.Z.zip`

**Contents** (from `MANIFEST/` directory):
- `CONFIG.TXT`: Device operational parameters
- `HMSTB1.BIN`: Himax bootloader
- `README.TXT`: User instructions
- `config_file.md`: Configuration documentation

**Public Access**: ✅ Anonymous SELECT enabled via RLS

**Deployment**:
1. Developers update `MANIFEST/` files on `main` branch
2. GitHub Action zips and uploads to Supabase
3. Streamlit App bundles latest config into public `MANIFEST.zip`
4. **Users**: Extract MANIFEST.zip to SD card root directory

**CI/CD**: `.github/workflows/upload_config_firmware.yml` in `Seeed_Grove_Vision_AI_Module_V2`

---

### Machine Learning (ML) Artifacts

#### Supabase Database Schema
```sql
CREATE TABLE public.ai_models (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  name TEXT NOT NULL,
  version TEXT NOT NULL,
  organisation_id uuid REFERENCES organisations(id),
  storage_path TEXT NOT NULL,
  file_size_bytes BIGINT,
  detection_capabilities TEXT[],
  description TEXT,
  uploaded_by uuid REFERENCES auth.users(id),
  modified_by uuid,
  created_at TIMESTAMPTZ DEFAULT now(),
  deleted_at TIMESTAMPTZ,
  file_type TEXT DEFAULT 'manifest'
);
```

#### Model Conversion & Upload ✅ **OPERATIONAL** (⚠️ Model Zoo Integration Pending)

**Current Workflow** (Streamlit App):
1. **Upload**: User logs in and uploads Edge Impulse zip (`model-custom-version.zip`)
2. **Convert**: App runs Vela compiler (`ethos-u55-64`) to optimize model
3. **Extract Labels**: Parsed from `model_variables.h`
4. **Package**: Creates `Manifest.zip` with `model_vela.tfl` and `labels.txt`
5. **Register**: User selects organization → uploads to Supabase Storage (`ai-models/{org_id}/`) and creates DB record

**Public Download**:
- **General Organization Models**: Public SELECT via RLS
- **MANIFEST.zip**: One-click download of Config Firmware + Default Model (no login required)

**Access Control**:
- **Uploads**: Requires `organisation_manager` role
- **Downloads**: 
  - General org: Public
  - Private orgs: Members only

---

### Remaining Work

#### 1. Himax OTA Updates 🚧
**Goal**: Enable firmware updates via mobile app (BLE or WiFi)  
**Current**: Manual USB flashing required  
**Blocker**: Requires BLE transfer protocol or integration with device bootloader

#### 2. Model Zoo Integration 🚧
**Goal**: Automated model conversion pipeline as backend service  
**Current**: Manual upload via Streamlit app  
**Future**:
- Admin portal for model upload
- Backend service validates & converts models automatically
- Performance metrics & validation checks
- Model versioning & rollback

#### 3. Admin Portal 🚧
**Goal**: Web interface for organization administrators  
**Features**:
- Manage organization-specific models
- View firmware update status
- User/role management
- Analytics dashboard

#### 4. Automated Testing & Validation 🚧
**Goal**: CI/CD integration tests for firmware  
**Current**: Manual testing  
**Future**:
- Checksum verification
- Compatibility checks
- Regression testing

---
