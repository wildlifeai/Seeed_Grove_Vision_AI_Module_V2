/**
 * metadata.c
 * created on: 24.12.2024
 * author: 	TBP
 *
 * @brief Metadata functions to add custom metadata to JPEG images via manually inserting data into the APP1 block
 * Checking to see if the images are successful, I was using the exiftool on the command line to check the metadata.
 * "exiftool -g1 -a -s -warning -validate image0001.jpg"
 * "exiftool image0001.jpg -v3"
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "metadata.h"

#define APP1_MARKER 0xE1
#define TIFF_HEADER_SIZE 8
#define IFD_ENTRY_SIZE 12
#define EXIF_HEADER "Exif\0\0"
#define BYTE_ORDER "II" // Little-endian format for Intel systems
#define TIFF_TAG_VERSION 42
#define TAG_OFFSET_IFD0 8
#define EXIF_TAG_COUNT 6 // Adjust this for the number of tags you are adding

#pragma pack(push, 1)
typedef struct
{
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t valueOffset;
} ExifTag;
#pragma pack(pop)

uint8_t *writeExifTag(uint8_t *ptr, uint16_t tag, uint16_t type, uint32_t count, uint32_t valueOffset)
{
    ExifTag *exifTag = (ExifTag *)ptr;
    exifTag->tag = tag;
    exifTag->type = type;
    exifTag->count = count;
    exifTag->valueOffset = valueOffset;
    return ptr + sizeof(ExifTag);
}

int createAPP1Block(ImageMetadata *metadata, unsigned char *buffer, int bufferSize)
{
    const char exifHeader[] = "Exif\0\0";                 // EXIF identifier
    const size_t exifHeaderSize = sizeof(exifHeader) - 1; // Exclude null terminator
    const uint32_t numTags = 4;
    const uint32_t xResolutionOffset = 0xA8;                  // example offset for XResolution
    const uint32_t yResolutionOffset = xResolutionOffset + 8; // example offset for YResolution

    // Verify buffer size
    if (bufferSize < exifHeaderSize + 2 + 8 + 2 + numTags * sizeof(ExifTag) + 4 + 256)
    {             // 2 bytes for length field, 8 bytes for TIFF header, 2 bytes for tag count, 4 bytes for next IFD offset, 256 bytes for metadata
        return 0; // Not enough space
    }

    // Start writing APP1 block
    uint8_t *ptr = buffer;

    // APP1 Marker
    *ptr++ = 0xFF;
    *ptr++ = APP1_MARKER;

    // Placeholder for APP1 block length (big-endian)
    uint8_t *lengthPtr = ptr;
    ptr += 2;

    // EXIF Header
    memcpy(ptr, exifHeader, exifHeaderSize);
    ptr += exifHeaderSize;

    // Start of TIFF header
    *ptr++ = 0x49;
    *ptr++ = 0x49;

    // TIFF Header: Fixed "42" marker
    *ptr++ = 0x2A;
    *ptr++ = 0x00;

    // Offset to first IFD (start of metadata), set to 8
    *ptr++ = 0x08;
    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0x00;

    // Number of IFD0 entries
    *ptr++ = (uint8_t)(numTags);
    *ptr++ = 0x00;

    // Write tags
    ptr = writeExifTag(ptr, 0x011A, 5, 1, xResolutionOffset); // XResolution
    ptr = writeExifTag(ptr, 0x011B, 5, 1, yResolutionOffset); // YResolution
    ptr = writeExifTag(ptr, 0x0128, 3, 1, 2);                 // ResolutionUnit
    ptr = writeExifTag(ptr, 0x0213, 3, 1, 1);                 // YCbCrPositioning

    // Next IFD offset, set to 0 (no more IFDs)
    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0x00;

    // Write the actual values for XResolution and YResolution
    uint32_t *xResolution = (uint32_t *)(buffer + xResolutionOffset);
    xResolution[0] = 72;
    xResolution[1] = 1;

    uint32_t *yResolution = (uint32_t *)(buffer + yResolutionOffset);
    yResolution[0] = 72;
    yResolution[1] = 1;

    // Add custom metadata after EXIF tags
    sprintf((char *)(ptr + 8), "MediaID: %s\nDeploymentID: %s\nCaptureMethod: %s\n"
                               "Latitude: %.6f\nLongitude: %.6f\nTimestamp: %s\nFavourite: %d\n",
            metadata->mediaID, metadata->deploymentID, metadata->captureMethod,
            metadata->latitude, metadata->longitude, metadata->timestamp,
            metadata->favourite);

    ptr += 8 + strlen((char *)(ptr + 8));

    // Calculate and set the APP1 block length
    uint16_t app1Length = (uint16_t)(ptr - buffer - 2); // Exclude the initial 2 bytes for the marker
    lengthPtr[0] = (app1Length >> 8) & 0xFF;            // High byte
    lengthPtr[1] = app1Length & 0xFF;                   // Low byte

    return (ptr - buffer); // Total size of the block
}

// Helper function to find the SOS marker in a JPEG file
uint8_t *findSOSMarker(uint8_t *jpeg_data, uint32_t jpeg_sz)
{
    for (size_t i = 0; i < jpeg_sz - 1; i++)
    {
        if (jpeg_data[i] == 0xFF && jpeg_data[i + 1] == 0xDA)
        { // SOS marker: 0xFFDA
            return &jpeg_data[i];
        }
    }
    return NULL; // SOS marker not found
}