/* exif_handler.c
 *
 * Author: TP
 * Date: 06/11/2024
 *
 * This file creates the EXIF handlers, set tags, save and destroy an EXIFHandler object.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exif-data.h"
#include "exif-content.h"
#include "exif_handler.h"
#include "ff.h"
#include <sys/time.h>

int _gettimeofday(struct timeval *tv, void *tzvp)
{
    (void)tv;   // Avoid unused parameter warning
    (void)tzvp; // Avoid unused parameter warning
    return 0;   // Stub always returns success
}

/* Create a new EXIFHandler
 * Returns: handler - a pointer to the new EXIFHandler
 */
EXIFHandler *media_exif_handler_create()
{
    EXIFHandler *handler = (EXIFHandler *)pvPortMalloc(sizeof(EXIFHandler));
    if (!handler)
    {
        fprintf(stderr, "Failed to allocate memory for EXIFHandler.\n");
        return NULL;
    }
    handler->exif_data = exif_data_new();
    if (!handler->exif_data)
    {
        fprintf(stderr, "Failed to create new ExifData.\n");
        free(handler);
        return NULL;
    }
    return handler;
}

/* Create a new EXIFHandler
 * Returns: handler - a pointer to the new EXIFHandler
 */
EXIFHandler *observations_exif_handler_create()
{
    EXIFHandler *handler;
    handler->exif_data = exif_data_new();
    if (!handler->exif_data)
    {
        fprintf(stderr, "Failed to create ExifData object.\n");
        return;
    }
    return handler;
}

/* Create a new EXIFHandler
 * Returns: handler - a pointer to the new EXIFHandler
 */
EXIFHandler *deployment_exif_handler_create()
{
    EXIFHandler *handler;
    handler->exif_data = exif_data_new();
    return handler;
}

/* Set a tag value in EXIF data
 * Parameters: handler - the EXIFHandler object
 *             tag - the tag to set
 *             value - the value to set
 * Returns: void
 */
void exif_handler_set_tag(EXIFHandler *handler, ExifTag tag, const char *value)
{
    if (!handler || !handler->exif_data || !value)
    {
        fprintf(stderr, "Invalid parameters to exif_handler_set_tag.\n");
        return;
    }

    // ExifEntry *entry = NULL;
    // Assume EXIF_IFD_0 as the default IFD for simplicity
    ExifIfd ifd = EXIF_IFD_0;

    // Preallocate entry for the specified tag in the IFD
    ExifEntry *entry = exif_content_get_entry(handler->exif_data->ifd[ifd], tag);
    if (!entry)
    {
        entry = exif_entry_new();
        if (!entry)
        {
            fprintf(stderr, "Failed to allocate EXIF entry.\n");
            return;
        }
        entry->tag = tag;
        // exif_content_add_entry(handler->exif_data->ifd[ifd], entry);
        exif_entry_unref(entry);
    }

    // exif_entry_initialize(entry, tag);
    // // Set the value manually
    // size_t value_len = strlen(value) + 1; // Include null-terminator
    // if (value_len > entry->size)
    // {
    //     fprintf(stderr, "Value length exceeds entry size. Truncating.\n");
    //     value_len = entry->size; // Truncate if necessary
    // }
    // memcpy(entry->data, value, value_len);
    // entry->data[entry->size - 1] = '\0'; // Ensure null-terminated string
}

// Save EXIF data to a file
// TODO: Implement in fatfs task instead
void exif_handler_save(EXIFHandler *handler, const char *file_path)
{
    FIL fsrc, fdst;          // File objects for source and destination files
    FRESULT res;             // FatFs function common result code
    UINT br, bw;             // Bytes read/written
    char temp_file_path[64]; // Temporary file path
    BYTE buffer[512];        // Buffer for file operations

    res = f_open(&fsrc, file_path, FA_READ);
    if (res)
    {
        xprintf("Fail opening file %s\n", file_path);
        return res;
    }

    // Create a temporary file for writing
    snprintf(temp_file_path, sizeof(temp_file_path), "%s.tmp", file_path);
    res = f_open(&fdst, temp_file_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK)
    {
        xprintf("Failed to create temp file %s (error: %d)\n", temp_file_path, res);
        f_close(&fsrc);
        return;
    }
    xprintf("Created temporary file: %s\n", temp_file_path);

    // Write EXIF metadata to the temp file
    if (handler->exif_data && handler->exif_data->size > 0)
    {
        res = f_write(&fdst, handler->exif_data, handler->exif_data->size, &bw);
        if (res != FR_OK || bw != handler->exif_data->size)
        {
            xprintf("Failed to write EXIF data to temp file (error: %d)\n", res);
            goto cleanup;
        }
        xprintf("Wrote EXIF data (%u bytes) to temp file\n", bw);
    }

    // Copy original file content to the temp file
    while ((res = f_read(&fsrc, buffer, sizeof(buffer), &br)) == FR_OK && br > 0)
    {
        res = f_write(&fdst, buffer, br, &bw);
        if (res != FR_OK || bw != br)
        {
            xprintf("Failed to copy image data to temp file (error: %d)\n", res);
            goto cleanup;
        }
    }
    if (res != FR_OK)
    {
        xprintf("Error reading from source file (error: %d)\n", res);
        goto cleanup;
    }

    xprintf("Successfully appended EXIF data to temp file\n");

    // Close both files
    f_close(&fsrc);
    f_close(&fdst);

    // Replace the original file with the temp file
    res = f_unlink(file_path); // Delete the original file
    if (res != FR_OK)
    {
        xprintf("Failed to delete original file (error: %d)\n", res);
        return;
    }
    res = f_rename(temp_file_path, file_path); // Rename temp file to original file name
    if (res != FR_OK)
    {
        xprintf("Failed to rename temp file to original file (error: %d)\n", res);
        return;
    }

    xprintf("Successfully saved EXIF data to file: %s\n", file_path);
    return;

cleanup:
    f_close(&fsrc);
    f_close(&fdst);
    f_unlink(temp_file_path); // Remove temp file in case of failure
    xprintf("Cleanup complete after error\n");
}

// Destroy the EXIFHandler and free resources
void exif_handler_destroy(EXIFHandler *handler)
{
    if (handler)
    {
        if (handler->exif_data)
            exif_data_unref(handler->exif_data);
        free(handler);
    }
}