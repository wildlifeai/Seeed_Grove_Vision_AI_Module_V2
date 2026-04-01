# Task: Refactor directory_manager.c
#### CGP 1 April 2026

## Status: Analysis complete, implementation complete

The analysis was carried out in the session of 1 April 2026. A refactoring proposal
was placed in a markdown file `directory_manager.md`. The user then asked Claude to make the changes
and this has been done. 

Task complete.

---

## Project context

Embedded C project for a Himax HX6538 processor, using GNU tools and Eclipse IDE.
Git-managed. The active branch at the time of writing is `filenames`.

The project is `ww500_md`, built via:
```
EPII_CM55M_APP_S/makefile
EPII_CM55M_APP_S/app/app.mk
EPII_CM55M_APP_S/app/ww_projects/ww.mk
EPII_CM55M_APP_S/app/ww_projects/ww500_md/ww500_md.mk
```

The SD card uses the FatFS file system. The application does two things with the SD card:

- Reads/writes configuration data from `CONFIG.TXT`
- Writes image files (.JPG or .BMP) into a series of directories, e.g. `IMAGES.000`, `IMAGES.001`

## The problem

`directory_manager.c` is poorly structured. A `directoryManager_t` struct was created
to hold information about both file types, and functions such as
`dir_mgr_add_capture_folder()` and `dir_mgr_delete_capture_folder()` were partly
implemented but never finished or tested (currently commented out). Not all fields in
the struct are used.

The only useful functions are:

1. `FRESULT dir_mgr_init_directories(directoryManager_t *dirManager)`
2. `void dir_mgr_generateImageFilename(char *imageFileName, uint8_t filenameLen, char *type)`
3. `void dir_mgr_generateImageDirName(char *imageDirName, uint8_t dirNameLen)`
4. `void dir_mgr_createImageDir(char *path_buf)`

Functions 2, 3, and 4 were written by CGP and are OK.

`dir_mgr_init_directories()` is problematic: it looks for the `CONFIG.TXT` folder and
then tries to set up the image folder, but the image folder name depends on values in
`operational_parameters[]` which are not yet read from CONFIG.TXT at that point.

The temporary workaround (to be cleaned up):

1. Inside `dir_mgr_init_directories()`:
```c
// TODO - temporary only as the file may change after we load the CONFIG.TXT values
dir_mgr_generateImageDirName(path_buf, sizeof(path_buf));
dir_mgr_createImageDir(path_buf);
```

2. Later, inside `vFatFsTask()` after CONFIG.TXT values are loaded:
```c
// TODO clean this up! We have to call dir_mgr_generateImageDirName()
// a second time now that the operational parameters have been read
char path_buf[IMAGEFILENAMELEN];
dir_mgr_generateImageDirName(path_buf, IMAGEFILENAMELEN);
dir_mgr_createImageDir(path_buf);
```

Code inside `#ifdef UNZIPMANIFEST` can be omitted — unlikely ever to be used.

## Tasks for Claude

1. Read the relevant files and propose a clean refactoring of `directory_manager.c`.
   Key files are within the `ww500_md` folder; it is unlikely files outside it will
   be needed.
2. Place the analysis/proposal in a markdown file for the user to review.
3. After review (and possible iteration), make the agreed changes.
