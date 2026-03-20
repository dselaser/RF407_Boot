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

/* ---- Progress tracking ---- */
static uint8_t s_erase_dots;  /* dots printed for erase progress */

/* ---- Forward declarations ---- */
static void cmd_list(void);
static void cmd_update(uint8_t index);
static void cmd_auto(void);
static void cmd_version(void);
static void cmd_help(void);
static void cmd_rs485_test(void);
static void flash_progress_cb(const char *phase, uint8_t pct);

/* ================================================================== */
void Boot_Print(const char *fmt, ...)
{
  char buf[96];
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
      else if ((c0 == 'U' || c0 == 'u') && s_cmdIdx >= 2) {
        uint8_t idx = (uint8_t)(s_cmdBuf[1] - '0');
        cmd_update(idx);
      }
      else if ((c0 == 'A' || c0 == 'a') && s_cmdIdx == 1) {
        cmd_auto();
      }
      else if ((c0 == 'V' || c0 == 'v') && s_cmdIdx == 1) {
        cmd_version();
      }
      else if ((c0 == 'H' || c0 == 'h') && s_cmdIdx == 1) {
        cmd_help();
      }
      else if ((c0 == 'R' || c0 == 'r') && s_cmdIdx == 1) {
        /* Run both 407 app and H562 app */
        Boot_Print("HP run\r\n");
        Boot_HP_BootApp();
        if (Boot_Flash_IsAppValid()) {
          Boot_Print("Main run\r\n");
          HAL_Delay(100);
          Boot_Flash_JumpToApp();
        } else {
          Boot_Print("M:bad\r\n");
        }
      }
      else if ((c0 == 'T' || c0 == 't') && s_cmdIdx == 1) {
        cmd_rs485_test();
      }
      else {
        Boot_Print("? Type H\r\n");
      }

      s_cmdIdx = 0;
      Boot_Print("\r\n> ");
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
    Boot_Print("NoUSB\r\n");
    return;
  }

  int n = Boot_USB_ScanElfFiles(&s_fileList);
  if (n < 0) {
    Boot_Print("USB err.\r\n");
    s_fileListValid = 0;
    return;
  }
  if (n == 0) {
    Boot_Print("No ELF.\r\n");
    s_fileListValid = 0;
    return;
  }

  s_fileListValid = 1;
  for (int i = 0; i < n; i++) {
    Boot_Print(" %d:%s\r\n", i + 1, s_fileList.files[i].name);
  }
  Boot_Print("%d files\r\n", n);
}

