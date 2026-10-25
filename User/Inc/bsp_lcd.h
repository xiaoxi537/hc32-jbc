/**
 *******************************************************************************
 * @file  bsp_lcd.h
 * @brief ST7789V2 显示屏（SPI3 + DMA1 + 条带缓冲）
 *
 * 硬件连接（原理图与原机固件实测一致）:
 *   TFT_CS   PA15   GPIO 输出，低有效（需先释放 JTDI）
 *   TFT_CLK  PB3    SPI3_SCK  复用功能 0x2B（需先释放 JTDO）
 *   TFT_SDA  PB5    SPI3_MOSI 复用功能 0x28
 *   TFT_A0   PB6    GPIO 输出，0=命令 1=数据
 *   TFT_RST  PB7    GPIO 输出，低有效
 *   TFT_LED  PB9    背光，由 bsp_buzzer_backlight.c 的 TMRA4_CH4 PWM 控制
 *
 * 面板：320×240 横向（MADCTL = 0x70），RGB565。
 *******************************************************************************
 */
#ifndef __BSP_LCD_H__
#define __BSP_LCD_H__

#include <stdint.h>

/* 1 = 横屏 320×240（MADCTL=0x70）；0 = 竖屏 240×320（MADCTL=0x00） */
#ifndef LCD_ROTATE_LANDSCAPE
#define LCD_ROTATE_LANDSCAPE    (1)
#endif

#if (LCD_ROTATE_LANDSCAPE != 0)
#define LCD_WIDTH               (320U)
#define LCD_HEIGHT              (240U)
#define LCD_MADCTL              (0x70U)
#else
#define LCD_WIDTH               (240U)
#define LCD_HEIGHT              (320U)
#define LCD_MADCTL              (0x00U)
#endif

/**
 * SPI3 分频（基准 PCLK1 = 100 MHz）：
 *   DIV64→1.5625 MHz、DIV32→3.125 MHz、DIV16→6.25 MHz、
 *   DIV8→12.5 MHz、DIV4→25 MHz（已实测稳定，当前默认）
 *
 * 25 MHz 档必须走 DMA：CPU 逐字节等 TDEF 会把 SPI 饿死，实测表现为全黑。
 */
#ifndef LCD_SPI_DIV
#define LCD_SPI_DIV             SPI_BR_CLK_DIV4
#endif

/**
 * @brief  初始化显示屏：GPIO → SPI3 → DMA1 → 硬件复位 → 命令序列 → 清屏
 * @note   背光不在这里打开，由调用方按用户亮度设置单独控制。
 *         本函数没有回读手段，屏不亮就是接线/供电问题，软件无法判断。
 */
void init_lcd(void);

/**
 * @brief  把一块 RGB565 像素送到屏上
 * @param  pixels 源像素，**高字节在前**（SGL 开了 COLOR16_SWAP 后就是这个顺序）
 * @param  w,h    区域尺寸；调用方保证 w*h*2 ≤ 65535（DMA 传输计数只有 16 位）
 */
void flush_lcd_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    const uint16_t *pixels);

/** 全屏填一种颜色（RGB565） */
void clear_lcd(uint16_t color);

#endif /* __BSP_LCD_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
