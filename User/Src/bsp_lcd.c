/**
 *******************************************************************************
 * @file  bsp_lcd.c
 * @brief ST7789V2 驱动：SPI3 + DMA1 + 320×30 条带缓冲
 *
 * 架构对齐原机 JBC.bin（见 analysis/ui_display.md §2.4 / §3.1）:
 *   - SPI3 主机、3 线制、只发送、8 bit、Mode 0
 *   - 像素由 DMA1 通道 0 搬运，AOS 触发源 = INT_SRC_SPI3_SPTI
 *   - 320×30 RGB565 条带缓冲（19200 B），整屏 240 行分 8 段送
 *
 * 本文件是全工程唯一直接操作 LCD 相关寄存器的位置。
 *******************************************************************************
 */
#include "bsp_lcd.h"
#include "bsp_clk.h"
#include "hc32_ll.h"

/*******************************************************************************
 * 引脚定义
 ******************************************************************************/
#define LCD_CS_PORT         GPIO_PORT_A
#define LCD_CS_PIN          GPIO_PIN_15
#define LCD_DC_PORT         GPIO_PORT_B
#define LCD_DC_PIN          GPIO_PIN_06
#define LCD_RST_PORT        GPIO_PORT_B
#define LCD_RST_PIN         GPIO_PIN_07

/* 这几个宏会被频繁调用，写成 static inline 有类型检查，也比宏更好调试 */
static inline void set_cs_low(void)   { GPIO_ResetPins(LCD_CS_PORT, LCD_CS_PIN); }
static inline void set_cs_high(void)  { GPIO_SetPins(LCD_CS_PORT, LCD_CS_PIN); }
static inline void set_dc_command(void) { GPIO_ResetPins(LCD_DC_PORT, LCD_DC_PIN); }
static inline void set_dc_data(void)  { GPIO_SetPins(LCD_DC_PORT, LCD_DC_PIN); }
static inline void set_rst_low(void)  { GPIO_ResetPins(LCD_RST_PORT, LCD_RST_PIN); }
static inline void set_rst_high(void) { GPIO_SetPins(LCD_RST_PORT, LCD_RST_PIN); }

/** SPI3 速率：8 bit + 分频档位（见 bsp_lcd.h） */
#define LCD_SPI_CFG2        (SPI_DATA_SIZE_8BIT | LCD_SPI_DIV)

/** 条带 30 行：320×30×2 = 19200 B，既能放进 DMA 16 位计数，也不浪费 RAM */
#define LCD_BAND_ROWS       (30U)
#define LCD_BAND_PIXELS     (LCD_WIDTH * LCD_BAND_ROWS)
#define LCD_BAND_BYTES      (LCD_BAND_PIXELS * 2U)

static uint8_t band_buffer[LCD_BAND_BYTES];

/* 全工程只留 3 条编译期断言，这是其中一条：
 * DMA 传输计数只有 16 位，条带一旦超了会被截断并且通道挂起（屏幕卡死）。 */
_Static_assert(LCD_BAND_BYTES <= 0xFFFFU,
               "band size must fit DMA 16-bit transfer count");

/*******************************************************************************
 * 初始化命令表（照抄原机 ST7789V2 序列）
 *
 * 二维常量表，每行 = [命令, 参数个数, 参数 0..13]。
 * 不用结构体数组、不用指针表：行数由 sizeof 自动得到，加一条命令不用改两处。
 ******************************************************************************/
#define LCD_INIT_PARAM_MAX  (14U)
#define LCD_INIT_COL_CMD    (0U)
#define LCD_INIT_COL_LEN    (1U)
#define LCD_INIT_COL_PARAM  (2U)

