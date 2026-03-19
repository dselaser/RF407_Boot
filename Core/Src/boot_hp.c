/**
  ******************************************************************************
  * @file    boot_hp.c
  * @brief   CNNX protocol - Master side for handpiece (H562) firmware update
  *          Uses USART6 register-level polling (no HAL interrupt RX)
  ******************************************************************************
  */
#include "boot_hp.h"
#include "boot_cmd.h"
#include "main.h"
#include <string.h>

/* ---- Packet ID counter ---- */
static uint16_t s_packetId = 0;

/* ================================================================== */
/* Low-level UART6 register access                                     */
/* ================================================================== */

static void uart6_send(const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i++) {
    while (!(USART6->SR & USART_SR_TXE));
    USART6->DR = data[i];
  }
  while (!(USART6->SR & USART_SR_TC));
}

static int uart6_recv_byte(uint8_t *byte, uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  while ((HAL_GetTick() - start) < timeout_ms) {
    uint32_t sr = USART6->SR;
    /* Check RX first (before error clear which also reads DR) */
    if (sr & USART_SR_RXNE) {
      *byte = (uint8_t)(USART6->DR & 0xFF);
      return 1;
    }
    /* Clear errors only when no data */
    if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE))
      (void)USART6->DR;
  }
  return 0;  /* Timeout */
}

static void uart6_flush_rx(void)
{
  while (USART6->SR & USART_SR_RXNE)
    (void)USART6->DR;
  if (USART6->SR & (USART_SR_ORE | USART_SR_FE | USART_SR_NE))
    (void)USART6->DR;
}

/* ================================================================== */
void Boot_HP_Init(void)
{
  /* No interrupt setup - using register polling */
  uart6_flush_rx();
}

/* ================================================================== */
/* ASCII protocol: @<payload>*XX\n                                     */
/* ================================================================== */

static uint8_t hp_xor_parity(const uint8_t *data, uint8_t len)
{
  uint8_t x = 0;
  for (uint8_t i = 0; i < len; i++)
    x ^= data[i];
  return x;
}

static void hp_ascii_send(char cmd)
{
  uint8_t payload = (uint8_t)cmd;
  uint8_t parity = hp_xor_parity(&payload, 1);
  char frame[12];
  int len = snprintf(frame, sizeof(frame), "@%c*%02X\n\n\n", cmd, parity);
  uart6_send((uint8_t *)frame, (uint16_t)len);
  HAL_Delay(2);  /* Ensure auto-direction transceiver settles */
}

static int hp_ascii_wait_ack(char expected_cmd, uint32_t timeout_ms)
{
  uint8_t buf[32];
  uint16_t idx = 0;
  uint32_t start = HAL_GetTick();

  while ((HAL_GetTick() - start) < timeout_ms) {
    uint8_t ch;
    if (!uart6_recv_byte(&ch, 1))
      continue;

    if (ch == '@')
      idx = 0;

    if (idx < sizeof(buf))
      buf[idx++] = ch;

    if (ch == '\n' && idx >= 6 && buf[0] == '@') {
      if (buf[1] == (uint8_t)expected_cmd)
        return 0;  /* ACK received */
      idx = 0;
    }
  }
  return -1;  /* Timeout */
}

/* ================================================================== */
/* CNNX binary protocol                                                */
/* ================================================================== */

static void hp_cnnx_build(uint8_t *pkt, uint8_t cmd, const uint8_t *data, uint8_t ndata)
{
  uint16_t idx = 0;
  pkt[idx++] = CNNX_HDR_0;
  pkt[idx++] = CNNX_HDR_1;
  pkt[idx++] = CNNX_NODE_HP;
  pkt[idx++] = CNNX_NODE_MASTER;
  pkt[idx++] = (uint8_t)(s_packetId >> 8);
  pkt[idx++] = (uint8_t)(s_packetId & 0xFF);
  s_packetId++;
  pkt[idx++] = cmd;
  pkt[idx++] = ndata;
  if (data && ndata > 0) {
    memcpy(&pkt[idx], data, ndata);
    idx += ndata;
  }
  pkt[idx++] = 0x00;
  pkt[idx++] = 0x00;
  pkt[idx++] = CNNX_TRL_0;
  pkt[idx++] = CNNX_TRL_1;
}

