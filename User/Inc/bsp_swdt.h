/**
 *******************************************************************************
 * @file  bsp_swdt.h
 * @brief 专用看门狗（SWDT）+ 调试暂停配置
 *
 * === 为什么需要这个模块 ===
 *
 * 2026-11-09 事故：烧录 v9.33 完成瞬间烙铁烧红（显示 999℃）。
 * 根因是 pyocd 连接目标时 **halt 内核**，而加热关断依赖 TMRA6 溢出中断里的
 * set_heat_pin(false)。内核停 → 中断不执行 → PA10 冻结在最后电平。
 * duty=80 时脉冲内高电平占 31.2%，一旦冻结为高就是 100% 持续导通。
 *
 * **软件保护在内核停转时全部失效。** 唯一能兜底的是让硬件强制复位：
 *   CPU 停 → 不再喂狗 → SWDT 超时 → 芯片复位
 *   → 复位后 init_heater() 把 PA10 拉低 → 加热停止
 *
 * === 为什么用 SWDT 而不是 WDT ===
 *
 * SWDT 有**独立的 10 kHz 内部低速振荡器（SWDTLRC）**，
 * 不依赖 PCLK3/系统时钟。即使时钟树配错、系统时钟停摆，SWDT 仍在计数。
 * WDT 用 PCLK3 分频，主时钟出问题时它也跟着失效。
 *
 * === 调试时怎么办（关键） ===
 *
 * 看门狗会让调试变成噩梦：每次 halt 都会被复位。
 * HC32F460 的 DBGC 提供了 **MCUSTPCTL（外设调试暂停寄存器）**，
 * bit0 = SWDTSTP：置 1 后内核停止时 SWDT 计数器**自动暂停**。
 *
 * 手册 36.5.2 记载该寄存器**复位值 0x0000_0003**
 * （bit0 SWDTSTP=1、bit1 WDTSTP=1），也就是**默认就是暂停的**。
 * 所以这里显式写一次，把这个保证固化到代码里，
 * 不依赖"复位值恰好如此"这种隐性前提。
 *******************************************************************************
 */
#ifndef BSP_SWDT_H
#define BSP_SWDT_H

#include <stdbool.h>
#include <stdint.h>

/* ---- SWDT 在"内核 halt"时是否暂停（安全 vs 调试的取舍） ----
 *
 * 这是一个**根本性的二选一**，两种诉求互斥：
 *
 *   SWDT_FREEZE_ON_HALT = 1（调试优先，当前默认）
 *     · halt 时 SWDT 暂停 → 断点/单步正常可用
 *     · 代价：调试器停住内核期间，若加热仍在导通，看门狗不兜底
 *     · 该场景由"烧录前强制 PA10 低电平"覆盖（tools/safe_heat_pin.py），
 *       所以实际风险可控
 *
 *   SWDT_FREEZE_ON_HALT = 0（安全优先，建议量产时采用）
 *     · halt 时 SWDT 继续计数 → 内核停转 819 ms 后强制复位
 *     · 复位后 init_heater() 立刻拉低 PA10，加热停止
 *     · 代价：调试器一 halt 就被复位，无法打断点/单步
 *
 * **量产固件必须改成 0。** 改动方式：
 *   在 Makefile 的 C_DEFS 里加 -DSWDT_FREEZE_ON_HALT=0，
 *   或直接修改下面的默认值。 */
#ifndef SWDT_FREEZE_ON_HALT
#define SWDT_FREEZE_ON_HALT         (1)
#endif

/**
 * @brief  喂狗（刷新 SWDT 计数器）
 * @note   必须由主循环周期调用。本工程周期最慢的分支是 UI 刷新 10 ms，
 *         而 SWDT 超时约 819 ms，余量充足（约 80 倍）。
 *         但**绝不能放在中断里**——那样即使主循环卡死，中断还在喂狗，
 *         看门狗就形同虚设。
 */
void feed_swdt(void);

/**
 * @brief  配置 DBGC：让 SWDT 在内核 halt 时暂停计数
 * @note   必须在调试器可能 halt 之前调用（init_app 早期）。
 *         不配置的话，每次调试 halt 都会被 SWDT 复位。
 */
void init_debug_freeze(void);

#endif /* BSP_SWDT_H */