static const uint8_t lcd_init_sequence[][LCD_INIT_COL_PARAM + LCD_INIT_PARAM_MAX] = {
    /* cmd, len, 参数... */
    {0x36U,  1U, 0x00U},                                          /* MADCTL 初值 */
    {0x3AU,  1U, 0x05U},                                          /* COLMOD 16bpp */
    {0xB2U,  5U, 0x0CU, 0x0CU, 0x00U, 0x33U, 0x33U},              /* PORCTRL */
    {0xB7U,  1U, 0x35U},                                          /* GCTRL */
    {0xBBU,  1U, 0x32U},                                          /* VCOMS */
    {0xC2U,  1U, 0x01U},                                          /* VDVVRHEN */
    {0xC3U,  1U, 0x15U},                                          /* VRHS */
    {0xC4U,  1U, 0x20U},                                          /* VDVS */
    {0xC6U,  1U, 0x0FU},                                          /* FRCTRL2 */
    {0xD0U,  2U, 0xA4U, 0xA1U},                                   /* PWCTRL1 */
    {0x21U,  0U},                                                 /* INVON（面板需反显）*/
    // 手动更改了 0xE0 和 0xE1 校正显示颜色
    {0xE0U, 14U, 0xD0U, 0x04U, 0x0DU, 0x11U, 0x13U, 0x2BU, 
                 0x3FU, 0x54U, 0x4CU, 0x18U, 0x0DU, 0x0BU, 0x1FU, 0x23U}, /* PVGAMCTRL */
    {0xE1U, 14U, 0xD0U, 0x04U, 0x0CU, 0x11U, 0x13U, 0x2CU, 
                 0x3FU, 0x44U, 0x51U, 0x2FU, 0x1FU, 0x1FU, 0x20U, 0x23U}, /* NVGAMCTRL */
    {0x11U,  0U},                                                 /* SLPOUT */
    {0x29U,  0U},                                                 /* DISPON */
};

/*******************************************************************************
 * SPI 基础收发（CPU 发命令与少量参数）
 ******************************************************************************/

/**
 * @brief  等最后一个字节真正移出移位寄存器
 * @note   25 MHz 下一个字节只有 320 ns，用毫秒延时会白等；
 *         这个循环约 20~40 µs，是单字节时间的 60~100 倍，足够排空。
 */
static void wait_shift_drain(void)
{
    volatile uint32_t i;
    /* 空转约 20 µs；用 volatile 防止被优化掉 */
    for (i = 0UL; i < 2000UL; i++) {
    }
}

/** 等发送缓冲空；SPI 没使能时最多等一会儿就走，避免整机卡死 */
static void wait_spi_txe(void)
{
    uint32_t guard = 0x100000UL;

    /* TDEF=1 表示发送缓冲空；SPI 没使能时超时返回，避免整机卡死 */
    while (0UL == READ_REG32_BIT(CM_SPI3->SR, SPI_SR_TDEF)) {
        if (0UL == (--guard)) {
            return;
        }
    }
}

/**
 * @brief 等发送缓冲空且移位器空闲（抬 CS 前必须）
 * @note  RM §SPI：IDLNF=1 表示"正在传送"，=0 才是空闲。
 *        原实现等 IDLNF 置 1，实际是在等"忙"，导致每次 flush 白等约 50 ms，
 *        现象是屏幕从上到下极慢地刷新。
 */
static void wait_spi_idle(void)
{
    uint32_t guard = 0x400000UL;

    wait_spi_txe();
    /* IDLNF=0 才是空闲；等超时也要往下走，宁可少一帧也别死机 */
    while ((SET == READ_REG32_BIT(CM_SPI3->SR, SPI_SR_IDLNF)) &&
           (0UL != (--guard))) {
    }
}

static void send_spi_bytes(const uint8_t *buffer, uint32_t length)
{
    uint32_t i;

    /* 逐字节等发送缓冲空，命令和少量参数走 CPU 足够快 */
    for (i = 0UL; i < length; i++) {
        wait_spi_txe();
        WRITE_REG32(CM_SPI3->DR, buffer[i]);
    }

    wait_spi_idle();
    wait_shift_drain();
}

static void write_lcd_command(uint8_t command)
{
    set_dc_command();
    set_cs_low();
    send_spi_bytes(&command, 1UL);
    set_cs_high();
}

