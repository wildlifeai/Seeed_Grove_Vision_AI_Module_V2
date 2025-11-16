# Wildlife Watcher Artifact Management

This document outlines the various digital artifacts that comprise the Wildlife Watcher ecosystem, how they are managed, and how they interact with the mobile application and backend services.

## 1. Artifact Definitions

The Wildlife Watcher device relies on two primary categories of digital artifacts to function: Device Firmware and Machine Learning (ML) Models.

### Device Firmware Artifacts

This set of artifacts defines the software running on the two microprocessors of the Wildlife Watcher device.

#### BLE and LoRaWAN Firmware
- **Filename Format**: `ble_firmware.zip` (example: `WildlifeWatcher_1_ww500_a00_000804.zip`)
- **Purpose**: Runs on the MKL62BA processor, managing Bluetooth Low Energy (BLE) and LoRaWAN communications
- **Version Control**: Version number is indicated in the filename (e.g., `000804`)
- **Update Protection**: The Device Firmware Update (DFU) process prevents the installation of earlier versions
- **Repository**: https://github.com/wildlifeai/ww-hardware/MokoTech/Workspace/WildlifeWatcher_1/ww500_c01 [TBC]

#### Himax AI Firmware
- **Filename**: `output.img`
- **Purpose**: Runs on the Himax AI processor, handling image capture, sensor management, and machine learning inference
- **Build Process**: Binary is generated during the application compilation process
- **Repository**: [Seeed_Grove_Vision_AI_Module_V2](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2)[TBC]
- **Build Documentation**: See [Seeed_Grove_Vision_AI_Module_V2](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/_Documentation/Compile_and_flash.md) [TBC]

#### Configuration File
- **Filename**: `CONFIG.TXT`
- **Location**: `/MANIFEST/` directory
- **Purpose**: Defines default device parameters including:
  - Operational parameters (e.g., `Delay before DPD`, `Num pics when triggered`)
  - Sensor configuration settings
  - Device recovery data
- **Runtime Override**: The mobile app can override default parameters by sending specific BLE commands
- **Repository**: [TBC - CP Link to hardware GitHub repository?]

### Machine Learning (ML) Artifacts

The AI capabilities of the device are defined by the ML model and its accompanying documentation.

#### ML Model Package
- **Package Name**: `Manifest.zip`
- **Contents**:
  - **ML Model**: `/MANIFEST/model_vela.tfl` - The trained neural network model that identifies species in images
  - **Model Labels**: `/MANIFEST/labels.txt` - Maps the model's output indices to human-readable species names
- **Training Guide**: https://www.notion.so/Machine-Learning-Models-2a78b68cc7b480d38b7ee00cdae251aa
- **Conversion Tool**: https://github.com/wildlifeai/wildlife-watcher-model-conversion

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
- **Versioning**: Versions are tracked manually using Git tags in the firmware's GitHub repository [TBC - Confirm with CP]
- **Storage**: Built locally by developers and shared via email. No central automated registry exists
- **Deployment Method**: Installed via the Device Firmware Update (DFU) mechanism over BLE using the NRF Toolbox mobile app [TBC - Link to NRF Toolbox documentation]
- **Deployment Process**: [TBC - Link to DFU deployment documentation]

#### Himax AI Firmware
- **Versioning**: Versions are tracked manually using Git tags in the firmware's GitHub repository
- **Storage**: Built locally by developers and transferred to the device only via USB connection
- **Deployment Method**: Loaded using the device's bootloader via XMODEM transfer over a serial connection
- **Deployment Process**: See [TBC - Link to Compile_and_flash.md]

#### Configuration File
- **Versioning**: No versioning mechanism currently exists
- **Storage**: The `CONFIG.TXT` file is stored in the hardware GitHub repository [TBC - Confirm with CP]
- **Deployment Method**: Loaded into the device via the SD card, typically included in the `Manifest.zip` folder
- **Deployment Process**: [TBC - Link to SD card deployment documentation]

### Machine Learning (ML) Artifacts

