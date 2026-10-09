#include "flash.h"
#include "ota_state.h"
#include "err.h"
#include "stdint.h"
#include <string.h>
#include "stm32f4xx.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_flash_ex.h"

/*Sector 0~3: 16 KB (0x4000)

Sector 4: 64 KB (0x10000)

Sector 5~11: 128 KB (0x20000)
stm32f4不使用外部flash就是这种如果使用外部flash就直接这样了用的是两份ld直接跳转的模式
boot： 0x08000000 ~ 0x0800FFFF sector 0~3   64 KB

state：0x08010000 ~ 0x0801FFFF sector 4     64kB

app1：0x08020000 ~ 0x0807FFFF sector 5~7    384 KB

app2: 0x08080000 ~ 0x080DFFFF sector 8~10   384 KB

Scratch:0x080E0000~ 0x080FFFFF sector 11    128kB
*/
/* 分区 id 必须用 flash.h 的 flash_area_id_t 枚举(IMAGE_0=0/IMAGE_1=1/BOOTLOADER=2/STATE=3),
 * mini-ota 内部按 FLASH_AREA_ID_* 查表, 自定义编号会整表错位 */
#define FLASH_STATE_SIZE      0x00010000UL /* 64 KB  (Sector 4)    */
#define FLASH_APP_1_SIZE      0x00060000UL /* 384 KB (Sector 5~7)  */
#define FLASH_APP_2_SIZE      0x00060000UL /* 384 KB (Sector 8~10) */
#define FLASH_SCRATCH_SIZE    0x00020000UL /* 128 KB (Sector 11)   */

static const flash_area_t s_flash_areas[] =
{
    { .fa_id = FLASH_AREA_ID_IMAGE_0,    .fa_device_id = 0, .fa_offset = 0x08020000, .fa_size = FLASH_APP_1_SIZE },
    { .fa_id = FLASH_AREA_ID_IMAGE_1,    .fa_device_id = 0, .fa_offset = 0x08080000, .fa_size = FLASH_APP_2_SIZE },
    { .fa_id = FLASH_AREA_ID_BOOTLOADER, .fa_device_id = 0, .fa_offset = 0x08000000, .fa_size = 0x00010000UL },
    { .fa_id = FLASH_AREA_ID_STATE,      .fa_device_id = 0, .fa_offset = 0x08010000, .fa_size = FLASH_STATE_SIZE },
    /* Scratch(0x080E0000, 128KB) mini-ota 无此概念, 暂不注册 */
};

#define FLASH_AREA_COUNT (sizeof(s_flash_areas) / sizeof(s_flash_areas[0]))


/* ---- ST 内部 flash 扇区布局（STM32F4 1MB，共 12 个 sector） ---- */
typedef struct
{
    uint32_t offset; /* sector 起始绝对地址 */
    uint32_t size;   /* sector 大小 */
    uint32_t number; /* HAL sector 编号（FLASH_SECTOR_x 的值） */
} flash_sector_layout_t;

static const flash_sector_layout_t s_sector_layout[] =
{
    { 0x08000000, 0x04000, 0 }, /* Sector 0~3:  16KB  */
    { 0x08004000, 0x04000, 1 },
    { 0x08008000, 0x04000, 2 },
    { 0x0800C000, 0x04000, 3 },
    { 0x08010000, 0x10000, 4 }, /* Sector 4:    64KB  */
    { 0x08020000, 0x20000, 5 }, /* Sector 5~11: 128KB */
    { 0x08040000, 0x20000, 6 },
    { 0x08060000, 0x20000, 7 },
    { 0x08080000, 0x20000, 8 },
    { 0x080A0000, 0x20000, 9 },
    { 0x080C0000, 0x20000, 10 },
    { 0x080E0000, 0x20000, 11 },
};
#define SECTOR_COUNT (sizeof(s_sector_layout) / sizeof(s_sector_layout[0]))

/**
 * @brief: 查找地址所在的 sector 在布局表中的下标
 */
static int flash_find_sector(uint32_t addr, uint32_t *index)
{
    for (uint32_t i = 0; i < SECTOR_COUNT; i++)
    {
        if ((addr >= s_sector_layout[i].offset) &&
            (addr < (s_sector_layout[i].offset + s_sector_layout[i].size)))
        {
            *index = i;
            return ERR_OK;
        }
    }
    return ERR_ARG;
}

/**
 * @brief: 按 fa_id 查表返回 area 描述。纯查表，不做任何硬件解锁
 *         （unlock/lock 由 erase/write 内部配对，open 无需 close）
 */
int flash_open(uint32_t fa_id, const flash_area_t **area)
{
    if (area == NULL)
    {
        return ERR_ARG;
    }
    *area = NULL;
    for (uint32_t i = 0; i < FLASH_AREA_COUNT; i++)
    {
        if (s_flash_areas[i].fa_id == fa_id)
        {
            *area = &s_flash_areas[i];
            return ERR_OK;
        }
    }
    return ERR_ARG;
}

/**
 * @brief: 擦除 [area->fa_offset + off, +len) 覆盖到的所有 sector（向上取整）
 */
