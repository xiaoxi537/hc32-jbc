/**
 *******************************************************************************
 * @file  app_station.h
 * @brief 焊台全局状态与四个状态机的共享接口
 *
 * 四个模块：
 *   app_check.c  check 任务   1 ms：联锁/在位/休眠/待机/自动关机
 *   app_indev.c  indev 任务   5 ms：按键事件 + 菜单状态机
 *   app_heat.c   heat 任务    1 ms：测温 → 识别 → PID → PWM
 *   app_ui.c     ui 任务     10 ms：界面刷新
 *
 * 本文件只有状态定义、默认参数和纯函数，不访问寄存器。
 *******************************************************************************
 */
#ifndef __APP_STATION_H__
#define __APP_STATION_H__

#include <stdbool.h>
#include <stdint.h>

#define CHANNEL_COUNT       (3U)
#define CHANNEL_NONE        (0xFFU)

/** 每个通道三个快捷温度 */
#define PRESET_COUNT        (3U)

/* 功率/安全状态字，与原机 ctx+0x08 的位定义一一对应 */
#define FLAG_ARMED          (0x0001U)   /* 输出允许 */
/* 0x0002：实测温度已达设定值（带滞回锁存）。
 * 原机没有这个位，但它 0x0002 空着；本工程需要它把"已到温"和
 * FLAG_READY(0x1000="已识别、可进 PID") 分开 —— 后者是加热闸门，
 * 借它当"到温"会让 PID 永远进不去。 */
#define FLAG_TEMP_REACHED   (0x0002U)   /* 已到温（滞回锁存） */
#define FLAG_TIP_FAULT      (0x0004U)   /* 烙铁头缺失或断偶 */
#define FLAG_MENU           (0x0008U)   /* 菜单打开：设置期间禁止加热，见 app_heat.c */
#define FLAG_JBC_SW         (0x0010U)   /* PA3 持续有效（安全联锁） */
#define FLAG_TIP_TYPE_B     (0x0020U)   /* C210 高电压降功率 */
#define FLAG_ANNOUNCE       (0x0100U)   /* 到温提示音待触发 */
#define FLAG_PRESENT        (0x0200U)   /* PA9 手柄在位 */
#define FLAG_SLEEP          (0x0400U)   /* 休眠：设定值 0 */
#define FLAG_STANDBY        (0x0800U)   /* 待机：设定值 = 待机温度 */
#define FLAG_READY          (0x1000U)   /* 已识别，PID 正常控制 */
#define FLAG_DETECT         (0x2000U)   /* 识别/预热验证中 */
#define FLAG_HEATING        (0x4000U)   /* 本周期有输出 */

/* 界面模式，编号沿用原机 ctx+0x31；0x0C 是本工程新增的"快捷温度"子页 */
#define MODE_MAIN           (0xFFU)
#define MODE_ROOT           (0x09U)
#define MODE_BASIC          (0x1CU)
#define MODE_TOOL           (0x1DU)
#define MODE_THEME          (0x23U)
#define MODE_RESET          (0x1EU)
#define MODE_THEME_CHOICE   (0x0BU)
#define MODE_PRESET         (0x0CU)
#define MODE_PARAM          (0x1BU)
#define MODE_ABOUT          (0x1FU)

/** 菜单每页最多 6 项；屏幕上一次只显示 5 行，多出来的靠滚动看 */
#define MENU_ROWS           (6U)
#define MENU_VISIBLE_ROWS   (5U)

/**
 * 运行期状态。
 * 注意：字段顺序对应不了 Flash 布局，掉电保存用的是下面的 settings_t。
 */
