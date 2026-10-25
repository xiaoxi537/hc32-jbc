/**
 *******************************************************************************
 * @file  sgl_config.h
 * @brief SGL 图形库在本工程的配置（放在 User/Inc，优先于 sgl/source/sgl_config.h）
 *
 * 关键项说明：
 *   CONFIG_SGL_COLOR16_SWAP = 1
 *     ST7789V2 的 RGB565 是 MSB 先出，而 SGL 缓冲区是 MCU 小端 uint16。
 *     置 1 后 SGL 会在调用 flush_area 之前就地交换字节，
 *     这样 DMA 可以直接把缓冲区搬到 SPI，无需再做转换。
 *
 *   CONFIG_SGL_FBDEV_PIXEL_DEPTH = 16
 *     与屏一致（COLMOD = 0x05，RGB565），不浪费带宽。
 *
 *   CONFIG_SGL_HEAP_MEMORY_SIZE = 24576（24 KB）
 *     SGL 内部静态堆（sgl_core.c 里的 sgl_mem_pool），不占外部 RAM 分配器。
 *     曾用 10 KB，堆满会覆盖紧邻的 sgl_system（dirty_num/surf）。
 *******************************************************************************
 */
#ifndef __SGL_CONFIG_H__
#define __SGL_CONFIG_H__

#define CONFIG_SGL_FBDEV_PIXEL_DEPTH                       16
#define CONFIG_SGL_COLOR16_SWAP                            1
#define CONFIG_SGL_FBDEV_ROTATION                          0
#define CONFIG_SGL_FBDEV_RUNTIME_ROTATION                  0
#define CONFIG_SGL_USE_FBDEV_VRAM                          0

#define CONFIG_SGL_SYSTICK_MS                              10
#define CONFIG_SGL_EVENT_QUEUE_SIZE                        16
#define CONFIG_SGL_LOG_LEVEL                               0
#define CONFIG_SGL_OBJ_USE_NAME                            0

/* 必须关闭！默认为 1 时 sgl_font_table_t.bitmap_index 是 uint16_t（最大 65535），
 * 中文字库点阵超过 64 KB 后索引会被截断，
 * 表现为"英文正常、中文碎片"。所以这里必须关掉。 */
#define CONFIG_SGL_FONT_SMALL_TABLE                         0

#define CONFIG_SGL_FONT_COMPRESSED                         0
#define CONFIG_SGL_BOOT_LOGO                               0
#define CONFIG_SGL_THEME_DARK                              0

#define CONFIG_SGL_HEAP_ALGO                               lwmem
#define CONFIG_SGL_HEAP_MEMORY_SIZE                        24576

/* 字体：只用 cjk16（GB2312 一级 + ASCII + 符号），不再链接 ASCII 专用字体。
 * app_ui.c 里通过 sgl_set_system_font(&cjk16) 设为系统字体。 */
#define CONFIG_SGL_FONT_SONG23                             0
#define CONFIG_SGL_FONT_CONSOLAS14                         0
#define CONFIG_SGL_FONT_CONSOLAS23                         0
#define CONFIG_SGL_FONT_CONSOLAS24                         0
#define CONFIG_SGL_FONT_CONSOLAS32                         0
#define CONFIG_SGL_FONT_CONSOLAS24_COMPRESS                0
#define CONFIG_SGL_FONT_SGL_ICON_FONTAWESOME14             0

#endif  /* __SGL_CONFIG_H__ */
