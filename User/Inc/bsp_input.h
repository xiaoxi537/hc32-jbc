/**
 *******************************************************************************
 * @file  bsp_input.h
 * @brief 输入设备：EC11 编码器、按键、手柄开关、在位检测
 *
 * 引脚（与原理图和原机固件一致）:
 *   PA0 = EC11_B, PA1 = EC11_A   正交编码
 *   PA2 = EC11 按下键            低有效
 *   PA3 = JBC_SW                 低有效（手柄座 / 换芯联锁）
 *   PA4 = JBC_SLE                低有效（休眠输入）
 *   PA9 = SLEEP                  高有效（手柄在位，自动关机判据）
 *
 * 编码器解码放在 SysTick 里（tick_encoder）：主循环有时要刷屏或写 Flash，
 * 一停就是毫秒级，放在主循环里扫会漏掉多个边沿，表现为"转得快就丢步"。
 *******************************************************************************
 */
#ifndef __BSP_INPUT_H__
#define __BSP_INPUT_H__

#include <stdbool.h>
#include <stdint.h>

/** 初始化 6 个输入脚（内部上拉）并把编码器状态清零 */
void init_inputs(void);

/** SysTick 每 1 ms 调用：只做正交解码，不碰其它外设 */
void tick_encoder(void);

/**
 * @brief  取走并清零编码器增量
 * @retval >0 正转，<0 反转，0 无动作
 */
int16_t take_encoder_delta(void);

/** 编码器按键（PA2）是否按下 */
bool is_button_down(void);

/** 手柄座 / 换芯联锁（PA3）是否拉低 */
bool is_jbc_sw_low(void);

/** 休眠输入（PA4）是否拉低 */
bool is_jbc_sle_low(void);

/** 手柄是否在位（PA9 高有效） */
bool is_handle_present(void);

#endif /* __BSP_INPUT_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
