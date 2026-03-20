/**
  ******************************************************************************
  * @file    boot_hp.h
  * @brief   CNNX protocol - Master side for handpiece (H562) firmware update
  *          Matches RF562_Boot bootloader protocol
  ******************************************************************************
  */
#ifndef BOOT_HP_H
#define BOOT_HP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ff.h"
#include "boot_elf.h"

/* ---- CNNX Protocol Constants ---- */
#define CNNX_HDR_0          0xAA
#define CNNX_HDR_1          0x55
#define CNNX_TRL_0          0x55
#define CNNX_TRL_1          0xAA

#define CNNX_NODE_MASTER    0x01
#define CNNX_NODE_HP        0x02

#define CNNX_CMD_FWPREPARE  7
#define CNNX_CMD_FWREADY    8
#define CNNX_CMD_FWSTART    9
#define CNNX_CMD_FWCODE     10
#define CNNX_CMD_FWNEXT     11
#define CNNX_CMD_FWEND      12
#define CNNX_CMD_FWSUCCESS  13
#define CNNX_CMD_FWFAILURE  14
#define CNNX_CMD_FWWAIT     4
#define CNNX_CMD_FWWRERR    18
#define CNNX_CMD_FWRDERR    19

#define CNNX_MAX_DATA       240     /* Max payload per packet */
#define CNNX_PKT_OVERHEAD   12      /* HDR(2)+DST+SRC+PID(2)+CMD+NDATA+CRC(2)+TRL(2) */

/* Enable verbose/probing FWPREPARE variants (adds code size). */
#ifndef BOOT_HP_CNNX_PROBE
#define BOOT_HP_CNNX_PROBE  0
#endif

/* Enable baud-rate probing for CNNX phase (adds code size). */
#ifndef BOOT_HP_BAUD_PROBE
#define BOOT_HP_BAUD_PROBE  0
#endif

/* Enable minimal RX sniff on FWPREPARE timeout (adds small code/rodata). */
#ifndef BOOT_HP_RX_SNIFF
#define BOOT_HP_RX_SNIFF    0
#endif

/* H562 app base address */
#define HP_APP_BASE         0x08008000U

/* ---- App Version struct (shared convention) ---- */
#define APP_VERSION_MAGIC   0x56455253U  /* "VERS" */
#define APP_VERSION_OFFSET  0x200U       /* 512 bytes from app flash base */

typedef struct {
  uint32_t magic;        /* APP_VERSION_MAGIC */
  uint8_t  major;
  uint8_t  minor;
  uint8_t  patch;
  uint8_t  _reserved;
  char     date[12];     /* "2026-03-20" */
  char     name[12];     /* "RF_Needle" etc */
} AppVersion_t;          /* 32 bytes */

/* ---- ASCII Phase ---- */
#define HP_ASCII_TIMEOUT_MS 3000

/* ---- Functions ---- */

/**
  * @brief  Initialize RS485 UART6 for handpiece communication
  */
void Boot_HP_Init(void);

/**
  * @brief  Get handpiece firmware version via RS485 ASCII 'U' command
  * @param  ver_buf   output buffer for version string
  * @param  buf_size  buffer size
  * @retval 0 on success, -1 on timeout/error
  */
int Boot_HP_GetVersion(char *ver_buf, uint8_t buf_size);

/**
  * @brief  Perform complete firmware update to handpiece
  *         Phase 1: ASCII '@U*55\n' to enter update mode
  *         Phase 2: CNNX binary transfer
  * @param  fp   opened ELF file handle
  * @param  elf  parsed ELF info
  * @retval 0 on success, negative on error
  */
int Boot_HP_UpdateFirmware(FIL *fp, const Elf_Info_t *elf);

/**
  * @brief  Send 'A' command to H562 to boot its application
  */
void Boot_HP_BootApp(void);

#ifdef __cplusplus
}
#endif
#endif /* BOOT_HP_H */
