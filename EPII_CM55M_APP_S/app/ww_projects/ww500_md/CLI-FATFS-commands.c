/*
 * CLI-FATFS-commands.c
 *
 * Implements CLI commands for the fatFs
 *
 *  Created on: 20 Aug 2024
 *      Author: CGP
 *
 * Only a few commands are implemented, but this can be expanded as and when required.
 * Some functions are optional and need to be enabled by editing ffconf.h.
 * See here for details of this: http://elm-chan.org/fsw/ff/doc/appnote.html
 *
 * See here for the inspiration for adding file system commands,
 * but note the API is for the FreeRTOS-Plus_FATFS which differs from the fatFS API we are using:
 * 		https://github.com/FreeRTOS/FreeRTOS/blob/520fc225eb2dd5e21c951ca325e1c51eed3a5c13/FreeRTOS-Plus/Demo/Common/FreeRTOS_Plus_CLI_Demos/File-Related-CLI-commands.c
 *
 * FatFs commands are documented here:
 * 		http://elm-chan.org/fsw/ff/
 *
 * IMPORTANT NOTE: At the time of writing the CLI commands implemented here don't use
 * the asynchronous method implemented in fatfs_task.c & .h
 *
 */

/*************************************** Includes *******************************************/

/* Modified by Maxim Integrated 26-Jun-2015 to quiet compiler warnings */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "FreeRTOS_CLI.h"
#include "CLI-commands.h"
#include "CLI-FATFS-commands.h"
#include "printf_x.h"

#include "ff.h"
#include "ffconf.h"		// This may need to be adjusted
#include "image_task.h"

#include "directory_manager.h"
#include "xip_manager.h"
#include "fatfs_task.h"
#include "crc16_ccitt.h"

/*************************************** Definitions *******************************************/

/**
 * A tiny state machine for the txfile command
 */
typedef enum {
    TXFILE_START,
	TXFILE_TRANSMITTING,
	TXFILE_FINISHED
} txfile_type_t;

/*************************************** External variables *******************************************/

// For binary responses this is set to a value between 0 and WW130_MAX_PAYLOAD_SIZE
// For string responses this is set to -1
extern int16_t binaryLength;

extern directoryManager_t dirManager;

/*************************************** Local variables *******************************************/

/*************************************** Local routine prototypes  *************************************/

static void vRegisterCLICommands( void );
static void strip_newline(char *str, uint16_t maxlen);

/*
 * Defines a command that returns a table showing the state of each task at the
 * time the command is called.
 */
static BaseType_t prvInfoCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );
static BaseType_t prvDirCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );
static BaseType_t prvPwdCommand( char * pcWriteBuffer, size_t xWriteBufferLen, const char * pcCommandString );
static BaseType_t prvChdirCommand( char * pcWriteBuffer, size_t xWriteBufferLen, const char * pcCommandString );
static BaseType_t prvMkdirCommand( char * pcWriteBuffer, size_t xWriteBufferLen, const char * pcCommandString );
static BaseType_t prvTypeCommand( char * pcWriteBuffer, size_t xWriteBufferLen, const char * pcCommandString );
static BaseType_t prvReadCommand( char * pcWriteBuffer, size_t xWriteBufferLen, const char * pcCommandString );
static BaseType_t prvTxFileCommand( char * pcWriteBuffer, size_t xWriteBufferLen, const char * pcCommandString );
static BaseType_t prvUnmountCommand( char * pcWriteBuffer, size_t xWriteBufferLen, const char * pcCommandString );
static BaseType_t prvDumpSelCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );
static BaseType_t prvFirmwareCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );
static BaseType_t prvCrcCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );
static FRESULT compute_file_crc( const char *path, uint16_t *crc_out, uint32_t *size_out );


/********************************** Structures that define CLI commands  *************************************/


// Structure that defines the ls command line command, which lists all the files in the current directory.
static const CLI_Command_Definition_t xInfo = {
    "info",         /* The command string to type. */
    "info:\r\n Print some information about the disk\r\n",
    prvInfoCommand, /* The function to run. */
    0              /* No parameters are expected. */
};