static void write_lcd_data(const uint8_t *data, uint32_t length)
{
    /* 命令表里有 0 参数的命令，长度为 0 时连 CS 都不用动 */
    if (0UL == length) {
        return;
    }
    set_dc_data();
    set_cs_low();
    send_spi_bytes(data, length);
    set_cs_high();
}

/*******************************************************************************
 * DMA1 通道 0 搬运像素
 ******************************************************************************/

/**
 * @brief  用 DMA 把 length 字节从缓冲搬到 SPI3_DR
 *
 * 严格按原机 lcd_flush_rect 的时序：
 *   等空闲 → 停 SPI → 清标志 → 停通道 → 写 SAR/DAR/DTCTL
 *   → 开通道 → **再开 SPI**（靠这一下把 SPTI 触发起来）
 *
 * 注意：
 *  * 块大小在 DMA_Init 里固定为 1，每条带都不改 DTCTL；
 *    BLKSIZE 只有 10 位，写成总长度会被截断并导致通道挂起。
 *  * 抬 CS 前不只等 IDLNF，再补一段固定延时，防止最后一字节被截断。
 *  * 收尾**只停 DMA，不停 SPI**：下一条带的 CASET/RASET/RAMWR 还要靠 CPU
 *    通过 SPI3 发出去，SPI 停了命令就发不出去（表现为顶部错位）。
 */
static void send_pixels_dma(const uint8_t *buffer, uint16_t length)
{
    uint32_t guard;

    wait_spi_idle();

    /* 停 SPI：重新配置 DMA 期间不会产生新的 SPTI */
    CM_SPI3->CR1 &= (uint32_t)(~SPI_CR1_SPE);

    (void)DMA_ChCmd(CM_DMA1, DMA_CH0, DISABLE);
    DMA_ClearTransCompleteStatus(CM_DMA1, DMA_FLAG_TC_CH0);
    DMA_ClearErrStatus(CM_DMA1, DMA_FLAG_REQ_ERR_CH0 | DMA_FLAG_TRANS_ERR_CH0);

    /* 源地址每次送屏都变，目标固定是 SPI3 数据寄存器 */
    (void)DMA_SetSrcAddr(CM_DMA1, DMA_CH0, (uint32_t)buffer);
    (void)DMA_SetDestAddr(CM_DMA1, DMA_CH0, (uint32_t)(&CM_SPI3->DR));
    (void)DMA_SetTransCount(CM_DMA1, DMA_CH0, length);

    (void)DMA_ChCmd(CM_DMA1, DMA_CH0, ENABLE);

    /* 开 SPI 的这一下才会产生 SPI3_SPTI，DMA 由此开始搬 */
    CM_SPI3->CR1 |= SPI_CR1_SPE;

    guard = 0x8000000UL;
    /* 等传输计数到 0；超时说明通道异常，继续往下也比死机好排查 */
    while ((RESET == DMA_GetTransCompleteStatus(CM_DMA1, DMA_FLAG_TC_CH0)) &&
           (0UL != (--guard))) {
    }

    /* TC 只说明计数到 0，最后一个字节可能还在移位寄存器里 */
    wait_spi_idle();
    wait_shift_drain();

    (void)DMA_ChCmd(CM_DMA1, DMA_CH0, DISABLE);
}

/*******************************************************************************
 * 窗口设置 / 矩形送屏
 ******************************************************************************/
static void set_lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t params[4];

    /* CASET/RASET 的参数都是 16 位大端 */
    params[0] = (uint8_t)(x0 >> 8);
    params[1] = (uint8_t)(x0 & 0xFFU);
    params[2] = (uint8_t)(x1 >> 8);
    params[3] = (uint8_t)(x1 & 0xFFU);
    write_lcd_command(0x2AU);
    write_lcd_data(params, 4UL);

    params[0] = (uint8_t)(y0 >> 8);
    params[1] = (uint8_t)(y0 & 0xFFU);
    params[2] = (uint8_t)(y1 >> 8);
    params[3] = (uint8_t)(y1 & 0xFFU);
    write_lcd_command(0x2BU);
    write_lcd_data(params, 4UL);

    /* RAMWR 之后的数据就整块往这个窗口里填 */
    write_lcd_command(0x2CU);
}

