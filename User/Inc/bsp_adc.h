/**
 *******************************************************************************
 * @file  bsp_adc.h
 * @brief ADC1 + DMA2 采样：烙铁头 ID、加热电流、热电偶、输入电压
 *
 * 通道映射（与原机一致）:
 *   ADC1_CH5 = PA5 = GET_ID        烙铁头 ID
 *   ADC1_CH6 = PA6 = ADC_CURRENT   加热电流
 *   ADC1_CH7 = PA7 = T12_AD        热电偶
 *   ADC1_CH8 = PB0 = PWR_ADC       输入电压
 *
 * 工作方式：序列 A 连续扫描 ch5→ch8，AOS 事件 448 触发 DMA2_CH0，
 * 每次块完成（4 个样本）产生 DMA2_BTC0 中断。
 *
 * 采样必须分相位：加热时测电流/电压，断电窗口才测热电偶。
 * 加热丝通电时热电偶上叠加的是几十伏的开关噪声，测出来完全没有意义。
 *******************************************************************************
 */
#ifndef __BSP_ADC_H__
#define __BSP_ADC_H__

#include <stdbool.h>
#include <stdint.h>

/** 采样相位，由加热状态机切换 */
#define ADC_PHASE_IDLE      (0U)
#define ADC_PHASE_HEAT      (1U)
#define ADC_PHASE_MEASURE   (2U)

/** 最近一次 DMA 块：[0]=ID [1]=电流 [2]=热电偶 [3]=VIN（调试用） */
extern volatile uint16_t adc_raw[4];

/** DMA 块完成次数（调试用，可确认采样链有没有停） */
extern volatile uint32_t adc_isr_count;

/** 加热相位检测到硬件过流；重新识别烙铁头后才清零 */
extern volatile bool adc_over_current;

/** 初始化 ADC1 / DMA2 / AOS / 中断，并启动连续扫描 */
void init_adc(void);

/** 切换采样相位；只允许传上面三个宏 */
void set_adc_phase(uint8_t phase);

/** 读烙铁头 ID 原始值（ch5） */
uint16_t read_tip_id(void);

/** 读 10 点热电偶原始平均（ch7） */
uint16_t read_temp_raw_avg(void);

/**
 * @brief  取走并清零加热相位的电流/电压累加值
 * @param  current_raw 平均电流原始值（ch6）
 * @param  vin_raw     平均输入电压原始值（ch8）
 * @note   取走后累加器归零，下一轮从一个空窗口重新开始
 */
void read_heat_averages(uint16_t *current_raw, uint16_t *vin_raw);

#endif /* __BSP_ADC_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
