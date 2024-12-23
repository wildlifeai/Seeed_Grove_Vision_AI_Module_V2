#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#define MAX_METADATA_SIZE 512

#ifndef METADATA_H
#define METADATA_H

// // Device-level metadata
// typedef struct
// {
//     char mediaID[20];
//     char cameraID[20];
//     double latitude;
//     double longitude;
//     char timestamp[20];
//     bool favourite;
// } DeviceMetadata;

// // Deployment-level metadata
// typedef struct
// {
//     char deploymentID[20];
//     char locationID[20];
//     char deploymentStart[20];
//     char deploymentEnd[20];
//     char setupBy[50];
// } DeploymentMetadata;

// Define a structure to hold your custom metadata fields

typedef struct
{
    char mediaID[20];
    char deploymentID[20];
    char captureMethod[50];
    double latitude;
    double longitude;
    char timestamp[20];
    bool favourite;
    // Add more fields as needed
} ImageMetadata;

int createAPP1Block(ImageMetadata *metadata, unsigned char *buffer, int bufferSize);
uint8_t *findSOSMarker(uint8_t *buffer, uint32_t length);

#endif // METADATA_H