/** 内部用：送一块字节数据（调用方保证长度不超过 65535） */
static void flush_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t *source)
{
    set_lcd_window(x, y,
                   (uint16_t)(x + w - 1U),
                   (uint16_t)(y + h - 1U));

    set_dc_data();
    set_cs_low();
    send_pixels_dma(source, (uint16_t)(w * h * 2U));
    wait_spi_txe();
    set_cs_high();
}

static void flush_band(uint16_t y0)
{
    flush_area(0U, y0, (uint16_t)LCD_WIDTH, (uint16_t)LCD_BAND_ROWS, band_buffer);
}

/** 把条带缓冲的第 0 行复制到其余各行（纯色填充用） */
static void fill_band_rows(void)
{
    uint32_t row;
    uint32_t byte;

    /* 第 0 行已经铺好颜色，整块复制到其余 29 行 */
    for (row = 1UL; row < LCD_BAND_ROWS; row++) {
        /* 每行 320 像素 × 2 字节 */
        for (byte = 0UL; byte < (LCD_WIDTH * 2UL); byte++) {
            band_buffer[(row * LCD_WIDTH * 2UL) + byte] = band_buffer[byte];
        }
    }
}

/*******************************************************************************
 * 初始化
 ******************************************************************************/
static void init_lcd_gpio(void)
{
    stc_gpio_init_t gpio_init;

    GPIO_REG_Unlock();

    /* 释放 PA15(JTDI) 与 PB3(JTDO)，SWD 的 SWCLK/SWDIO 仍然可用 */
    CM_GPIO->PSPCR &= (uint16_t)(~(uint16_t)(0x0008U | 0x0004U));

    (void)GPIO_StructInit(&gpio_init);
    gpio_init.u16PinState = PIN_SET;
    gpio_init.u16PinDir = PIN_DIR_OUT;
    gpio_init.u16PinOutputType = PIN_OUT_TYPE_CMOS;
    gpio_init.u16PinDrv = PIN_HIGH_DRV;
    gpio_init.u16Latch = PIN_LATCH_OFF;
    gpio_init.u16PullUp = PIN_PU_OFF;
    gpio_init.u16Invert = PIN_INVT_OFF;
    gpio_init.u16ExtInt = PIN_EXTINT_OFF;
    gpio_init.u16PinAttr = PIN_ATTR_DIGITAL;
    (void)GPIO_Init(LCD_CS_PORT, LCD_CS_PIN, &gpio_init);
    (void)GPIO_Init(LCD_DC_PORT, LCD_DC_PIN, &gpio_init);
    (void)GPIO_Init(LCD_RST_PORT, LCD_RST_PIN, &gpio_init);

    /* PB3 → SPI3_SCK（功能 0x2B），PB5 → SPI3_MOSI（功能 0x28） */
    GPIO_SetFunc(GPIO_PORT_B, GPIO_PIN_03, 0x2BU);
    GPIO_SetFunc(GPIO_PORT_B, GPIO_PIN_05, 0x28U);
}

static void init_lcd_spi(void)
{
    FCG_Fcg1PeriphClockCmd(FCG1_PERIPH_SPI3, ENABLE);

    /* 先关 SPI 再改配置，运行中改 CFG 会让时序错乱 */
    CM_SPI3->CR1 &= (uint32_t)(~SPI_CR1_SPE);
    CM_SPI3->CFG1 = 0UL;
    CM_SPI3->CFG2 = LCD_SPI_CFG2;
    CM_SPI3->CR1 = SPI_CR1_SPIMDS | SPI_CR1_TXMDS | SPI_CR1_MSTR;
    CM_SPI3->CR1 |= SPI_CR1_SPE;
}