/* ================================================================== */
/* Ux - Unified update: auto-detect Main Board vs Handpiece by ELF address */
/* ================================================================== */
static void cmd_update(uint8_t index)
{
  if (!s_fileListValid) {
    Boot_Print("L first\r\n");
    return;
  }
  if (index < 1 || index > s_fileList.count) {
    Boot_Print("Bad#\r\n");
    return;
  }

  FIL fp;
  if (Boot_USB_OpenFile(&s_fileList, index, &fp) != 0) {
    Boot_Print("Open err.\r\n");
    return;
  }

  Elf_Info_t elf;
  if (Elf_Parse(&fp, &elf) != 0) {
    Boot_Print("Bad ELF.\r\n");
    f_close(&fp);
    return;
  }

  /* Auto-detect target by flash base address */
  if (elf.flash_min >= APP_FLASH_BASE) {
    /* ---- Main Board (407) ---- */
    Boot_Print("[Main] %d seg, %luKB\r\n",
               elf.seg_count, (unsigned long)(elf.total_flash_size / 1024));

    if (elf.flash_max > (APP_FLASH_END + 1)) {
      Boot_Print("[Main]Addr!\r\n");
      f_close(&fp);
      return;
    }

    Boot_Print("[Main]Erase ");
    s_erase_dots = 0;
    if (Boot_Flash_EraseApp(flash_progress_cb) != BOOT_FLASH_OK) {
      Boot_Print(" Fail\r\n");
      f_close(&fp);
      return;
    }
    Boot_Print(" OK\r\n");

    Boot_Print("[Main]Prog ");
    uint8_t buf[256];
    uint32_t total_written = 0;
    uint8_t last_dot = 0;  /* dots printed so far (0-10) */

    for (uint8_t s = 0; s < elf.seg_count; s++) {
      uint32_t addr   = elf.seg[s].flash_addr;
      uint32_t remain = elf.seg[s].file_size;
      uint32_t foff   = elf.seg[s].file_offset;

      while (remain > 0) {
        uint32_t chunk = (remain > sizeof(buf)) ? sizeof(buf) : remain;
        UINT br;

        if (f_lseek(&fp, foff) != FR_OK || f_read(&fp, buf, chunk, &br) != FR_OK || br != chunk) {
          Boot_Print(" RdE\r\n");
          f_close(&fp);
          return;
        }

        if (Boot_Flash_Program(addr, buf, chunk) != BOOT_FLASH_OK) {
          Boot_Print(" WrE\r\n");
          f_close(&fp);
          return;
        }

        if (Boot_Flash_Verify(addr, buf, chunk) != BOOT_FLASH_OK) {
          Boot_Print(" VfE\r\n");
          f_close(&fp);
          return;
        }

        addr   += chunk;
        foff   += chunk;
        remain -= chunk;
        total_written += chunk;

        uint8_t dots = (uint8_t)((total_written * 10UL) / elf.total_flash_size);
        while (last_dot < dots) {
          Boot_Print(".");
          last_dot++;
        }
      }
    }

    f_close(&fp);
    Boot_Print(" OK\r\n");
    Boot_Version_RecordPost(s_fileList.files[index - 1].name,
                            s_fileList.files[index - 1].fdate);

    if (Boot_Flash_IsAppValid())
      Boot_Print("[Main]Done\r\n");
    else
      Boot_Print("[Main]VFail!\r\n");

  } else if (elf.flash_min >= HP_APP_BASE) {
    /* ---- Handpiece (H562) via RS485 ---- */
    Boot_Print("[HP] %d seg, %luKB\r\n",
               elf.seg_count, (unsigned long)(elf.total_flash_size / 1024));

    int ret = Boot_HP_UpdateFirmware(&fp, &elf);
    f_close(&fp);

    if (ret == 0) {
      Boot_Print("[HP]OK\r\n");
      Boot_Version_RecordHP(s_fileList.files[index - 1].fdate);
    } else
      Boot_Print("[HP]F%d\r\n", ret);

  } else {
    Boot_Print("? target\r\n");
    f_close(&fp);
  }
}

