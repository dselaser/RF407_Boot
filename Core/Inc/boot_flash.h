/**
  ******************************************************************************
  * @file    boot_flash.h
  * @brief   STM32F407 internal flash programming for bootloader
  ******************************************************************************
  */
#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---------- Flash Memory Map ---------- */
#define BOOT_FLASH_BASE         0x08000000U
#define BOOT_SIZE               (64U * 1024U)      /* 64KB bootloader */
#define APP_FLASH_BASE          0x08010000U         /* Sector 4 start */
#define APP_FLASH_END           0x0807FFFFU         /* End of 512KB */
#define APP_MAX_SIZE            (448U * 1024U)      /* 448KB */

/* Sector layout for STM32F407VET6 (512KB) */
#define APP_FIRST_SECTOR        4                   /* 64KB sector */
#define APP_LAST_SECTOR          7                   /* 128KB sector */

/* ---------- Return codes ---------- */
typedef enum {
  BOOT_FLASH_OK = 0,
  BOOT_FLASH_ERR_ERASE,
  BOOT_FLASH_ERR_PROGRAM,
  BOOT_FLASH_ERR_VERIFY,
  BOOT_FLASH_ERR_ADDR,
} Boot_Flash_Status_t;

/* ---------- Progress callback ---------- */
/* Called during erase/program with percent 0-100 */
typedef void (*Boot_Flash_ProgressCb)(const char *phase, uint8_t percent);

/**
  * @brief  Erase application flash area (Sectors 4-7)
  * @param  cb  progress callback (may be NULL)
  * @retval BOOT_FLASH_OK on success
  */
Boot_Flash_Status_t Boot_Flash_EraseApp(Boot_Flash_ProgressCb cb);

/**
  * @brief  Program flash with data buffer
  * @param  addr  destination address (must be in app area, word-aligned)
  * @param  data  source data
  * @param  len   data length in bytes (will be rounded up to word boundary)
  * @retval BOOT_FLASH_OK on success
  */
Boot_Flash_Status_t Boot_Flash_Program(uint32_t addr, const uint8_t *data, uint32_t len);

/**
  * @brief  Verify flash content against data buffer
  * @param  addr  flash address
  * @param  data  expected data
  * @param  len   data length
  * @retval BOOT_FLASH_OK if match, BOOT_FLASH_ERR_VERIFY if mismatch
  */
Boot_Flash_Status_t Boot_Flash_Verify(uint32_t addr, const uint8_t *data, uint32_t len);

/**
  * @brief  Check if valid application exists at APP_FLASH_BASE
  *         Validates SP (must be in RAM) and PC (must be in flash)
  * @retval 1 = valid, 0 = invalid
  */
uint8_t Boot_Flash_IsAppValid(void);

/**
  * @brief  Jump to application at APP_FLASH_BASE
  *         Uses system reset for clean hardware state
  * @note   This function does not return
  */
void Boot_Flash_JumpToApp(void);

/**
  * @brief  Check boot magic and jump to app if set
  *         Must be called at very beginning of main(), before HAL_Init()
  * @retval 0 = no boot request (continue bootloader), 1 = jumped (never returns)
  */
uint8_t Boot_Flash_CheckAndJump(void);

#ifdef __cplusplus
}
#endif
#endif /* BOOT_FLASH_H */
