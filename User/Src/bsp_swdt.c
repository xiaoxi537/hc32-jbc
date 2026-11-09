/**
 *******************************************************************************
 * @file  bsp_swdt.c
 * @brief 专用看门狗喂狗 + DBGC 调试暂停配置
 *
 * 配置依据：《HC32F460_F45x_A460 系列参考手册》Rev1.71
 *   §24   看门狗计数器（WDT/SWDT）
 *   §36.5.2 外设调试暂停寄存器（MCUSTPCTL）
 *******************************************************************************
 */
#include "bsp_swdt.h"
#include "hc32_ll.h"

/* ---- DBGC 外设调试暂停寄存器（MCUSTPCTL） ----
 *
 * 手册 §36.5.2。位定义与寄存器结构由 DDL 提供：
 *   CM_DBGC->MCUSTPCTL
 *   DBGC_MCUSTPCTL_SWDTSTP (bit0)
 *   DBGC_MCUSTPCTL_WDTSTP  (bit1)
 *
 * bit0 SWDTSTP：0=内核停止时 SWDT 仍计数；1=内核停止时 SWDT 暂停
 * bit1 WDTSTP ：同上，对应 WDT
 * 复位值 0x0000_0003（两位默认都是 1）
 *
 * 这里显式写回同样的值，目的不是改变行为，而是把
 * "调试 halt 时看门狗必须暂停" 这个约定固化在代码里，
 * 避免将来有人误改 ICG 或换芯片后破坏它。 */

void init_debug_freeze(void)
{
    /* 让 SWDT 和 WDT 都在内核 halt 时暂停。
     *
     * 为什么这件事很关键：调试器（pyocd/J-Link）连接目标会立刻 halt 内核。
     * 如果 SWDT 在 halt 期间继续计数，约 819 ms 后芯片就被复位，
     * 断点根本停不住、单步也走不了，调试完全不可用。
     *
     * 注意：这里只影响"内核停止（halt）"这一种情形。
     * 如果主循环卡死但内核仍在跑（死循环），SWDT 照常超时复位——
     * 那正是我们想要的保护行为，两者不冲突。 */
#if (SWDT_FREEZE_ON_HALT != 0)
    CM_DBGC->MCUSTPCTL |= (DBGC_MCUSTPCTL_SWDTSTP | DBGC_MCUSTPCTL_WDTSTP);
    /* 调试优先：halt 时暂停，断点/单步可用。
     * 代价：内核被调试器停住期间若加热仍在导通，SWDT 不会兜底。
     * 该场景由"烧录前强制 PA10 低电平"（tools/safe_heat_pin.py）覆盖。 */
#else
    /* 安全优先（量产）：halt 时也不暂停，内核停转 819 ms 后强制复位。
     * 代价：调试器一 halt 就被复位，无法打断点。 */
    CM_DBGC->MCUSTPCTL &= (uint32_t)~(DBGC_MCUSTPCTL_SWDTSTP | DBGC_MCUSTPCTL_WDTSTP);
#endif
}

void feed_swdt(void)
{
    /* DDL 内部按手册 §24.2.4 的时序写 0x0123 → 0x3210 完成一次刷新。
     * 手册明确说明两次写之间访问其他寄存器不影响刷新，
     * 所以不用关中断，也不需要临界区。 */
    SWDT_FeedDog();
}