static int hp_cnnx_wait_cmd(uint8_t expected_cmd, uint32_t timeout_ms)
{
  uint8_t buf[CNNX_PKT_OVERHEAD + CNNX_MAX_DATA];
  uint16_t idx = 0;
  int state = 0;
  int ndata = 0;
  int expected_len = 0;
  uint32_t start = HAL_GetTick();

  while ((HAL_GetTick() - start) < timeout_ms) {
    uint8_t ch;
    if (!uart6_recv_byte(&ch, 1))
      continue;

    switch (state) {
    case 0:
      if (ch == CNNX_HDR_0) { buf[0] = ch; idx = 1; state = 1; }
      break;
    case 1:
      if (ch == CNNX_HDR_1) { buf[1] = ch; idx = 2; state = 2; }
      else state = 0;
      break;
    case 2:
      buf[idx++] = ch;
      if (idx == 8) {  /* After NDATA byte */
        ndata = ch;
        expected_len = ndata + CNNX_PKT_OVERHEAD;
      }
      if (idx >= CNNX_PKT_OVERHEAD && idx >= expected_len) {
        if (buf[idx - 2] == CNNX_TRL_0 && buf[idx - 1] == CNNX_TRL_1) {
          uint8_t cmd = buf[6];
          if (cmd == expected_cmd)
            return 0;
          if (cmd == CNNX_CMD_FWFAILURE || cmd == CNNX_CMD_FWWRERR ||
              cmd == CNNX_CMD_FWRDERR)
            return -(int)cmd;
        }
        state = 0; idx = 0;
      }
      break;
    }
  }
  return -1;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

void Boot_HP_BootApp(void)
{
  uart6_flush_rx();
  hp_ascii_send('A');
  hp_ascii_wait_ack('A', HP_ASCII_TIMEOUT_MS);
}

/* ================================================================== */
int Boot_HP_GetVersion(char *ver_buf, uint8_t buf_size)
{
  uart6_flush_rx();
  hp_ascii_send('U');

  /* Wait for response - HP might echo 'U' as ACK */
  uint8_t buf[32];
  uint16_t idx = 0;
  uint32_t start = HAL_GetTick();

  while ((HAL_GetTick() - start) < 2000) {
    uint8_t ch;
    if (!uart6_recv_byte(&ch, 1))
      continue;
    if (ch == '@') idx = 0;
    if (idx < sizeof(buf)) buf[idx++] = ch;
    if (ch == '\n' && idx >= 6 && buf[0] == '@' && buf[1] == 'V') {
      uint8_t vi = 0;
      for (uint16_t i = 2; i < idx && buf[i] != '*'; i++) {
        if (vi < buf_size - 1)
          ver_buf[vi++] = (char)buf[i];
      }
      ver_buf[vi] = '\0';
      return 0;
    }
  }
  return -1;
}

/* ================================================================== */
int Boot_HP_UpdateFirmware(FIL *fp, const Elf_Info_t *elf)
{
  int ret;

  uart6_flush_rx();
  s_packetId = 0;

  /* ---- Phase 1: ASCII - Enter update mode ---- */
  Boot_Print("[H562] Sending update cmd...\r\n");

  hp_ascii_send('U');
  ret = hp_ascii_wait_ack('U', HP_ASCII_TIMEOUT_MS);
  if (ret != 0) {
    Boot_Print("[H562] No response.\r\n");
    return -1;
  }
  Boot_Print("[H562] ACK received.\r\n");

  HAL_Delay(200);
  uart6_flush_rx();

  /* ---- Phase 2: CNNX - Firmware transfer ---- */
  uint32_t fw_size   = elf->total_flash_size;
  uint32_t block_cnt = (fw_size + CNNX_MAX_DATA - 1) / CNNX_MAX_DATA;

  uint8_t prep_data[10];
  prep_data[0] = (uint8_t)(fw_size >> 24);
  prep_data[1] = (uint8_t)(fw_size >> 16);
  prep_data[2] = (uint8_t)(fw_size >>  8);
  prep_data[3] = (uint8_t)(fw_size);
  prep_data[4] = (uint8_t)(block_cnt >> 24);
  prep_data[5] = (uint8_t)(block_cnt >> 16);
  prep_data[6] = (uint8_t)(block_cnt >>  8);
  prep_data[7] = (uint8_t)(block_cnt);
  prep_data[8] = 0x01;
  prep_data[9] = 0x00;

  uint8_t pkt[CNNX_PKT_OVERHEAD + CNNX_MAX_DATA];
  hp_cnnx_build(pkt, CNNX_CMD_FWPREPARE, prep_data, 10);
  uart6_send(pkt, CNNX_PKT_OVERHEAD + 10);

  Boot_Print("[H562] Erasing...\r\n");
  ret = hp_cnnx_wait_cmd(CNNX_CMD_FWREADY, 10000);
  if (ret != 0) {
    Boot_Print("[H562] Erase timeout.\r\n");
    return -2;
  }

  ret = hp_cnnx_wait_cmd(CNNX_CMD_FWSTART, 5000);
  if (ret != 0) {
    Boot_Print("[H562] Start timeout.\r\n");
    return -2;
  }

  /* ---- Send firmware data blocks ---- */
  Boot_Print("[H562] 0%%");
  uint32_t addr = elf->flash_min;
  uint8_t  data_buf[CNNX_MAX_DATA];
  uint8_t  last_pct = 0;

  for (uint32_t blk = 0; blk < block_cnt; blk++) {
    uint32_t remain = fw_size - (blk * CNNX_MAX_DATA);
    uint8_t  chunk = (remain > CNNX_MAX_DATA) ? CNNX_MAX_DATA : (uint8_t)remain;

    if (Elf_ReadAt(fp, elf, addr, data_buf, chunk) < 0) {
      Boot_Print("\r\n[H562] Read err.\r\n");
      return -3;
    }

    hp_cnnx_build(pkt, CNNX_CMD_FWCODE, data_buf, chunk);
    uart6_send(pkt, CNNX_PKT_OVERHEAD + chunk);

    ret = hp_cnnx_wait_cmd(CNNX_CMD_FWNEXT, 5000);
    if (ret != 0) {
      Boot_Print("\r\n[H562] Blk %lu err.\r\n", (unsigned long)blk);
      return -4;
    }

    addr += chunk;
    uint8_t pct = (uint8_t)(((blk + 1) * 100UL) / block_cnt);
    if (pct / 10 > last_pct / 10) {
      Boot_Print("\r[H562] %d%%", pct);
      last_pct = pct;
    }
  }

  Boot_Print("\r[H562] 100%%\r\n");

  hp_cnnx_build(pkt, CNNX_CMD_FWEND, NULL, 0);
  uart6_send(pkt, CNNX_PKT_OVERHEAD);

  ret = hp_cnnx_wait_cmd(CNNX_CMD_FWSUCCESS, 5000);
  if (ret != 0) {
    Boot_Print("[H562] Verify err.\r\n");
    return -5;
  }

  Boot_Print("[H562] Update OK!\r\n");
  return 0;
}
