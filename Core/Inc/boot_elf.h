/**
  ******************************************************************************
  * @file    boot_elf.h
  * @brief   32-bit ARM ELF file parser for firmware update
  ******************************************************************************
  */
#ifndef BOOT_ELF_H
#define BOOT_ELF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ff.h"

/* Max loadable segments we track */
#define ELF_MAX_SEGMENTS  8

/* ELF segment info (extracted from PT_LOAD program headers) */
typedef struct {
  uint32_t flash_addr;    /* p_paddr - physical/load address in flash */
  uint32_t file_offset;   /* p_offset - byte offset within .elf file */
  uint32_t file_size;     /* p_filesz - bytes to read from file */
} Elf_Segment_t;

/* Parsed ELF info */
typedef struct {
  uint8_t         valid;           /* 1 if parsing succeeded */
  uint8_t         seg_count;       /* number of loadable segments */
  Elf_Segment_t   seg[ELF_MAX_SEGMENTS];
  uint32_t        flash_min;       /* lowest flash address across segments */
  uint32_t        flash_max;       /* highest (addr + size) across segments */
  uint32_t        total_flash_size;/* flash_max - flash_min */
} Elf_Info_t;

/**
  * @brief  Parse an ELF file and extract loadable segment info
  * @param  fp     pointer to opened FIL (FATFS file handle)
  * @param  info   [out] parsed segment info
  * @retval 0 on success, -1 on error
  */
int Elf_Parse(FIL *fp, Elf_Info_t *info);

/**
  * @brief  Read data from an ELF file for a given flash address range
  *         Handles gaps between segments (fills with 0xFF)
  * @param  fp         pointer to opened FIL
  * @param  info       parsed ELF info
  * @param  flash_addr target flash address to read data for
  * @param  buf        destination buffer
  * @param  len        bytes to read
  * @retval actual bytes filled (always == len on success), -1 on error
  */
int Elf_ReadAt(FIL *fp, const Elf_Info_t *info,
               uint32_t flash_addr, uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif /* BOOT_ELF_H */
