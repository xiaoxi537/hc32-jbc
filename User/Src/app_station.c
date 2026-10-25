/**
 *******************************************************************************
 * @file  app_station.c
 * @brief 焊台全局状态、出厂默认参数与设置块存取
 *
 * 默认值取自原机 load_channel_defaults（0x21BAA）：
 *   通道 0 = T12  : 预设 320/350/380，校准 2000，输出 100%，关机 15 min
 *   通道 1 = C245 : 预设 300/320/350，校准 2400，输出  70%，关机  0 min
 *   通道 2 = C210 : 预设 300/320/350，校准  830，输出  80%，休眠 10 min
 *******************************************************************************
 */
#include "app_station.h"
#include "bsp_clk.h"
#include "bsp_settings.h"

#include <string.h>

/* "JBCS"，用来区分"存过设置"和"空片全 0xFF" */
#define SETTINGS_MAGIC      (0x4A424353UL)
#define SETTINGS_VERSION    (1U)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t crc;
    settings_t settings;
} settings_blob_t;

station_t station;

/* 默认参数用并列常量数组，按通道下标取，不造结构体 */
static const uint16_t standby_defaults[CHANNEL_COUNT] = {100U, 100U, 100U};
static const uint16_t sleep_defaults[CHANNEL_COUNT] = {30U, 30U, 10U};
static const uint16_t off_defaults[CHANNEL_COUNT] = {15U, 0U, 0U};
static const uint8_t power_defaults[CHANNEL_COUNT] = {100U, 70U, 80U};
static const uint8_t ki_defaults[CHANNEL_COUNT] = {2U, 3U, 3U};
static const uint16_t kp_defaults[CHANNEL_COUNT] = {25U, 25U, 25U};
static const uint16_t kd_defaults[CHANNEL_COUNT] = {15U, 15U, 15U};
static const uint16_t setpoint_defaults[CHANNEL_COUNT] = {300U, 300U, 300U};
static const uint16_t cal_defaults[CHANNEL_COUNT] = {2000U, 2400U, 830U};
static const uint8_t auto_off_defaults[CHANNEL_COUNT] = {1U, 0U, 0U};
static const uint16_t preset_defaults[CHANNEL_COUNT][PRESET_COUNT] = {
    {320U, 350U, 380U},
    {300U, 320U, 350U},
    {300U, 320U, 350U},
};

/** CRC-16/CCITT（初值 0xFFFF，多项式 0x1021） */
static uint16_t compute_crc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc = 0xFFFFU;
    uint32_t i;
    uint8_t bit;

    /* 逐字节喂进 CRC；设置块只有 80 B，速度不是问题 */
    for (i = 0UL; i < length; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8U);
        /* 逐位处理是原机的算法；设置块只有 80 字节，速度无所谓 */
        for (bit = 0U; bit < 8U; bit++) {
            /* 只有最高位为 1 才异或多项式，否则只左移 */
        if (0U != (crc & 0x8000U)) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1U);
            }
        }
    }
    return crc;
}

static uint16_t clamp_u16(uint16_t value, uint16_t low, uint16_t high)
{
    /* 两个 if 比三目运算符更好读，也不会漏掉相等的情况 */
    /* 低于下限就抬到下限 */
    if (value < low) {
        return low;
    }
    /* 高于上限就压到上限 */
    if (value > high) {
        return high;
    }
    return value;
}

bool save_settings(void)
{
    settings_blob_t blob;
    uint8_t channel;

    (void)memset(&blob, 0, sizeof(blob));
    blob.magic = SETTINGS_MAGIC;
    blob.version = SETTINGS_VERSION;
    blob.size = (uint16_t)sizeof(settings_t);

    /* 把运行态里的用户设置逐项抄进 Flash 结构，运行态字段一个都不存 */
    for (channel = 0U; channel < CHANNEL_COUNT; channel++) {
        blob.settings.standby_temperature[channel] = station.standby_temperature[channel];
        blob.settings.sleep_minutes[channel] = station.sleep_minutes[channel];
        blob.settings.off_minutes[channel] = station.off_minutes[channel];
        blob.settings.power_scale[channel] = station.power_scale[channel];
        blob.settings.ki_tenths[channel] = station.ki_tenths[channel];
        blob.settings.kp[channel] = station.kp[channel];
        blob.settings.kd[channel] = station.kd[channel];
        blob.settings.default_setpoint[channel] = station.default_setpoint[channel];
        blob.settings.cal_raw[channel] = station.cal_raw[channel];
        blob.settings.auto_off_enabled[channel] = station.auto_off_enabled[channel];
        blob.settings.channel_presets[channel][0] = station.channel_presets[channel][0];
        blob.settings.channel_presets[channel][1] = station.channel_presets[channel][1];
        blob.settings.channel_presets[channel][2] = station.channel_presets[channel][2];
    }

    blob.settings.brightness = station.brightness;
    blob.settings.sound_enabled = station.sound_enabled;
    blob.settings.temp_step = station.temp_step;
    blob.settings.temp_min = station.temp_min;
    blob.settings.temp_max = station.temp_max;
    /* reserved[0] 里挤了两个小设置，省 Flash 也省版本升级：
     * bit0 主题、bit3 滚珠开关。
     * bit1..2 原来是"系统风格"，该功能已删除，位子留空 —— 这样旧 Flash
     * 里的数据仍然读得对，不用升 SETTINGS_VERSION 去清用户设置。 */
    blob.settings.reserved[0] = (uint8_t)((station.theme & 0x01U) |
                                         ((station.ball_switch_enabled & 0x01U) << 3U));
    blob.settings.reserved[1] = 0U;

    blob.crc = compute_crc16((const uint8_t *)&blob.settings, sizeof(blob.settings));

    return write_settings(&blob, sizeof(blob));
}

