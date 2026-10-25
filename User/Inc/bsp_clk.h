/**
 *******************************************************************************
 * @file  bsp_clk.h
 * @brief 系统时钟（16 MHz 晶振 → 200 MHz）与 1 ms 裸机时基
 *
 * 目标时钟树（逆向原机 JBC.bin，并用 J-Link 实机回读核对过）：
 *   XTAL 16 MHz --/2--> 8 MHz --x50--> VCO 400 MHz --/2--> HCLK = 200 MHz
 *   PCLK1 = 100 MHz（SPI3、TMRA 用），PCLK2/PCLK3 = 50 MHz
 *
 * 这些数字有任何一处改动，SPI 分频、TMRA 周期、ADC 采样和 SysTick 会一起错，
 * 所以下面每一个寄存器常量都必须和原机逐位一致。
 *******************************************************************************
 */
#ifndef __BSP_CLK_H__
#define __BSP_CLK_H__

#include <stdbool.h>
#include <stdint.h>

/** 板上晶振频率；Makefile 的 -DXTAL_VALUE 必须与它相等，否则编译不过 */
#define CLOCK_XTAL_HZ           (16000000UL)
#define CLOCK_HCLK_HZ           (200000000UL)

/** CMU_PLLCFGR = 0x11103101：MPLLM=/2、源=XTAL、MPLLN=49(x50)、R/Q/P 各 /2 */
#define CLOCK_PLLCFGR           (0x11103101UL)

/** CMU_SCFGR 分频域 = 0x00112210：HCLK/1、PCLK0/1、PCLK1/2、PCLK2/4、PCLK3/4、PCLK4/2 */
#define CLOCK_SCFGR_DIV         (0x00112210UL)
#define CLOCK_SCFGR_DIV_MASK    (0x07777777UL)

/**
 * EFM_FRMC.FLWT = 5（bit4..7）。
 * 200 MHz 下 Flash 取指必须插 5 个等待周期，且必须在提频之前写入。
 */
#define CLOCK_FLASH_WAIT        (0x00000050UL)

/**
 * SRAM_WTCR = 0x11001111：SRAM1/2、SRAM3、Ret_SRAM 各 1 等待，
 * SRAMH 给 0 等待换零等待访问。200 MHz 下少写一个等待会读到错数据。
 */
#define CLOCK_SRAM_WTCR         (0x11001111UL)

/** 裸机节拍 1 kHz：和原机 FreeRTOS 的 configTICK_RATE_HZ=1000 等价 */
#define TICK_HZ                 (1000UL)

/** 毫秒计数，由 SysTick 中断每 1 ms 加一（定义在 hc32f460_it.c） */
extern volatile uint32_t systick_ms;

/**
 * @brief  把系统时钟切到 200 MHz，并逐项回读确认
 * @retval true 时钟树与原机一致；false 有寄存器没写进去
 * @note   调用前芯片跑在复位默认的 8 MHz 内部时钟上
 */
bool init_clock(void);

/**
 * @brief  启动 1 ms 的 SysTick 节拍
 * @param  rate_hz 节拍频率，只允许 1…1000 Hz
 * @retval true 节拍已启动；false 参数越界或 LOAD 超出 24 位
 */
bool init_tick(uint32_t rate_hz);

/** @brief 取开机以来的毫秒数（溢出后按无符号减法比较，见 task_app） */
uint32_t get_tick_ms(void);

/** @brief 忙等若干毫秒；只在初始化阶段用，别在加热控制里调用 */
void wait_ms(uint32_t ms);

/** @brief 软复位整片 MCU；设置写坏或用户主动重置时用 */
void reset_mcu(void);

#endif /* __BSP_CLK_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