// Structure that defines the ls command line command, which lists all the files in the current directory.
static const CLI_Command_Definition_t xDir = {
    "dir",         /* The command string to type. */
    "dir:\r\n Lists the files in the current directory\r\n",
    prvDirCommand, /* The function to run. */
    0              /* No parameters are expected. */
};

// Structure that defines the 'pwd' command line command, which prints the current directory.
static const CLI_Command_Definition_t xPwd = {
    "pwd",         /* The command string to type. */
    "pwd:\r\n Displays current directory\r\n",
    prvPwdCommand, /* The function to run. */
    0              /* No parameters are expected. */
};

// Structure that defines the 'cd' command line command, which prints the current directory.
static const CLI_Command_Definition_t xChdir = {
    "cd",         /* The command string to type. */
    "cd <new_dir>:\r\n Change directory to <new_dir>\r\n",
    prvChdirCommand, /* The function to run. */
    1              /* 1 parameter is expected. */
};

// Structure that defines the 'mkdir' command line command, which prints the current directory.
static const CLI_Command_Definition_t xMkdir = {
    "mkdir",         /* The command string to type. */
    "mkdir <new_dir>:\r\n Make new directory <new_dir>\r\n",
    prvMkdirCommand, /* The function to run. */
    1              /* 1 parameter is expected. */
};

// Structure that defines the 'type' command line command, which prints the current directory.
static const CLI_Command_Definition_t xType = {
    "type",         /* The command string to type. */
    "type <file>:\r\n Prints (text) contents of <file>\r\n",
    prvTypeCommand, /* The function to run. */
    1              /* 1 parameter is expected. */
};

// Structure that defines the 'read' command line command, which ?
static const CLI_Command_Definition_t xRead = {
    "read",         /* The command string to type. */
    "read <file>:\r\n Prints (binary) contents of <file>\r\n",
    prvReadCommand, /* The function to run. */
    1              /* 1 parameter is expected. */
};

// Structure that defines the 'txfile' command line command, which ?
static const CLI_Command_Definition_t xTxFile = {
    "txfile",         /* The command string to type. */
    "txfile <file>:\r\n Prints (binary) contents of <file> (last picture if <file> is '.')\r\n",
    prvTxFileCommand, /* The function to run. */
    1              /* 1 parameter is expected. */
};

// Structure that defines the 'unmount' command line command, which prints the current directory.
static const CLI_Command_Definition_t xUnmount = {
    "unmount",         /* The command string to type. */
    "unmount:\r\n Unmount (save writes?)\r\n",
    prvUnmountCommand, /* The function to run. */
    0              /* No parameters are expected. */
};


// Structure that defines the dump-sel command, which prints the flash slot selector sector.
static const CLI_Command_Definition_t xDumpSel = {
    "dump-sel",        /* The command string to type. */
    "dump-sel:\r\n Print first 32 bytes of flash slot selector sector to console\r\n",
    prvDumpSelCommand, /* The function to run. */
    0              /* No parameters are expected. */
};

// Structure that defines the firmware command, which updates firmware from SD card.
static const CLI_Command_Definition_t xFirmware = {
    "firmware",
    "firmware <filename> [0xCRC]:\r\n"
    " Update firmware from /MANIFEST/<filename>.\r\n"
    " Optional 0xCRC: CRC16-CCITT of file must match before flash is touched.\r\n"
    " Type 'reset' to boot after update.\r\n",
    prvFirmwareCommand,
    -1              /* variable: 1 required, 1 optional */
};

// Structure that defines the crc command, which reports the CRC16-CCITT of a file.
static const CLI_Command_Definition_t xCrc = {
    "crc",
    "crc <filename>:\r\n Print CRC16-CCITT and size of <filename> in current directory\r\n",
    prvCrcCommand,
    1
};


/********************************** Private Function Definitions - for CLI commands ****************************/

// One of these commands for each activity invoked by the CLI