void load_settings(void)
{
    settings_blob_t blob;
    uint8_t channel;

    read_settings(&blob, sizeof(blob));

    /* 空片读出来是全 0xFF，magic 对不上就保持出厂默认；
     * CRC 不符说明上次写到一半掉电，也不能信。 */
    if ((SETTINGS_MAGIC != blob.magic) ||
        (SETTINGS_VERSION != blob.version) ||
        ((uint16_t)sizeof(settings_t) != blob.size) ||
        (blob.crc != compute_crc16((const uint8_t *)&blob.settings, sizeof(blob.settings)))) {
        return;
    }

    /* 逐项钳位：Flash 里的值可能来自旧版本或被写坏，
     * 直接拿去驱动加热会出安全问题（例如温度上限 600℃）。 */
    station.brightness = (uint8_t)clamp_u16(blob.settings.brightness, 0U, 10U);
    station.sound_enabled = (uint8_t)clamp_u16(blob.settings.sound_enabled, 0U, 1U);
    station.temp_step = clamp_u16(blob.settings.temp_step, 1U, 20U);
    station.temp_min = clamp_u16(blob.settings.temp_min, 50U, 300U);
    station.temp_max = clamp_u16(blob.settings.temp_max, 100U, 500U);
    station.theme = (uint8_t)(blob.settings.reserved[0] & 0x01U);
    station.ball_switch_enabled = (uint8_t)((blob.settings.reserved[0] >> 3U) & 0x01U);

    /* 上下限顺序颠倒会让钳位函数失效，直接退回安全区间 */
    if (station.temp_min >= station.temp_max) {
        station.temp_min = 80U;
        station.temp_max = 450U;
    }

    /* 逐通道钳位：Flash 里的值可能来自旧版本或被写坏 */
    for (channel = 0U; channel < CHANNEL_COUNT; channel++) {
        station.standby_temperature[channel] =
            clamp_u16(blob.settings.standby_temperature[channel], 50U, 250U);
        station.sleep_minutes[channel] =
            clamp_u16(blob.settings.sleep_minutes[channel], 0U, 120U);
        /* 待机时间只对 T12 有意义（C245/C210 靠休眠座，见 app_indev.c
         * clamp_tool_value）。旧存档里这三格都可能是 1/0/0 以外的值，
         * 对非 T12 通道一律读成 0，免得界面显示一个不生效的数字。 */
        station.off_minutes[channel] =
            (0U == channel)
                ? clamp_u16(blob.settings.off_minutes[channel], 0U, 120U)
                : 0U;
        station.power_scale[channel] =
            (uint8_t)clamp_u16(blob.settings.power_scale[channel], 10U, 100U);
        station.ki_tenths[channel] =
            (uint8_t)clamp_u16(blob.settings.ki_tenths[channel], 1U, 100U);
        station.kp[channel] = clamp_u16(blob.settings.kp[channel], 1U, 100U);
        station.kd[channel] = clamp_u16(blob.settings.kd[channel], 0U, 100U);
        station.default_setpoint[channel] =
            clamp_u16(blob.settings.default_setpoint[channel],
                      station.temp_min, station.temp_max);
        station.cal_raw[channel] =
            clamp_u16(blob.settings.cal_raw[channel], 100U, 4000U);
        station.auto_off_enabled[channel] =
            (0U != blob.settings.auto_off_enabled[channel]) ? 1U : 0U;
        station.channel_presets[channel][0] =
            clamp_u16(blob.settings.channel_presets[channel][0],
                      station.temp_min, station.temp_max);
        station.channel_presets[channel][1] =
            clamp_u16(blob.settings.channel_presets[channel][1],
                      station.temp_min, station.temp_max);
        station.channel_presets[channel][2] =
            clamp_u16(blob.settings.channel_presets[channel][2],
                      station.temp_min, station.temp_max);
    }
}

