/**
 *******************************************************************************
 * @file  bsp_heater.c
 * @brief PA10 + TMRA6 软件 PWM（等价原机 tmra6_heater_isr）
 *
 * 时序：
 *   PCLK1 100 MHz / 16 = 6.25 MHz；PERAR=630 → 溢出周期 631 / 6.25 MHz ≈ 101 µs
 *   一个加热脉冲 = 251 次溢出 ≈ 25.3 ms
 *******************************************************************************
 */
#include "bsp_heater.h"
#include "hc32_ll.h"

#define HEATER_TMRA         CM_TMRA_6
#define HEATER_PORT         GPIO_PORT_A
#define HEATER_PIN          GPIO_PIN_10

/* 251 次溢出与原机 "250 < cnt+1" 等价；少一次脉冲宽度就差 0.4% */
#define HEATER_PULSE_TICKS  (251U)
#define HEATER_PERIOD       (630U)

/* 占空比上限就是脉冲长度；再大也只会一直开到本周期结束 */
#define HEATER_DUTY_MAX     (250U)

/* 这两个量会被 TMRA6 中断改写，调试器看波形时直接读它们 */
volatile uint8_t heater_duty;
volatile uint16_t heater_tick;
volatile uint32_t heater_pulses;

static volatile bool pulse_done = true;

/** 只在中断里用的引脚翻转，宏展开比函数调用更短 */
static void set_heat_pin(bool high)
{
    /* 高电平开加热、低电平关；任何路径都不能让它悬空 */
    if (high) {
        GPIO_SetPins(HEATER_PORT, HEATER_PIN);
    } else {
        GPIO_ResetPins(HEATER_PORT, HEATER_PIN);
    }
}

/**
 * @brief  TMRA6 溢出中断：逐周期软件 PWM
 * @note   与原机 0x1BE4 相同：跑满 251 个周期后停定时器、拉低 PA10、置完成标志
 */
static void handle_pulse_tick(void)
{
    TMRA_ClearStatus(HEATER_TMRA, TMRA_FLAG_OVF);

    /* 到达脉冲末尾：先断电再停表，保证 PA10 一定是低电平 */
    if (heater_tick >= (HEATER_PULSE_TICKS - 1U)) {
        heater_tick = 0U;
        set_heat_pin(false);
        TMRA_Stop(HEATER_TMRA);
        heater_pulses++;
        pulse_done = true;
        return;
    }

    heater_tick++;
    /* 前 duty 个周期开、其余关；+1 是与原机计数从 0 开始的偏移对齐 */
    if ((uint16_t)(heater_tick + 1U) < (uint16_t)heater_duty) {
        set_heat_pin(true);
    } else {
        set_heat_pin(false);
    }
}

void init_heater(void)
{
    stc_tmra_init_t tmra_init;
    stc_irq_signin_config_t irq_sign;
    stc_gpio_init_t gpio_init;

    /* TMRA6 挂在 FCG2 上，没有时钟写寄存器全是无效操作 */
    FCG_Fcg2PeriphClockCmd(FCG2_PERIPH_TMRA_6, ENABLE);

    /* PA10 推挽输出，初始低：上电时绝对不能有加热输出 */
    (void)GPIO_StructInit(&gpio_init);
    gpio_init.u16PinState = PIN_RESET;
    gpio_init.u16PinDir = PIN_DIR_OUT;
    gpio_init.u16PinOutputType = PIN_OUT_TYPE_CMOS;
    gpio_init.u16PinDrv = PIN_HIGH_DRV;
    gpio_init.u16Latch = PIN_LATCH_OFF;
    gpio_init.u16PullUp = PIN_PU_OFF;
    gpio_init.u16Invert = PIN_INVT_OFF;
    gpio_init.u16ExtInt = PIN_EXTINT_OFF;
    gpio_init.u16PinAttr = PIN_ATTR_DIGITAL;
    (void)GPIO_Init(HEATER_PORT, HEATER_PIN, &gpio_init);
    set_heat_pin(false);

    /* 软件时钟源 + 16 分频，得到 101 µs 的固定溢出周期 */
    (void)TMRA_StructInit(&tmra_init);
    tmra_init.u8CountSrc = TMRA_CNT_SRC_SW;
    tmra_init.sw_count.u8ClockDiv = TMRA_CLK_DIV16;
    tmra_init.sw_count.u8CountMode = TMRA_MD_SAWTOOTH;
    tmra_init.sw_count.u8CountDir = TMRA_DIR_UP;
    tmra_init.u32PeriodValue = HEATER_PERIOD;
    (void)TMRA_Init(HEATER_TMRA, &tmra_init);

    TMRA_IntCmd(HEATER_TMRA, TMRA_INT_OVF, ENABLE);

    /* 溢出中断走 INT082；优先级压到最低，别和 SysTick/ADC 抢 */
    irq_sign.enIntSrc = INT_SRC_TMRA_6_OVF;
    irq_sign.enIRQn = INT082_IRQn;
    irq_sign.pfnCallback = &handle_pulse_tick;
    (void)INTC_IrqSignIn(&irq_sign);
    NVIC_ClearPendingIRQ(INT082_IRQn);
    NVIC_SetPriority(INT082_IRQn, 15U);
    NVIC_EnableIRQ(INT082_IRQn);

    /* 先跑一个空脉冲，让应用层从"已完成"的已知状态起步 */
    start_heat_pulse(0U);
}

void start_heat_pulse(uint8_t duty)
{
    /* 超过 250 已经等于整周期常开，再大没有意义，还可能算错比较点 */
    heater_duty = (duty > HEATER_DUTY_MAX) ? HEATER_DUTY_MAX : duty;
    heater_tick = 0U;
    pulse_done = false;
    set_heat_pin(false);
    TMRA_Start(HEATER_TMRA);
}

void stop_heat_output(void)
{
    heater_duty = 0U;
    heater_tick = 0U;
    set_heat_pin(false);
    TMRA_Stop(HEATER_TMRA);
    pulse_done = true;
}

bool take_pulse_done(void)
{
    /* 本周期还没跑完就不动标志，让调用方下一拍再问 */
    if (!pulse_done) {
        return false;
    }
    pulse_done = false;
    return true;
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
