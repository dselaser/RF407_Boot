/**
  ******************************************************************************
  * @file    boot_usb.c
  * @brief   USB memory stick file operations for bootloader
  ******************************************************************************
  */
#include "boot_usb.h"
#include "fatfs.h"
#include "usb_host.h"
#include <string.h>

extern ApplicationTypeDef Appli_state;
extern char  USBHPath[4];
extern FATFS USBHFatFS;

static uint8_t s_mounted = 0;

/* ------------------------------------------------------------------ */
uint8_t Boot_USB_IsReady(void)
{
  return (Appli_state == APPLICATION_READY) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
int Boot_USB_Mount(void)
{
  if (s_mounted)
    return 0;

  FRESULT res = f_mount(&USBHFatFS, (TCHAR const *)USBHPath, 1);
  if (res != FR_OK)
    return -1;

  s_mounted = 1;
  return 0;
}

/* ------------------------------------------------------------------ */
void Boot_USB_Unmount(void)
{
  if (s_mounted) {
    f_mount(NULL, (TCHAR const *)USBHPath, 0);
    s_mounted = 0;
  }
}

/* ------------------------------------------------------------------ */
static int str_endswith_elf(const char *name)
{
  size_t len = strlen(name);
  if (len < 5) return 0;
  const char *ext = &name[len - 4];
  return (ext[0] == '.' &&
          (ext[1] == 'E' || ext[1] == 'e') &&
          (ext[2] == 'L' || ext[2] == 'l') &&
          (ext[3] == 'F' || ext[3] == 'f'));
}

/* ------------------------------------------------------------------ */
int Boot_USB_ScanElfFiles(USB_FileList_t *list)
{
  memset(list, 0, sizeof(*list));

  if (Boot_USB_Mount() != 0)
    return -1;

  DIR dir;
  FILINFO fno;

  if (f_opendir(&dir, "/") != FR_OK) {
    Boot_USB_Unmount();
    return -1;
  }

  while (list->count < USB_MAX_FILES) {
    if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
      break;

    /* Skip directories */
    if (fno.fattrib & AM_DIR)
      continue;

    /* Filter .elf files only */
    if (!str_endswith_elf(fno.fname))
      continue;

    strncpy(list->files[list->count].name, fno.fname, 12);
    list->files[list->count].name[12] = '\0';
    list->files[list->count].size = (uint32_t)fno.fsize;
    list->files[list->count].fdate = fno.fdate;
    list->count++;
  }

  f_closedir(&dir);
  /* Keep mounted for file operations */
  return (int)list->count;
}

/* ------------------------------------------------------------------ */
int Boot_USB_OpenFile(const USB_FileList_t *list, uint8_t index, FIL *fp)
{
  if (index < 1 || index > list->count)
    return -1;

  if (Boot_USB_Mount() != 0)
    return -1;

  const char *name = list->files[index - 1].name;
  if (f_open(fp, name, FA_READ) != FR_OK)
    return -1;

  return 0;
}