static void init_lcd_dma(void)
{
    stc_dma_init_t dma_init;

    /* DMA1 + AOS 时钟都在 FCG0。PWC_FCG0PC.PRT0 复位值是 0，
     * 表示 FCG0 写入无效，必须先写解锁键，否则时钟使能位被静默丢弃。
     * 解锁后保持解锁：后面 ADC 还要继续开 FCG0 外设时钟。 */
    PWC_FCG0_REG_Unlock();
    FCG_Fcg0PeriphClockCmd(FCG0_PERIPH_DMA1 | FCG0_PERIPH_AOS, ENABLE);

    (void)DMA_StructInit(&dma_init);
    dma_init.u32IntEn = DMA_INT_DISABLE;      /* 轮询，不用中断 */
    dma_init.u32SrcAddr = (uint32_t)band_buffer;
    dma_init.u32DestAddr = (uint32_t)(&CM_SPI3->DR);
    dma_init.u32DataWidth = DMA_DATAWIDTH_8BIT;
    dma_init.u32BlockSize = 1U;               /* 每个 SPTI 搬 1 字节 */
    dma_init.u32TransCount = (uint16_t)LCD_BAND_BYTES;
    dma_init.u32SrcAddrInc = DMA_SRC_ADDR_INC;
    dma_init.u32DestAddrInc = DMA_DEST_ADDR_FIX;
    (void)DMA_Init(CM_DMA1, DMA_CH0, &dma_init);

    /* DMA1 通道 0 的触发源 = SPI3 发送缓冲空（与原机 0x136 一致） */
    AOS_SetTriggerEventSrc(AOS_DMA1_0, INT_SRC_SPI3_SPTI);

    DMA_Cmd(CM_DMA1, ENABLE);
}

void init_lcd(void)
{
    uint32_t i;
    uint8_t madctl = (uint8_t)LCD_MADCTL;

    init_lcd_gpio();
    init_lcd_spi();
    init_lcd_dma();

    /* 硬件复位： datasheet 要求复位低至少 10 µs，这里给 20 ms 余量；
     * 释放后再留 120 ms 等内部电荷泵稳定，否则首批命令会被丢掉。 */
    set_rst_high();
    wait_ms(5U);
    set_rst_low();
    wait_ms(20U);
    set_rst_high();
    wait_ms(120U);

    /* 逐条执行初始化命令；行数由 sizeof 自动得到 */
    for (i = 0UL; i < (sizeof(lcd_init_sequence) / sizeof(lcd_init_sequence[0])); i++) {
        const uint8_t *row = lcd_init_sequence[i];
        uint8_t command = row[LCD_INIT_COL_CMD];

        write_lcd_command(command);
        write_lcd_data(&row[LCD_INIT_COL_PARAM], (uint32_t)row[LCD_INIT_COL_LEN]);

        /* 退出睡眠和开显示是唯一需要等内部状态机的两条命令 */
        if (0x11U == command) {
            wait_ms(120U);
        } else if (0x29U == command) {
            wait_ms(20U);
        }
    }

    /* 方向：横屏 0x70 / 竖屏 0x00 */
    write_lcd_command(0x36U);
    write_lcd_data(&madctl, 1UL);

    clear_lcd(0x0000U);
}

void flush_lcd_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    const uint16_t *pixels)
{
    /* 调用方（SGL 行缓冲）保证区域在屏内、字节数不超过 65535 */
    flush_area(x, y, w, h, (const uint8_t *)pixels);
}

void clear_lcd(uint16_t color)
{
    uint32_t i;
    uint32_t y;

    /* 先铺好条带的第一行，再整块复制，比逐像素写快一个数量级 */
    for (i = 0UL; i < LCD_WIDTH; i++) {
        band_buffer[(i * 2UL) + 0UL] = (uint8_t)(color >> 8);
        band_buffer[(i * 2UL) + 1UL] = (uint8_t)(color & 0xFFU);
    }
    fill_band_rows();

    /* 240 行分 8 段送，每段 30 行 */
    for (y = 0UL; y < LCD_HEIGHT; y += LCD_BAND_ROWS) {
        flush_band((uint16_t)y);
    }
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