/**
 * Prints some status.
 *
 * Uses f_getfree - see http://elm-chan.org/fsw/ff/doc/getfree.html
 *
 */
static BaseType_t prvInfoCommand( char * pcWriteBuffer,
                                 size_t xWriteBufferLen,
                                 const char * pcCommandString ) {
    FRESULT res;
    FATFS *getFreeFs; 	  // Read information
    DWORD free_clusters;  // Free Clusters
    DWORD free_sectors;	  // Free Sectors
    DWORD total_sectors;  // Total Sectors
    TCHAR label[12]; 	  // Buffer for the volume label
    DWORD vsn; 			  // Volume serial number

	memset( pcWriteBuffer, 0x00, xWriteBufferLen );

	f_getlabel("", label, &vsn);
	cli_append(&pcWriteBuffer, &xWriteBufferLen,
			"Label: %s\nSerial No: 0x%08x\n", (char *) label, (int) vsn);

	// Get some statistics from the SD card
	res = f_getfree("", &free_clusters, &getFreeFs);

	if (res) {
    	cli_append(&pcWriteBuffer, &xWriteBufferLen, "f_getfree() failed (%u)\r\n", res);
		return pdFALSE;
	}

	// Formula comes from ChaN's documentation
	total_sectors = (getFreeFs->n_fatent - 2) * getFreeFs->csize;
	free_sectors = free_clusters * getFreeFs->csize;

	cli_append(&pcWriteBuffer, &xWriteBufferLen,
			"%10lu K total drive space.\r\n%10lu K available.\r\n"
			"Cluster size: %lu sectors (%lu bytes).",
			total_sectors / 2, free_sectors / 2,
			(unsigned long)getFreeFs->csize,
			(unsigned long)getFreeFs->csize * 512UL);

	/* There is no more data to return after this single string, so return pdFALSE. */
	return pdFALSE;
}

/**
 * Directory listing.
 *
 * This uses the CLI mechanism that allows you to return a partial result and expect the function to be called
 * again. That way I am printing one entry each time.
 */
static BaseType_t prvDirCommand( char * pcWriteBuffer,
                                 size_t xWriteBufferLen,
                                 const char * pcCommandString ) {
    FRESULT res;
    FILINFO fno;
    char cur_dir[CLI_FATFS_FILE_NAME_BUF_SIZE];
    UINT len = CLI_FATFS_FILE_NAME_BUF_SIZE;

    // These are static for processing of multiple entries,one at a time
    static DIR dir;					// directory structure
    static uint16_t nfile = 0;		// count files
    static uint16_t ndir = 0;		// count directories
    static bool listing = false;	// True after we have processed the first entry

    memset( pcWriteBuffer, 0x00, xWriteBufferLen );

    if (!listing) {
    	// This is stuff to do the first time we enter this function.
    	res = f_getcwd(cur_dir, len);      /* Get current directory */

    	if (res) {
    		cli_append(&pcWriteBuffer, &xWriteBufferLen, "f_getcwd res = %d", res);
    		return pdFALSE;
    	}
//    	else  {
//    		cli_append(&pcWriteBuffer, &xWriteBufferLen, "cur_dir = %s\r\n", cur_dir);
//    	}

    	res = f_opendir(&dir, cur_dir);    /* Open the directory */

    	if (res) {
        	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Failed to open '%s'. (%u)", cur_dir, res);
        	listing = false;
        	nfile = 0;
        	ndir = 0;
        	return pdFALSE;
    	}
    	else {
        	listing = true;
    	}
    }

    // Here when the directory  has been opened successfully.
    // This might be the first attempt to read for the directory, or not.
    res = f_readdir(&dir, &fno);                   /* Read a directory item */

    if (res != FR_OK || fno.fname[0] == 0) {
    	// Error or end of dir
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "%d dirs, %d files.", ndir, nfile);

    	f_closedir(&dir);
    	listing = false;
    	nfile = 0;
    	ndir = 0;
    	return pdFALSE;
    }

    // On 24/3/25 I added the seconds to the file time
    cli_append(&pcWriteBuffer, &xWriteBufferLen,
    		"%c%c%c%c%c %u-%02u-%02u, %02u:%02u:%02u %10d %s",
    					((fno.fattrib & AM_DIR) ? 'D' : '-'),
    					((fno.fattrib & AM_RDO) ? 'R' : '-'),
    					((fno.fattrib & AM_SYS) ? 'S' : '-'),
    					((fno.fattrib & AM_HID) ? 'H' : '-'),
    					((fno.fattrib & AM_ARC) ? 'A' : '-'),
    					((fno.fdate >> 9) + 1980), (fno.fdate >> 5 & 15),
    					(fno.fdate & 31), (fno.ftime >> 11), (fno.ftime >> 5 & 63), (fno.ftime & 0x1f) << 1,
    					(int) fno.fsize, fno.fname);
    if (fno.fattrib & AM_DIR) {
    	// Directory
    	//cli_append(&pcWriteBuffer, &xWriteBufferLen, "   <DIR>   %s", fno.fname);
    	ndir++;
    }
    else {
    	// File
    	//cli_append(&pcWriteBuffer, &xWriteBufferLen, "%10u %s", (int) fno.fsize, fno.fname);
    	nfile++;
    }
    // Assume there is more to come
	return pdTRUE;
}