#### ML Model Package
- **Versioning**: ML models are versioned based on the scheme used in the training platform (e.g., Edge Impulse)
- **Storage**: No central automated registry. Models are prepared locally using the model formatting and conversion tool [TBC - Link to conversion tool documentation]
- **Deployment Method**: Loaded into the device via the SD card, typically included in the `Manifest.zip` folder
- **Deployment Process**: [TBC - Link to model deployment documentation]

---

## 3. Target State for Beta

The goal is to create a semi automated, single source of truth for all artifacts, managed through Supabase and the mobile app.

### Architecture Overview

```
┌───────────────────────────────────────────────────────────────────────┐
│                         BETA STATE                                    │
├───────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                    GITHUB REPOSITORIES                          │  │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │  │
│  │  │ BLE Firmware     │  │ Himax Firmware   │  │ Config Repo   │  │  │
│  │  │ Repo             │  │ Repo             │  │               │  │  │
│  │  └────────┬─────────┘  └────────┬─────────┘  └───────┬───────┘  │  │
│  │           │                     │                    │          │  │
│  │           │ Git tag/release     │ Git tag/release    │ Git tag  │  │
│  │           ▼                     ▼                     ▼         │  │
│  │  ┌────────────────────────────────────────────────────────────┐ │  │
│  │  │           GITHUB ACTIONS WORKFLOWS                         │ │  │
│  │  │  • Build firmware         • Build firmware  • Package      │ │  │
│  │  │  • Upload to Supabase     • Upload to SB    • Upload to SB │ │  │
│  │  │  • Update DB registry     • Update DB       • Update DB    │ │  │
│  │  └────────────────────────┬───────────────────────────────────┘ │  │
│  └───────────────────────────┼─────────────────────────────────────┘  │
│                              │                                        │
│                              ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                      SUPABASE                                   │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │ Storage: firmware-releases/                              │   │  │
│  │  │  • ble_firmware_v{version}.zip                           │   │  │
│  │  │  • himax_firmware_v{version}.img                         │   │  │
│  │  │  • config_v{version}.txt                                 │   │  │
│  │  └──────────────────────────────────────────────────────────┘   │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │ Database: firmware table                                 │   │  │
│  │  │  • id, type, version, storage_path, file_size, metadata  │   │  │
│  │  └──────────────────────────────────────────────────────────┘   │  │
│  └───────────────────────────┬─────────────────────────────────────┘  │
│                              │                                        │
│                              ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │              WILDLIFE WATCHER MOBILE APP                        │  │
│  │  • Query firmware table for latest versions                     │  │
│  │  • Download artifacts from Supabase Storage                     │  │
│  │  • Deploy via "Update Firmware" workflow                        │  │
│  │  • Deploy config via BLE or SD card                             │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                    ML ARTIFACTS (MANUAL)                        │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │ Developer → Conversion Tool → Supabase Storage           │   │  │
│  │  │ Storage: model-releases/manifest_v{version}.zip          │   │  │
│  │  │ Database: ai_models table (manual update)                │   │  │
│  │  │ Deployment: SD card via Manifest.zip                     │   │  │
│  │  └──────────────────────────────────────────────────────────┘   │  │
│  └─────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┘
```

### Implementation Strategy

To achieve proper artifact separation and automation, the following repository structure is recommended:

#### Repository Structure
1. **BLE Firmware Repository** (`ww-ble-firmware`)
   - Contains: BLE/LoRaWAN firmware source code
   - CI/CD: GitHub Actions workflow triggered on Git tag creation
   - Output: `ble_firmware_v{version}.zip`

2. **Himax Firmware Repository** (`ww-himax-firmware`)
   - Contains: Himax AI processor firmware source code
   - CI/CD: GitHub Actions workflow triggered on Git tag creation
   - Output: `output_v{version}.img`

3. **Configuration Repository** (`ww-device-config`)
   - Contains: `CONFIG.TXT` template and versioning
   - CI/CD: GitHub Actions workflow triggered on Git tag creation
   - Output: `config_v{version}.txt`
   - Note: This decouples configuration from ML artifacts for independent versioning

4. **ML Models Repository** (Optional for Beta)
   - Contains: Model training scripts and documentation
   - Manual process for Beta: Developer uploads to Supabase directly

