/**
 *******************************************************************************
 * @file  app_indev.c
 * @brief 按键消抖/长短按 + 菜单状态机
 *
 * 对应原机 indev_task(0xAC6C) / menu_state_update(0x2159C)。
 * 菜单分三层：
 *   主界面 --长按--> 设置（根） --> 基本设置 / 工具设置 / 主题风格 / 系统重置 / 关于本机
 *   转动 = 选择或调值，短按 = 进入或确认，长按 = 返回或取消
 *******************************************************************************
 */
#include "app_check.h"
#include "app_indev.h"
#include "app_station.h"
#include "bsp_buzzer_backlight.h"
#include "bsp_clk.h"
#include "bsp_input.h"

#include <stdio.h>

/* 4 × 5 ms = 20 ms；EC11 按下的抖动一般不超过 10 ms */
#define BTN_DEBOUNCE_COUNT  (4U)
/* 800 ms 足够区分"想确认"和"想返回"，再短容易误判 */
#define BTN_LONG_MS         (800U)

typedef enum {
    EVENT_NONE = 0,
    EVENT_SHORT,
    EVENT_LONG,
} key_event_t;

/* 菜单项名称全部是静态常量，界面可以直接长期持有指针 */
static const char *const root_items[] = {
    "基本设置", "工具设置", "主题风格", "系统重置", "关于本机",
};
static const char *const basic_items[] = {
    "系统声音", "系统亮度", "最低温度", "最高温度", "温度步进",
};
static const char *const tool_items[] = {
    "待机温度", "待机时间", "休眠时间", "快捷温度", "参数调整", "滚珠开关",
};
static const char *const theme_items[] = {
    "系统主题",
};
static const char *const reset_items[] = {
    "重置配置", "系统重置",
};
static const char *const preset_items[] = {
    "快捷一", "快捷二", "快捷三",
};
static const char *const theme_choice_items[] = {
    "深色主题", "浅色主题",
};
static const char *const param_items[] = {
    "比例 Kp", "积分 Ki", "微分 Kd", "温度校准", "输出功率",
};

#define ROOT_ITEM_COUNT         (sizeof(root_items) / sizeof(root_items[0]))
#define BASIC_ITEM_COUNT        (sizeof(basic_items) / sizeof(basic_items[0]))
#define TOOL_ITEM_COUNT         (sizeof(tool_items) / sizeof(tool_items[0]))
#define THEME_ITEM_COUNT        (sizeof(theme_items) / sizeof(theme_items[0]))
#define RESET_ITEM_COUNT        (sizeof(reset_items) / sizeof(reset_items[0]))
#define PRESET_ITEM_COUNT       (sizeof(preset_items) / sizeof(preset_items[0]))
#define THEME_CHOICE_ITEM_COUNT (sizeof(theme_choice_items) / sizeof(theme_choice_items[0]))
#define PARAM_ITEM_COUNT        (sizeof(param_items) / sizeof(param_items[0]))

/* 各页转动一格的步进；用常量表按下标取，比 switch 短且不会漏 case */
static const uint16_t basic_steps[] = {1U, 1U, 5U, 5U, 1U};
static const uint16_t tool_steps[] = {5U, 5U, 5U, 5U, 1U, 1U};
static const uint16_t preset_steps[] = {5U, 5U, 5U};
static const uint16_t param_steps[] = {1U, 1U, 1U, 10U, 1U};

/* ---- 按键状态 ---- */
static uint8_t btn_raw;
static uint8_t btn_stable;
static uint8_t btn_count;
static bool long_sent;
static uint32_t btn_press_ms;

uint8_t get_page_item_count(uint8_t mode)
{
    /* 每个页面的行数固定，直接查表；未知页面按 0 行处理 */
    switch (mode) {
    case MODE_ROOT:         return (uint8_t)ROOT_ITEM_COUNT;
    case MODE_BASIC:        return (uint8_t)BASIC_ITEM_COUNT;
    case MODE_TOOL:         return (uint8_t)TOOL_ITEM_COUNT;
    case MODE_THEME:        return (uint8_t)THEME_ITEM_COUNT;
    case MODE_RESET:        return (uint8_t)RESET_ITEM_COUNT;
    case MODE_PRESET:       return (uint8_t)PRESET_ITEM_COUNT;
    case MODE_THEME_CHOICE: return (uint8_t)THEME_CHOICE_ITEM_COUNT;
    case MODE_PARAM:        return (uint8_t)PARAM_ITEM_COUNT;
    default:                return 0U;
    }
}