/**
 * Print current directory
 *
 * See
 */

static BaseType_t prvPwdCommand( char * pcWriteBuffer,
                                 size_t xWriteBufferLen,
                                 const char * pcCommandString ) {
    FRESULT res;
    char cur_dir[CLI_FATFS_FILE_NAME_BUF_SIZE];
    UINT len = CLI_FATFS_FILE_NAME_BUF_SIZE;

	memset( pcWriteBuffer, 0x00, xWriteBufferLen );

    res = f_getcwd(cur_dir, len);      /* Get current directory */

	if (res)  {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "f_getcwd res = %d", res);
	}
	else  {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "%s", cur_dir);
	}
	/* There is no more data to return after this single string, so return pdFALSE. */
	return pdFALSE;
}

/**
 * Change directory
 * See http://elm-chan.org/fsw/ff/doc/chdir.html
 *
 */
static BaseType_t prvChdirCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString ) {
    FRESULT res;
	const char *pcParameter;
	BaseType_t lParameterStringLength;
    char cur_dir[CLI_FATFS_FILE_NAME_BUF_SIZE];
    UINT len = CLI_FATFS_FILE_NAME_BUF_SIZE;

	/* Get parameter */
	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
	if (pcParameter != NULL) {
		//Maybe some checking here?
		res = f_chdir(pcParameter);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Must supply directory name.");
		return pdFALSE;
	}

    if (res == FR_OK) {
    	// Print the new directory
    	res = f_getcwd(cur_dir, len);      /* Get current directory */
    	if (res)  {
    		cli_append(&pcWriteBuffer, &xWriteBufferLen, "f_getcwd res = %d", res);
    	}
    	else  {
    		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Now %s", cur_dir);
    	}
    }
    else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "f_chdir %s failed: %d", pcParameter, res);
    }

	return pdFALSE;
}

/**
 * Make directory
 * See http://elm-chan.org/fsw/ff/doc/mkdir.html
 *
 */
static BaseType_t prvMkdirCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString ) {
    FRESULT res;
	const char *pcParameter;
	BaseType_t lParameterStringLength;

	/* Get parameter */
	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
	if (pcParameter != NULL) {
		//Maybe some checking here?
		res = f_mkdir(pcParameter);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Must supply directory name.");
		return pdFALSE;
	}

    if (res == FR_OK) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Created %s", pcParameter);
    }
    else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "f_mkdir %s failed: %d", pcParameter,res);
    }

	return pdFALSE;
}


