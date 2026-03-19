/**
  ******************************************************************************
  * @file    boot_version.c
  * @brief   Version management - records update history
  *
  *          Version info is stored in the last 256 bytes of Sector 3
  *          (0x0800FF00 - 0x0800FFFF), within bootloader flash area.
  *          This area is written once and not erased during app updates.
  *
  *          Layout at 0x0800FF00:
  *            [0x00] Magic: 0x56455253 ("VERS")
  *            [0x04] Pre-update version: 12 bytes (null-terminated string)
  *            [0x10] Post-update version: 12 bytes
  *            [0x1C] Update filename: 16 bytes
  ******************************************************************************
  */
#include "boot_version.h"
#include "boot_cmd.h"
#include "stm32f4xx_hal.h"
#include <string.h>
#include <stdio.h>

/* Version storage address in bootloader flash (end of sector 3) */
#define VER_STORE_ADDR    0x0800FF00U
#define VER_MAGIC         0x56455253U  /* "VERS" */

/* Storage structure */
typedef struct {
  uint32_t magic;
  char     pre_version[12];
  char     post_version[12];
  char     filename[16];
} VerStore_t;

/* Month name lookup for __DATE__ parsing */
static const char *s_months[] = {
  "Jan","Feb","Mar","Apr","May","Jun",
  "Jul","Aug","Sep","Oct","Nov","Dec"
};

/* Static buffers */
static char s_bootVer[VERSION_STR_LEN];
static char s_appVer[VERSION_STR_LEN];

/* ------------------------------------------------------------------ */
/* Convert __DATE__ ("Mar 18 2026") to "2026-03-18"                   */
/* ------------------------------------------------------------------ */
static void date_to_version(const char *date_str, char *out)
{
  /* __DATE__ format: "Mmm DD YYYY" */
  int month = 0;
  for (int i = 0; i < 12; i++) {
    if (date_str[0] == s_months[i][0] &&
        date_str[1] == s_months[i][1] &&
        date_str[2] == s_months[i][2]) {
      month = i + 1;
      break;
    }
  }

  int day = 0;
  if (date_str[4] == ' ')
    day = date_str[5] - '0';
  else
    day = (date_str[4] - '0') * 10 + (date_str[5] - '0');

  /* Year is at offset 7 */
  snprintf(out, VERSION_STR_LEN, "%.4s-%02d-%02d", &date_str[7], month, day);
}

/* ------------------------------------------------------------------ */
const char* Boot_Version_GetBoot(void)
{
  if (s_bootVer[0] == '\0')
    date_to_version(__DATE__, s_bootVer);
  return s_bootVer;
}

/* ------------------------------------------------------------------ */
const char* Boot_Version_GetApp(void)
{
  const VerStore_t *store = (const VerStore_t *)VER_STORE_ADDR;

  if (store->magic == VER_MAGIC && store->post_version[0] != '\0' &&
      store->post_version[0] != (char)0xFF) {
    strncpy(s_appVer, store->post_version, VERSION_STR_LEN - 1);
    s_appVer[VERSION_STR_LEN - 1] = '\0';
    return s_appVer;
  }
  return "unknown";
}

/* ------------------------------------------------------------------ */
void Boot_Version_RecordPre(void)
{
  /* Read current version before update */
  const char *cur = Boot_Version_GetApp();
  Boot_Print("[VER] Pre-update: %s\r\n", cur);
}

/* ------------------------------------------------------------------ */
void Boot_Version_RecordPost(const char *filename)
{
  /* Try to extract version from filename (e.g., "RF_NE~1.ELF" or similar)
     For 8.3 filenames, just record the filename itself.
     The actual version is derived from the build date of the new firmware,
     which we can read from the ELF. For simplicity, we use current date. */

  VerStore_t newStore;
  newStore.magic = VER_MAGIC;

  /* Copy pre-update version */
  const VerStore_t *oldStore = (const VerStore_t *)VER_STORE_ADDR;
  if (oldStore->magic == VER_MAGIC) {
    strncpy(newStore.pre_version, oldStore->post_version, 11);
  } else {
    strncpy(newStore.pre_version, "none", 11);
  }
  newStore.pre_version[11] = '\0';

  /* Post-update version = current date (build date of bootloader as proxy) */
  date_to_version(__DATE__, newStore.post_version);

  /* Record filename */
  strncpy(newStore.filename, filename ? filename : "unknown", 15);
  newStore.filename[15] = '\0';

  /* Write to flash (word by word) - this area is in bootloader sector 3 */
  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                         FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                         FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

  /* Note: We can only write to erased (0xFF) flash.
     If this area was already written, we skip to avoid errors.
     In production, this area should be erased during bootloader update. */
  const uint32_t *check = (const uint32_t *)VER_STORE_ADDR;
  uint8_t area_erased = 1;
  for (int i = 0; i < (int)(sizeof(VerStore_t) / 4); i++) {
    if (check[i] != 0xFFFFFFFF) {
      area_erased = 0;
      break;
    }
  }

  if (area_erased) {
    const uint32_t *src = (const uint32_t *)&newStore;
    for (uint32_t i = 0; i < sizeof(VerStore_t); i += 4) {
      HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, VER_STORE_ADDR + i, src[i / 4]);
    }
  }

  HAL_FLASH_Lock();

  Boot_Print("[VER] Post-update: %s (file: %s)\r\n",
             newStore.post_version, newStore.filename);
}
