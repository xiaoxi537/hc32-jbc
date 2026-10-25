/**
 *******************************************************************************
 * @file  bsp_settings.c
 * @brief 内部 Flash 设置区读写（EFM sector 31，地址 0x0003E000）
 *
 * 为什么放在最后一个扇区：固件长到 76 KB 也碰不到它，
 * 而且擦除一个扇区只要几毫秒，不必把整片 Flash 停下来。
 *******************************************************************************
 */
#include "bsp_settings.h"
#include "bsp_heater.h"
#include "hc32_ll.h"

#include <string.h>

#define SETTINGS_FLASH_ADDR     EFM_SECTOR_ADDR(31U)

void read_settings(void *buffer, uint32_t size)
{
    (void)memcpy(buffer, (const void *)SETTINGS_FLASH_ADDR, size);
}

bool write_settings(const void *buffer, uint32_t size)
{
    const uint32_t *words = (const uint32_t *)buffer;
    uint32_t word_count;
    uint32_t i;
    uint32_t primask;

    /* EFM 只能按 32 位字编程；长度不是 4 的倍数说明上层传错了，直接拒绝 */
    if (0UL != (size % 4UL)) {
        return false;
    }
    word_count = size / 4UL;

    /* Flash 擦写期间 CPU 取指会停顿十几毫秒，
     * 先关加热再关中断，避免 PA10 在这段时间里一直保持高电平。 */
    stop_heat_output();
    primask = __get_PRIMASK();
    __disable_irq();

    EFM_REG_Unlock();
    EFM_FWMC_Cmd(ENABLE);

    /* 等 Flash 状态机空闲。真卡住说明 EFM 坏了，此时加热已关，
     * 死等比写进去半截数据更安全。 */
    while (SET != EFM_GetStatus(EFM_FLAG_RDY)) {
    }

    bool ok = (LL_OK == EFM_SectorErase(SETTINGS_FLASH_ADDR));

    /* 擦除失败就没必要再编程，否则写进去的是一堆旧数据碎片 */
    for (i = 0UL; ok && (i < word_count); i++) {
        /* 任何一个字写失败就停，避免写进去半截设置 */
        if (LL_OK != EFM_ProgramWord(SETTINGS_FLASH_ADDR + (i * 4UL), words[i])) {
            ok = false;
        }
    }

    EFM_FWMC_Cmd(DISABLE);
    EFM_REG_Lock();

    /* 只有原本开着中断才恢复，别把别人关的中断打开 */
    if (0UL == primask) {
        __enable_irq();
    }

    return ok;
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