/**
 * Type file
 *
 * Assumes it is a text file. Prints one line at a time, using the CLI facility to call this function multiple
 * times until it returns false (complete).
 *
 * It uses f_gets() to fetch the lines and this reads either until it finds '\n' or until the buffer is full.
 * The buffer size is set to 244 which is the size used for BLE comms, so this command delivers
 * the file to the BLE app in chunks of 243 bytes (a trailing \0 is added so the buffer appears as a string.
 *
 * At the moment I am stripping out any \r or \n from the file. It might be better to deliver these across BLE as well.
 *
 * See http://elm-chan.org/fsw/ff/doc/gets.html
 */
static BaseType_t prvTypeCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString ) {
	FRESULT res;
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	char line[CLI_OUTPUT_BUF_SIZE]; /* Line buffer */

	static bool listing  = false;
	static FIL fil;

	if (!listing) {
		// This is stuff to do the first time we enter this function.
		pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
		if (pcParameter != NULL) {
			//Maybe some checking here?
			res = f_open(&fil, pcParameter, FA_READ);
		}
		else {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Must supply file name.");
			return pdFALSE;
		}

		if (res) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Failed to open '%s'. (%u)", pcParameter, res);
			return pdFALSE;
		}
	}

	// Here if the file is open
	listing = true;

	if (f_gets(line, CLI_OUTPUT_BUF_SIZE, &fil)) {
		// f_gets() returns when it finds \n or when it has CLI_OUTPUT_BUF_SIZE -1 characters (appends '\0')
		// strip trailing \n or \r since these will be added when the pcWriteBuffer is printed.
		strip_newline(line, CLI_OUTPUT_BUF_SIZE);
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "%s", line);

		if (f_eof(&fil)) {
			// No data left
			f_close(&fil);
			listing = false;
			return pdFALSE;
		}
		else {
			// There is more to read
			return pdTRUE;
		}
	}
	else {
		// No data left
		f_close(&fil);
		listing = false;
		return pdFALSE;
	}
}

/**
 * Read file
 *
 * Assumes it is a binary file. It delivers all of the bytes in chunks of 241 bytes,
 * using the CLI facility to call this function multiple times until it returns false (complete).
 *
 * See http://elm-chan.org/fsw/ff/doc/read.html
 *
 */
static BaseType_t prvReadCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString ) {
	FRESULT res;
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	char line[CLI_OUTPUT_BUF_SIZE]; /* Line buffer */

	static bool listing  = false;
	static FIL fil;
	UINT br;			// Bytes read

	if (!listing) {
		// This is stuff to do the first time we enter this function.
		pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
		if (pcParameter != NULL) {
			//Maybe some checking here?
			res = f_open(&fil, pcParameter, FA_READ);
		}
		else {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Must supply file name.");
			return pdFALSE;
		}

		if (res) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Failed to open '%s'. (%u)", pcParameter, res);
			return pdFALSE;
		}
	}

	// Here if the file is open
	listing = true;

	// Read 244 - 3 bytes (since we will pre-pend 3 bytes)
	res = f_read(&fil, line, (CLI_OUTPUT_BUF_SIZE - 3), &br);

	if (res == FR_OK) {
		memcpy(pcWriteBuffer, line, br);
		binaryLength = br;	// Changed here from -1 to the actual data length, to be accessed in processCommand()
		if (br == (CLI_OUTPUT_BUF_SIZE - 3)) {
			// We have read 241 bytes and there are probably more to come.
			return pdTRUE;
		}
		else {
			// No data left
			f_close(&fil);
			listing = false;
			return pdFALSE;
		}
	}
	else {
		// error
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error reading file. (%u)", res);
		f_close(&fil);
		listing = false;
		return pdFALSE;
	}
}

/**
 * Read file
 *
 * Assumes it is a binary file. It delivers all of the bytes in chunks of 241 bytes,
 * using the CLI facility to call this function multiple times until it returns false (complete).
 *
 * A tiny state machine causes different actions depending on these states:
 * 	TXFILE_START - open the file
	TXFILE_TRANSMITTING,
	TXFILE_FINISHED
 *
 * See http://elm-chan.org/fsw/ff/doc/read.html
 *
 */
