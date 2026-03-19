/**
  ******************************************************************************
  * @file    boot_flash.c
  * @brief   STM32F407 internal flash programming for bootloader
  ******************************************************************************
  */
#include "boot_flash.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* Sector sizes for STM32F407VET6 (512KB) */
static const struct {
  uint8_t  sector_id;
  uint32_t addr;
  uint32_t size;
} s_app_sectors[] = {
  { FLASH_SECTOR_4, 0x08010000,  64U * 1024U },  /* 64KB  */
  { FLASH_SECTOR_5, 0x08020000, 128U * 1024U },  /* 128KB */
  { FLASH_SECTOR_6, 0x08040000, 128U * 1024U },  /* 128KB */
  { FLASH_SECTOR_7, 0x08060000, 128U * 1024U },  /* 128KB */
};
#define NUM_APP_SECTORS  (sizeof(s_app_sectors) / sizeof(s_app_sectors[0]))

/* ------------------------------------------------------------------ */
Boot_Flash_Status_t Boot_Flash_EraseApp(Boot_Flash_ProgressCb cb)
{
  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                         FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                         FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

  FLASH_EraseInitTypeDef erase;
  uint32_t sectorError = 0;

  for (uint32_t i = 0; i < NUM_APP_SECTORS; i++) {
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Banks        = FLASH_BANK_1;
    erase.Sector       = s_app_sectors[i].sector_id;
    erase.NbSectors    = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;  /* 2.7~3.6V, word program */

    if (HAL_FLASHEx_Erase(&erase, &sectorError) != HAL_OK) {
      HAL_FLASH_Lock();
      return BOOT_FLASH_ERR_ERASE;
    }

    if (cb) {
      uint8_t pct = (uint8_t)(((i + 1) * 100U) / NUM_APP_SECTORS);
      cb("Erase", pct);
    }
  }

  HAL_FLASH_Lock();
  return BOOT_FLASH_OK;
}

/* ------------------------------------------------------------------ */
Boot_Flash_Status_t Boot_Flash_Program(uint32_t addr, const uint8_t *data, uint32_t len)
{
  /* Validate address range */
  if (addr < APP_FLASH_BASE || (addr + len) > (APP_FLASH_END + 1))
    return BOOT_FLASH_ERR_ADDR;

  HAL_FLASH_Unlock();

  /* Program word by word (32-bit) */
  uint32_t i = 0;
  while (i < len) {
    uint32_t word = 0xFFFFFFFF;
    uint32_t remaining = len - i;
    uint32_t copyLen = (remaining >= 4) ? 4 : remaining;
    memcpy(&word, &data[i], copyLen);

    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word) != HAL_OK) {
      HAL_FLASH_Lock();
      return BOOT_FLASH_ERR_PROGRAM;
    }
    i += 4;
  }

  HAL_FLASH_Lock();
  return BOOT_FLASH_OK;
}

/* ------------------------------------------------------------------ */
Boot_Flash_Status_t Boot_Flash_Verify(uint32_t addr, const uint8_t *data, uint32_t len)
{
  const uint8_t *flash = (const uint8_t *)addr;

  for (uint32_t i = 0; i < len; i++) {
    if (flash[i] != data[i])
      return BOOT_FLASH_ERR_VERIFY;
  }
  return BOOT_FLASH_OK;
}

/* ------------------------------------------------------------------ */
uint8_t Boot_Flash_IsAppValid(void)
{
  uint32_t sp = *(volatile uint32_t *)(APP_FLASH_BASE);       /* Initial SP */
  uint32_t pc = *(volatile uint32_t *)(APP_FLASH_BASE + 4);   /* Reset_Handler */

  /* SP must point to RAM */
  if ((sp < 0x20000000) || (sp > 0x20020000))
    return 0;

  /* PC must point to flash (app area) */
  if ((pc < APP_FLASH_BASE) || (pc > APP_FLASH_END))
    return 0;

  /* SP must be word-aligned */
  if (sp & 0x3)
    return 0;

  return 1;
}

/* ---- Boot-to-app via system reset using RTC backup register ---- */
#define BOOT_MAGIC_VALUE  0xB00710ADU

static void bkp_enable(void)
{
  /* Enable PWR clock and backup domain access (register level, no HAL needed) */
  RCC->APB1ENR |= RCC_APB1ENR_PWREN;
  PWR->CR |= PWR_CR_DBP;
}

/* ------------------------------------------------------------------ */
void Boot_Flash_RequestBoot(void)
{
  bkp_enable();
  RTC->BKP0R = BOOT_MAGIC_VALUE;
  NVIC_SystemReset();
}

/* ------------------------------------------------------------------ */
uint8_t Boot_Flash_CheckAndJump(void)
{
  /* Called at very beginning of main(), before HAL_Init() */
  bkp_enable();

  if (RTC->BKP0R != BOOT_MAGIC_VALUE)
    return 0;

  /* Clear magic so next reset goes to bootloader normally */
  RTC->BKP0R = 0;

  if (!Boot_Flash_IsAppValid())
    return 0;

  /* ---- Direct jump (hardware is clean after system reset) ---- */
  typedef void (*pFunction)(void);

  uint32_t appStack = *(volatile uint32_t *)(APP_FLASH_BASE);
  uint32_t appEntry = *(volatile uint32_t *)(APP_FLASH_BASE + 4);

  /* Set vector table to app */
  SCB->VTOR = APP_FLASH_BASE;

  /* Set MSP */
  __set_MSP(appStack);

  /* Jump to app's Reset_Handler */
  pFunction jump = (pFunction)appEntry;
  jump();

  while (1) {}
  return 1;
}

/* ------------------------------------------------------------------ */
void Boot_Flash_JumpToApp(void)
{
  Boot_Flash_RequestBoot();
}
