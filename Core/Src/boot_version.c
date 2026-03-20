/**
  ******************************************************************************
  * @file    boot_version.c
  * @brief   Version management - minimal slot-based storage
  *
  *          0x0800FF00-0x0800FFFF: 32 x 8-byte slots (magic + fdate).
  *          Cannot erase sector 3 at runtime. Write to next empty slot.
  *          Main app uses VER_MAGIC, HP app uses HP_MAGIC.
  ******************************************************************************
  */
#include "boot_version.h"
#include "stm32f4xx_hal.h"

#define VER_BASE    0x0800FF00U
#define VER_MAGIC   0x56523032U  /* "VR02" - main app */
#define HP_MAGIC    0x48503032U  /* "HP02" - HP app */

typedef struct { uint32_t magic; uint16_t fdate; uint16_t pad; } VSlot_t;
#define VER_SLOTS   (256 / sizeof(VSlot_t))  /* 32 */

static char s_buf[VERSION_STR_LEN];

/* "YYYY-MM-DD" from FAT date, no snprintf */
static void fmt_date(uint16_t fd, char *o)
{
  uint16_t y = ((fd >> 9) & 0x7F) + 1980;
  uint8_t  m = (fd >> 5) & 0xF;
  uint8_t  d = fd & 0x1F;
  o[0] = '0' + y / 1000;
  o[1] = '0' + (y / 100 % 10);
  o[2] = '0' + (y / 10 % 10);
  o[3] = '0' + (y % 10);
  o[4] = '-';
  o[5] = '0' + m / 10;
  o[6] = '0' + m % 10;
  o[7] = '-';
  o[8] = '0' + d / 10;
  o[9] = '0' + d % 10;
  o[10] = '\0';
}

/* Parse __DATE__ month without string table */
static uint8_t parse_month(const char *d)
{
  if (d[0] == 'J') return (d[1] == 'a') ? 1 : (d[2] == 'n') ? 6 : 7;
  if (d[0] == 'F') return 2;
  if (d[0] == 'M') return (d[2] == 'r') ? 3 : 5;
  if (d[0] == 'A') return (d[1] == 'p') ? 4 : 8;
  if (d[0] == 'S') return 9;
  if (d[0] == 'O') return 10;
  if (d[0] == 'N') return 11;
  if (d[0] == 'D') return 12;
  return 0;
}

/* ---- Shared helpers (one copy of loop, called with different magic) ---- */

static const char* get_ver(uint32_t magic)
{
  int last = -1;
  for (int i = 0; i < (int)VER_SLOTS; i++) {
    const VSlot_t *s = (const VSlot_t *)(VER_BASE + i * sizeof(VSlot_t));
    if (s->magic == magic) last = i;
  }
  if (last >= 0) {
    const VSlot_t *s = (const VSlot_t *)(VER_BASE + last * sizeof(VSlot_t));
    if (s->fdate != 0 && s->fdate != 0xFFFF) {
      fmt_date(s->fdate, s_buf);
      return s_buf;
    }
  }
  return "--";
}

static void record_ver(uint32_t magic, uint16_t fdate)
{
  int target = -1;
  for (int i = 0; i < (int)VER_SLOTS; i++) {
    const uint32_t *p = (const uint32_t *)(VER_BASE + i * sizeof(VSlot_t));
    if (p[0] == 0xFFFFFFFF && p[1] == 0xFFFFFFFF) { target = i; break; }
  }
  if (target < 0) return;

  VSlot_t slot;
  slot.magic = magic;
  slot.fdate = fdate;
  slot.pad   = 0xFFFF;

  uint32_t addr = VER_BASE + target * sizeof(VSlot_t);
  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                         FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                         FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
  const uint32_t *src = (const uint32_t *)&slot;
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr,     src[0]);
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + 4, src[1]);
  HAL_FLASH_Lock();
}

/* ------------------------------------------------------------------ */
const char* Boot_Version_GetBoot(void)
{
  static char bv[VERSION_STR_LEN];
  if (bv[0] == '\0') {
    const char *d = __DATE__;
    uint8_t  mo = parse_month(d);
    uint8_t  dy = (d[4] == ' ') ? (d[5] - '0')
                                : ((d[4] - '0') * 10 + (d[5] - '0'));
    uint16_t yr = (uint16_t)((d[7]-'0')*1000 + (d[8]-'0')*100
                           + (d[9]-'0')*10   + (d[10]-'0'));
    uint16_t fd = ((yr - 1980) << 9) | (mo << 5) | dy;
    fmt_date(fd, bv);
  }
  return bv;
}

/* ------------------------------------------------------------------ */
const char* Boot_Version_GetApp(void) { return get_ver(VER_MAGIC); }
const char* Boot_Version_GetHP(void)  { return get_ver(HP_MAGIC); }

void Boot_Version_RecordPre(void) { /* no-op */ }

void Boot_Version_RecordPost(const char *filename, uint16_t fdate)
{
  (void)filename;
  record_ver(VER_MAGIC, fdate);
}

void Boot_Version_RecordHP(uint16_t fdate)
{
  record_ver(HP_MAGIC, fdate);
}
