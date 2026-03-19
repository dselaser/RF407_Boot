/**
  ******************************************************************************
  * @file    boot_version.h
  * @brief   Version management - records update history
  ******************************************************************************
  */
#ifndef BOOT_VERSION_H
#define BOOT_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Version string format: "2026-02-15" (10 chars + null) */
#define VERSION_STR_LEN  11

/**
  * @brief  Get bootloader build version (compile-time date)
  * @retval pointer to static version string "YYYY-MM-DD"
  */
const char* Boot_Version_GetBoot(void);

/**
  * @brief  Get last recorded application version
  * @retval pointer to static version string, or "unknown"
  */
const char* Boot_Version_GetApp(void);

/**
  * @brief  Record current app version before update (pre-update)
  */
void Boot_Version_RecordPre(void);

/**
  * @brief  Record new app version after update (post-update)
  * @param  filename  name of the .elf file used for update
  */
void Boot_Version_RecordPost(const char *filename);

#ifdef __cplusplus
}
#endif
#endif /* BOOT_VERSION_H */
