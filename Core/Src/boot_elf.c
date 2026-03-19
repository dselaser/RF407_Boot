/**
  ******************************************************************************
  * @file    boot_elf.c
  * @brief   32-bit ARM ELF file parser for firmware update
  ******************************************************************************
  */
#include "boot_elf.h"
#include <string.h>

/* ---- ELF32 header structures ---- */
#define EI_NIDENT   16
#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS32  1
#define ELFDATA2LSB 1     /* Little-endian */
#define EM_ARM      40
#define PT_LOAD     1

#pragma pack(push, 1)
typedef struct {
  uint8_t  e_ident[EI_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_entry;
  uint32_t e_phoff;       /* Program header table offset */
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;   /* Size of one program header */
  uint16_t e_phnum;       /* Number of program headers */
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_offset;      /* Offset in file */
  uint32_t p_vaddr;       /* Virtual address */
  uint32_t p_paddr;       /* Physical address (LMA) */
  uint32_t p_filesz;      /* Size in file */
  uint32_t p_memsz;       /* Size in memory */
  uint32_t p_flags;
  uint32_t p_align;
} Elf32_Phdr;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
int Elf_Parse(FIL *fp, Elf_Info_t *info)
{
  memset(info, 0, sizeof(*info));

  /* Read ELF header */
  Elf32_Ehdr ehdr;
  UINT br;
  if (f_lseek(fp, 0) != FR_OK)
    return -1;
  if (f_read(fp, &ehdr, sizeof(ehdr), &br) != FR_OK || br != sizeof(ehdr))
    return -1;

  /* Validate magic */
  if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
      ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3)
    return -1;

  /* Must be 32-bit, little-endian, ARM */
  if (ehdr.e_ident[4] != ELFCLASS32)
    return -1;
  if (ehdr.e_ident[5] != ELFDATA2LSB)
    return -1;
  if (ehdr.e_machine != EM_ARM)
    return -1;

  /* Validate program header info */
  if (ehdr.e_phnum == 0 || ehdr.e_phentsize < sizeof(Elf32_Phdr))
    return -1;

  /* Read program headers and extract PT_LOAD segments */
  uint32_t flash_min = 0xFFFFFFFF;
  uint32_t flash_max = 0;
  uint8_t  seg_idx = 0;

  for (uint16_t i = 0; i < ehdr.e_phnum && seg_idx < ELF_MAX_SEGMENTS; i++) {
    uint32_t offset = ehdr.e_phoff + (uint32_t)i * ehdr.e_phentsize;

    Elf32_Phdr phdr;
    if (f_lseek(fp, offset) != FR_OK)
      return -1;
    if (f_read(fp, &phdr, sizeof(phdr), &br) != FR_OK || br != sizeof(phdr))
      return -1;

    if (phdr.p_type != PT_LOAD)
      continue;
    if (phdr.p_filesz == 0)
      continue;

    /* Only include segments that load to flash (0x08xxxxxx) */
    if ((phdr.p_paddr & 0xFF000000) != 0x08000000)
      continue;

    info->seg[seg_idx].flash_addr  = phdr.p_paddr;
    info->seg[seg_idx].file_offset = phdr.p_offset;
    info->seg[seg_idx].file_size   = phdr.p_filesz;
    seg_idx++;

    if (phdr.p_paddr < flash_min)
      flash_min = phdr.p_paddr;
    if ((phdr.p_paddr + phdr.p_filesz) > flash_max)
      flash_max = phdr.p_paddr + phdr.p_filesz;
  }

  if (seg_idx == 0)
    return -1;

  info->seg_count       = seg_idx;
  info->flash_min       = flash_min;
  info->flash_max       = flash_max;
  info->total_flash_size = flash_max - flash_min;
  info->valid           = 1;

  return 0;
}

/* ------------------------------------------------------------------ */
int Elf_ReadAt(FIL *fp, const Elf_Info_t *info,
               uint32_t flash_addr, uint8_t *buf, uint32_t len)
{
  /* Fill with 0xFF (erased flash state) by default */
  memset(buf, 0xFF, len);

  for (uint8_t s = 0; s < info->seg_count; s++) {
    const Elf_Segment_t *seg = &info->seg[s];

    /* Calculate overlap between [flash_addr, flash_addr+len)
       and [seg->flash_addr, seg->flash_addr+seg->file_size) */
    uint32_t seg_start = seg->flash_addr;
    uint32_t seg_end   = seg->flash_addr + seg->file_size;
    uint32_t req_start = flash_addr;
    uint32_t req_end   = flash_addr + len;

    if (req_start >= seg_end || req_end <= seg_start)
      continue;  /* No overlap */

    uint32_t overlap_start = (req_start > seg_start) ? req_start : seg_start;
    uint32_t overlap_end   = (req_end < seg_end) ? req_end : seg_end;
    uint32_t overlap_len   = overlap_end - overlap_start;

    /* Calculate file position and buffer position */
    uint32_t file_pos = seg->file_offset + (overlap_start - seg_start);
    uint32_t buf_pos  = overlap_start - req_start;

    if (f_lseek(fp, file_pos) != FR_OK)
      return -1;

    UINT br;
    if (f_read(fp, &buf[buf_pos], overlap_len, &br) != FR_OK || br != overlap_len)
      return -1;
  }

  return (int)len;
}
