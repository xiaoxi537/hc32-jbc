/**
 *******************************************************************************
 * @file  bsp_input.c
 * @brief 输入脚读取与 EC11 正交解码（PA0/PA1/PA2/PA3/PA4/PA9）
 *******************************************************************************
 */
#include "bsp_input.h"
#include "hc32_ll.h"

#define INPUT_PORT          GPIO_PORT_A
#define INPUT_PINS          (GPIO_PIN_00 | GPIO_PIN_01 | GPIO_PIN_02 | \
                             GPIO_PIN_03 | GPIO_PIN_04 | GPIO_PIN_09)

/* 4 位 {旧 A, 旧 B, 新 A, 新 B} 只有两种迁移是正常转动的方向 */
#define ENCODER_POSITIVE    (0x2814UL)
#define ENCODER_NEGATIVE    (0x4182UL)

/* EC11 每格 4 个边沿；少攒几个会把机械抖动也算成一格 */
#define ENCODER_STEPS_PER_DETENT    (4)

/* 一次最多攒 100 格，防止主循环长时间不取导致增量溢出 int16 */
#define ENCODER_DELTA_LIMIT         (100)

static uint8_t encoder_previous;
static int8_t encoder_accumulator;
static volatile int16_t encoder_delta;

/** 内部用：把 A/B 两相读成 **bit0=B、bit1=A**（与原机的 idx 编码同构）
 *
 * 为什么 A 要放在 bit1：原机 `encoder_poll` 拼的是
 *     idx = (prev<<2) | (A<<1) | B
 * 即低位字节的 **bit1 = A、bit0 = B**；两张掩码表 0x2814/0x4182 就是按
 * 这个位序枚举出来的（逆向报告 `analysis/input_storage.md` §2.2 的 16 项状态表）。
 *
 * 曾经这里写成 bit0=A、bit1=B，于是拼出的 idx 把 A/B 换了位置：
 * A 领先变成落在 0x4182 上、B 领先落在 0x2814 上，**两个方向整体对调**，
 * 实机表现就是"编码器转的方向和界面变化相反"。已按原机位序修正。
 */
static uint8_t read_encoder(void)
{
    uint16_t pins = GPIO_ReadInputPort(INPUT_PORT);
    uint8_t state = 0U;

    /* B 相在 PA0 -> bit0 */
    if (0U != (pins & GPIO_PIN_00)) {
        state |= 0x01U;
    }
    /* A 相在 PA1 -> bit1 */
    if (0U != (pins & GPIO_PIN_01)) {
        state |= 0x02U;
    }
    return state;
}

void init_inputs(void)
{
    stc_gpio_init_t gpio_init;

    /* 六个脚全是机械触点或开关，对地闭合；内部上拉让断开时读到高电平 */
    (void)GPIO_StructInit(&gpio_init);
    gpio_init.u16PinDir = PIN_DIR_IN;
    gpio_init.u16PullUp = PIN_PU_ON;
    gpio_init.u16Latch = PIN_LATCH_OFF;
    gpio_init.u16Invert = PIN_INVT_OFF;
    gpio_init.u16ExtInt = PIN_EXTINT_OFF;
    gpio_init.u16PinAttr = PIN_ATTR_DIGITAL;
    (void)GPIO_Init(INPUT_PORT, INPUT_PINS, &gpio_init);

    encoder_previous = read_encoder();
    encoder_accumulator = 0;
    encoder_delta = 0;
}

void tick_encoder(void)
{
    uint8_t current = read_encoder();
    uint8_t transition = (uint8_t)(current | (uint8_t)(encoder_previous << 2U));
    uint32_t mask = (1UL << transition);

    /* 只认两个合法迁移方向，其余组合是抖动、回弹或漏采样，直接忽略 */
    if (0UL != (mask & ENCODER_POSITIVE)) {
        encoder_accumulator++;
    } else if (0UL != (mask & ENCODER_NEGATIVE)) {
        encoder_accumulator--;
    }
    encoder_previous = current;

    /* 攒满 4 个同向边沿才算转了一格，避免一格报成两三次 */
    if (encoder_accumulator >= ENCODER_STEPS_PER_DETENT) {
        encoder_accumulator = 0;
        /* 到上限就丢弃，防止 int16 溢出把方向翻过来 */
        if (encoder_delta < ENCODER_DELTA_LIMIT) {
            encoder_delta++;
        }
    } else if (encoder_accumulator <= -ENCODER_STEPS_PER_DETENT) {
        encoder_accumulator = 0;
        /* 反向同样限幅 */
        if (encoder_delta > -ENCODER_DELTA_LIMIT) {
            encoder_delta--;
        }
    }
}

int16_t take_encoder_delta(void)
{
    int16_t delta;
    uint32_t primask;

    /* 这里真的会被 SysTick 抢：边取边清零必须关中断，
     * 否则会丢掉这 1 ms 内新增的格子，或者把同一格重复算两次。 */
    primask = __get_PRIMASK();
    __disable_irq();
    delta = encoder_delta;
    encoder_delta = 0;
    /* 只有原本开着中断才恢复 */
    if (0UL == primask) {
        __enable_irq();
    }
    return delta;
}

bool is_button_down(void)
{
    /* 按键另一端接地，读到低电平就是按下 */
    return PIN_RESET == GPIO_ReadInputPins(INPUT_PORT, GPIO_PIN_02);
}

bool is_jbc_sw_low(void)
{
    return PIN_RESET == GPIO_ReadInputPins(INPUT_PORT, GPIO_PIN_03);
}

bool is_jbc_sle_low(void)
{
    return PIN_RESET == GPIO_ReadInputPins(INPUT_PORT, GPIO_PIN_04);
}

bool is_handle_present(void)
{
    /* PA9 由手柄座拉高，高电平表示手柄插着 */
    return PIN_SET == GPIO_ReadInputPins(INPUT_PORT, GPIO_PIN_09);
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
