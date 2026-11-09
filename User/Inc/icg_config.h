/**
 *******************************************************************************
 * @file  icg_config.h
 * @brief ICG（初始化配置寄存器）覆盖值 —— 启用 SWDT 看门狗
 *
 * === 为什么需要这个文件 ===
 *
 * HC32F460 复位后硬件会读 Flash 0x0000_0400 起的 ICG0/ICG1，
 * 决定看门狗、BOR、NMI 等"必须在应用启动前就生效"的配置。
 *
 * DDL 默认（hc32_ll_icg.h）的 ICG0 是 0xFFDFFFBF，其中：
 *   SWDTAUTS = 1  → **复位后 SWDT 停止**（看门狗没启用）
 *
 * 本工程原来就吃这个默认值，所以事故发生时没有任何硬件兜底。
 * 这里把它改成 **复位后自动启动**，超时约 819 ms。
 *
 * === 为什么通过宏覆盖而不是直接改 hc32_ll_icg.h ===
 *
 * hc32_ll_icg.h 是厂商 DDL（`Library/` 下，只读引用）。改它会在
 * 升级 DDL 时被覆盖，也不符合"厂商库不动"的既有约定。
 * DDL 已经预留了 `#ifndef ICG_REG_CFG0_CONST` 的保护，
 * 所以只要在编译时先定义这个宏即可。
 *
 * === 具体参数 ===
 *
 * ICG0 低 16 位是 SWDT 配置区：
 *
 *   bit12     SWDTSLPOFF = 1   sleep/stop 模式下暂停计数
 *   bit11-8   SWDTWDPT   = 15  刷新窗口 0%~100%（不限制刷新时机）
 *   bit7-4    SWDTCKS    = 5   SWDTCLK/32
 *   bit3-2    SWDTPERI   = 0   256 周期
 *   bit1      SWDTITS    = 1   溢出请求**复位**（不是中断）
 *   bit0      SWDTAUTS   = 0   **复位后自动启动**
 *
 * 超时 = 256 × 32 ÷ 10000 Hz = **819.2 ms**
 *
 * 为什么取 819 ms：
 *   · 本工程主循环最慢分支是 UI 10 ms、加热周期 30.34 ms，
 *     819 ms 留了 20 倍以上余量，正常运行时绝不误复位；
 *   · 又足够短：真失控时（内核停、加热持续导通）819 ms 内就会复位，
 *     复位后 init_heater() 立刻把 PA10 拉低，不会烧红烙铁头。
 *   · 对比事故：从 350℃ 满功率烧红需要数秒，819 ms 完全来得及。
 *
 * 为什么 SWDTWDPT 设为 15（0%~100% 不限制）：
 *   窗口式看门狗要求"只能在计数值处于某个区间时刷新"，刷早了会被判
 *   刷新错误并复位。本工程喂狗点分布在不同任务分支里，时间抖动较大，
 *   用严格窗口容易误触发。先把主保护（超时复位）做扎实，
 *   窗口限制留待将来需要检测"喂狗过快"时再收紧。
 *
 * === 调试时会怎样 ===
 *
 * 内核 halt 时 SWDT 是否暂停由 DBGC 的 MCUSTPCTL.SWDTSTP 决定，
 * 见 bsp_swdt.c:init_debug_freeze()。该位复位值就是 1（暂停），
 * 所以用调试器打断点/单步不会触发看门狗复位。
 *******************************************************************************
 */
#ifndef ICG_CONFIG_H
#define ICG_CONFIG_H

/* ICG0：保留原有 WDT/BOR 等高位，只改写 SWDT 段
 *
 * 原值 0xFFDFFFBF → 新值 0xFFDF1F52
 *
 * 高位 0xFFDF____ 是 WDT 配置（WDTSLPOFF=1、WDTWDPT=15、
 * WDTCKS=11、WDTPERI=3、WDTITS=1、WDTAUTS=1 → WDT 不自动启动），
 * 保持不动：本工程用 SWDT 而不是 WDT，没必要启用两个看门狗。 */
#define ICG_REG_CFG0_CONST      (0xFFDF1F52UL)

#endif /* ICG_CONFIG_H */
