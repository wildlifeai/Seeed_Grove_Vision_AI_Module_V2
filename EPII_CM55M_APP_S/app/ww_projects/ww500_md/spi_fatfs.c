// file: spi_fatfs.c


/*************************************** Includes *******************************************/

#include "xprintf.h"
#include "printf_x.h"	// For colour
#include "ff.h"
#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"

/*************************************** Definitions *******************************************/

#define DRV         ""

/*
 * Bug with cache management? See here:
 * https://chatgpt.com/share/687583ae-c920-8005-ba60-0c2e75fe797b
*/
#define CACHEFIX

/*************************************** Local Function Declarations *****************************/

FRESULT list_dir (const char *path);
FRESULT scan_files (char* path);

/*************************************** External variables *******************************************/


/*************************************** Local variables *******************************************/

// Not usedstatic FATFS fs;             /* Filesystem object */

/********************************** Private Function definitions  *************************************/

/* List contents of a directory */
FRESULT list_dir (const char *path)
{
    FRESULT res;
    DIR dir;
    FILINFO fno;
    int nfile, ndir;


    res = f_opendir(&dir, path);                       /* Open the directory */
    if (res == FR_OK) {
        nfile = ndir = 0;
        for (;;) {
            res = f_readdir(&dir, &fno);                   /* Read a directory item */
            if (res != FR_OK || fno.fname[0] == 0) break;  /* Error or end of dir */
            if (fno.fattrib & AM_DIR) {            /* Directory */
                printf("   <DIR>   %s\r\n", fno.fname);
                ndir++;
            } else {                               /* File */
                printf("%10u %s\r\n", (int) fno.fsize, fno.fname);
                nfile++;
            }
        }
        f_closedir(&dir);
        printf("%d dirs, %d files.\r\n", ndir, nfile);
    } else {
        printf("Failed to open \"%s\". (%u)\r\n", path, res);
    }
    return res;
}


/* Recursive scan of all items in the directory */
FRESULT scan_files (char* path)     /* Start node to be scanned (***also used as work area***) */
{
    FRESULT res;
    DIR dir;
    UINT i;
    static FILINFO fno;


    res = f_opendir(&dir, path);                       /* Open the directory */
    if (res == FR_OK) {
        for (;;) {
            res = f_readdir(&dir, &fno);                   /* Read a directory item */
            if (res != FR_OK || fno.fname[0] == 0) break;  /* Break on error or end of dir */
            if (fno.fattrib & AM_DIR) {                    /* It is a directory */
                i = strlen(path);
                sprintf(&path[i], "/%s", fno.fname);
                res = scan_files(path);                    /* Enter the directory */
                if (res != FR_OK) break;
                path[i] = 0;
            } else {                                       /* It is a file. */
                printf("%s/%s\r\n", path, fno.fname);
            }
        }
        f_closedir(&dir);
    }

    return res;
}

/********************************** Global Function definitions - fatfs port  ***************************/

/**
 * Control of SD card chip select...
 *
 * Himax notes:
 * app implement GPIO_Output_Level/GPIO_Pinmux/GPIO_Dir for fatfs\port\mmc_spi\mmc_we2_spi.c ARM_SPI_SS_MASTER_SW
 *
 * CGP - could it be that the SSPI_CS_GPIO_Pinmux() sets PB5 to be either a GPIO pin (and "GPIO16" for that matter)
 * then the other two functions set the pin high or low, and input or output.
 *
 * This is used by mmc_we2_spi.c in the fatfs code.
 */
void SSPI_CS_GPIO_Pinmux(bool setGpioFn) {
    if (setGpioFn) {
        hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_GPIO16, 0);
    }
    else {
        hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_SPI_M_CS_1, 0);
    }
}

void SSPI_CS_GPIO_Output_Level(bool setLevelHigh) {
    hx_drv_gpio_set_out_value(GPIO16, (GPIO_OUT_LEVEL_E) setLevelHigh);
}

void SSPI_CS_GPIO_Dir(bool setDirOut) {
    if (setDirOut) {
        hx_drv_gpio_set_output(GPIO16, GPIO_OUT_HIGH);
    }
    else {
        hx_drv_gpio_set_input(GPIO16);
    }
}

/********************************** Global Function definitions  *************************************/

/**
 * Redundant. Replaced by code in fatfs_task.c, fatFsInit()
 *
 * See notes there for explanation.
 */