static BaseType_t prvTxFileCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString ) {
	FRESULT res;
	const char *pcParameter;
	char fileName[FF_MAX_LFN];	// should we use this? I think it is 255
	BaseType_t lParameterStringLength;
	char line[CLI_OUTPUT_BUF_SIZE]; /* Line buffer */

	static txfile_type_t state = TXFILE_START;
	static uint8_t packetNum = 0;
	static bool asciiReplacement = false;

	static UINT brTotal = 0; // Accumulate bytes
	static FIL fil;
	UINT br;			// Bytes read

	switch (state) {

	case TXFILE_START:

		// This is stuff to do the first time we enter this function.
		pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
		asciiReplacement = false;
		packetNum = 0;

		if (pcParameter[0] == '.') {
			// special case - use the "latest" picture file
			// For now just hard code this
			snprintf(fileName, IMAGEFILENAMELEN, "%s", image_getLastImageFile());
		}
		else if (pcParameter[0] == '-') {
			// special case - replace file contents with ASCII
			snprintf(fileName, FF_MAX_LFN, "%s", &pcParameter[1]);
			asciiReplacement = true;
		}
		else {
			snprintf(fileName, FF_MAX_LFN, "%s", pcParameter);
		}

		// Must change directory to the dirManager->current_capture_dir
		res = f_chdir(dirManager.current_capture_dir);
		if (res != FR_OK) {
			snprintf(pcWriteBuffer, xWriteBufferLen, "Failed to change directory: error %d", res);
			return pdFALSE;
		}

		//Maybe some checking here?
		res = f_open(&fil, fileName, FA_READ);
		if (res == FR_OK) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "%d bytes in %s", (int) f_size(&fil), fileName);
			state = TXFILE_TRANSMITTING;
			return pdTRUE;
		}
		else {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Failed to open '%s'. (%u)", fileName, res);
			return pdFALSE;
		}
		break;

	case TXFILE_TRANSMITTING:
		// Here on the second and subsequent calls, until the file is all read.

		// Read 244 - 3 bytes (since we will pre-pend 3 bytes)
		res = f_read(&fil, line, (CLI_OUTPUT_BUF_SIZE - 3), &br);

		if (res == FR_OK) {
			if (asciiReplacement) {
				// send ASCII binary pattern instead
				// always in the range 0-9
				memset(pcWriteBuffer, ((packetNum % 10) + '0'), br);
			}
			else {
				memcpy(pcWriteBuffer, line, br);
			}
			packetNum++;

			binaryLength = br;	// Changed here from -1 to the actual data length, to be accessed in processCommand()
			brTotal += br;
			if (br == (CLI_OUTPUT_BUF_SIZE - 3)) {
				// We have read 241 bytes and there are probably more to come.
				return pdTRUE;
			}
			else {
				// No data left
				f_close(&fil);
				state = TXFILE_FINISHED;
				return pdTRUE;
			}
		}
		else {
			// error
			binaryLength = NOTBINARY;	// Indicate the message is text, not binary
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error reading file. (%u)", res);
			f_close(&fil);
			state = TXFILE_START;
			brTotal = 0;
			return pdFALSE;
		}
		break;

	case TXFILE_FINISHED:
		// Here when all the file has been transmitted.
		// Send a text message to move the BLE processor out of binary mode
		binaryLength = NOTBINARY;	// Indicate the message is text, not binary

		cli_append(&pcWriteBuffer, &xWriteBufferLen,
				"Finished sending %u bytes (%d packets)", brTotal, packetNum);
		state = TXFILE_START;
		brTotal = 0;

		packetNum = 0;
		asciiReplacement = false;

		return pdFALSE;
		break;

	default:
		// should not happen
		state = TXFILE_START;
		brTotal = 0;
		packetNum = 0;
		asciiReplacement = false;
		return pdFALSE;
		break;
	} // switch
}

/**
 * Unmount
 *
 * See http://elm-chan.org/fsw/ff/doc/mount.html
 */

