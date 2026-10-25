/**
 *******************************************************************************
 * @file  bsp_buzzer_backlight.h
 * @brief 蜂鸣器（PB8 / TMRA4_CH3）与背光（PB9 / TMRA4_CH4）
 *
 * TMRA4：PCLK1 100 MHz / 32 = 3.125 MHz，PERAR=1000 → ≈3.12 kHz，
 * 落在 MLT-8530 蜂鸣器的谐振区附近，同样电压下最响。
 *******************************************************************************
 */
#ifndef __BSP_BUZZER_BACKLIGHT_H__
#define __BSP_BUZZER_BACKLIGHT_H__

#include <stdint.h>

/** 初始化 TMRA4 两路 PWM；背光默认全亮 */
void init_buzzer_backlight(void);

/**
 * @brief  响指定时长
 * @param  ms 10…500 ms；短鸣 30 ms，上电提示 200 ms
 * @note   只负责打开输出和记截止时间，关断由 task_buzzer 在 1 ms 节拍做
 */
void beep_buzzer(uint16_t ms);

/** 1 ms 节拍调用：到时间关掉蜂鸣输出 */
void task_buzzer(void);

/** 背光亮度 0…10（0 = 灭，10 = 100%） */
void set_backlight(uint8_t level);

#endif /* __BSP_BUZZER_BACKLIGHT_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
