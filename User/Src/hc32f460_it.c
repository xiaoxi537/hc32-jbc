/**
 *******************************************************************************
 * @file  hc32f460_it.c
 * @brief 中断与异常入口
 *
 * 裸机方案下只保留：
 *   - Cortex-M4 内核异常（NMI / HardFault）
 *   - SysTick：1 kHz 系统时基 + 加热 + 编码器 + SGL tick
 * 外设中断在各自 bsp_*.c 里通过 INTC 注册，不在这里列。
 *******************************************************************************
 */
#include "hc32f460_it.h"
#include "app_heat.h"
#include "bsp_clk.h"
#include "bsp_input.h"
#include "sgl_port_hc32.h"

/* 全局毫秒计数；定义在这里，其它文件通过 bsp_clk.h 的 extern 访问 */
volatile uint32_t systick_ms;

/* 内核异常一律停机：此时系统已经不可信，继续跑可能乱开加热 */
void NMI_Handler(void)       { for (;;) { } }
void HardFault_Handler(void) { for (;;) { } }
void SVC_Handler(void)       { }
void PendSV_Handler(void)    { }

void SysTick_Handler(void)
{
    systick_ms++;

    /* 加热放最前面：脉冲计数、5 ms 测温窗口和 PID 都不能被刷屏拖慢 */
    tick_heat();

    /* 编码器必须在中断里扫，主循环写 Flash 停十几毫秒也不会丢步 */
    tick_encoder();

    /* SGL 只要一个毫秒计数，真正的渲染仍在主循环 */
    tick_sgl_port();
}

/* 链接用了 -nostartfiles，但 newlib 的启动代码仍会引用这两个符号 */
void _init(void) { }
void _fini(void) { }

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
