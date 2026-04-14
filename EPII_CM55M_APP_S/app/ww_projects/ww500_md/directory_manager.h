/**
 * @file directory_manager.h
 *
 *  Created: 31 Jul 2025
 *      Author: TBP
 *
 * @brief Header file for directory management functions.
 *
 * This file contains declarations for functions that manage directories,
 * including creating, deleting, and listing directories.
 *
 * CGP 13/11/25 - Need to think about all of these functions and how they are used.
 */

#ifndef APP_WW_PROJECTS_WW500_MD_DIRECTORY_MANAGER_H_
#define APP_WW_PROJECTS_WW500_MD_DIRECTORY_MANAGER_H_

/********************************** Includes ******************************************/

#include <stdbool.h>
#include "ww500_md.h"
#include "ff.h"
#include "image_task.h"

// Warning: if using 8.3 file names then this applies to directories also.
// Names are upper case.

#define STATE_FILE "CONFIG.TXT"

#define CONFIG_DIR "/MANIFEST"
#define CAPTURE_DIR "IMAGES"
#define MEDIA_DIR "/MEDIA"

// Buffer length for directory path strings.
// Kept separate from IMAGEFILENAMELEN (which is sized for 8.3 filenames only).
// Typically:
// /MEDIA/xxxxxxxx/IMAGES.000 = 27 characters including trailing \0
#define DIRNAMELEN 32

// #define MAX_TRACKED_DIRS 5
// #define MAX_DIR_NAME_LEN 64

/**************************************** Type declarations  *************************************/

// typedef enum {
//     DIR_CONFIG,
//     DIR_IMAGES,
//     DIR_TFLITE,
//     DIR_LOGS,
//     DIR_MODELS,
//     DIR_COUNT  
// } DirectoryType;

// typedef struct {
//     FIL file;                     
//     FRESULT res;                  
//     bool isOpen;                  
//     char path[MAX_DIR_NAME_LEN]; 
// } DirectoryEntry;

// typedef struct {
//     DirectoryEntry dirs[DIR_COUNT]; // One entry per tracked directory
//     int imageDirIdx;                
//     char baseDir[MAX_DIR_NAME_LEN]; // Base mount path (e.g., "")
// } DirectoryManager;

typedef struct
{
    FIL configFile;                         // File object for the config directory
    FIL imagesFile;                         // File object for the images directory
    FRESULT configRes;                      // Result code for config operations
    FRESULT imagesRes;                      // Result code for image operations
    bool configOpen;                        // Flag to indicate if config file is open
    bool imagesOpen;                        // Flag to indicate if images file is open
    char current_config_dir[DIRNAMELEN];    // Current config directory path
    char current_capture_dir[DIRNAMELEN];   // Current capture directory path
} directoryManager_t;

/**************************************** Global Defines  *************************************/

// extern char current_dir[256];
extern directoryManager_t dirManager;

/**************************************** Global Function Declarations  *************************************/

// Phase 1 init: called early (before CONFIG.TXT is loaded). Sets up /MANIFEST and
// initialises the dirManager state.
FRESULT dir_mgr_init_config(directoryManager_t *dirManager);

// Phase 2 init: called after load_configuration(), once op_parameter[] is valid.
// Determines and creates the correct image directory.
FRESULT dir_mgr_init_image_dir(directoryManager_t *dirManager);

void dir_mgr_generateImageFilename(char *imageFileName, uint8_t filenameLen, char *type);


#endif /* APP_WW_PROJECTS_WW500_MD_DIRECTORY_MANAGER_H_ */