static BaseType_t prvUnmountCommand( char * pcWriteBuffer,
                                 size_t xWriteBufferLen,
                                 const char * pcCommandString ) {
    FRESULT res;

    res = f_unmount("");

	if (res)  {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "f_unmount failed. res = %d", res);
	}
	else  {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Unmounted OK.NOW WHAT?");
	}
	/* There is no more data to return after this single string, so return pdFALSE. */
	return pdFALSE;
}


/**
 * Print the first 32 bytes of the flash slot selector sector to the console.
 *
 * Calls xip_dump_slot_selector() in xip_manager.c.  Output goes via xprintf /
 * printf_x_printBuffer rather than the CLI write buffer, so the CLI response
 * is just a short status line.
 */
static BaseType_t prvDumpSelCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString ) {
    (void)pcCommandString;

    memset(pcWriteBuffer, 0x00, xWriteBufferLen);

    int result = xip_dump_slot_selector();
    if (result != 0) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen,
                   "dump-sel failed (error %d)", result);
    }

    return pdFALSE;
}

/**
 * Update firmware from a file in /MANIFEST on the SD card.
 *
 * Calls xip_update_firmware_from_sd() which handles the full sequence:
 * erase inactive slot, write image, verify, update slot selector.
 * Detailed progress is printed to the console via xprintf.
 *
 * Optional second parameter: CRC16-CCITT of the file in 0xNNNN format.
 * If supplied, the file CRC is checked before any flash operation begins.
 */
static BaseType_t prvFirmwareCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString ) {
    const char *pcParameter;
    BaseType_t  lParameterStringLength;
    char        filename[64];
    int         result;

    memset(pcWriteBuffer, 0x00, xWriteBufferLen);

    /* Parameter 1: filename */
    pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
    if (pcParameter == NULL) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Usage: firmware <filename> [0xCRC]");
        return pdFALSE;
    }

    if (lParameterStringLength >= (BaseType_t)sizeof(filename)) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error: filename too long (max 63 chars)");
        return pdFALSE;
    }

    memcpy(filename, pcParameter, lParameterStringLength);
    filename[lParameterStringLength] = '\0';

    /* Parameter 2: optional CRC — "0x1234" is always exactly 6 characters */
    pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 2, &lParameterStringLength);
    if (pcParameter != NULL) {
        if (lParameterStringLength != 6) {
            cli_append(&pcWriteBuffer, &xWriteBufferLen,
                       "Error: CRC must be in format 0xNNNN");
            return pdFALSE;
        }

        char crc_str[7];
        memcpy(crc_str, pcParameter, 6);
        crc_str[6] = '\0';

        char *end;
        unsigned long parsed = strtoul(crc_str, &end, 0);
        if (end == crc_str || *end != '\0') {
            cli_append(&pcWriteBuffer, &xWriteBufferLen,
                       "Error: invalid CRC '%s' (expected 0xNNNN)", crc_str);
            return pdFALSE;
        }
        uint16_t expected_crc = (uint16_t)(parsed & 0xFFFFu);

        char filepath[sizeof(CONFIG_DIR) + sizeof(filename) + 1];
        snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, filename);

        uint16_t actual_crc;
        uint32_t file_size;
        FRESULT res = compute_file_crc(filepath, &actual_crc, &file_size);
        if (res != FR_OK) {
            cli_append(&pcWriteBuffer, &xWriteBufferLen,
                       "Error: cannot read '%s' for CRC check (%d)", filepath, res);
            return pdFALSE;
        }

        if (actual_crc != expected_crc) {
            cli_append(&pcWriteBuffer, &xWriteBufferLen,
                       "Error: CRC mismatch - file 0x%04X, expected 0x%04X. Flash NOT modified.",
                       actual_crc, expected_crc);
            return pdFALSE;
        }
        else {
        	cli_append(&pcWriteBuffer, &xWriteBufferLen,
        	                   "Firmware CRC 0x%04X matched. ", expected_crc);
        }
    }

    result = xip_update_firmware_from_sd(filename);
    if (result == 0) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen,
                   "Firmware update OK. Type 'reset' to boot the new image.");
    } else {
        cli_append(&pcWriteBuffer, &xWriteBufferLen,
                   "Firmware update FAILED (error %d). Existing firmware unchanged.", result);
    }

    return pdFALSE;
}