const char *get_page_title(uint8_t mode)
{
    /* 标题都是常量字符串，调用方可以长期持有 */
    switch (mode) {
    case MODE_ROOT:         return "设置";
    case MODE_BASIC:        return "基本设置";
    case MODE_TOOL:         return "工具设置";
    case MODE_THEME:        return "主题风格";
    case MODE_RESET:        return "系统重置";
    case MODE_PRESET:       return "快捷温度";
    case MODE_THEME_CHOICE: return "系统主题";
    case MODE_PARAM:        return "参数调整";
    case MODE_ABOUT:        return "关于本机";
    default:                return "";
    }
}

const char *get_item_name(uint8_t mode, uint8_t index)
{
    /* 调用方（界面循环）用 get_page_item_count 限定了 index，这里直接取 */
    switch (mode) {
    case MODE_ROOT:         return root_items[index];
    case MODE_BASIC:        return basic_items[index];
    case MODE_TOOL:         return tool_items[index];
    case MODE_THEME:        return theme_items[index];
    case MODE_RESET:        return reset_items[index];
    case MODE_PRESET:       return preset_items[index];
    case MODE_THEME_CHOICE: return theme_choice_items[index];
    case MODE_PARAM:        return param_items[index];
    default:                return "";
    }
}

const char *get_param_unit(uint8_t index)
{
    /* 只有校准值和功率有独立单位，其它参数没有后缀 */
    switch (index) {
    case 3U: return "raw";   /* 350℃ 校准原始值 */
    case 4U: return "%";     /* 输出功率 */
    default: return "";
    }
}

bool is_item_enterable(uint8_t mode, uint8_t index)
{
    /* 短按这一行会"进入下一页"或"立即执行"，界面就在右侧画一个
     * NK57 的实心三角 ▶ 提示可以进（取代了以前的 ">" 文本）。
     * 判定集中在这里，界面不要自己去猜 format_item_value 返回了什么。 */
    switch (mode) {
    case MODE_ROOT:
    case MODE_THEME:
    case MODE_RESET:
        /* 整页都是跳转/动作项 */
        return true;
    case MODE_TOOL:
        /* 第 3 行进快捷温度子页，第 4 行进参数调整页 */
        return (3U == index) || (4U == index);
    default:
        return false;
    }
}

