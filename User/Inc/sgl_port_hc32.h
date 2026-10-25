/**
 *******************************************************************************
 * @file  sgl_port_hc32.h
 * @brief SGL 图形库在 HC32F460 + ST7789V2 上的移植层
 *
 * 本文件是**唯一**允许出现函数指针成员的位置（SGL 的 flush_area 回调），
 * 详见 docs/架构与编码规范.md §1.3 与 sgl_port_hc32.c 文件头。
 *******************************************************************************
 */
#ifndef __SGL_PORT_HC32_H__
#define __SGL_PORT_HC32_H__

#include <stdbool.h>

/**
 * @brief  注册 SGL 帧缓冲设备并初始化图形库
 * @retval true 成功；false 注册失败（SGL 堆或对象池耗尽）
 * @note   必须在 init_lcd() 之后调用
 */
bool init_sgl_port(void);

/** 主循环反复调用：渲染脏区并通过 LCD 送屏 */
void task_sgl_port(void);

/** SysTick 每 1 ms 调用：推进 SGL 的动画/超时计数 */
void tick_sgl_port(void);

#endif /* __SGL_PORT_HC32_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