### Device Firmware Artifacts

All firmware artifacts will be managed through an automated CI/CD pipeline with a centralized registry in Supabase.

#### Supabase Database Schema
```sql
-- firmware table structure
{
  id: uuid,
  type: enum('ble', 'himax', 'config'),
  version: string,
  storage_path: string,
  file_size: integer,
  created_at: timestamp,
  metadata: jsonb
}
```

#### BLE and LoRaWAN Firmware
- **Versioning**: Automated via GitHub Actions workflow triggered on Git tag or release creation
- **Storage**: Artifacts automatically built and uploaded to `firmware-releases/ble/` bucket in Supabase Storage
- **Registry**: Metadata registered in the `firmware` table with `type='ble'`
- **Deployment Method**: Wildlife Watcher mobile app queries the `firmware` table to discover every time the app is initiated and there is internet connection and download latest artifacts locally
- **Deployment Process**: 
  1. User initiates "Update Firmware" operation in mobile app
  2. App installs local `ble_firmware_v{version}.zip` firmware via DFU connection
- **CI/CD Workflow**: [TBC - Link to BLE firmware GitHub Actions workflow]

#### Himax AI Firmware
- **Versioning**: Automated via GitHub Actions workflow triggered on Git tag or release creation
- **Storage**: The `.img` file automatically built and uploaded to `firmware-releases/himax/` bucket in Supabase Storage
- **Registry**: Metadata registered in the `firmware` table with `type='himax'`
- **Deployment Method**: Wildlife Watcher mobile app queries the `firmware` table to discover every time the app is initiated and there is internet connection and download latest artifacts locally
- **Deployment Process**: 
  1. User initiates "Update Firmware" operation in mobile app
  2. App transfers local `output_v{version}.img` firmware to device via BLE [CP to confirm]
- **CI/CD Workflow**: [TBC - Link to Himax firmware GitHub Actions workflow]

#### Configuration File
- **Versioning**: Automated via GitHub Actions workflow triggered on Git tag or release creation in dedicated config repository
- **Storage**: Managed in Supabase Storage at `firmware-releases/config/` as part of the single source of truth
- **Registry**: Metadata registered in the `firmware` table with `type='config'`
- **Parameter Types**: [TBC by CP and TP]
  - **Generic operational parameters**: DPD delay, LoRaWAN heartbeat interval (same for all devices)
  - **Project-specific parameters**: Motion detection vs time-lapse mode (modified by app in "Prepare and Test" workflow)
- **Deployment Method**: Wildlife Watcher mobile app queries the `firmware` table to discover every time the app is initiated and there is internet connection and download latest artifacts locally
- **Deployment Process**: 
  1. User initiates "Update Firmware" operation in mobile app (bundled with firmware updates)
  2. App transfers local `config_v{version}.txt` via BLE commands
- **Runtime Override**: The mobile app overrides parameters during "Prepare and Test" workflow [TBC - Link to Prepare and Test workflow documentation]
- **CI/CD Workflow**: [TBC - Link to config GitHub Actions workflow]

### Machine Learning (ML) Artifacts

ML artifacts remain primarily manual for Beta, with a simplified registry system.

