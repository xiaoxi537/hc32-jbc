/**
 *******************************************************************************
 * @file  bsp_buzzer_backlight.c
 * @brief TMRA4 CH3/CH4：蜂鸣器与背光 PWM
 *******************************************************************************
 */
#include "bsp_buzzer_backlight.h"
#include "bsp_clk.h"
#include "hc32_ll.h"

#include <stdbool.h>

#define PWM_TMRA            CM_TMRA_4
#define PWM_PERIOD          (1000U)

/* 45% 占空比对应原机 MLT-8530 的驱动点，再高只会更费电不会更响 */
#define BEEP_COMPARE        (450U)
#define BEEP_PORT           GPIO_PORT_B
#define BEEP_PIN            GPIO_PIN_08
#define BACKLIGHT_PORT      GPIO_PORT_B
#define BACKLIGHT_PIN       GPIO_PIN_09

/* 背光亮度按原机线性公式 v*90+100 换算成比较值，保证 10 档观感一致 */
#define BACKLIGHT_STEP      (90UL)
#define BACKLIGHT_BASE      (100UL)
#define BACKLIGHT_MAX_LEVEL (10U)

static volatile bool beep_active;
static volatile uint32_t beep_deadline;

static void init_pwm_channel(uint32_t channel, uint32_t compare)
{
    stc_tmra_pwm_init_t pwm_init;

    /* 起始/结束电平固定，避免改比较值时输出线上出现窄毛刺 */
    (void)TMRA_PWM_StructInit(&pwm_init);
    pwm_init.u32CompareValue = compare;
    pwm_init.u16StartPolarity = TMRA_PWM_HIGH;
    pwm_init.u16StopPolarity = TMRA_PWM_LOW;
    pwm_init.u16CompareMatchPolarity = TMRA_PWM_LOW;
    pwm_init.u16PeriodMatchPolarity = TMRA_PWM_HIGH;
    (void)TMRA_PWM_Init(PWM_TMRA, channel, &pwm_init);
    TMRA_PWM_OutputCmd(PWM_TMRA, channel, ENABLE);
}

void init_buzzer_backlight(void)
{
    stc_tmra_init_t tmra_init;
    stc_gpio_init_t gpio_init;

    FCG_Fcg2PeriphClockCmd(FCG2_PERIPH_TMRA_4, ENABLE);

    /* PB8/PB9 复用功能 4 = TMRA4 CH3/CH4；先解锁 GPIO 复用配置 */
    GPIO_REG_Unlock();
    (void)GPIO_StructInit(&gpio_init);
    gpio_init.u16PinDrv = PIN_HIGH_DRV;
    (void)GPIO_Init(BEEP_PORT, BEEP_PIN, &gpio_init);
    (void)GPIO_Init(BACKLIGHT_PORT, BACKLIGHT_PIN, &gpio_init);
    GPIO_SetFunc(BEEP_PORT, BEEP_PIN, 4U);
    GPIO_SetFunc(BACKLIGHT_PORT, BACKLIGHT_PIN, 4U);

    (void)TMRA_StructInit(&tmra_init);
    tmra_init.u8CountSrc = TMRA_CNT_SRC_SW;
    tmra_init.sw_count.u8ClockDiv = TMRA_CLK_DIV32;
    tmra_init.sw_count.u8CountMode = TMRA_MD_SAWTOOTH;
    tmra_init.sw_count.u8CountDir = TMRA_DIR_UP;
    tmra_init.u32PeriodValue = PWM_PERIOD;
    (void)TMRA_Init(PWM_TMRA, &tmra_init);

    init_pwm_channel(TMRA_CH3, BEEP_COMPARE);
    init_pwm_channel(TMRA_CH4, PWM_PERIOD);

    TMRA_Start(PWM_TMRA);

    /* 上电先静音，背光开满：屏还没初始化时背光亮着便于确认供电正常 */
    TMRA_SetCompareValue(PWM_TMRA, TMRA_CH3, 0U);
    TMRA_PWM_OutputCmd(PWM_TMRA, TMRA_CH3, DISABLE);
    TMRA_SetCompareValue(PWM_TMRA, TMRA_CH4, PWM_PERIOD);
    TMRA_PWM_OutputCmd(PWM_TMRA, TMRA_CH4, ENABLE);
}

void beep_buzzer(uint16_t ms)
{
    /* 0 ms 表示"不响"；调用方按静音设置可能传进来，不能让它响 1 ms */
    if (0U == ms) {
        return;
    }
    beep_deadline = get_tick_ms() + (uint32_t)ms;
    beep_active = true;
    TMRA_SetCompareValue(PWM_TMRA, TMRA_CH3, BEEP_COMPARE);
    TMRA_PWM_OutputCmd(PWM_TMRA, TMRA_CH3, ENABLE);
}

void task_buzzer(void)
{
    /* 没在响就什么都不做，1 ms 调用一次也不会有额外开销 */
    if (!beep_active) {
        return;
    }
    /* 用 >= 比较：主循环被 Flash 写入阻塞时也不会漏掉关断 */
    if (get_tick_ms() >= beep_deadline) {
        beep_active = false;
        TMRA_SetCompareValue(PWM_TMRA, TMRA_CH3, 0U);
        TMRA_PWM_OutputCmd(PWM_TMRA, TMRA_CH3, DISABLE);
    }
}

void set_backlight(uint8_t level)
{
    uint32_t compare;

    /* 0 档直接把输出关掉，比把比较值写 0 更省电，也不会留窄脉冲 */
    if (0U == level) {
        TMRA_SetCompareValue(PWM_TMRA, TMRA_CH4, 0U);
        TMRA_PWM_OutputCmd(PWM_TMRA, TMRA_CH4, DISABLE);
        return;
    }
    /* 面板只认 0…10，超过就按最亮处理 */
    if (level > BACKLIGHT_MAX_LEVEL) {
        level = BACKLIGHT_MAX_LEVEL;
    }
    compare = ((uint32_t)level * BACKLIGHT_STEP) + BACKLIGHT_BASE;
    TMRA_SetCompareValue(PWM_TMRA, TMRA_CH4, compare);
    TMRA_PWM_OutputCmd(PWM_TMRA, TMRA_CH4, ENABLE);
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