/* ================================================================== */
/* A - Auto: scan USB, update ALL ELF files, then boot                */
/*     No questions asked — update every file found, then run.        */
/* ================================================================== */
static void cmd_auto(void)
{
  if (!Boot_USB_IsReady()) {
    Boot_Print("NoUSB\r\n");
    return;
  }

  int n = Boot_USB_ScanElfFiles(&s_fileList);
  if (n <= 0) {
    Boot_Print("No ELF.\r\n");
    s_fileListValid = 0;
    return;
  }
  s_fileListValid = 1;

  /* --- Phase 1: Classify all ELF files --- */
  int8_t main_idx = -1;   /* file index for 407 (1-based) */
  int8_t hp_idx   = -1;   /* file index for H562 (1-based) */

  for (int i = 0; i < n; i++) {
    FIL fp;
    if (Boot_USB_OpenFile(&s_fileList, (uint8_t)(i + 1), &fp) != 0)
      continue;

    Elf_Info_t elf;
    if (Elf_Parse(&fp, &elf) != 0) { f_close(&fp); continue; }
    f_close(&fp);

    Boot_Print("%d:%s @%08lX", i + 1, s_fileList.files[i].name,
               (unsigned long)elf.flash_min);
    if (elf.flash_min >= APP_FLASH_BASE) {
      main_idx = (int8_t)(i + 1);
      Boot_Print(" ->Main\r\n");
    } else if (elf.flash_min >= HP_APP_BASE) {
      hp_idx = (int8_t)(i + 1);
      Boot_Print(" ->HP\r\n");
    } else {
      Boot_Print(" ->skip\r\n");
    }
  }

  if (main_idx < 0 && hp_idx < 0) {
    Boot_Print("No target.\r\n");
    return;
  }

  /* --- Phase 2: Update H562 first (if present) --- */
  /*     H562 first because 407 update ends with reset */
  if (hp_idx > 0) {
    Boot_Print("\r\n==HP==\r\n");
    cmd_update((uint8_t)hp_idx);
  }

  /* --- Phase 3: Update 407 (if present) --- */
  if (main_idx > 0) {
    Boot_Print("\r\n==Main==\r\n");
    cmd_update((uint8_t)main_idx);
  }

  /* --- Phase 4: Boot both --- */
  Boot_Print("\r\n==Boot==\r\n");

  if (hp_idx > 0) {
    Boot_Print("HP run\r\n");
    Boot_HP_BootApp();
    HAL_Delay(200);
  }

  if (Boot_Flash_IsAppValid()) {
    Boot_Print("Main run\r\n");
    HAL_Delay(100);
    Boot_Flash_JumpToApp();
  } else if (main_idx > 0) {
    Boot_Print("M:bad\r\n");
  }
}

/* ================================================================== */
/* V - Version display                                                */
/* ================================================================== */
static void cmd_version(void)
{
  Boot_Print("Main:%s\r\n", Boot_Version_GetApp());
  Boot_Print("HP  :%s\r\n", Boot_Version_GetHP());
}

/* ================================================================== */
/* H - Help                                                           */
/* ================================================================== */
static void cmd_help(void)
{
  Boot_Print(
    "V  Version\r\n"
    "L  List USB\r\n"
    "Ux Update #x\r\n"
    "A  Auto update\r\n");
  Boot_Print(
    "T  RS485 test\r\n"
    "R  Run application\r\n"
    "H  Help\r\n");
}


/* ================================================================== */
/* T - RS485 test                                                     */
/* ================================================================== */
static void cmd_rs485_test(void)
{
  uint8_t rx_buf[32];
  uint16_t rx_cnt = 0;

  USART6->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_TXEIE);
  while (USART6->SR & USART_SR_RXNE) (void)USART6->DR;
  if (USART6->SR & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) (void)USART6->DR;

  Boot_Print("TX @T ...");
  const char cmd[] = "@T*54\n";
  for (int i = 0; cmd[i]; i++) {
    while (!(USART6->SR & USART_SR_TXE));
    USART6->DR = cmd[i];
  }
  while (!(USART6->SR & USART_SR_TC));

  uint32_t start = HAL_GetTick();
  while ((HAL_GetTick() - start) < 1500 && rx_cnt < sizeof(rx_buf)) {
    uint32_t sr = USART6->SR;
    if (sr & USART_SR_RXNE)
      rx_buf[rx_cnt++] = (uint8_t)(USART6->DR & 0xFF);
    else if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE))
      (void)USART6->DR;
  }

  if (rx_cnt > 0) {
    Boot_Print(" RX %d:", rx_cnt);
    for (uint16_t i = 0; i < rx_cnt; i++)
      Boot_Print(" %02X", rx_buf[i]);
  } else {
    Boot_Print(" NO RX");
  }
  Boot_Print("\r\n");
}

/* ================================================================== */
static void flash_progress_cb(const char *phase, uint8_t pct)
{
  (void)phase;
  uint8_t dots = pct / 10;
  while (s_erase_dots < dots) {
    Boot_Print(".");
    s_erase_dots++;
  }
}