static uint16_t clamp_u16(uint16_t value, uint16_t low, uint16_t high)
{
    /* 先压下限再压上限，保证返回值落在闭区间内 */
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

/* ---- 读当前值 ---- */

static uint16_t read_basic_value(uint8_t index)
{
    switch (index) {
    case 0U: return station.sound_enabled;
    case 1U: return station.brightness;
    case 2U: return station.temp_min;
    case 3U: return station.temp_max;
    case 4U: return station.temp_step;
    default: return 0U;
    }
}

static uint16_t read_tool_value(uint8_t index)
{
    uint8_t channel = get_active_channel();

    /* 第 3 行"快捷温度"和第 4 行"参数调整"是跳转项，没有值可读 */
    switch (index) {
    case 0U: return station.standby_temperature[channel];
    case 1U: return station.off_minutes[channel];
    case 2U: return station.sleep_minutes[channel];
    case 5U: return station.ball_switch_enabled;
    default: return 0U;
    }
}

/** 快捷温度页：三行分别对应三个预设槽，跟当前通道走 */
static uint16_t read_preset_value(uint8_t index)
{
    return station.preset_temperature[index];
}

static uint16_t read_param_value(uint8_t index)
{
    uint8_t channel = get_active_channel();

    switch (index) {
    case 0U: return station.kp[channel];
    case 1U: return station.ki_tenths[channel];
    case 2U: return station.kd[channel];
    case 3U: return station.cal_raw[channel];
    case 4U: return station.power_scale[channel];
    default: return 0U;
    }
}

static uint16_t read_menu_value(uint8_t mode, uint8_t index)
{
    /* 按页面分发到各自的读值函数，各页参数互不相干 */
    if (MODE_BASIC == mode) {
        return read_basic_value(index);
    }
    if (MODE_TOOL == mode) {
        return read_tool_value(index);
    }
    if (MODE_PRESET == mode) {
        return read_preset_value(index);
    }
    if (MODE_PARAM == mode) {
        return read_param_value(index);
    }
    return 0U;
}

/* ---- 写回用户改动 ---- */

static void write_basic_value(uint8_t index, uint16_t value)
{
    switch (index) {
    case 0U:
        station.sound_enabled = (uint8_t)value;
        break;
    case 1U:
        station.brightness = (uint8_t)value;
        /* 亮度要立刻生效，否则用户看不出调到哪一档 */
        set_backlight(station.brightness);
        break;
    case 2U:
        station.temp_min = value;
        /* 下限必须比上限至少低 5℃，否则钳位区间会颠倒 */
        if (station.temp_min >= station.temp_max) {
            station.temp_min = (uint16_t)(station.temp_max - 5U);
        }
        set_setpoint(station.set_temperature);
        break;
    case 3U:
        station.temp_max = value;
        /* 上限必须比下限至少高 5℃，否则钳位区间会颠倒 */
        if (station.temp_max <= station.temp_min) {
            station.temp_max = (uint16_t)(station.temp_min + 5U);
        }
        set_setpoint(station.set_temperature);
        break;
    case 4U:
        station.temp_step = value;
        break;
    default:
        break;
    }
}

static void write_tool_value(uint8_t index, uint16_t value)
{
    uint8_t channel = get_active_channel();

    /* 只写用户改得动的行；第 3、4 行是跳转项，没有值可写 */
    switch (index) {
    case 0U:
        station.standby_temperature[channel] = value;
        break;
    case 1U:
        /* 待机时间对 C245/C210 锁定为 0（见 clamp_tool_value 的说明）。
         * clamp 已经归零，这里再挡一次写入，避免绕过钳位直接落盘。 */
        station.off_minutes[channel] = (0U == channel) ? value : 0U;
        /* 改完时间立刻按新值重装倒计时，不用等下一次插拔 */
        reload_timers();
        break;
    case 2U:
        station.sleep_minutes[channel] = value;
        reload_timers();
        break;
    case 5U:
        station.ball_switch_enabled = (uint8_t)value;
        break;
    default:
        break;
    }
}

/**
 * @brief 写入某个快捷温度槽
 * @note  两个数组必须一起改：preset_temperature 是主界面和 PID 用的当前值，
 *        channel_presets 是掉电保存用的每通道表，只改一个就会出现
 *        "调完还在用，重启就变回去"。
 */
static void write_preset_value(uint8_t index, uint16_t value)
{
    uint8_t channel = get_active_channel();

    station.preset_temperature[index] = value;
    station.channel_presets[channel][index] = value;

    /* 改的正好是当前选中的那一档时，工作设定值要跟着走，
     * 否则主界面会出现"卡片显示 330℃、设定显示 320℃"的矛盾 */
    if (station.preset_index == index) {
        set_setpoint(value);
    }
}

static void write_param_value(uint8_t index, uint16_t value)
{
    uint8_t channel = get_active_channel();

    /* 参数按行号写回当前通道；校准值还要同步给正在使用的烙铁头 */
    switch (index) {
    case 0U:
        station.kp[channel] = value;
        break;
    case 1U:
        station.ki_tenths[channel] = (uint8_t)value;
        break;
    case 2U:
        station.kd[channel] = value;
        break;
    case 3U:
        station.cal_raw[channel] = value;
        /* 只有正在用这个通道时才更新实时校准值，否则改了看不到效果 */
        if (station.tip_channel == channel) {
            station.cal_raw_at_350 = (float)value;
        }
        break;
    case 4U:
        station.power_scale[channel] = (uint8_t)value;
        break;
    default:
        break;
    }
}

static void write_menu_value(uint8_t mode, uint8_t index, uint16_t value)
{
    /* 按页面分发到各自的写值函数 */
    if (MODE_BASIC == mode) {
        write_basic_value(index, value);
    } else if (MODE_TOOL == mode) {
        write_tool_value(index, value);
    } else if (MODE_PRESET == mode) {
        write_preset_value(index, value);
    } else if (MODE_PARAM == mode) {
        write_param_value(index, value);
    }
}

/* ---- 合法范围 ---- */

static uint16_t clamp_basic_value(uint8_t index, uint16_t value)
{
    switch (index) {
    case 0U: return (value != 0U) ? 1U : 0U;
    case 1U: return clamp_u16(value, 0U, 10U);
    case 2U: return clamp_u16(value, 50U, (uint16_t)(station.temp_max - 5U));
    case 3U: return clamp_u16(value, (uint16_t)(station.temp_min + 5U), 500U);
    case 4U: return clamp_u16(value, 1U, 20U);
    default: return value;
    }
}

static uint16_t clamp_tool_value(uint8_t index, uint16_t value)
{
    switch (index) {
    case 0U: return clamp_u16(value, 0U, 250U);
    case 1U:
        /* **"待机时间"对 C245/C210 锁定为 0**（用户 2026-10-24 决定）
         *
         * 原机行为：C245/C210 下这一项**也可以调整**，但对它们没有意义 ——
         * 它们靠**休眠座**进出待机，闲置计时这条路只对 T12 接通
         * （原机 `ctx+0x5A+ch = {1,0,0}`，只有 T12 = 1 时启用 PA9 闲置计时）。
         *
         * 本工程选择**直接把值锁在 0**，而不是"能调但不起作用"：
         *   · 界面上不会出现一个实际不生效的数字，避免让人以为设了定时待机；
         *   · 这一行仍然是"可进入"的（菜单结构不变，用户能看到它是关的）。
         * 想恢复成"原机那样可调"，把下面这个 return 0U 去掉即可 ——
         * 效果等价，因为 auto_off_enabled[1]/[2] == 0 已经让计时不生效。 */
        if (0U != get_active_channel()) {
            return 0U;
        }
        return clamp_u16(value, 0U, 120U);
    case 2U: return clamp_u16(value, 0U, 120U);
    case 5U: return (value != 0U) ? 1U : 0U;
    default: return value;
    }
}

/** 快捷温度只能落在用户自己设的上下限之间，否则一按就报超温 */
static uint16_t clamp_preset_value(uint16_t value)
{
    return clamp_u16(value, station.temp_min, station.temp_max);
}

static uint16_t clamp_param_value(uint8_t index, uint16_t value)
{
    switch (index) {
    case 0U:
    case 1U: return clamp_u16(value, 1U, 100U);
    case 2U: return clamp_u16(value, 0U, 100U);
    case 3U: return clamp_u16(value, 100U, 4000U);
    case 4U: return clamp_u16(value, 10U, 100U);
    default: return value;
    }
}

static uint16_t clamp_menu_value(uint8_t mode, uint8_t index, uint16_t value)
{
    /* 每个页面的合法范围不同，分发到各自的钳位函数 */
    if (MODE_BASIC == mode) {
        return clamp_basic_value(index, value);
    }
    if (MODE_TOOL == mode) {
        return clamp_tool_value(index, value);
    }
    if (MODE_PRESET == mode) {
        return clamp_preset_value(value);
    }
    if (MODE_PARAM == mode) {
        return clamp_param_value(index, value);
    }
    return value;
}

static uint16_t get_menu_step(uint8_t mode, uint8_t index)
{
    /* 步进按页面查常量表，比 switch 短且不会漏 case */
    if (MODE_BASIC == mode) {
        return basic_steps[index];
    }
    if (MODE_TOOL == mode) {
        return tool_steps[index];
    }
    if (MODE_PRESET == mode) {
        return preset_steps[index];
    }
    if (MODE_PARAM == mode) {
        return param_steps[index];
    }
    return 1U;
}

/* ---- 显示文本 ---- */

static const char *format_basic_value(uint8_t index, uint16_t value,
                                      char *buffer, uint32_t size)
{
    switch (index) {
    case 0U:
        return (0U != value) ? "开" : "关";
    case 1U:
        (void)snprintf(buffer, size, "%u", (unsigned)value);
        return buffer;
    case 2U:
    case 3U:
    case 4U:
        (void)snprintf(buffer, size, "%u°C", (unsigned)value);
        return buffer;
    default:
        return "";
    }
}

static const char *format_tool_value(uint8_t index, uint16_t value,
                                     char *buffer, uint32_t size)
{
    switch (index) {
    case 0U:
        /* 待机温度 0 表示关掉待机功能 */
        if (0U == value) {
            return "关";
        }
        (void)snprintf(buffer, size, "%u°C", (unsigned)value);
        return buffer;
    case 1U:
    case 2U:
        /* 时间 0 表示"从不"，不要显示成 0 分。
         * 待机时间（index 1）在 C245/C210 下恒为 0 —— 它们靠休眠座，
         * 所以这一行会自然显示成"关"，语义正确，不需要额外分支。 */
        if (0U == value) {
            return "关";
        }
        (void)snprintf(buffer, size, "%u分", (unsigned)value);
        return buffer;
    case 3U:
    case 4U:
        /* 第 3、4 行是跳转项（进快捷温度子页 / 参数页），没有数值；
         * "可以进入"由界面上的实心三角 ▶ 表示，不再返回 ">" */
        return "";
    case 5U:
        return (0U != value) ? "开" : "关";
    default:
        return "";
    }
}

/** 快捷温度页三行都显示成 xxx°C */
static const char *format_preset_value(uint16_t value, char *buffer,
                                       uint32_t size)
{
    (void)snprintf(buffer, size, "%u°C", (unsigned)value);
    return buffer;
}

static const char *format_param_value(uint8_t index, uint16_t value,
                                      char *buffer, uint32_t size)
{
    /* Ki 存的是 ×10，显示时补小数点；其余参数直接按整数显示 */
    if (1U == index) {
        (void)snprintf(buffer, size, "%u.%u", (unsigned)(value / 10U),
                       (unsigned)(value % 10U));
        return buffer;
    }
    (void)snprintf(buffer, size, "%u", (unsigned)value);
    return buffer;
}

const char *format_item_value(uint8_t mode, uint8_t index, char *buffer, uint32_t size)
{
    uint16_t value;
    uint8_t current;

    /* 纯跳转项没有数值可显示；界面用实心三角 ▶ 表示"可以进入"，
     * 所以这里返回空串，不再返回 ">"。 */
    if ((MODE_ROOT == mode) || (MODE_THEME == mode) || (MODE_RESET == mode)) {
        return "";
    }
    /* 关于页没有右侧值 */
    if (MODE_ABOUT == mode) {
        return "";
    }

    /* 主题选择页：把当前生效的那一项标成"已选" */
    if (MODE_THEME_CHOICE == mode) {
        current = station.theme;
        return (index == current) ? "已选" : "";
    }

    /* 编辑中先显示正在改的值，确认后才写回 station */
    value = ((0U != station.menu_editing) && (index == station.menu_selected))
                ? station.edit_value
                : read_menu_value(mode, index);

    /* 按页面类型分发格式化，避免一个大 switch 套 switch */
    if (MODE_BASIC == mode) {
        return format_basic_value(index, value, buffer, size);
    }
    if (MODE_TOOL == mode) {
        return format_tool_value(index, value, buffer, size);
    }
    if (MODE_PRESET == mode) {
        return format_preset_value(value, buffer, size);
    }
    return format_param_value(index, value, buffer, size);
}

/* ---- 菜单动作 ---- */

static void beep_key_press(void)
{
    /* 静音设置下按键不响 */
    if (0U != station.sound_enabled) {
        beep_buzzer(30U);
    }
}

static void enter_page(uint8_t mode)
{
    station.ui_mode = mode;
    station.menu_selected = 0U;
    station.menu_editing = 0U;
}

static void go_back(void)
{
    /* 返回目标由页面层级决定，不能一律回根页面 */
    switch (station.ui_mode) {
    case MODE_PARAM:
        /* 参数页返回工具设置，并停在"参数调整"这一行 */
        enter_page(MODE_TOOL);
        station.menu_selected = 4U;
        break;
    case MODE_PRESET:
        /* 快捷温度页返回工具设置，停在"快捷温度"这一行 */
        enter_page(MODE_TOOL);
        station.menu_selected = 3U;
        break;
    case MODE_THEME_CHOICE:
        /* 从主题选择返回时停在"系统主题"这一行 */
        enter_page(MODE_THEME);
        station.menu_selected = 0U;
        break;
    case MODE_BASIC:
    case MODE_TOOL:
    case MODE_THEME:
    case MODE_RESET:
    case MODE_ABOUT:
        enter_page(MODE_ROOT);
        break;
    default:
        close_menu();
        break;
    }
}

static void commit_edit(void)
{
    write_menu_value(station.ui_mode, station.menu_selected,
                     clamp_menu_value(station.ui_mode, station.menu_selected,
                                      station.edit_value));
    station.menu_editing = 0U;
    /* 统一等回到主界面再写 Flash，别在 5 ms 任务里擦扇区 */
    station.settings_dirty = 1U;
}

static void open_selected_item(void)
{
    uint8_t selected = station.menu_selected;

    /* 不同页面的"短按选中项"含义不同，按页面分发 */
    switch (station.ui_mode) {
    case MODE_ROOT:
        switch (selected) {
        case 0U: enter_page(MODE_BASIC); break;
        case 1U: enter_page(MODE_TOOL); break;
        case 2U: enter_page(MODE_THEME); break;
        case 3U: enter_page(MODE_RESET); break;
        case 4U: enter_page(MODE_ABOUT); break;
        default: close_menu(); break;
        }
        break;

    case MODE_TOOL:
        /* 第 3 行进快捷温度子页，第 4 行进参数页，其余行是可编辑的数值 */
        if (3U == selected) {
            enter_page(MODE_PRESET);
        } else if (4U == selected) {
            enter_page(MODE_PARAM);
        } else {
            station.menu_editing = 1U;
            station.edit_value = read_menu_value(MODE_TOOL, selected);
        }
        break;

    case MODE_THEME:
        /* 主题风格下只剩"系统主题"一行 */
        enter_page(MODE_THEME_CHOICE);
        station.menu_selected = (0U != station.theme) ? 1U : 0U;
        break;

    case MODE_BASIC:
        station.menu_editing = 1U;
        station.edit_value = read_menu_value(MODE_BASIC, selected);
        break;

    case MODE_PRESET:
        /* 快捷温度三行各自独立可编辑。进入编辑（金色高亮）后转动调值，
         * 再短按由 handle_edit_short() → commit_edit() 保存回青色选中态。
         * 这里只需要负责"进入编辑"；之前整个分支缺失，短按落进 default
         * 什么都不做，表现就是"快捷温度三行怎么按都改不动"。 */
        station.menu_editing = 1U;
        station.edit_value = read_menu_value(MODE_PRESET, selected);
        break;

    case MODE_ABOUT:
        go_back();
        break;

    case MODE_PARAM:
        /* 参数页：第一次进入编辑，第二次提交 */
        if (0U == station.menu_editing) {
            station.menu_editing = 1U;
            station.edit_value = read_menu_value(MODE_PARAM, selected);
        } else {
            commit_edit();
        }
        break;

    default:
        break;
    }

}

static void wake_from_sleep(void)
{
    /* 不在休眠/待机就没什么可唤醒的 */
    if (0U == (station.state & (FLAG_SLEEP | FLAG_STANDBY))) {
        return;
    }

    /* **从休眠醒来回到待机，不是直接回工作温度**（v9.14）
     *
     * 状态链是：工作 →(休眠座/待机时间)→ 待机(保温) →(休眠时间到)→ 休眠(挂起)。
     * 所以休眠是"待机的延长"，碰一下理应回到**待机**那一格：
     *   · 温度回到待机温度保温（黄框显示待机温度）
     *   · 用户要继续干活，再从待机回到工作温度（离开休眠座 / 动滚珠）
     * 若直接跳到工作温度，休眠座还压着时会被立刻打回待机、
     * 滚珠开关没动却满功率加热，两种都不可接受。
     *
     * 从**待机**唤醒（PA4/PA9 方向）则正常回工作温度，那两个来源
     * 本来就直接改标志，不走这个函数。 */
    if (0U != (station.state & FLAG_SLEEP)) {
        /* 休眠 → 待机：保留保温语义，等物理开关把它带回工作温度 */
        station.state &= (uint16_t)(~FLAG_SLEEP);
        station.state |= FLAG_STANDBY;
        station.sleep_timer_ms =
            (uint32_t)station.sleep_minutes[get_active_channel()] * 60000UL;
    } else {
        /* 待机 → 工作 */
        station.state &= (uint16_t)(~FLAG_STANDBY);
    }
    reload_timers();

    /* 唤醒后抑制待机判定（v9.13）：用户按键唤醒时手柄通常还压在座子上，
     * 不压住的话下一毫秒 app_check 又把人打进待机，
     * 表现为"菜单开着但温度显示 0、加热锁死"。等 PA4 真抬起会自动解除。 */
    suppress_standby();

    /* 唤醒一律回到主页：休眠/待机时不可能正处在菜单里
     * （菜单期间被禁止进这两态，见 task_check 的 FLAG_MENU 闸） */
    close_menu();
}

/** 主界面短按：按 0→1→2→0 循环三个快捷温度 */
static void cycle_preset_temperature(void)
{
    /* 当前是自定义温度（0xFF）时，第一次按回到预设 0 */
    if (station.preset_index >= PRESET_COUNT) {
        station.preset_index = 0U;
    } else {
        station.preset_index = (uint8_t)((station.preset_index + 1U) % PRESET_COUNT);
    }
    set_setpoint(station.preset_temperature[station.preset_index]);
}

/** 主题选择页：短按选中并返回；不是这一页就返回 false */
static bool handle_choice_short(void)
{
    /* 主题选择是"选中即返回主题页" */
    if (MODE_THEME_CHOICE == station.ui_mode) {
        station.theme = (0U == station.menu_selected) ? 0U : 1U;
        station.settings_dirty = 1U;
        enter_page(MODE_THEME);
        station.menu_selected = 0U;
        return true;
    }
    return false;
}

/** 重置/关于页：短按立即执行；不是这两页就返回 false */
static bool handle_action_short(void)
{
    /* 动作类页面：短按立即执行，不再二次确认 */
    if (MODE_RESET == station.ui_mode) {
        /* 第 0 行恢复出厂配置，第 1 行软复位 */
        if (0U == station.menu_selected) {
            reset_config();
        } else {
            reset_system();
        }
        return true;
    }
    /* 关于页只读，短按直接返回 */
    if (MODE_ABOUT == station.ui_mode) {
        go_back();
        return true;
    }
    return false;
}

/** 数值编辑：第一次短按进入，第二次短按提交 */
static void handle_edit_short(void)
{
    /* 参数页：第一次短按进入编辑，第二次短按提交 */
    if (MODE_PARAM == station.ui_mode) {
        open_selected_item();
        return;
    }
    /* 编辑中短按 = 提交当前值；不在编辑态才进入编辑 */
    if (0U != station.menu_editing) {
        commit_edit();
        return;
    }
    open_selected_item();
}

static void handle_short_press(void)
{
    /* **只有休眠才需要短按唤醒**（v9.14 纠正）
     *
     * 休眠 = 工具设置里的"休眠时间"到点后焊台挂起，大数字显示「--」，
     *        这时焊台真的停摆了，按一下唤醒是唯一有意义的动作。
     *
     * 待机 ≠ 休眠：C245/C210 靠手柄休眠座、T12 靠手柄内滚珠开关，
     *        由**物理开关**决定进出，焊台仍在保温（黄框显示待机温度）。
     *        用户实测：这种状态下**短按没有功能**，只有长按进菜单。
     *        v9.13 曾把待机也当唤醒点，属于把两种状态混为一谈。 */
    if (0U != (station.state & FLAG_SLEEP)) {
        wake_from_sleep();
        beep_key_press();
        return;
    }

    /* 主界面短按只切快捷温度，不进菜单 */
    if (MODE_MAIN == station.ui_mode) {
        cycle_preset_temperature();
        beep_key_press();
        return;
    }

    /* 菜单里的短按：选择页 → 动作页 → 数值编辑，命中一类就结束 */
    if (handle_choice_short()) {
        beep_key_press();
        return;
    }
    /* 动作页（重置/关于）命中就结束，别继续走编辑逻辑 */
    if (handle_action_short()) {
        beep_key_press();
        return;
    }
    handle_edit_short();
    beep_key_press();
}

static void handle_long_press(void)
{
    /* 长按进菜单。两种"非正常"状态分开处理（v9.14 纠正）：
     *
     *   · 休眠（超时挂起，大数字「--」）：长按要**先唤醒**，再进菜单，
     *     这样休眠时一步就能操作，不用先短按唤醒。
     *   · 待机（休眠座/滚珠开关，仍在保温）：**根本不需要唤醒** ——
     *     它不是睡眠，只是保温态。直接进菜单即可。
     *
     * 注意不能写成 `FLAG_SLEEP | FLAG_STANDBY` 一起判（v9.13 的错误）：
     * 那样待机时也会走 wake_from_sleep()，而它会 close_menu() 并把
     * 状态清掉，虽然最后菜单能开，但多了一次无谓的状态翻转。 */
    if (0U != (station.state & FLAG_SLEEP)) {
        wake_from_sleep();
    }

    /* 主界面长按才进菜单，菜单里长按是返回 */
    if (MODE_MAIN == station.ui_mode) {
        open_menu();
        beep_key_press();
        return;
    }

    /* 编辑中长按 = 取消，不保存 */
    if (0U != station.menu_editing) {
        station.menu_editing = 0U;
        beep_key_press();
        return;
    }

    go_back();
    beep_key_press();
}

/**
 * @brief 主界面转动：按温度步进改设定值
 *
 * **待机时改的是"待机温度"，正常时改的是"工作温度"**（v9.14）
 *
 * 为什么必须分开：黄框显示的永远是 `get_setpoint()` 的结果 ——
 * 待机时返回 `standby_temperature[channel]`、正常时返回 `set_temperature`。
 * 如果待机时仍去改 `set_temperature`，用户转编码器**屏幕上什么都不会变**
 * （因为显示的是另一个变量），还以为按键坏了。
 *
 * 用户实测原机行为：C245/C210 放在休眠座上转动编码器，
 * 改的就是黄框里那个（待机）温度值。
 */
static void adjust_main_temperature(int16_t delta)
{
    int32_t value;

    /* 待机改待机温度；其余情况改工作温度 */
    if (0U != (station.state & FLAG_STANDBY)) {
        uint8_t channel = get_active_channel();

        value = (int32_t)station.standby_temperature[channel] +
                ((int32_t)delta * (int32_t)station.temp_step);
        if (value < (int32_t)station.temp_min) {
            value = (int32_t)station.temp_min;
        }
        if (value > (int32_t)station.temp_max) {
            value = (int32_t)station.temp_max;
        }
        station.standby_temperature[channel] = (uint16_t)value;
        return;
    }

    /* 用 int32 中间量再钳位，避免 16 位加减在边界溢出 */
    value = (int32_t)station.set_temperature +
            ((int32_t)delta * (int32_t)station.temp_step);

    /* 先粗钳到上下限，防止 16 位加减在边界溢出 */
    if (value < (int32_t)station.temp_min) {
        value = (int32_t)station.temp_min;
    }
    /* 再压上限，两端都钳住才安全 */
    if (value > (int32_t)station.temp_max) {
        value = (int32_t)station.temp_max;
    }
    station.set_temperature = (uint16_t)value;
    /* 手动调温后就不再属于任何快捷预设 */
    station.preset_index = 0xFFU;
}

/** 编辑中转动：按该行的步进调值 */
static void adjust_edit_value(int16_t delta)
{
    uint16_t step = get_menu_step(station.ui_mode, station.menu_selected);
    int32_t value = (int32_t)station.edit_value + ((int32_t)delta * (int32_t)step);

    /* 先钳到 uint16 范围，再按页面规则细钳 */
    if (value < 0) {
        value = 0;
    }
    if (value > 65535) {
        value = 65535;
    }
    station.edit_value = clamp_menu_value(station.ui_mode, station.menu_selected,
                                          (uint16_t)value);
}

/** 非编辑状态转动：在菜单行之间环形移动 */
static void move_menu_selection(int16_t delta)
{
    uint8_t count = get_page_item_count(station.ui_mode);
    int32_t value;

    /* 关于页没有可选项 */
    if (0U == count) {
        return;
    }

    value = (int32_t)station.menu_selected + (int32_t)delta;
    /* C 的 % 对负数会给负结果，先补到非负才能环形滚动 */
    while (value < 0) {
        value += (int32_t)count;
    }
    station.menu_selected = (uint8_t)(value % (int32_t)count);
}

static void handle_encoder_turn(int16_t delta)
{
    /* 没转动就别改状态，也省掉后面的蜂鸣 */
    if (0 == delta) {
        return;
    }

    /* **只有休眠才需要转动唤醒**；待机下转动要能改设定温度（v9.14 纠正）
     *
     * 待机（休眠座/滚珠开关，仍在保温）时黄框显示的是待机温度，
     * 用户实测原机行为就是"待在座子上转编码器能改那个温度"。
     * v9.13 把待机也拦下来去唤醒，等于把该功能关掉了，属过度修正。
     *
     * 休眠（超时挂起、大数字「--」、设定值 0）时黄框没有可调的意义，
     * 转动只唤醒，不偷偷改隐藏的工作温度。 */
    if (0U != (station.state & FLAG_SLEEP)) {
        wake_from_sleep();
        return;
    }

    /* 主界面转 = 调工作温度；菜单转 = 选行或调值 */
    if (MODE_MAIN == station.ui_mode) {
        adjust_main_temperature(delta);
        return;
    }
    /* 关于页只读，转动没有意义 */
    if (MODE_ABOUT == station.ui_mode) {
        return;
    }
    /* 编辑中转动是调当前值，不是换行 */
    if (0U != station.menu_editing) {
        adjust_edit_value(delta);
        return;
    }
    move_menu_selection(delta);
}

/* ---- 按键事件 ---- */

static key_event_t handle_key_change(uint8_t raw, uint32_t now)
{
    /* raw=1 是按下，raw=0 是松开 */
    if (0U != raw) {
        /* 按下：记住时间，准备判长按 */
        btn_press_ms = now;
        long_sent = false;
        return EVENT_NONE;
    }
    /* 松开：没发过长按才算一次短按 */
    return long_sent ? EVENT_NONE : EVENT_SHORT;
}

static key_event_t read_key_event(void)
{
    uint8_t raw = is_button_down() ? 1U : 0U;
    uint32_t now = get_tick_ms();

    /* 原始电平一变就重置消抖计数，抖动的每一次跳变都要重新确认 */
    if (raw != btn_raw) {
        btn_raw = raw;
        btn_count = 0U;
    } else if (btn_count < BTN_DEBOUNCE_COUNT) {
        btn_count++;
    }

    /* 连续 4 次（20 ms）电平一致才认账 */
    if ((btn_count >= BTN_DEBOUNCE_COUNT) && (btn_stable != raw)) {
        btn_stable = raw;
        return handle_key_change(raw, now);
    }

    /* 按住超过 800 ms 只发一次长按；松手时因为 long_sent=1 不再补发短按 */
    if ((0U != btn_stable) && (!long_sent) && ((now - btn_press_ms) >= BTN_LONG_MS)) {
        long_sent = true;
        return EVENT_LONG;
    }

    return EVENT_NONE;
}

void init_indev(void)
{
    /* 上电时按键可能已经按着，先读一次当作稳态，避免误触发 */
    btn_raw = is_button_down() ? 1U : 0U;
    btn_stable = btn_raw;
    btn_count = BTN_DEBOUNCE_COUNT;
    long_sent = false;
    btn_press_ms = 0UL;
}

void task_indev(void)
{
    key_event_t event = read_key_event();
    int16_t delta = take_encoder_delta();

    /* 没插烙铁头时屏蔽整套输入：这时唯一有意义的动作就是插上工具。
     * 事件照读不误、读完直接丢掉 —— 编码器增量是累加的，不取就会攒着，
     * 等插上工具的一瞬间把攒下来的步数一次性爆发出来。
     * 同时强制回主页并清掉菜单标志：
     *   * 界面据此切到"无工具"告警页（resolve_page 看 ui_mode + 工具在位），
     *     所以**在菜单里拔掉工具不会卡在菜单**；
     *   * 菜单标志一并清掉，否则加热会被一直锁住，插回工具后也升不了温。 */
    if (!is_tip_present()) {
        if (MODE_MAIN != station.ui_mode) {
            close_menu();
        }
        return;
    }

    /* 有按键事件才处理；编码器增量每拍都要取，不能漏 */
    if (EVENT_SHORT == event) {
        handle_short_press();
    } else if (EVENT_LONG == event) {
        handle_long_press();
    }

    handle_encoder_turn(delta);

    /* 每转一格响一声；静音设置关掉时完全不响 */
    if ((0 != delta) && (0U != station.sound_enabled)) {
        beep_buzzer(20U);
    }
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