typedef struct {
    /* 定时器 */
    uint32_t off_timer_ms;              /* 自动关机倒计时 */
    uint32_t sleep_timer_ms;            /* 待机 → 休眠倒计时 */

    /* 功率/安全 */
    uint16_t state;                     /* FLAG_* 位 */
    uint16_t pid_out;                   /* 0…1000 */
    uint16_t set_temperature;           /* 当前工作设定温度 */
    uint16_t preset_temperature[PRESET_COUNT];     /* 当前通道的三个快捷温度 */
    uint16_t temp_max;
    uint16_t temp_min;
    uint16_t temp_step;

    /* 测量值 */
    float temperature;                  /* 滤波后的温度（℃），PID 用 */
    float input_voltage;                /* 输入电压（V） */
    float heat_current;                 /* 加热电流（A） */
    float cal_raw_at_350;               /* 350℃ 对应的原始 ADC 值 */
    int16_t temperature_int;            /* 整数温度，给 UI 用（慢滤波，只显示） */
    uint8_t heat_duty;                  /* 0…250，写硬件 */
    uint8_t tip_channel;                /* 0/1/2，0xFF = 未识别 */
    uint8_t over_current_latch;         /* 过流闩锁，重新识别后清 */
    volatile uint8_t adc_phase;         /* 0 空闲 / 1 加热 / 2 测温 */

    /* 按通道保存的用户设置 */
    uint16_t standby_temperature[CHANNEL_COUNT];
    uint16_t sleep_minutes[CHANNEL_COUNT];
    uint16_t off_minutes[CHANNEL_COUNT];
    uint8_t power_scale[CHANNEL_COUNT];     /* 输出功率 % */
    uint8_t ki_tenths[CHANNEL_COUNT];       /* Ki × 10 */
    uint16_t kp[CHANNEL_COUNT];
    uint16_t kd[CHANNEL_COUNT];
    uint16_t default_setpoint[CHANNEL_COUNT];
    uint16_t channel_presets[CHANNEL_COUNT][PRESET_COUNT];
    uint16_t cal_raw[CHANNEL_COUNT];
    uint8_t auto_off_enabled[CHANNEL_COUNT];

    /* 全局设置 */
    uint8_t brightness;                 /* 0…10 */
    uint8_t sound_enabled;              /* 0/1 */
    uint8_t theme;                      /* 0 深色 / 1 浅色 */
    uint8_t ball_switch_enabled;        /* 滚珠开关 0/1 */

    /* 界面/菜单 */
    uint8_t ui_mode;
    uint8_t menu_selected;
    uint8_t menu_editing;
    uint16_t edit_value;
    uint8_t preset_index;               /* 0..2；0xFF = 当前是自定义温度 */
    uint8_t settings_dirty;             /* 菜单已改，回主界面后统一写 Flash */

    /* 调试 */
    uint32_t heat_cycles;
} station_t;

/**
 * 掉电保存的设置项。
 * 只存用户改得动的参数，运行态（温度、状态位、计时）不存。
 * 字段顺序和大小直接对应 Flash 里的数据，改动必须同时升 SETTINGS_VERSION。
 */
typedef struct {
    uint16_t standby_temperature[CHANNEL_COUNT];
    uint16_t sleep_minutes[CHANNEL_COUNT];
    uint16_t off_minutes[CHANNEL_COUNT];
    uint8_t power_scale[CHANNEL_COUNT];
    uint8_t ki_tenths[CHANNEL_COUNT];
    uint16_t kp[CHANNEL_COUNT];
    uint16_t kd[CHANNEL_COUNT];
    uint16_t default_setpoint[CHANNEL_COUNT];
    uint16_t channel_presets[CHANNEL_COUNT][PRESET_COUNT];
    uint16_t cal_raw[CHANNEL_COUNT];
    uint8_t auto_off_enabled[CHANNEL_COUNT];
    uint8_t brightness;
    uint8_t sound_enabled;
    uint16_t temp_step;
    uint16_t temp_min;
    uint16_t temp_max;
    uint8_t reserved[2];                /* 补齐到 4 字节倍数 */
} settings_t;

extern station_t station;

/** 上电初始化：清零 → 出厂默认 → 尝试读回保存项 */
void init_station(void);

/** 按通道载入工作参数（设定温度、预设、校准、超时） */
void load_channel(uint8_t channel);

/** 当前有效设定值（休眠 0 / 待机温度 / 正常工作温度） */
uint16_t get_setpoint(void);

/** 设置工作温度，自动钳到 [temp_min, temp_max] */
void set_setpoint(uint16_t temperature);

/** 把温度钳到 [temp_min, temp_max] */
uint16_t clamp_temperature(uint16_t temperature);

/** 按当前通道的分钟数重装自动关机/休眠计时（分钟 → ms） */
void reload_timers(void);

/** 当前有效通道（未识别时回退 0，供菜单编辑用） */
uint8_t get_active_channel(void);

/**
 * 烙铁头在位且没有断偶。
 * 界面用它决定显示主界面还是"无工具"告警页；输入层用同一个判定屏蔽编码器。
 */
bool is_tip_present(void);

/** 把用户设置写入内部 Flash */
bool save_settings(void);

/** 从内部 Flash 载入设置；magic/CRC/版本不符时保持出厂默认 */
void load_settings(void);

/** 恢复出厂配置并写回 Flash（不重启） */
void reset_config(void);

/** 软复位整机 */
void reset_system(void);

/** 打开主菜单（只在主界面有效） */
void open_menu(void);

/** 关闭菜单，回主界面 */
void close_menu(void);

#endif /* __APP_STATION_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