static void load_defaults(void)
{
    uint8_t channel;

    (void)memset(&station, 0, sizeof(station));

    /* 逐通道铺出厂值，数组下标就是通道号 */
    for (channel = 0U; channel < CHANNEL_COUNT; channel++) {
        station.standby_temperature[channel] = standby_defaults[channel];
        station.sleep_minutes[channel] = sleep_defaults[channel];
        /* off_defaults 本身就是 {15,0,0}；非 T12 强制 0 与读档/菜单三方一致 */
        station.off_minutes[channel] = (0U == channel) ? off_defaults[channel] : 0U;
        station.power_scale[channel] = power_defaults[channel];
        station.ki_tenths[channel] = ki_defaults[channel];
        station.kp[channel] = kp_defaults[channel];
        station.kd[channel] = kd_defaults[channel];
        station.default_setpoint[channel] = setpoint_defaults[channel];
        station.cal_raw[channel] = cal_defaults[channel];
        station.auto_off_enabled[channel] = auto_off_defaults[channel];
        station.channel_presets[channel][0] = preset_defaults[channel][0];
        station.channel_presets[channel][1] = preset_defaults[channel][1];
        station.channel_presets[channel][2] = preset_defaults[channel][2];
    }

    station.temp_max = 450U;
    station.temp_min = 80U;
    station.temp_step = 5U;
    station.brightness = 10U;
    station.sound_enabled = 1U;
    station.theme = 0U;
    station.ball_switch_enabled = 1U;
    station.tip_channel = CHANNEL_NONE;
    station.ui_mode = MODE_MAIN;
    station.adc_phase = 0U;
}

void init_station(void)
{
    load_defaults();

    /* 存过设置就用用户的，没存过（或校验失败）就保持出厂值 */
    load_settings();

    /* 还没识别烙铁头时，主界面和菜单先按通道 0 显示 */
    load_channel(0U);
    station.preset_index = 0xFFU;
}

void reset_config(void)
{
    load_defaults();
    load_channel(0U);
    station.preset_index = 0xFFU;
    (void)save_settings();
    close_menu();
}

void reset_system(void)
{
    reset_mcu();
}

void load_channel(uint8_t channel)
{
    /* 通道号来自 ID 识别结果，越界说明识别逻辑坏了，不往下写数组 */
    if (channel >= CHANNEL_COUNT) {
        return;
    }

    station.set_temperature = station.default_setpoint[channel];
    station.cal_raw_at_350 = (float)station.cal_raw[channel];
    station.preset_temperature[0] = station.channel_presets[channel][0];
    station.preset_temperature[1] = station.channel_presets[channel][1];
    station.preset_temperature[2] = station.channel_presets[channel][2];
    /* 换了通道就没有"当前预设"的概念了，等用户再按一次 */
    station.preset_index = 0xFFU;
    reload_timers();
}

uint16_t clamp_temperature(uint16_t temperature)
{
    /* 低于下限就抬到下限 */
    if (temperature < station.temp_min) {
        return station.temp_min;
    }
    /* 高于上限就压到上限 */
    if (temperature > station.temp_max) {
        return station.temp_max;
    }
    return temperature;
}

void set_setpoint(uint16_t temperature)
{
    station.set_temperature = clamp_temperature(temperature);
}

uint16_t get_setpoint(void)
{
    uint8_t channel = get_active_channel();

    /* 休眠就是 0℃：PID 会自然把占空比压到 0，不需要额外关输出 */
    if (0U != (station.state & FLAG_SLEEP)) {
        return 0U;
    }
    /* 待机用该通道单独配的保温温度 */
    if (0U != (station.state & FLAG_STANDBY)) {
        return station.standby_temperature[channel];
    }
    return station.set_temperature;
}

void reload_timers(void)
{
    uint8_t channel = get_active_channel();

    station.off_timer_ms = (uint32_t)station.off_minutes[channel] * 60000UL;
    station.sleep_timer_ms = (uint32_t)station.sleep_minutes[channel] * 60000UL;
}

uint8_t get_active_channel(void)
{
    /* 未识别烙铁头时，菜单仍然要能改参数，按通道 0 处理 */
    return (station.tip_channel < CHANNEL_COUNT) ? station.tip_channel : 0U;
}

bool is_tip_present(void)
{
    /* 识别出通道且不是断偶，才算"有工具"。
     * 界面用它决定显示主界面还是"无工具"告警页，
     * 输入层用同一个判定来屏蔽编码器 —— 两处必须完全一致，
     * 否则会出现"屏幕显示无工具、但编码器还能翻菜单"的错位。 */
    return (station.tip_channel < CHANNEL_COUNT) &&
           (0U == (station.state & FLAG_TIP_FAULT));
}

void open_menu(void)
{
    /* 只有在主界面才能进菜单，避免在参数页重复按出菜单 */
    if (MODE_MAIN != station.ui_mode) {
        return;
    }
    station.ui_mode = MODE_ROOT;
    station.menu_selected = 0U;
    station.menu_editing = 0U;
    station.state |= FLAG_MENU;
}

void close_menu(void)
{
    station.ui_mode = MODE_MAIN;
    station.menu_editing = 0U;
    station.state &= (uint16_t)(~FLAG_MENU);
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
