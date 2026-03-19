/**
  ******************************************************************************
  * @file    boot_cmd.c
  * @brief   UART1 command parser for bootloader (L, Mx, Hx, V, H)
  ******************************************************************************
  */
#include "boot_cmd.h"
#include "boot_usb.h"
#include "boot_elf.h"
#include "boot_flash.h"
#include "boot_hp.h"
#include "boot_version.h"
#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern UART_HandleTypeDef huart1;

/* ---- RX circular buffer ---- */
#define RX_QUEUE_SIZE  128
static volatile uint8_t  s_rxQueue[RX_QUEUE_SIZE];
static volatile uint8_t  s_rxHead = 0;
static volatile uint8_t  s_rxTail = 0;
uint8_t                  boot_cmd_rxByte;   /* HAL RX IT buffer (non-static for callback access) */

/* ---- Command line buffer ---- */
#define CMD_BUF_SIZE   32
static char    s_cmdBuf[CMD_BUF_SIZE];
static uint8_t s_cmdIdx = 0;

/* ---- File list (shared across commands) ---- */
static USB_FileList_t s_fileList;
static uint8_t        s_fileListValid = 0;

/* ---- Forward declarations ---- */
static void cmd_list(void);
static void cmd_main_update(uint8_t index);
static void cmd_hp_update(uint8_t index);
static void cmd_version(void);
static void cmd_help(void);
static void cmd_rs485_test(void);
static void flash_progress_cb(const char *phase, uint8_t pct);

/* ================================================================== */
void Boot_Print(const char *fmt, ...)
{
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (len > 0)
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, 100);
}

/* ================================================================== */
void Boot_Cmd_Init(void)
{
  HAL_UART_Receive_IT(&huart1, &boot_cmd_rxByte, 1);
}

/* ================================================================== */
void Boot_Cmd_RxCallback(uint8_t data)
{
  uint8_t next = (s_rxHead + 1) % RX_QUEUE_SIZE;
  if (next != s_rxTail) {
    s_rxQueue[s_rxHead] = data;
    s_rxHead = next;
  }
  HAL_UART_Receive_IT(&huart1, &boot_cmd_rxByte, 1);
}

/* ================================================================== */
void Boot_Cmd_Process(void)
{
  while (s_rxHead != s_rxTail) {
    uint8_t ch = s_rxQueue[s_rxTail];
    s_rxTail = (s_rxTail + 1) % RX_QUEUE_SIZE;

    if (ch == '\r' || ch == '\n') {
      if (s_cmdIdx == 0)
        continue;

      s_cmdBuf[s_cmdIdx] = '\0';
      Boot_Print("\r\n");

      /* ---- Parse command ---- */
      char c0 = s_cmdBuf[0];
      if (c0 == 'L' || c0 == 'l') {
        cmd_list();
      }
      else if ((c0 == 'M' || c0 == 'm') && s_cmdIdx >= 2) {
        uint8_t idx = (uint8_t)(s_cmdBuf[1] - '0');
        cmd_main_update(idx);
      }
      else if ((c0 == 'H' || c0 == 'h') && s_cmdIdx >= 2 && s_cmdBuf[1] != '\0'
               && s_cmdBuf[1] != 'E' && s_cmdBuf[1] != 'e') {
        /* Hx = handpiece update (but not "HELP" or "H" alone) */
        uint8_t idx = (uint8_t)(s_cmdBuf[1] - '0');
        cmd_hp_update(idx);
      }
      else if ((c0 == 'V' || c0 == 'v') && s_cmdIdx == 1) {
        cmd_version();
      }
      else if ((c0 == 'H' || c0 == 'h') && s_cmdIdx == 1) {
        cmd_help();
      }
      else if ((c0 == 'T' || c0 == 't') && s_cmdIdx == 1) {
        cmd_rs485_test();
      }
      else if ((c0 == 'B' || c0 == 'b') && s_cmdIdx == 1) {
        /* Boot both 407 app and H562 app */
        Boot_Print("Sending A to H562...\r\n");
        Boot_HP_BootApp();
        if (Boot_Flash_IsAppValid()) {
          Boot_Print("Booting 407 app...\r\n");
          HAL_Delay(100);
          Boot_Flash_JumpToApp();
        } else {
          Boot_Print("407 app: not valid.\r\n");
        }
      }
      else {
        Boot_Print("Unknown cmd. Type H for help.\r\n");
      }

      s_cmdIdx = 0;
      Boot_Print("> ");
    }
    else if (ch == '\b' || ch == 0x7F) {
      /* Backspace */
      if (s_cmdIdx > 0) {
        s_cmdIdx--;
        Boot_Print("\b \b");  /* Erase character on terminal */
      }
    }
    else {
      /* Echo printable characters */
      HAL_UART_Transmit(&huart1, &ch, 1, 10);
      if (s_cmdIdx < CMD_BUF_SIZE - 1)
        s_cmdBuf[s_cmdIdx++] = (char)ch;
    }
  }
}

