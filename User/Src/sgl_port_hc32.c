/**
 *******************************************************************************
 * @file  sgl_port_hc32.c
 * @brief SGL 移植层（HC32F460 + ST7789V2 + SPI3/DMA1）
 *
 * 数据流：
 *   SGL 渲染到行缓冲（320×10 像素）
 *     → flush_area 回调（此处 CONFIG_SGL_COLOR16_SWAP=1，字节序已就地交换）
 *     → flush_lcd_rect() 用 DMA 送屏
 *     → sgl_fbdev_flush_ready() 告诉 SGL 这一块已经真正发出
 *
 * 为什么用行缓冲而不是整屏帧缓冲：
 *   320×240×2 = 153600 B，占 188 KB RAM 的 82%，M4 上不划算。
 *   SGL 支持"分块渲染 + 逐块 flush"，10 行双缓冲只要 12.8 KB。
 *
 * 本文件是全工程**唯一**出现函数指针成员的位置：
 *   sgl_fbinfo_t.flush_area 是第三方库强制的接口，不是本工程的设计。
 *******************************************************************************
 */
#include "sgl_port_hc32.h"
#include "bsp_lcd.h"

#include <sgl.h>

/*******************************************************************************
 * 行缓冲配置
 ******************************************************************************/
#define PORT_LINE_COUNT         (10U)
#define PORT_BUFFER_PIXELS      (LCD_WIDTH * PORT_LINE_COUNT)

/* 两块缓冲乒乓：一块在送屏时，另一块可以继续渲染 */
static sgl_color_t buffer0[PORT_BUFFER_PIXELS];
static sgl_color_t buffer1[PORT_BUFFER_PIXELS];

/*******************************************************************************
 * 第三方回调
 ******************************************************************************/

/**
 * @brief  SGL 把一块矩形交给硬件显示
 * @param  area 区域（x1,y1 左上 / x2,y2 右下，闭区间）
 * @param  pixels RGB565 像素，字节序已由 SGL 换成 MSB 先出
 */
static void flush_area(sgl_area_t *area, sgl_color_t *pixels)
{
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1U);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1U);

    flush_lcd_rect((uint16_t)area->x1, (uint16_t)area->y1, w, h,
                   (const uint16_t *)pixels);

    /* 必须通知 SGL 这块已完成，否则下一块不会开始渲染 */
    sgl_fbdev_flush_ready();
}

/** 日志直接丢弃：本工程没有 printf 通道，调试靠 J-Link 读全局量 */
static void discard_log_text(const char *text)
{
    (void)text;
}

/*******************************************************************************
 * 对外接口
 ******************************************************************************/
bool init_sgl_port(void)
{
    sgl_fbinfo_t info;

    info.xres = (int16_t)LCD_WIDTH;
    info.yres = (int16_t)LCD_HEIGHT;
    info.flush_area = flush_area;
    info.buffer[0] = buffer0;
    info.buffer[1] = buffer1;
    info.buffer_size = PORT_BUFFER_PIXELS;

    sgl_logdev_register(discard_log_text);

    /* 注册失败一般是内存池不够；初始化失败 main 会直接停机，
     * 现场看到的现象就是背光亮、屏幕全黑。 */
    if (0 != sgl_fbdev_register(&info)) {
        return false;
    }
    return 0 == sgl_init();
}

void task_sgl_port(void)
{
    sgl_task_handler();
}

void tick_sgl_port(void)
{
    sgl_tick_inc(1U);
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
