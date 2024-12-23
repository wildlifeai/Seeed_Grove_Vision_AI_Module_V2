#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "metadata.h"

#define MAX_METADATA_SIZE 8192 // 512

#define APP1_MARKER 0xE1

// Helper function to create an APP1 block with metadata
int createAPP1Block(ImageMetadata *metadata, unsigned char *buffer, int bufferSize)
{
    int offset = 0;

    // Ensure buffer is large enough for the minimum structure
    if (bufferSize < 10)
    {
        return -1; // Buffer too small
    }

    // APP1 Marker and Placeholder for Length (filled later)
    buffer[offset++] = 0xFF;
    buffer[offset++] = APP1_MARKER; // APP1 Marker
    buffer[offset++] = 0x00; // Placeholder for Length (high byte)
    buffer[offset++] = 0x00; // Placeholder for Length (low byte)

    // EXIF Header
    memcpy(buffer + offset, "Exif\0\0", 6); // EXIF identifier
    offset += 6;

    // TIFF Header
    // Endianness: Big Endian
    buffer[offset++] = 0x4D; // 'M'
    buffer[offset++] = 0x4D; // 'M'
    buffer[offset++] = 0x00; // Fixed value
    buffer[offset++] = 0x2A; // Fixed value
    buffer[offset++] = 0x00; // Offset to IFD (placeholder)
    buffer[offset++] = 0x08;

    // Custom Metadata (Example as ASCII tag fields)
    int metadataSize = snprintf((char *)(buffer + offset), bufferSize - offset,
                                "mediaID=%s\n"
                                "deploymentID=%s\n"
                                "captureMethod=%s\n"
                                "latitude=%f\n"
                                "longitude=%f\n"
                                "timestamp=%s\n"
                                "favourite=%d\n",
                                metadata->mediaID,
                                metadata->deploymentID,
                                metadata->captureMethod,
                                metadata->latitude,
                                metadata->longitude,
                                metadata->timestamp,
                                metadata->favourite);

    if (metadataSize <= 0 || metadataSize + offset > bufferSize)
    {
        return -1; // Metadata too large for buffer
    }

    offset += metadataSize;

    // Set the total APP1 block length (excluding the marker itself)
    int app1Length = offset - 2; // Subtract `FF E1` marker (2 bytes)
    buffer[2] = (app1Length >> 8) & 0xFF; // High byte
    buffer[3] = app1Length & 0xFF;        // Low byte

    return offset; // Return the size of the block
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