/* ================================================================== */
/* L - List .elf files on USB                                         */
/* ================================================================== */
static void cmd_list(void)
{
  if (!Boot_USB_IsReady()) {
    Boot_Print("ERROR: USB not connected.\r\n");
    return;
  }

  int n = Boot_USB_ScanElfFiles(&s_fileList);
  if (n < 0) {
    Boot_Print("ERROR: Cannot read USB.\r\n");
    s_fileListValid = 0;
    return;
  }
  if (n == 0) {
    Boot_Print("No .elf files found.\r\n");
    s_fileListValid = 0;
    return;
  }

  s_fileListValid = 1;
  Boot_Print("--- ELF Files on USB ---\r\n");
  for (int i = 0; i < n; i++) {
    /* FAT date: bit15:9=Year(0=1980), bit8:5=Month, bit4:0=Day */
    uint16_t fd = s_fileList.files[i].fdate;
    uint16_t year  = ((fd >> 9) & 0x7F) + 1980;
    uint8_t  month = (fd >> 5) & 0x0F;
    uint8_t  day   = fd & 0x1F;
    Boot_Print("  [%d] %s: %04d-%02d-%02d\r\n",
               i + 1, s_fileList.files[i].name,
               year, month, day);
  }
  Boot_Print("Total: %d file(s)\r\n", n);
}

/* ================================================================== */
/* Mx - Flash file #x to main MCU (407)                               */
/* ================================================================== */
static void cmd_main_update(uint8_t index)
{
  if (!s_fileListValid) {
    Boot_Print("Run L first to scan files.\r\n");
    return;
  }
  if (index < 1 || index > s_fileList.count) {
    Boot_Print("Invalid file index.\r\n");
    return;
  }

  FIL fp;
  if (Boot_USB_OpenFile(&s_fileList, index, &fp) != 0) {
    Boot_Print("[407] Cannot open file.\r\n");
    return;
  }

  /* Parse ELF */
  Elf_Info_t elf;
  if (Elf_Parse(&fp, &elf) != 0) {
    Boot_Print("[407] Invalid ELF file.\r\n");
    f_close(&fp);
    return;
  }

  Boot_Print("[407] ELF: %d segment(s), %luKB\r\n",
             elf.seg_count, (unsigned long)(elf.total_flash_size / 1024));

  /* Validate address range */
  if (elf.flash_min < APP_FLASH_BASE || elf.flash_max > (APP_FLASH_END + 1)) {
    Boot_Print("[407] ERROR: Address out of app range!\r\n");
    f_close(&fp);
    return;
  }

  /* Record pre-update version */
  Boot_Version_RecordPre();

  /* Erase */
  Boot_Print("[407] Erasing app sectors...\r\n");
  if (Boot_Flash_EraseApp(flash_progress_cb) != BOOT_FLASH_OK) {
    Boot_Print("[407] ERASE FAILED!\r\n");
    f_close(&fp);
    return;
  }

  /* Program segment by segment */
  Boot_Print("[407] Programming...\r\n");
  uint8_t buf[256];
  uint32_t total_written = 0;

  for (uint8_t s = 0; s < elf.seg_count; s++) {
    uint32_t addr   = elf.seg[s].flash_addr;
    uint32_t remain = elf.seg[s].file_size;
    uint32_t foff   = elf.seg[s].file_offset;

    while (remain > 0) {
      uint32_t chunk = (remain > sizeof(buf)) ? sizeof(buf) : remain;
      UINT br;

      if (f_lseek(&fp, foff) != FR_OK || f_read(&fp, buf, chunk, &br) != FR_OK || br != chunk) {
        Boot_Print("[407] READ ERROR!\r\n");
        f_close(&fp);
        return;
      }

      if (Boot_Flash_Program(addr, buf, chunk) != BOOT_FLASH_OK) {
        Boot_Print("[407] PROGRAM ERROR at 0x%08lX!\r\n", (unsigned long)addr);
        f_close(&fp);
        return;
      }

      if (Boot_Flash_Verify(addr, buf, chunk) != BOOT_FLASH_OK) {
        Boot_Print("[407] VERIFY ERROR at 0x%08lX!\r\n", (unsigned long)addr);
        f_close(&fp);
        return;
      }

      addr   += chunk;
      foff   += chunk;
      remain -= chunk;
      total_written += chunk;

      uint8_t pct = (uint8_t)((total_written * 100UL) / elf.total_flash_size);
      Boot_Print("\r[407] Programming... %d%%", pct);
    }
  }

  f_close(&fp);

  Boot_Print("\r\n[407] Verify complete. %lu bytes written.\r\n",
             (unsigned long)total_written);

  /* Record post-update version */
  Boot_Version_RecordPost(s_fileList.files[index - 1].name);

  if (Boot_Flash_IsAppValid()) {
    Boot_Print("[407] Update OK! Press B to boot.\r\n");
  } else {
    Boot_Print("[407] WARNING: App validation failed.\r\n");
  }
}