//int fatfs_init(bool printDiskInfo) {
//    // CGP many unused
//	//FIL fil_w, fil_r;   /* File object */
//    FRESULT res;        /* API result code */
//    //UINT bw;            /* Bytes written */
//    //UINT br;            /* Bytes read */
//    //BYTE buffer[128];
//    //DIR dir;
//    FILINFO fno;
//    char file_dir[20];
//    UINT file_dir_idx = 0;
//    //char filename[20];
//    //char filecontent[256];
//    char cur_dir[128];
//    UINT len = 128;
//
//    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_M_DO_1, 1);
//    hx_drv_scu_set_PB3_pinmux(SCU_PB3_PINMUX_SPI_M_DI_1, 1);
//    hx_drv_scu_set_PB4_pinmux(SCU_PB4_PINMUX_SPI_M_SCLK_1, 1);
//    hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_SPI_M_CS_1, 1);
//
//    XP_CYAN;
//    printf("Mount SD card fatfs\r\n");
//    XP_WHITE;
//
//    // This is probably blocking...
//    res = f_mount(&fs, DRV, 1);
//
//    if (res) {
//    	XP_RED;
//        printf("f_mount fail, res = %d\r\n", res);
//        XP_WHITE;
//        return res;	// exit with the error code
//
//        // CGP removed this, as otherwise this task halts.
//        //while(1);
//    }
//
//    // This scans the disk and prints all directories and files
//    // Let's add a switch so we only do it once
//    if (printDiskInfo) {
//    	res = f_getcwd(cur_dir, len);      /* Get current directory */
//    	if (res)  {
//    		XP_RED;
//    		printf("f_getcwd res = %d\r\n", res);
//    		XP_WHITE;
//    		return res;	// exit with the error code
//    	}
//    	else  {
//    		printf("cur_dir = %s\r\n", cur_dir);
//    	}
//
//    	res = list_dir(cur_dir);
//    	if (res)  {
//    		XP_RED;
//    		printf("list_dir res = %d\r\n", res);
//    		XP_WHITE;
//    		return res;	// exit with the error code
//    	}
//
//    	res = scan_files(cur_dir);
//    	if (res)  {
//    		XP_RED;
//    		printf("scan_files res = %d\r\n", res);
//    		XP_WHITE;
//    		return res;	// exit with the error code
//    	}
//    }
//    else {
//    	printf("Initialising fatfs - searching for a directory:\r\n");
//    }
//
//    while ( 1 )  {
//    	xsprintf(file_dir, "%s%04d", CAPTURE_DIR, file_dir_idx);
//    	res = f_stat(file_dir, &fno);
//    	if (res == FR_OK)  {
//    		// Don't print this as we get a large number of directories quickly...
//    		//printf("Directory '%s' exists.\r\n", file_dir);
//    		file_dir_idx++;
//    	}
//    	else {
//    		printf("Create directory '%s'\r\n", file_dir);
//    		res = f_mkdir(file_dir);
//            if (res) { printf("f_mkdir res = %d\r\n", res); }
//
//            //printf("Change directory '%s'\r\n", file_dir);
//            res = f_chdir(file_dir);
//
//            res = f_getcwd(cur_dir, len);      /* Get current directory */
//            //printf("cur_dir = %s\r\n", cur_dir);
//            break;
//        }
//    }
//
//    return 0;
//}



int fastfs_write_image(uint32_t SRAM_addr, uint32_t img_size, uint8_t *filename) {
    FIL fil_w;          /* File object */
    FRESULT res;        /* API result code */
    UINT bw;            /* Bytes written */

    // tp added this to write over existing files with the same name for development phase
	res = f_open(&fil_w, (TCHAR*) filename,  FA_WRITE | FA_CREATE_ALWAYS);
    // res = f_open(&fil_w, (TCHAR*) filename, FA_CREATE_NEW | FA_WRITE);
    if (res == FR_OK)  {

#ifdef CACHEFIX
        // This ensures that any data in the D-cache is committed to RAM
        SCB_CleanDCache_by_Addr ((void *)SRAM_addr, img_size);
#if 0
    	// Check by printing some of jpeg buffer
        uint16_t bytesToPrint = 16;
        uint8_t * buffer = (uint8_t * ) SRAM_addr;
        XP_LT_GREY;

    	xprintf("Used 'SCB_CleanDCache' - writing %d bytes beginning:\n", img_size);
    	for (int i = 0; i < bytesToPrint; i++) {
    	    xprintf("%02X ", buffer[i]);
    	    if (i%16 == 15) {
    	    	xprintf("\n");
    	    }
    	}
    	xprintf("\n");
        XP_WHITE;
#endif // 1 (print buffer)
#else
    	// This discards any data in the data cache for the specified memory range
        SCB_InvalidateDCache_by_Addr ((void *)SRAM_addr, img_size);
#if 1
    	// Check by printing some of jpeg buffer
        uint16_t bytesToPrint = 16;
        uint8_t * buffer = (uint8_t * ) SRAM_addr;
        XP_LT_GREY;

    	xprintf("Used 'SCB_InvalidateDCache' - writing %d bytes beginning:\n", img_size);
    	for (int i = 0; i < bytesToPrint; i++) {
    	    xprintf("%02X ", buffer[i]);
    	    if (i%16 == 15) {
    	    	xprintf("\n");
    	    }
    	}
    	xprintf("\n");
        XP_WHITE;
#endif // 1 (print buffer)
#endif // CACHEFIX

        //printf("write file : %s.\r\n", filename);
        res = f_write(&fil_w, (void *)SRAM_addr, img_size, &bw);
        if (res) { printf("f_write res = %d\r\n", res); }
        f_close(&fil_w);
    }
    else
    {
        printf("f_open res = %d\r\n", res);
    }
    return 0;
}