#### ML Model Package
- **Versioning**: ML models versioned based on training platform scheme (e.g., Edge Impulse version numbers) [TBC by TP]
- **Storage**: Central registry in Supabase database (`ai_models` table) with one rat detection model for Beta [Potentially a person detection for testing purposes?]
- **Registry Update**: Developers manually update registry after preparing and uploading new models to Supabase Storage at `model-releases/`
- **Deployment Method**: Loaded into device via SD card in the `Manifest.zip` folder
- **Deployment Process**: 
  1. Developer uses [conversion tool to prepare model ](https://github.com/wildlifeai/wildlife-watcher-model-conversion)
  2. Developer uploads `Manifest.zip` to Supabase Storage
  3. Developer manually updates `ai_models` table with version metadata
  4. User downloads Manifest.zip via [conversion tool to prepare model ](https://github.com/wildlifeai/wildlife-watcher-model-conversion)
  5. User writes to SD card and inserts into device
- **Beta Scope**: Single rat detection model only

#### Supabase Database Schema [TBC by VA and TP]
```sql
-- ai_models table structure
{
  id: uuid,
  model_name: string,
  version: string,
  storage_path: string,
  labels_path: string,
  created_at: timestamp,
  metadata: jsonb
}
```

---

## 4. Target State Beyond Beta

Post-Beta, the goal is to create a fully automated, user-friendly system for all artifact management with minimal manual intervention.

### Architecture Overview

```
┌───────────────────────────────────────────────────────────────────────┐
│                      POST-BETA STATE                                  │
├───────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │              GITHUB REPOSITORIES (AUTOMATED)                    │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐   │  │
│  │  │ BLE Firmware │  │ Himax        │  │ Config Repo          │   │  │
│  │  │              │  │ Firmware     │  │                      │   │  │
│  │  └──────┬───────┘  └──────┬───────┘  └────────-──┬──────────┘   │  │
│  │         │                 │                      │              │  │
│  │         └─────────────────┴──────────────────────┘              │  │
│  │                           │                                     │  │
│  │                    Git tag/release                              │  │
│  │                           ▼                                     │  │
│  │         ┌─────────────────────────────────────────┐             │  │
│  │         │  GITHUB ACTIONS (Full CI/CD)            │             │  │
│  │         │  • Automated build                      │             │  │
│  │         │  • Automated upload to Supabase         │             │  │
│  │         │  • Automated DB registry update         │             │  │
│  │         │  • Automated testing & validation       │             │  │
│  │         └─────────────────┬───────────────────────┘             │  │
│  └───────────────────────────┼─────────────────────────────────────┘  │
│                              │                                        │
│                              ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                      SUPABASE BACKEND                           │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │ Storage (Centralized)                                    │   │  │
│  │  │  firmware-releases/                                      │   │  │
│  │  │    • ble_firmware_v{version}.zip                         │   │  │
│  │  │    • himax_firmware_v{version}.img                       │   │  │
│  │  │    • config_v{version}.txt                               │   │  │
│  │  │  model-releases/                                         │   │  │
│  │  │    • manifest_v{version}.zip (org-specific)              │   │  │
│  │  └──────────────────────────────────────────────────────────┘   │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │ Database (Automated Registry)                            │   │  │
│  │  │  • firmware table (auto-updated)                         │   │  │
│  │  │  • ai_models table (auto-updated)                        │   │  │
│  │  └──────────────────────────────────────────────────────────┘   │  │
│  │  ┌──────────────────────────────────────────────────────────┐   │  │
│  │  │ Edge Functions                                           │   │  │
│  │  │  • Model validation service                              │   │  │
│  │  │  • Model conversion service (Model Zoo integration)      │   │  │
│  │  │  • Firmware validation service                           │   │  │
│  │  └──────────────────────────────────────────────────────────┘   │  │
│  └───────────────────────────┬─────────────────────────────────────┘  │
│                              │                                        │
│                              ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │              ADMIN PORTAL (Web Interface)                       │  │
│  │  Organisation Administrators can:                               │  │
│  │  • Upload custom-trained models                                 │  │
│  │  • Trigger automated conversion (Model Zoo)                     │  │
│  │  • Manage organization-specific artifacts                       │  │
│  │  • View firmware update status                                  │  │
│  └───────────────────────────┬─────────────────────────────────────┘  │
│                              │                                        │
│                              ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │              WILDLIFE WATCHER MOBILE APP                        │  │
│  │  • Query registries for latest versions                         │  │
│  │  • Download artifacts from Supabase Storage                     │  │
│  │  • Deploy via unified "Update Firmware" workflow                │  │
│  │  • Support over-the-air (OTA) updates                           │  │
│  │  • Access organization-specific models                          │  │
│  └─────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┘
```

### Device Firmware Artifacts

A full CI/CD pipeline will be implemented to automate the versioning and uploading of all firmware artifacts to Supabase.

#### Automated CI/CD Pipeline

**Trigger**: New release or Git tag creation in any firmware GitHub repository

**Workflow Steps**:
1. **Build**: GitHub Actions workflow checks out code and builds the firmware
   - BLE firmware: Produces `ble_firmware_v{version}.zip`
   - Himax firmware: Produces `output_v{version}.img`
   - Config: Produces `config_v{version}.txt`

2. **Upload to Supabase Storage**: Built artifact uploaded to dedicated bucket
   - Storage location: `firmware-releases/{type}/`
   - Naming convention: `{type}_firmware_v{version}.{ext}`

3. **Update Database Registry**: Workflow calls Supabase Edge Function to automatically update `firmware` table
   - Metadata includes: name, version number, storage path, file size, checksums
   - Edge Function: [TBC - Link to registry update Edge Function]

4. **Validation & Testing**: Automated tests verify artifact integrity
   - Checksum verification
   - Basic functionality tests
   - Compatibility checks

**CI/CD Workflows**:
- [TBC - Link to BLE firmware CI/CD workflow]
- [TBC - Link to Himax firmware CI/CD workflow]
- [TBC - Link to config CI/CD workflow]

#### BLE and LoRaWAN Firmware
- **Versioning**: Fully automated via CI/CD pipeline
- **Storage**: `firmware-releases/ble/` in Supabase Storage
- **Registry**: Auto-updated `firmware` table with `type='ble'`
- **Deployment Method**: Over-the-air (OTA) updates via mobile app
- **Deployment Process**: [TBC - Link to OTA update documentation]

#### Himax AI Firmware
- **Versioning**: Fully automated via CI/CD pipeline
- **Storage**: `firmware-releases/himax/` in Supabase Storage
- **Registry**: Auto-updated `firmware` table with `type='himax'`
- **Deployment Method**: Over-the-air (OTA) updates via mobile app or USB
- **Deployment Process**: [TBC - Link to OTA update documentation]

#### Configuration File
- **Versioning**: Fully automated via CI/CD pipeline
- **Storage**: `firmware-releases/config/` in Supabase Storage
- **Registry**: Auto-updated `firmware` table with `type='config'`
- **Deployment Method**: OTA updates bundled with firmware updates or via BLE commands
- **Deployment Process**: Seamlessly integrated into unified firmware update workflow

### Machine Learning (ML) Artifacts

The Model Zoo will be integrated with Supabase to create a user-friendly platform for custom model management with full automation.

#### Model Zoo Integration Architecture

**User Interface**: Admin Portal (web-based) for Organization Administrators

**Upload Process**:
1. **User Upload**: Administrator uploads custom-trained model through Admin Portal
   - Upload interface: [TBC - Link to Admin Portal documentation]
   - Supported formats: [TBC - Link to supported model formats]

2. **Automated Conversion**: Upload triggers backend service that uses Model Zoo conversion scripts
   - Service validates model format and structure
   - Converts model to Himax-compatible format (`model_vela.tfl`)
   - Generates or validates `labels.txt`
   - Packages into `Manifest.zip` with `/MANIFEST/` structure
   - Model Zoo documentation: [TBC - Link to Model Zoo integration documentation]

3. **Registry Update**: Once processed, model automatically added to `ai_models` registry
   - Storage location: `model-releases/{org_id}/`
   - Made available to specific organization only
   - Registry includes: model metadata, performance metrics, training info

4. **Validation**: Automated validation ensures model meets requirements
   - Format validation
   - Size constraints
   - Performance benchmarks

**Deployment Process**:
1. Mobile app queries `ai_models` table for organization-specific models
2. User selects desired model version
3. App downloads `Manifest.zip` from Supabase Storage
4. User deploys to device via SD card or OTA (future capability)

#### Organization-Specific Model Management
- **Isolation**: Each organization has separate storage bucket and registry entries
- **Access Control**: Models only visible to users within the organization
- **Versioning**: Full version history maintained per organization
- **Rollback**: Easy rollback to previous model versions

#### ML Artifact Automation
- **Storage**: `model-releases/{org_id}/` in Supabase Storage
- **Registry**: `ai_models` table with `organization_id` field
- **Conversion**: Automated via Model Zoo Edge Function [TBC - Link to conversion Edge Function]
- **Deployment**: Via mobile app with organization filtering