/* ================================================================== */
/* Hx - Send file #x to handpiece via RS485 CNNX                     */
/* ================================================================== */
static void cmd_hp_update(uint8_t index)
{
  if (!s_fileListValid) {
    Boot_Print("Run L first to scan files.\r\n");
    return;
  }
  if (index < 1 || index > s_fileList.count) {
    Boot_Print("Invalid file index.\r\n");
    return;
  }

  FIL fp;
  if (Boot_USB_OpenFile(&s_fileList, index, &fp) != 0) {
    Boot_Print("[H562] Cannot open file.\r\n");
    return;
  }

  Elf_Info_t elf;
  if (Elf_Parse(&fp, &elf) != 0) {
    Boot_Print("[H562] Invalid ELF file.\r\n");
    f_close(&fp);
    return;
  }

  Boot_Print("[H562] ELF: %d segment(s), %luKB\r\n",
             elf.seg_count, (unsigned long)(elf.total_flash_size / 1024));

  int ret = Boot_HP_UpdateFirmware(&fp, &elf);

  f_close(&fp);
  Boot_USB_Unmount();

  if (ret == 0) {
    Boot_Print("[H562] Update OK!\r\n");
  } else {
    Boot_Print("[H562] Update FAILED (err=%d)\r\n", ret);
  }
}

/* ================================================================== */
/* V - Version display                                                */
/* ================================================================== */
static void cmd_version(void)
{
  Boot_Print("--- Version Info ---\r\n");
  Boot_Print("Bootloader : %s\r\n", Boot_Version_GetBoot());
  Boot_Print("407 App    : %s\r\n", Boot_Version_GetApp());

  /* Query handpiece version via RS485 */
  char hp_ver[16];
  if (Boot_HP_GetVersion(hp_ver, sizeof(hp_ver)) == 0) {
    Boot_Print("H562       : %s\r\n", hp_ver);
  } else {
    Boot_Print("H562       : (no response)\r\n");
  }
}

/* ================================================================== */
/* H - Help                                                           */
/* ================================================================== */
static void cmd_help(void)
{
  Boot_Print("\r\nL=List Mx=407update Hx=H562update\r\n");
  Boot_Print("V=Version T=RS485test B=Boot H=Help\r\n");
}

/* ================================================================== */
/* T - RS485 (UART6) raw communication test                            */
/* ================================================================== */
static void cmd_rs485_test(void)
{
  uint8_t rx_buf[32];
  uint16_t rx_cnt = 0;
  uint32_t start;

  /* Disable UART6 interrupts, use register-level access */
  USART6->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_TXEIE);

  /* Clear any pending errors/data */
  while (USART6->SR & USART_SR_RXNE)
    (void)USART6->DR;
  if (USART6->SR & (USART_SR_ORE | USART_SR_FE | USART_SR_NE))
    (void)USART6->DR;

  /* TX: send @U*55\n + padding via register */
  Boot_Print("TX @U*55 ...");
  const char cmd[] = "@U*55\n\n\n";
  for (int i = 0; cmd[i]; i++) {
    while (!(USART6->SR & USART_SR_TXE));
    USART6->DR = cmd[i];
  }
  while (!(USART6->SR & USART_SR_TC));  /* Wait TX complete */

  /* RX: poll for response (3 seconds) */
  start = HAL_GetTick();
  while ((HAL_GetTick() - start) < 3000 && rx_cnt < sizeof(rx_buf)) {
    uint32_t sr = USART6->SR;
    if (sr & USART_SR_RXNE) {
      rx_buf[rx_cnt++] = (uint8_t)(USART6->DR & 0xFF);
    } else if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
      (void)USART6->DR;
    }
  }

  if (rx_cnt > 0) {
    Boot_Print(" RX %d:", rx_cnt);
    for (uint16_t i = 0; i < rx_cnt; i++)
      Boot_Print(" %02X", rx_buf[i]);
  } else {
    Boot_Print(" NO RX");
  }
  Boot_Print("\r\n");

  /* UART6 now uses register polling, no restore needed */
}

/* ================================================================== */
static void flash_progress_cb(const char *phase, uint8_t pct)
{
  Boot_Print("\r[407] %s... %d%%", phase, pct);
  if (pct >= 100)
    Boot_Print("\r\n");
}
