/**
  ******************************************************************************
  * @file    boot_usb.h
  * @brief   USB memory stick file operations for bootloader
  ******************************************************************************
  */
#ifndef BOOT_USB_H
#define BOOT_USB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ff.h"

/* Max number of .elf files to track */
#define USB_MAX_FILES  20

/* File info entry */
typedef struct {
  char     name[13];       /* 8.3 filename (null-terminated) */
  uint32_t size;           /* file size in bytes */
  uint16_t fdate;          /* FAT date: version number (YYYY-MM-DD) */
} USB_FileEntry_t;

/* File list */
typedef struct {
  uint8_t         count;
  USB_FileEntry_t files[USB_MAX_FILES];
} USB_FileList_t;

/**
  * @brief  Check if USB stick is connected and ready
  * @retval 1 = ready, 0 = not ready
  */
uint8_t Boot_USB_IsReady(void);

/**
  * @brief  Mount the USB filesystem
  * @retval 0 on success, -1 on error
  */
int Boot_USB_Mount(void);

/**
  * @brief  Unmount the USB filesystem
  */
void Boot_USB_Unmount(void);

/**
  * @brief  Scan root directory for .elf files
  * @param  list [out] populated file list
  * @retval number of files found, -1 on error
  */
int Boot_USB_ScanElfFiles(USB_FileList_t *list);

/**
  * @brief  Open an .elf file by index (1-based)
  * @param  list    file list from Boot_USB_ScanElfFiles
  * @param  index   1-based file index
  * @param  fp      [out] FATFS file handle
  * @retval 0 on success, -1 on error
  */
int Boot_USB_OpenFile(const USB_FileList_t *list, uint8_t index, FIL *fp);

#ifdef __cplusplus
}
#endif
#endif /* BOOT_USB_H */