/**
 * Report the CRC16-CCITT and byte count of a file in the current directory.
 */
static BaseType_t prvCrcCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString ) {
    const char *pcParameter;
    BaseType_t  lParameterStringLength;
    char        filename[64];
    uint16_t    crc;
    uint32_t    size;
    FRESULT     res;

    memset(pcWriteBuffer, 0x00, xWriteBufferLen);

    pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
    if (pcParameter == NULL) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Usage: crc <filename>");
        return pdFALSE;
    }

    if (lParameterStringLength >= (BaseType_t)sizeof(filename)) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error: filename too long");
        return pdFALSE;
    }

    memcpy(filename, pcParameter, lParameterStringLength);
    filename[lParameterStringLength] = '\0';

    res = compute_file_crc(filename, &crc, &size);
    if (res != FR_OK) {
        cli_append(&pcWriteBuffer, &xWriteBufferLen,
                   "Error: cannot open '%s' (%d)", filename, (int)res);
        return pdFALSE;
    }

    cli_append(&pcWriteBuffer, &xWriteBufferLen,
               "CRC 0x%04X (%lu bytes)", (unsigned)crc, (unsigned long)size);
    return pdFALSE;
}

/**
 * Compute CRC16-CCITT of the file at 'path' using the streaming API.
 * Reads in 512-byte chunks to bound stack usage.
 * Returns FR_OK on success; writes crc and size. Returns a FatFS error code on failure.
 */
static FRESULT compute_file_crc( const char *path, uint16_t *crc_out, uint32_t *size_out ) {
    FIL      file;
    FRESULT  res;
    uint8_t  buf[512];
    UINT     bytes_read;
    uint32_t total = 0;
    uint16_t crc;

    res = f_open(&file, path, FA_READ);
    if (res != FR_OK) {
        return res;
    }

    crc = crc16_ccitt_stream_init();

    for (;;) {
        res = f_read(&file, buf, sizeof(buf), &bytes_read);
        if (res != FR_OK) {
            f_close(&file);
            return res;
        }
        if (bytes_read == 0) {
            break;
        }
        crc    = crc16_ccitt_stream_update(buf, (uint16_t)bytes_read, crc);
        total += bytes_read;
    }

    f_close(&file);
    *crc_out  = crc16_ccitt_stream_final(crc);
    *size_out = total;
    return FR_OK;
}

/********************************** Private Function Definitions - Other **************************/

/**
 * Register CLI commands
 */
static void vRegisterCLICommands( void ) {
	FreeRTOS_CLIRegisterCommand( &xInfo );
	FreeRTOS_CLIRegisterCommand( &xDir );
	FreeRTOS_CLIRegisterCommand( &xPwd );
	FreeRTOS_CLIRegisterCommand( &xChdir );
	FreeRTOS_CLIRegisterCommand( &xMkdir );
	FreeRTOS_CLIRegisterCommand( &xType );
	FreeRTOS_CLIRegisterCommand( &xRead );
	FreeRTOS_CLIRegisterCommand( &xTxFile );
	FreeRTOS_CLIRegisterCommand( &xUnmount );
	FreeRTOS_CLIRegisterCommand( &xDumpSel );
	FreeRTOS_CLIRegisterCommand( &xFirmware );
	FreeRTOS_CLIRegisterCommand( &xCrc );
}

/**
 * Remove \r\n from strings when using type command, since they will be added back later
 */
static void strip_newline(char *str, uint16_t maxlen) {
    size_t len = strnlen(str, maxlen);
    if (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[len - 1] = '\0';
        len--;
    }
    if (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[len - 1] = '\0';
    }
}

/********************************** Public Function Definitions  *************************************/

void cli_fatfs_init(void) {

	/* Register available CLI commands */
	vRegisterCLICommands();
}
