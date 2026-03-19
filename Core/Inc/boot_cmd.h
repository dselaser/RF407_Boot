/**
  ******************************************************************************
  * @file    boot_cmd.h
  * @brief   UART1 command parser for bootloader (L, Mx, Hx, V, H)
  ******************************************************************************
  */
#ifndef BOOT_CMD_H
#define BOOT_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  Initialize command parser (enable UART1 RX interrupt)
  */
void Boot_Cmd_Init(void);

/**
  * @brief  Process pending UART1 commands (call from main loop)
  *         Non-blocking: returns immediately if no complete command
  */
void Boot_Cmd_Process(void);

/**
  * @brief  Print formatted string to UART1
  */
void Boot_Print(const char *fmt, ...);

/**
  * @brief  UART1 RX interrupt callback (call from HAL_UART_RxCpltCallback)
  * @param  data  received byte
  */
void Boot_Cmd_RxCallback(uint8_t data);

#ifdef __cplusplus
}
#endif
#endif /* BOOT_CMD_H */
