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

static void uart6_flush_rx(void);

#if BOOT_HP_BAUD_PROBE
static void uart6_set_baud(uint32_t baud)
{
  /* Re-init USART6 via HAL to change BRR cleanly. */
  extern UART_HandleTypeDef huart6;
  huart6.Init.BaudRate = baud;
  (void)HAL_UART_DeInit(&huart6);
  (void)HAL_UART_Init(&huart6);
  /* Keep using polling mode */
  HAL_NVIC_DisableIRQ(USART6_IRQn);
  uart6_flush_rx();
}
#endif

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

#if BOOT_HP_RX_SNIFF
static void uart6_sniff_hex(uint32_t window_ms, uint16_t max_bytes)
{
  uint16_t n = 0;
  uint32_t start = HAL_GetTick();
  uint32_t last = start;

  while ((HAL_GetTick() - start) < window_ms && n < max_bytes) {
    uint8_t b;
    if (uart6_recv_byte(&b, 2)) {
      if (n == 0) Boot_Print("HP RX:");
      Boot_Print(" %02X", b);
      n++;
      last = HAL_GetTick();
      continue;
    }
    if (n > 0 && (HAL_GetTick() - last) > 20)
      break;
  }
  if (n > 0) Boot_Print("\r\n");
}
#endif

/* ================================================================== */
void Boot_HP_Init(void)
{
  /* Use register polling only; keep NVIC/USART6 IRQ from interfering */
  HAL_NVIC_DisableIRQ(USART6_IRQn);
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

    /* Some HP bootloaders ACK by echoing the command byte only (e.g., 'U'). */
    if (ch == (uint8_t)expected_cmd)
      return 0;

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

static uint8_t hp_cnnx_xor_parity(const uint8_t *data, uint16_t len)
{
  uint8_t x = 0;
  for (uint16_t i = 0; i < len; i++)
    x ^= data[i];
  return x;
}

typedef enum {
  HP_CNNX_CHECK_ZERO = 0,
  HP_CNNX_CHECK_XOR_DST_TO_PAYLOAD_DUP,   /* XOR over [DST..payload], store (p,p) */
  HP_CNNX_CHECK_XOR_ALL_EXCL_CHECK_DUP,   /* XOR over [HDR..endPayload], store (p,p) */
  HP_CNNX_CHECK_SUM16_DST_TO_PAYLOAD,     /* SUM16 over [DST..payload], store (sumHi,sumLo) */
} hp_cnnx_check_mode_t;

static uint16_t hp_cnnx_sum16(const uint8_t *data, uint16_t len)
{
  uint32_t s = 0;
  for (uint16_t i = 0; i < len; i++)
    s += data[i];
  return (uint16_t)s;
}

static void hp_cnnx_set_check(uint8_t *pkt, uint16_t data_end_idx, hp_cnnx_check_mode_t mode)
{
  /* data_end_idx: index right after payload (i.e. where check starts) */
  uint8_t c0 = 0, c1 = 0;

  switch (mode) {
    case HP_CNNX_CHECK_ZERO:
      c0 = 0x00; c1 = 0x00;
      break;

    case HP_CNNX_CHECK_XOR_DST_TO_PAYLOAD_DUP: {
      uint8_t p = hp_cnnx_xor_parity(&pkt[2], (uint16_t)(data_end_idx - 2));
      c0 = p; c1 = p;
      break;
    }

    case HP_CNNX_CHECK_XOR_ALL_EXCL_CHECK_DUP: {
      uint8_t p = hp_cnnx_xor_parity(&pkt[0], (uint16_t)data_end_idx);
      c0 = p; c1 = p;
      break;
    }

    case HP_CNNX_CHECK_SUM16_DST_TO_PAYLOAD: {
      uint16_t s = hp_cnnx_sum16(&pkt[2], (uint16_t)(data_end_idx - 2));
      c0 = (uint8_t)(s >> 8);
      c1 = (uint8_t)(s & 0xFF);
      break;
    }
  }

  pkt[data_end_idx + 0] = c0;
  pkt[data_end_idx + 1] = c1;
}

static void hp_cnnx_build(uint8_t *pkt,
                          uint8_t hdr0,
                          uint8_t hdr1,
                          uint8_t dst,
                          uint8_t src,
                          uint16_t pid,
                          uint8_t pid_little_endian,
                          uint8_t cmd,
                          const uint8_t *data,
                          uint8_t ndata,
                          hp_cnnx_check_mode_t check_mode,
                          uint8_t trl0,
                          uint8_t trl1)
{
  uint16_t idx = 0;
  pkt[idx++] = hdr0;
  pkt[idx++] = hdr1;
  pkt[idx++] = dst;
  pkt[idx++] = src;
  if (pid_little_endian) {
    pkt[idx++] = (uint8_t)(pid & 0xFF);
    pkt[idx++] = (uint8_t)(pid >> 8);
  } else {
    pkt[idx++] = (uint8_t)(pid >> 8);
    pkt[idx++] = (uint8_t)(pid & 0xFF);
  }
  pkt[idx++] = cmd;
  pkt[idx++] = ndata;
  if (data && ndata > 0) {
    memcpy(&pkt[idx], data, ndata);
    idx += ndata;
  }
  hp_cnnx_set_check(pkt, idx, check_mode);
  idx += 2;
  pkt[idx++] = trl0;
  pkt[idx++] = trl1;
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
  /* FWPREPARE */
#if BOOT_HP_CNNX_PROBE
  /* Try multiple check/parity + field order variants (debug/probing mode). */
  static const hp_cnnx_check_mode_t modes[] = {
      HP_CNNX_CHECK_XOR_DST_TO_PAYLOAD_DUP,
      HP_CNNX_CHECK_XOR_ALL_EXCL_CHECK_DUP,
      HP_CNNX_CHECK_SUM16_DST_TO_PAYLOAD,
      HP_CNNX_CHECK_ZERO,
  };

  int ready = 0;
  uint16_t pid = 0;
  for (uint8_t mi = 0; mi < (uint8_t)(sizeof(modes)/sizeof(modes[0])) && !ready; mi++) {
    for (uint8_t swap = 0; swap < 2 && !ready; swap++) {
      for (uint8_t ple = 0; ple < 2 && !ready; ple++) {
        uint8_t dst = swap ? CNNX_NODE_MASTER : CNNX_NODE_HP;
        uint8_t src = swap ? CNNX_NODE_HP : CNNX_NODE_MASTER;

        hp_cnnx_build(pkt,
                      CNNX_HDR_0, CNNX_HDR_1,
                      dst, src,
                      pid, ple,
                      CNNX_CMD_FWPREPARE,
                      prep_data, 10,
                      modes[mi],
                      CNNX_TRL_0, CNNX_TRL_1);
        uart6_send(pkt, (uint16_t)(CNNX_PKT_OVERHEAD + 10));

        Boot_Print("[H562] Erasing...\r\n");
        ret = hp_cnnx_wait_cmd(CNNX_CMD_FWREADY, 2500);
        if (ret == 0) {
          ready = 1;
          break;
        }

        HAL_Delay(50);
        uart6_flush_rx();
        pid++;
      }
    }
  }

  if (!ready) {
    Boot_Print("[H562] Erase timeout.\r\n");
    return -2;
  }
#else
  /* Slim retry set to cope with protocol mismatches without large debug strings.
     (Keeps code size within 64KB boot region.) */
  typedef struct {
    uint8_t dst, src;
    uint8_t pid_le;
    uint8_t check; /* hp_cnnx_check_mode_t (stored compactly) */
  } prep_variant_t;

  static const prep_variant_t v[] = {
      /* Most likely variants first */
      { CNNX_NODE_HP,     CNNX_NODE_MASTER, 0, (uint8_t)HP_CNNX_CHECK_XOR_DST_TO_PAYLOAD_DUP },
      { CNNX_NODE_HP,     CNNX_NODE_MASTER, 1, (uint8_t)HP_CNNX_CHECK_XOR_DST_TO_PAYLOAD_DUP },
      { CNNX_NODE_MASTER, CNNX_NODE_HP,     0, (uint8_t)HP_CNNX_CHECK_XOR_DST_TO_PAYLOAD_DUP },
      { CNNX_NODE_HP,     CNNX_NODE_MASTER, 0, (uint8_t)HP_CNNX_CHECK_XOR_ALL_EXCL_CHECK_DUP },
  };

  int ready = 0;
  uint16_t pid0 = s_packetId; /* Keep PID constant during FWPREPARE retries */
  for (uint8_t i = 0; i < (uint8_t)(sizeof(v)/sizeof(v[0])); i++) {
    hp_cnnx_build(pkt,
                  CNNX_HDR_0, CNNX_HDR_1,
                  v[i].dst, v[i].src,
                  pid0, v[i].pid_le,
                  CNNX_CMD_FWPREPARE,
                  prep_data, 10,
                  (hp_cnnx_check_mode_t)v[i].check,
                  CNNX_TRL_0, CNNX_TRL_1);
    uart6_send(pkt, (uint16_t)(CNNX_PKT_OVERHEAD + 10));

    Boot_Print("[H562] Erasing...\r\n");
    ret = hp_cnnx_wait_cmd(CNNX_CMD_FWREADY, 2000);
    if (ret == 0) {
      ready = 1;
      break;
    }
#if BOOT_HP_RX_SNIFF
    uart6_sniff_hex(120, 48);
#endif
    HAL_Delay(50);
    uart6_flush_rx();
  }

#if BOOT_HP_BAUD_PROBE
  if (!ready) {
    static const uint32_t bauds[] = { 230400, 460800 };
    for (uint8_t bi = 0; bi < (uint8_t)(sizeof(bauds)/sizeof(bauds[0])) && !ready; bi++) {
      uart6_set_baud(bauds[bi]);
      for (uint8_t i = 0; i < (uint8_t)(sizeof(v)/sizeof(v[0])); i++) {
        hp_cnnx_build(pkt,
                      CNNX_HDR_0, CNNX_HDR_1,
                      v[i].dst, v[i].src,
                      pid0, v[i].pid_le,
                      CNNX_CMD_FWPREPARE,
                      prep_data, 10,
                      (hp_cnnx_check_mode_t)v[i].check,
                      CNNX_TRL_0, CNNX_TRL_1);
        uart6_send(pkt, (uint16_t)(CNNX_PKT_OVERHEAD + 10));

        Boot_Print("[H562] Erasing...\r\n");
        ret = hp_cnnx_wait_cmd(CNNX_CMD_FWREADY, 2000);
        if (ret == 0) {
          ready = 1;
          break;
        }
        HAL_Delay(50);
        uart6_flush_rx();
      }
    }
  }
#endif

  if (!ready) {
    Boot_Print("[H562] Erase timeout.\r\n");
    return -2;
  }

  /* Consume this PID only once FWPREPARE succeeds */
  s_packetId = (uint16_t)(pid0 + 1);
#endif

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

    hp_cnnx_build(pkt,
                  CNNX_HDR_0, CNNX_HDR_1,
                  CNNX_NODE_HP, CNNX_NODE_MASTER,
                  (uint16_t)(s_packetId++), 0,
                  CNNX_CMD_FWCODE,
                  data_buf, chunk,
                  HP_CNNX_CHECK_XOR_DST_TO_PAYLOAD_DUP,
                  CNNX_TRL_0, CNNX_TRL_1);
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

  hp_cnnx_build(pkt,
                CNNX_HDR_0, CNNX_HDR_1,
                CNNX_NODE_HP, CNNX_NODE_MASTER,
                (uint16_t)(s_packetId++), 0,
                CNNX_CMD_FWEND,
                NULL, 0,
                HP_CNNX_CHECK_XOR_DST_TO_PAYLOAD_DUP,
                CNNX_TRL_0, CNNX_TRL_1);
  uart6_send(pkt, CNNX_PKT_OVERHEAD);

  ret = hp_cnnx_wait_cmd(CNNX_CMD_FWSUCCESS, 5000);
  if (ret != 0) {
    Boot_Print("[H562] Verify err.\r\n");
    return -5;
  }

  Boot_Print("[H562] Update OK!\r\n");
  return 0;
}