int flash_erase(const flash_area_t *area, uint32_t off, uint32_t len)
{
    if ((area == NULL) || (len == 0u))
    {
        return ERR_ARG;
    }
    if ((off > area->fa_size) || (len > (area->fa_size - off)))
    {
        return ERR_ARG;
    }

    const uint32_t start = area->fa_offset + off;
    const uint32_t end   = start + len - 1u; /* 区间内最后一个字节 */

    uint32_t first = 0u;
    uint32_t last  = 0u;
    int rc = flash_find_sector(start, &first);
    if (rc != ERR_OK)
    {
        return rc;
    }
    rc = flash_find_sector(end, &last);
    if (rc != ERR_OK)
    {
        return rc;
    }

    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase =
    {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .Banks        = FLASH_BANK_1,
        .Sector       = s_sector_layout[first].number,
        .NbSectors    = (s_sector_layout[last].number - s_sector_layout[first].number) + 1u,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3, /* 2.7~3.6V 供电 */
    };
    uint32_t sector_error = 0u;
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&erase, &sector_error);
    HAL_FLASH_Lock();

    return (st == HAL_OK) ? ERR_OK : ERR_OTA_HW;
}

/**
 * @brief: 写入。中间 4 字节对齐段按字编程，头/尾不对齐段按字节编程
 *         注意：内部 flash 只能 1→0，目标区域必须预先擦除（全 0xFF）
 */
int flash_write(const flash_area_t *area, uint32_t off, const void *buf, uint32_t len)
{
    if ((area == NULL) || (buf == NULL) || (len == 0u))
    {
        return ERR_ARG;
    }
    if ((off > area->fa_size) || (len > (area->fa_size - off)))
    {
        return ERR_ARG;
    }

    uint32_t addr      = area->fa_offset + off;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = len;
    HAL_StatusTypeDef st;

    HAL_FLASH_Unlock();

    /* 头部：地址未 4 字节对齐的部分逐字节编程 */
    while (((addr & 3u) != 0u) && (remaining > 0u))
    {
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr, (uint64_t)src[0]);
        if (st != HAL_OK)
        {
            goto out;
        }
        addr++;
        src++;
        remaining--;
    }
    /* 中间：按字（4 字节）编程 */
    while (remaining >= 4u)
    {
        const uint32_t word = (uint32_t)src[0] |
                              ((uint32_t)src[1] << 8)  |
                              ((uint32_t)src[2] << 16) |
                              ((uint32_t)src[3] << 24);
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, (uint64_t)word);
        if (st != HAL_OK)
        {
            goto out;
        }
        addr      += 4u;
        src       += 4u;
        remaining -= 4u;
    }
    /* 尾部：剩余不足 4 字节部分逐字节编程 */
    while (remaining > 0u)
    {
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr, (uint64_t)src[0]);
        if (st != HAL_OK)
        {
            goto out;
        }
        addr++;
        src++;
        remaining--;
    }

    st = HAL_OK;
out:
    HAL_FLASH_Lock();
    return (st == HAL_OK) ? ERR_OK : ERR_OTA_HW;
}

/**
 * @brief: 读取。内部 flash 内存映射，直接 memcpy，无需解锁
 */
int flash_read(const flash_area_t *area, uint32_t off, void *buf, uint32_t len)
{
    if ((area == NULL) || (buf == NULL) || (len == 0u))
    {
        return ERR_ARG;
    }
    if ((off > area->fa_size) || (len > (area->fa_size - off)))
    {
        return ERR_ARG;
    }

    memcpy(buf, (const void *)(area->fa_offset + off), len);
    return ERR_OK;
}

/**
 * @brief: 输出与 area 重叠的 sector 清单（fs_off 为相对 area 起始的偏移）
 */
int flash_get_sectors(const flash_area_t *area, uint32_t max_count,
                      flash_sector_t *sectors, uint32_t *count)
{
    if ((area == NULL) || (sectors == NULL) || (count == NULL))
    {
        return ERR_ARG;
    }

    const uint32_t area_start = area->fa_offset;
    const uint32_t area_end   = area->fa_offset + area->fa_size;
    uint32_t n = 0u;

    for (uint32_t i = 0; i < SECTOR_COUNT; i++)
    {
        const uint32_t s_start = s_sector_layout[i].offset;
        const uint32_t s_end   = s_start + s_sector_layout[i].size;
        if ((s_start >= area_start) && (s_end <= area_end))
        {
            if (n >= max_count)
            {
                return ERR_BUF_TOO_SMALL;
            }
            sectors[n].fs_off  = s_start - area_start;
            sectors[n].fs_size = s_sector_layout[i].size;
            n++;
        }
    }

    *count = n;
    return (n > 0u) ? ERR_OK : ERR_ARG;
}

/* ---- 平台操作表 + 注册入口（在 main 初始化阶段调用一次） ---- */
static const flash_ops_t s_flash_stm32f4_ops =
{
    .open        = flash_open,
    .erase       = flash_erase,
    .write       = flash_write,
    .read        = flash_read,
    .get_sectors = flash_get_sectors,
};

int flash_stm32f4_init(void)
{
    int ret = flash_ops_register(&s_flash_stm32f4_ops);
    if (ret != 0)
    {
        return ret;
    }
    return ota_state_flash_register(); /* 状态后端(内置 flash 版), boot/app 两侧各调一次 */
}
