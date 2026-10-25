/**
 *******************************************************************************
 * @file  app_ui.c
 * @brief 界面渲染：主界面 + 无工具告警 + 设置菜单 + 参数曲线
 *
 * 版式依据 docs/ui/sgl_new_full_UI.html（320×240）：
 *   主界面：顶栏（型号/主页/状态徽标）、青色温度大面板、黄色设定框、
 *           橙色电压电流框、MODE 框、底部三个快捷温度
 *   无工具：深色顶栏（---- + 黄色 Warning 胶囊）+ 黄色边框面板 +
 *           52px "无工具" + 固定提示"请插入工具或检查烙铁头"
 *   菜单  ：34px 顶栏（型号胶囊 + 金色标题胶囊）+ 提示行 + 行卡片，
 *           一屏 5 行、超过 5 行滚动；**可进入的行**右侧画一个 NK57 实心三角
 *           ▶（取代以前的 ">"），其它行右侧画数值
 *   参数页：参数名/值/单位 + 下半屏实时温度曲线
 *
 * 字重约定：数字/读数/徽标用 NK57 Cd Rg（HTML 的 600），
 *           常规小字用 NK57 Cd Sb（HTML 的 400/500），
 *           中文一律 cjk16/cjk52（NK57 没有汉字）。
 *
 * 省 RAM 的关键：SGL 用 10 行行缓冲分块渲染，不做整屏帧缓冲。
 * 省重绘的关键：文本/颜色/宽度都做"上次值"缓存，只有真正变化才碰控件。
 *******************************************************************************
 */
#include "app_ui.h"
#include "app_indev.h"
#include "app_station.h"
#include "bsp_clk.h"
#include "font_arrow16.h"
#include "font_cjk16.h"
#include "font_cjk52.h"
#include "font_num16.h"
#include "font_num28.h"
#include "font_num32.h"
#include "font_num90.h"
#include "font_ui11.h"
#include "font_ui11s.h"
#include "font_ui13.h"
#include "font_ui14.h"
#include "font_ui14s.h"

#include <sgl.h>

#include <stdio.h>
#include <string.h>

/* 曲线 60 点、每 500 ms 采一次，满屏正好 30 秒历史 */
#define CURVE_POINT_COUNT   (60)

/* 菜单一屏 5 行（MENU_VISIBLE_ROWS），每页最多 6 项，多出来的靠滚动 */
#define MENU_HEADER_H       (34)
#define MENU_HINT_Y         (38)
#define MENU_HINT_H         (20)
#define MENU_ROW_TOP        (66)
#define MENU_ROW_PITCH      (33)
#define MENU_ROW_HEIGHT     (29)
#define MENU_ROW_LEFT       (12)
#define MENU_ROW_WIDTH      (296)

/* 行内三段：名称 / 数值 / "可进入"三角。
 * 数值列右边与行内容右边缘对齐（行 12..308，左右各留 12 px = 296）；
 * 三角 ▶ 占数值列最右 14 px —— 它取代了以前的 ">" 文本，两者不会同时出现。 */
#define MENU_NAME_X         (24)
#define MENU_NAME_W         (144)
#define MENU_VALUE_X        (176)
#define MENU_VALUE_W        (120)
#define MENU_ARROW_X        (282)
#define MENU_ARROW_W        (14)

/* 顶栏胶囊：型号胶囊固定 56 宽，金色标题胶囊跟在后面。
 * 标题胶囊有长短两档，长度与主页那颗（64）对齐：
 *   2 个字（设置）用短档，4 个字（基本设置/快捷温度……）用长档。
 * 切换只改 rect 与 label 的宽度，不改 x —— 左侧起点固定在 CHIP_X。 */
#define CHIP_X              (77)
#define CHIP_GOLD_SHORT_W   (64)    /* 2 个汉字：与主页金色胶囊同长 */
#define CHIP_GOLD_LONG_W    (96)    /* 4 个汉字 */
#define CHIP_H              (24)
#define CHIP_Y              (5)
/* 一屏 5 行菜单最多 4 个汉字（"基本设置"），第 7 字节起就是第二个汉字之后；
 * 用字节长度判断字数：UTF-8 每个汉字 3 B，4 字 → 12 B，2 字 → 6 B。 */
#define CHIP_GOLD_WIDE_UTF8 (6U)

/* 关于本机是四行纯文本，不画卡片 */
#define ABOUT_LINE_TOP      (62)
#define ABOUT_LINE_PITCH    (30)

/* 输出条 166 px，对应 0…250 的硬件占空比 */
#define DUTY_BAR_WIDTH      (166U)
#define DUTY_BAR_HEIGHT     (6U)
#define DUTY_FULL_SCALE     (250U)

/* 主界面版式常量：全部来自 HTML 的绝对定位，改版式只改这里 */
#define SCREEN_WIDTH        (320)
#define SCREEN_HEIGHT       (240)
#define MAIN_PANEL_X        (8)
#define MAIN_PANEL_Y        (36)
#define MAIN_PANEL_W        (198)
#define MAIN_PANEL_H        (148)
/* 温度数字 90px、单位 28px：两个标签的行盒高度都恰好等于各自字库的
 * font_height，标签 y 就是行盒顶端，墨迹位置由字库自身度量决定（见下）。 */
#define MAIN_TEMP_BASE_Y    (60)
#define DUTY_BAR_Y          (164)
#define RIGHT_PANEL_X       (214)
#define SET_BOX_Y           (36)
#define SET_BOX_H           (62)
#define POWER_BOX_Y         (106)
#define POWER_BOX_H         (48)
#define MODE_BOX_Y          (162)
#define MODE_BOX_H          (22)
#define PRESET_TOP          (192)
#define PRESET_W            (96)
#define PRESET_H            (31)
#define PRESET_X0           (8)
#define PRESET_GAP          (104)

/* ---- 静态排版常量（离线算好，运行时不量字宽） ----------------------
 *
 * SGL 把标签里的文字画在对象内部，墨迹高度由字库自带度量决定：
 *   字形墨迹底边（相对行盒顶端） = font_height + bbox_bottom - base_line
 * 两个不同字号要"底边平齐"，小字号那个行盒必须相对大字号下移
 *     dy = (fh_big + b_big - bl_big) - (fh_small + b_small - bl_small)
 * 下面这些数字全部来自 tools/font_metrics.py（读的字体/字号与
 * tools/gen_fonts.sh 完全一致），固件里只留加法和常量：
 *
 *   num32: fh=40  bl=8  b('0')=0  -> 墨迹底边 32
 *   num16: fh=20  bl=4  b('°')=0  -> 墨迹底边 16
 *   num90: fh=109 bl=20 b('0')=1  -> 墨迹底边 90
 *   num28: fh=35  bl=7  b('°')=0  -> 墨迹底边 28
 *
 * 改字号/字重后必须重跑 `python3 tools/font_metrics.py`，把下面六个数
 * 连同 gen_fonts.sh 一起更新；写错的表现是"°C 比数字高一截或低一截"。 */
#define NUM32_FONT_HEIGHT   (40)
#define NUM32_BASE_LINE     (8)
#define NUM32_DIGIT_BOTTOM  (0)         /* num32 '0' 的 bbox_bottom */
#define NUM16_FONT_HEIGHT   (20)
#define NUM16_BASE_LINE     (4)
#define NUM16_UNIT_BOTTOM   (0)         /* num16 '°' 的 bbox_bottom */
#define NUM90_FONT_HEIGHT   (109)
#define NUM90_BASE_LINE     (20)
#define NUM90_DIGIT_BOTTOM  (1)         /* num90 '0' 的 bbox_bottom */
#define NUM28_FONT_HEIGHT   (35)
#define NUM28_BASE_LINE     (7)
#define NUM28_UNIT_BOTTOM   (0)         /* num28 '°' 的 bbox_bottom */

/* 每字前进宽度（像素）。NK57 与微米黑 Mono 都是等宽字体，一个字符一个
 * 定值，字符串宽度 = 字数 × 单字宽 —— 所以居中位置能离线算成常量。 */
#define NUM32_ADV           (16)    /* 设定值数字 */
#define NUM16_ADV           (8)     /* 设定值单位 °C */
#define NUM90_ADV           (44)    /* 温度数字 */
#define NUM28_ADV           (14)    /* 温度单位 °C */

/* cjk16 是**混排字库**：汉字取自微米黑 Mono，ASCII 与 °±×÷Ωμ▶ 等符号
 * 取自 NK57 —— 生成期就合并进同一个 sgl_font_t，所以一个标签能同时
 * 渲染 "100°C" 和 "30分"。两种字形的前进宽度不同，算字符串宽度时要分开数：
 *     宽度 = 汉字数 × CJK16_ADV + ASCII/符号数 × CJK16_ASCII_ADV
 * 行盒度量统一取 NK57 的 20/4（能容纳两套字形的全部上下沿）。 */
#define CJK16_ADV           (16)    /* cjk16 汉字（微米黑 Mono） */
#define CJK16_ASCII_ADV     (8)     /* cjk16 ASCII 与符号（NK57 Cd Rg） */
#define CJK16_FONT_HEIGHT   (20)    /* 混排后统一行高 */
#define CJK16_BASE_LINE     (4)     /* 混排后统一基线 */

/* 数字与单位的间隔：设定框 4 px（HTML 的 margin-left）；温度面板 4 px */
#define SET_GROUP_GAP       (4)
#define TEMP_GROUP_GAP      (4)

/* ---- 设定框：32 px 数字 + 16 px 单位 °C -----------------------------
 * 数字行盒顶端固定在 SET_DIGITS_Y；单位行盒顶端 = 数字行盒顶端 + 偏移量，
 * 偏移量让两者的**墨迹底边**落在同一条水平线上（用户要求"符号底部和
 * 显示的数字底部对齐"）。偏移只跟两个字库的度量有关，与数值无关。 */
#define SET_DIGITS_Y        (60)
#define SET_UNIT_Y_OFFSET   ((NUM32_FONT_HEIGHT + NUM32_DIGIT_BOTTOM - NUM32_BASE_LINE) \
                             - (NUM16_FONT_HEIGHT + NUM16_UNIT_BOTTOM - NUM16_BASE_LINE))
#define SET_UNIT_Y          (SET_DIGITS_Y + SET_UNIT_Y_OFFSET)

/* "数字 + 单位"整体在 98 px 框里居中；位数只有 1…3 三种，
 * 直接写成常量表，运行时不量宽度。x = 214 + (98 - 组宽) / 2 */
#define SET_DIGITS_X_1      (245)   /* "5°C"   */
#define SET_DIGITS_X_2      (237)   /* "50°C"  */
#define SET_DIGITS_X_3      (229)   /* "300°C" */

/* ---- 温度大面板：90 px 数字 + 28 px 单位 °C -------------------------
 * 同样让墨迹底边平齐；水平方向按设计稿的 translateX(-10) 再左移 10。 */
#define MAIN_TEMP_DIGIT_Y   (MAIN_TEMP_BASE_Y)
#define MAIN_TEMP_UNIT_Y    (MAIN_TEMP_DIGIT_Y + \
                             ((NUM90_FONT_HEIGHT + NUM90_DIGIT_BOTTOM - NUM90_BASE_LINE) \
                              - (NUM28_FONT_HEIGHT + NUM28_UNIT_BOTTOM - NUM28_BASE_LINE)))

#define MAIN_TEMP_X_1       (59)    /* "5°C"   */
#define MAIN_TEMP_X_2       (37)    /* "50°C"  */
#define MAIN_TEMP_X_3       (15)    /* "300°C" */

/* ---- 页面编号 ------------------------------------------------------ */

typedef enum {
    PAGE_MAIN = 0,
    PAGE_NO_TOOL,
    PAGE_MENU,
    PAGE_PARAM,
    PAGE_COUNT,
} page_id_t;

/* ---- 配色 ---------------------------------------------------------- */

/**
 * 一套完整配色。主题切换时只换这个结构体并重刷控件，
 * 避免把颜色写死在几十个 set_color 调用里。
 */
typedef struct {
    sgl_color_t bg;
    sgl_color_t topbar;
    sgl_color_t header_line;     /* 顶栏下沿 1px 分隔线 */
    sgl_color_t text;
    sgl_color_t muted;
    sgl_color_t faint;
    sgl_color_t hint;            /* 菜单底部提示行（比 muted 更暗） */
    sgl_color_t panel;
    sgl_color_t card;
    sgl_color_t row_bg;          /* 菜单未选中行底色 */
    sgl_color_t row_border;
    sgl_color_t item_name;       /* 菜单行名称 */
    sgl_color_t item_value;      /* 菜单行右侧数值 */
    sgl_color_t set_bg;
    sgl_color_t power_bg;
    sgl_color_t mode_bg;
    sgl_color_t card_border;
    sgl_color_t card_text;
    sgl_color_t selected_bg;
    sgl_color_t selected_border;
    sgl_color_t selected_text;
    sgl_color_t edit_bg;         /* 编辑态行底色 */
    sgl_color_t edit_border;
    sgl_color_t accent;
    sgl_color_t setpoint;
    sgl_color_t warning;
    sgl_color_t power;
    sgl_color_t power_border;
    sgl_color_t mode_value;
    sgl_color_t dim;
    sgl_color_t line;
    sgl_color_t handle;          /* 型号胶囊边框 */
    sgl_color_t handle_bg;       /* 型号胶囊底色（浅色主题需要更深的底） */
    sgl_color_t handle_text;
    sgl_color_t badge_text;
    sgl_color_t badge_ready;
    sgl_color_t badge_stby;
    sgl_color_t badge_heat;
    sgl_color_t badge_sleep;
    sgl_color_t badge_warn;
    sgl_color_t badge_aux;
} ui_palette_t;

/* 深色配色全部取自 HTML；浅色配色按同一明度关系推导 */
static const ui_palette_t palette_dark = {
    .bg              = sgl_rgb(0x06, 0x09, 0x0D),
    .topbar          = sgl_rgb(0x0C, 0x12, 0x18),
    .header_line     = sgl_rgb(0x1A, 0x2A, 0x33),
    .text            = sgl_rgb(0xF8, 0xFC, 0xFF),
    .muted           = sgl_rgb(0x7E, 0x8D, 0x98),
    .faint           = sgl_rgb(0xC0, 0xCA, 0xD0),
    .hint            = sgl_rgb(0x69, 0x7A, 0x84),
    .panel           = sgl_rgb(0x0E, 0x15, 0x1D),
    .card            = sgl_rgb(0x11, 0x1A, 0x21),
    .row_bg          = sgl_rgb(0x10, 0x18, 0x20),
    .row_border      = sgl_rgb(0x26, 0x31, 0x3A),
    .item_name       = sgl_rgb(0xDC, 0xE7, 0xED),
    .item_value      = sgl_rgb(0x8F, 0xA0, 0xAB),
    /* 设定框也要有"淡填充"：原来是 panel 同色，屏上看起来是空框。
     * 这里给一点琥珀调，和金色边框呼应（其它小框都有各自的淡色底）。 */
    .set_bg          = sgl_rgb(0x1F, 0x1B, 0x0E),
    .power_bg        = sgl_rgb(0x12, 0x16, 0x1C),
    .mode_bg         = sgl_rgb(0x11, 0x18, 0x21),
    .card_border     = sgl_rgb(0x26, 0x31, 0x3A),
    .card_text       = sgl_rgb(0xCB, 0xD6, 0xDE),
    /* 型号胶囊：淡紫底（把紫边色按 ~20% 混进面板底，就是"淡淡的半透明紫"），
     * 字色随主题反色 —— 深色主题白字、浅色主题黑字，和金色胶囊同规矩。 */
    .handle_bg       = sgl_rgb(0x2A, 0x28, 0x49),
    .selected_bg     = sgl_rgb(0x17, 0x31, 0x3A),
    .selected_border = sgl_rgb(0x2D, 0xD4, 0xBF),
    .selected_text   = sgl_rgb(0x8F, 0xED, 0xE5),
    .edit_bg         = sgl_rgb(0x31, 0x27, 0x17),
    .edit_border     = sgl_rgb(0xFF, 0xCF, 0x5A),
    .accent          = sgl_rgb(0x2D, 0xD4, 0xBF),
    .setpoint        = sgl_rgb(0xFF, 0xCF, 0x00), // 手动调整了颜色
    .warning         = sgl_rgb(0xFF, 0xCF, 0x00), // 手动调整了颜色
    .power           = sgl_rgb(0xFF, 0x7A, 0x4D),
    .power_border    = sgl_rgb(0xFF, 0x7A, 0x4D),
    .mode_value      = sgl_rgb(0x8F, 0xA7, 0xFF),
    .dim             = sgl_rgb(0x20, 0x34, 0x3B),
    .line            = sgl_rgb(0x25, 0x34, 0x4B),
    .handle          = sgl_rgb(0x9C, 0x75, 0xFC),
    .handle_text     = sgl_rgb(0xFF, 0xFF, 0xFF),
    .badge_text      = sgl_rgb(0x07, 0x10, 0x0D),
    .badge_ready     = sgl_rgb(0x2D, 0xD4, 0xBF),
    .badge_stby      = sgl_rgb(0x42, 0xE6, 0xA4),
    .badge_heat      = sgl_rgb(0xFF, 0x7A, 0x4D),
    .badge_sleep     = sgl_rgb(0x7E, 0x8D, 0x98),
    .badge_warn      = sgl_rgb(0xFF, 0xCF, 0x00),
    .badge_aux       = sgl_rgb(0x8F, 0xA7, 0xFF),
};

static const ui_palette_t palette_light = {
    .bg              = sgl_rgb(0xF0, 0xF2, 0xF4),
    .topbar          = sgl_rgb(0xFF, 0xFF, 0xFF),
    .header_line     = sgl_rgb(0xD8, 0xE0, 0xE6),
    .text            = sgl_rgb(0x10, 0x18, 0x20),
    .muted           = sgl_rgb(0x5F, 0x6B, 0x73),
    .faint           = sgl_rgb(0x3A, 0x46, 0x50),
    .hint            = sgl_rgb(0x7C, 0x8A, 0x94),
    .panel           = sgl_rgb(0xFF, 0xFF, 0xFF),
    .card            = sgl_rgb(0xE9, 0xEE, 0xF2),
    .row_bg          = sgl_rgb(0xE9, 0xEE, 0xF2),
    .row_border      = sgl_rgb(0xC5, 0xD0, 0xD8),
    .item_name       = sgl_rgb(0x2A, 0x36, 0x3E),
    .item_value      = sgl_rgb(0x6B, 0x78, 0x81),
    .set_bg          = sgl_rgb(0xFF, 0xFA, 0xE8),   /* 淡黄填充，与金色边框呼应 */
    .power_bg        = sgl_rgb(0xFF, 0xF1, 0xEC),
    .mode_bg         = sgl_rgb(0xEE, 0xF2, 0xF7),
    .card_border     = sgl_rgb(0xC5, 0xD0, 0xD8),
    .card_text       = sgl_rgb(0x24, 0x30, 0x38),
    /* 型号胶囊底色：浅色主题下不能沿用"选中行"的极浅青，
     * 否则白字压白底完全不可读（实机浅色主题确认）。
     * 这里用深一号的紫调底，保持"深底+紫边+白字"的设计语义。 */
    .handle_bg       = sgl_rgb(0xF0, 0xEA, 0xFE),   /* 淡紫，与金色胶囊同档亮度 */
    .selected_bg     = sgl_rgb(0xDD, 0xF6, 0xF3),
    .selected_border = sgl_rgb(0x12, 0x89, 0x7F),
    .selected_text   = sgl_rgb(0x0B, 0x6B, 0x64),
    .edit_bg         = sgl_rgb(0xFD, 0xF3, 0xD8),
    .edit_border     = sgl_rgb(0xB2, 0x7B, 0x00),
    .accent          = sgl_rgb(0x12, 0x89, 0x7F),
    .setpoint        = sgl_rgb(0xB2, 0x7B, 0x00),
    .warning         = sgl_rgb(0xFF, 0xCF, 0x00),
    .power           = sgl_rgb(0xC2, 0x4A, 0x23),
    .power_border    = sgl_rgb(0xE0, 0x5A, 0x2B),
    .mode_value      = sgl_rgb(0x3A, 0x5B, 0xD9),
    .dim             = sgl_rgb(0xD5, 0xDE, 0xE5),
    .line            = sgl_rgb(0xC5, 0xD0, 0xD8),
    .handle          = sgl_rgb(0x7A, 0x4B, 0xD9),
    .handle_text     = sgl_rgb(0x10, 0x18, 0x20),   /* 浅色主题：黑字 */
    .badge_text      = sgl_rgb(0x07, 0x10, 0x0D),
    .badge_ready     = sgl_rgb(0x2D, 0xD4, 0xBF),
    .badge_stby      = sgl_rgb(0x42, 0xE6, 0xA4),
    .badge_heat      = sgl_rgb(0xFF, 0x7A, 0x4D),
    .badge_sleep     = sgl_rgb(0x7E, 0x8D, 0x98),
    .badge_warn      = sgl_rgb(0xFF, 0xCF, 0x00),
    .badge_aux       = sgl_rgb(0x8F, 0xA7, 0xFF),
};

static const ui_palette_t *palette = &palette_dark;

/* ---- 页面与控件 ---------------------------------------------------- */

static sgl_obj_t *main_page;
static sgl_obj_t *no_tool_page;
static sgl_obj_t *menu_page;
static sgl_obj_t *param_page;

/* 主界面 */
static sgl_obj_t *main_top_bar;
static sgl_obj_t *main_header_line;
static sgl_obj_t *main_handle_bg;
static sgl_obj_t *main_handle_label;
static sgl_obj_t *main_page_bg;
static sgl_obj_t *main_page_label;
static sgl_obj_t *main_state_bg;
static sgl_obj_t *main_state_label;
static sgl_obj_t *main_temp_panel;
static sgl_obj_t *main_temp_label;
static sgl_obj_t *main_temp_unit;
static sgl_obj_t *main_duty_bg;
static sgl_obj_t *main_duty_fill;
static sgl_obj_t *main_set_box;
static sgl_obj_t *main_set_title;
static sgl_obj_t *main_set_digits;
static sgl_obj_t *main_set_unit;
static sgl_obj_t *main_power_box;
static sgl_obj_t *main_power_voltage;
static sgl_obj_t *main_power_current;
static sgl_obj_t *main_mode_box;
static sgl_obj_t *main_mode_title;
static sgl_obj_t *main_mode_value;
static sgl_obj_t *main_preset_bg[3];
static sgl_obj_t *main_preset_label[3];

/* 无工具告警：顶栏两个胶囊 + 内容面板，全部是固定文案，没有刷新逻辑 */
static sgl_obj_t *no_tool_top_bar;
static sgl_obj_t *no_tool_header_line;
static sgl_obj_t *no_tool_handle_bg;
static sgl_obj_t *no_tool_handle_label;
static sgl_obj_t *no_tool_warn_bg;
static sgl_obj_t *no_tool_warn_label;
static sgl_obj_t *no_tool_panel;
static sgl_obj_t *no_tool_title;
static sgl_obj_t *no_tool_hint;

/* 菜单：一屏 MENU_VISIBLE_ROWS 行，每行一个卡片 + 名称 + 数值 + 选中箭头 */
static sgl_obj_t *menu_top_bar;
static sgl_obj_t *menu_header_line;
static sgl_obj_t *menu_handle_bg;
static sgl_obj_t *menu_handle_label;
static sgl_obj_t *menu_title_bg;
static sgl_obj_t *menu_title_label;
static sgl_obj_t *menu_hint_label;
static sgl_obj_t *menu_row_card[MENU_VISIBLE_ROWS];
static sgl_obj_t *menu_row_name[MENU_VISIBLE_ROWS];
static sgl_obj_t *menu_row_value[MENU_VISIBLE_ROWS];
static sgl_obj_t *menu_row_arrow[MENU_VISIBLE_ROWS];

/* 参数页 */
static sgl_obj_t *param_top_bar;
static sgl_obj_t *param_accent;
static sgl_obj_t *param_title_label;
static sgl_obj_t *param_name_label;
static sgl_obj_t *param_value_label;
static sgl_obj_t *param_unit_label;
static sgl_obj_t *param_chart;

/* 曲线数据与状态 */
static int32_t curve_values[CURVE_POINT_COUNT];
static sgl_linechart_series_t chart_series;
static uint32_t curve_sample_ms;
static uint8_t curve_needs_reset = 1U;

/* 参数页创建时要把曲线铺平，但曲线函数在页面创建之后才定义 */
static void reset_curve(void);

/* ---- 变化检测缓存 -------------------------------------------------- */

static page_id_t last_page = PAGE_COUNT;
static uint8_t last_theme = 0xFEU;
static uint8_t menu_layout_mode = 0xFEU;
static uint8_t preset_color_dirty = 1U;
static uint8_t state_color_valid;
static sgl_color_t state_color_cache;
static uint8_t duty_fill_visible = 0xFFU;
static int16_t duty_fill_width = -1;
static int16_t temp_label_x = -1;
static int16_t temp_unit_x = -1;
static int16_t set_digits_x = -1;
static int16_t set_unit_x = -1;
/* 金色标题胶囊当前宽度（-1 = 还没设过，第一帧必刷一次） */
static int16_t title_chip_width = -1;
static uint8_t last_active_preset = 0xFEU;
static uint8_t last_param_editing = 0xFEU;

/* ---- 显示侧稳定化（v9.22） ----------------------------------------
 *
 * 背景：app_heat.c 的加热控制部分已定档冻结，不再改动。
 * 但 `station.temperature_int` 是 `(int16_t)(raw * 350 / cal)` **向零截断**
 * 得到的，300℃ 附近真实值在 299.6~300.4 摆动时，整数就会在 299/300
 * 之间跳 —— 用户看到的就是"末位闪"。
 * 同理 `FLAG_TEMP_REACHED` 的滞回带只有 1℃，也会反复置位/清除，
 * 直接翻译成"已到温/加热中"就是徽标闪烁。
 *
 * 这两件事**只能在 UI 层兜住**（app_heat.c 不动），
 * 所以这里各做一层"显示侧滤波"，只影响显示，绝不回写 station.state。 */

/* "已到温"显示保持的确认时长（ms）。
 *
 * **v9.26：300 → 800 ms**，与温度慢跟窗口（TEMP_SLOW_FOLLOW_MS）对齐。
 *
 * 为什么必须加长：`FLAG_TEMP_REACHED` 的滞回带只有 1℃（进入 >=设定值-1、
 * 退出 <设定值-2），而 `station.temperature_int`（10 点慢环）在实测中
 * 会在 298~302 范围摆动 —— 一旦跌破 298 该位就被清除。
 * 300 ms 窗口太短：只要"低于带"的持续时间偶尔超过 300 ms，
 * 徽标就会切回"加热中"，然后温度一回升又切回"已到温"——肉眼就是不停跳。
 *
 * 800 ms 与温度慢跟同窗口，能覆盖同一类慢摆动的持续时间。
 * 代价：温度真掉下去后，徽标约 800 ms 才切回"加热中"（用户已确认接受）。
 *
 * 只看**撤销**方向，置位是立即的 —— 见 update_ready_display()。 */
#define READY_HOLD_DISPLAY_MS       (800U)

/* 显示用温度整数 + "是否已采纳过"标志。
 *
 * 为什么用独立的 temp_valid，而不是拿 `shown_temp_int < 0` 当哨兵
 * （v9.24 修正）：温度本身可能是负数（冷态截断、将来的冷端补偿、
 * NTC 反相等都会产生负值）。若用"负值 = 未初始化"，
 * 那么每次拿到负温度都会被当成"首帧重新采纳"，
 * 慢跟与死区**全部失效**（每帧都直接跳到新值，末位照旧闪）。
 * 布尔量与数值分离，任何合法温度值都不会产生歧义。 */
static int16_t shown_temp_int;
static uint8_t temp_valid;

/* "慢跟"计时（v9.24）：
 *   delta == 1 时**不立刻跟随**，但要开始计时；持续满 500 ms 才接受新值。
 *   temp_hold_ms 记录"偏差持续为 1℃ 的起点"，delta == 0 时必须清零
 *   （否则下次再出现 1℃ 偏差会沿用旧的起点，立刻跳变）。 */
#define TEMP_SLOW_FOLLOW_MS     (800U)

/* 显示温度的两个阈值 + 慢跟时长（v9.26）
 *
 * 实机实测：待机 99~103、恒温 299~303，**跨度都是 4℃**。
 *
 * 演变过程（都是实机逼出来的）：
 *   · v9.24 用 delta>=2 立刻跟随 → 跨度 4 直接跟随，末位照跳
 *   · v9.25 改成 4/3 → 阈值恰等于摆幅，首帧落边缘时逐拍跳
 *   · v9.25 再改 6/5 → 恒温 300 稳了，但**待机仍 99↔103 单次跳**
 *
 * 为什么 6/5 还不够（v9.26）：
 *   不是连续振荡，而是"首帧采纳 99，慢跟满 500 ms 后接受 103"这一**单次**跳变。
 *   499 ms 与 500 ms 只差一瞬，慢跟窗口对抖动来说**太短** ——
 *   只要抖动持续时间偶尔超过窗口，就会跟上去一次。
 *
 * 现在取 10 / 9 + 800 ms：
 *   · 慢跟窗口 800 ms 远大于该摆动的回摆周期，抖不到窗口；
 *   · 阈值 10 给出 6℃ 余量（摆幅 4℃），首帧无论落在哪里都按得住。
 *
 *   · **delta >= TEMP_JUMP_THRESHOLD（10）**：真变化，立刻跟随；
 *   · **delta <= TEMP_HOLD_STEPS（9）**：按住，持续满 800 ms 才跟。
 *
 * 代价（用户已确认接受）：改设定值 1~9℃ 时显示延后 800 ms 才跟上；
 * 改 10℃ 以上立刻到位。升温/降温过程动辄几十度，不受影响。 */
#define TEMP_JUMP_THRESHOLD     (10)    /* 偏差 >= 此值：立刻跟随 */
#define TEMP_HOLD_STEPS         (9)     /* 偏差 <= 此值：按住，800 ms 后跟 */
static uint32_t temp_hold_ms;

/* "已到温"的显示保持（三个量各司其职，**不要用其中一个当另一个的哨兵**）：
 *   ready_shown    当前是否按"已到温"显示（1=是）—— 只影响显示
 *   ready_pending  是否正在"等待撤销确认"（1=是）
 *   ready_change_ms 撤销等待的**起点时刻**（仅 ready_pending=1 时有效）
 *
 * 为什么必须单独有 ready_pending，而不是拿 `ready_change_ms == 0`
 * 当"没有在等待"的哨兵（v9.22）：
 *   如果 get_tick_ms() 恰好返回 0（上电后第一拍，或 32 位回绕到 0），
 *   那么 `ready_change_ms == 0` 既可能是"未初始化"也可能是
 *   "上次切换发生在 tick=0" —— 两种语义无法区分，只能靠猜。
 *   把"是否在等待"（布尔）与"何时开始等"（时刻）分开，
 *   任何 tick 值都不会产生歧义。 */
static uint8_t ready_shown;
static uint8_t ready_pending;
static uint32_t ready_change_ms;
/* 第一屏显示的第几项：超过 5 项的页面靠它滚动 */
static uint8_t menu_scroll = 0xFFU;
static uint8_t row_value_visible[MENU_VISIBLE_ROWS] =
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
static uint8_t row_arrow_visible[MENU_VISIBLE_ROWS] =
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
static uint8_t row_card_visible_cache[MENU_VISIBLE_ROWS] =
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
static uint8_t row_name_visible_cache[MENU_VISIBLE_ROWS] =
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
static uint8_t row_name_color_valid[MENU_VISIBLE_ROWS];
static uint8_t row_value_color_valid[MENU_VISIBLE_ROWS];
static uint8_t row_arrow_color_valid[MENU_VISIBLE_ROWS];
static uint8_t row_card_color_valid[MENU_VISIBLE_ROWS];
static sgl_color_t row_name_color_cache[MENU_VISIBLE_ROWS];
static sgl_color_t row_value_color_cache[MENU_VISIBLE_ROWS];
static sgl_color_t row_arrow_color_cache[MENU_VISIBLE_ROWS];
static sgl_color_t row_card_fill_cache[MENU_VISIBLE_ROWS];
static sgl_color_t row_card_border_cache[MENU_VISIBLE_ROWS];

/* 文本缓冲必须是静态的：sgl_label_set_text 只存指针，不拷贝内容 */
static char main_handle_text[8];
static char main_state_text[16];
static char main_temp_text[8];
static char main_set_text[8];
static char main_voltage_text[16];
static char main_current_text[16];
static char main_mode_text[8];
static char main_preset_text[3][12];
static char menu_title_text[32];
static char menu_handle_text[8];
static char menu_hint_text[64];
static char menu_row_name_text[MENU_VISIBLE_ROWS][32];
static char menu_row_value_text[MENU_VISIBLE_ROWS][24];
static char param_name_text[32];
static char param_value_text[24];
static char param_unit_text[16];

/* 上一次真正写进控件的文本，用来跳过重复的 set_text */
static char main_handle_shown[8];
static char main_state_shown[16];
static char main_temp_shown[8];
static char main_set_shown[8];
static char main_voltage_shown[16];
static char main_current_shown[16];
static char main_mode_shown[8];
static char main_preset_shown[3][12];
static char menu_title_shown[32];
static char menu_handle_shown[8];
static char menu_hint_shown[64];
static char menu_row_name_shown[MENU_VISIBLE_ROWS][32];
static char menu_row_value_shown[MENU_VISIBLE_ROWS][24];
static char param_name_shown[32];
static char param_value_shown[24];
static char param_unit_shown[16];

/* ---- 控件创建小工具 ------------------------------------------------ */

static sgl_obj_t *create_rect(sgl_obj_t *parent, int16_t x, int16_t y,
                              int16_t w, int16_t h, sgl_color_t fill,
                              sgl_color_t border, uint8_t border_width,
                              uint16_t radius)
{
    sgl_obj_t *rect = sgl_rect_create(parent);

    /* 建不出来只可能是 SGL 堆耗尽，交给页面创建函数统一判死 */
    if (NULL == rect) {
        return NULL;
    }
    sgl_obj_set_pos(rect, x, y);
    /* 先定尺寸再设圆角：圆角会被尺寸除半钳位 */
    sgl_obj_set_size(rect, w, h);
    sgl_rect_set_color(rect, fill);
    sgl_rect_set_border_color(rect, border);
    sgl_rect_set_border_width(rect, border_width);
    sgl_rect_set_radius(rect, radius);
    return rect;
}

static sgl_obj_t *create_label(sgl_obj_t *parent, int16_t x, int16_t y,
                               int16_t w, int16_t h, const sgl_font_t *font,
                               const char *text, sgl_color_t color,
                               sgl_align_type_t align)
{
    sgl_obj_t *label = sgl_label_create(parent);

    /* 建不出来只可能是 SGL 堆耗尽，返回 NULL 让页面创建函数统一判死 */
    if (NULL == label) {
        return NULL;
    }
    sgl_obj_set_pos(label, x, y);
    sgl_obj_set_size(label, w, h);
    sgl_label_set_font(label, font);
    /* 先设对齐再设文本：set_text 会按当前对齐算脏区 */
    sgl_label_set_text_align(label, align);
    sgl_label_set_text(label, text);
    sgl_label_set_text_color(label, color);
    return label;
}

/**
 * @brief  文本没变就不碰控件
 * @param  shown 上一次真正写进去的文本
 * @note   SGL 每次 set_text 都会把整个标签标脏，重复设置会让整屏一直重绘。
 */
static void update_text(sgl_obj_t *label, char *shown, uint32_t size,
                        const char *text)
{
    uint32_t length = 0U;

    /* 文本没变就完全不碰控件，省掉一次整标签重绘 */
    if ((0U == size) || (0 == strncmp(text, shown, size))) {
        return;
    }
    /* 手写拷贝而不是 snprintf：GCC 会把 snprintf 的截断当成潜在错误，
     * 这里所有缓冲都按最长文案分配，截断只是为了越界保护。 */
    while ((length < (size - 1U)) && ('\0' != text[length])) {
        shown[length] = text[length];
        length++;
    }
    shown[length] = '\0';
    sgl_label_set_text(label, text);
}

/** 换页或换配色后让所有缓存失效，强制下一帧全量刷新 */
static void invalidate_text_cache(void)
{
    uint8_t i;

    /* **温度记忆必须重置**（v9.22）
     *
     * 不重置的后果：温度还留着旧值 → 从告警页切回主界面时"沿用旧温度"，
     * 屏幕上先显示拔工具前的数字，再跳回真实值。
     *
     * 重置成"无效值"而不是 0：
     *   temp_valid = 0 表示"还没算过"（布尔量与数值分离，
     *   0℃ 是合法温度、负温度也合法，所以都不能当哨兵）。
     *
     * **temp_hold_ms 也必须清**（v9.24）：它是"慢跟计时"。
     * 这里是"换页 / 换主题 / 换手柄"的统一复位点，漏了它的话，
     * 换页后残留的计时起点会让下一次小偏差**立刻**满足"持续够了"，
     * 慢跟直接失效（表现为换页后数字立刻跳一下）。 */
    temp_valid = 0U;
    shown_temp_int = 0;
    temp_hold_ms = 0U;

    /* **ready_shown / ready_pending / ready_change_ms 三行已删除**（v9.26）
     *
     * 为什么不能在这里重置 —— 两个原因，其中一个是真 bug：
     *
     * ① **顺序 bug（v9.23 引入）**：`task_ui()` 里先调 `load_page(page)`
     *    （内部 `sync_ready_display()` 把 ready_shown 同步成真实状态），
     *    紧接着调 `invalidate_text_cache()`。若这里把 ready_shown 清成 0，
     *    刚同步好的值**立刻被抹掉**，等于同步从未发生。
     *
     * ② **换主题/换页时徽标要多闪一次**：该位若正处在"已到温"，
     *    被清 0 后界面会先显示"加热中"，再等 `update_ready_display()`
     *    置回 —— 每次换主题都闪一下。（注意 `invalidate_text_cache()`
     *    只在换页/换主题/换手柄时调用，**不是每帧**。）
     *
     * 现在由 `sync_ready_display()`（页切换时）与 `update_ready_display()`
     * （每帧）共同维护这两个量，`invalidate` 不再插手。 */


    main_handle_shown[0] = '\0';
    main_state_shown[0] = '\0';
    main_temp_shown[0] = '\0';
    main_set_shown[0] = '\0';
    main_voltage_shown[0] = '\0';
    main_current_shown[0] = '\0';
    main_mode_shown[0] = '\0';
    menu_title_shown[0] = '\0';
    menu_handle_shown[0] = '\0';
    menu_hint_shown[0] = '\0';
    param_name_shown[0] = '\0';
    param_value_shown[0] = '\0';
    param_unit_shown[0] = '\0';

    /* 逐行清掉"上次显示"缓存，保证换页后第一帧全量刷新 */
    for (i = 0U; i < MENU_VISIBLE_ROWS; i++) {
        menu_row_name_shown[i][0] = '\0';
        menu_row_value_shown[i][0] = '\0';
        row_name_color_valid[i] = 0U;
        row_value_color_valid[i] = 0U;
        row_arrow_color_valid[i] = 0U;
        row_card_color_valid[i] = 0U;
        row_value_visible[i] = 0xFFU;
        row_arrow_visible[i] = 0xFFU;
        row_card_visible_cache[i] = 0xFFU;
        row_name_visible_cache[i] = 0xFFU;
    }
    for (i = 0U; i < 3U; i++) {
        main_preset_shown[i][0] = '\0';
    }
    state_color_valid = 0U;
    preset_color_dirty = 1U;
    last_active_preset = 0xFEU;
    last_param_editing = 0xFEU;
    menu_scroll = 0xFFU;
    duty_fill_width = -1;
    duty_fill_visible = 0xFFU;
    temp_label_x = -1;
    temp_unit_x = -1;
    set_digits_x = -1;
    set_unit_x = -1;
    /* 换页/换主题后胶囊长度要按新页名重新判一次 */
    title_chip_width = -1;
}

/* ---- 主题 ---------------------------------------------------------- */

static void select_palette(void)
{
    palette = (0U != station.theme) ? &palette_light : &palette_dark;
}

/* apply_palette 末尾要用到曲线样式（换主题时线色也要跟着变） */
static void apply_chart_style(void);

/**
 * @brief 把当前配色应用到所有静态控件
 * @note  动态颜色（状态徽标、选中行、编辑态）由各自的 refresh 函数覆盖
 */
static void apply_palette(void)
{
    uint8_t i;

    sgl_page_set_color(main_page, palette->bg);
    sgl_page_set_color(no_tool_page, palette->bg);
    sgl_page_set_color(menu_page, palette->bg);
    sgl_page_set_color(param_page, palette->bg);

    /* 顶栏与主面板 */
    sgl_rect_set_color(main_top_bar, palette->topbar);
    sgl_rect_set_color(main_header_line, palette->header_line);
    /* 型号徽标是"深底 + 紫边 + 白字"的胶囊，不是实心色块 */
    sgl_rect_set_color(main_handle_bg, palette->handle_bg);
    sgl_rect_set_border_color(main_handle_bg, palette->handle);
    sgl_label_set_text_color(main_handle_label, palette->handle_text);
    /* 主页名是金色胶囊：底色/边框必须一起切主题。
     * 之前这里漏了 main_page_bg，深色主题下创建后浅色主题仍沿用深色值，
     * 表现为"主页胶囊一片黑"（实机浅色主题确认）。 */
    sgl_rect_set_color(main_page_bg, palette->edit_bg);
    sgl_rect_set_border_color(main_page_bg, palette->setpoint);
    sgl_label_set_text_color(main_page_label, palette->text);
    sgl_label_set_text_color(main_state_label, palette->badge_text);

    sgl_rect_set_color(main_temp_panel, palette->panel);
    sgl_rect_set_border_color(main_temp_panel, palette->accent);
    sgl_label_set_text_color(main_temp_label, palette->text);
    sgl_label_set_text_color(main_temp_unit, palette->text);

    sgl_rect_set_color(main_duty_bg, palette->dim);
    sgl_rect_set_color(main_duty_fill, palette->accent);

    sgl_rect_set_color(main_set_box, palette->set_bg);
    sgl_rect_set_border_color(main_set_box, palette->setpoint);
    sgl_label_set_text_color(main_set_title, palette->faint);
    sgl_label_set_text_color(main_set_digits, palette->setpoint);
    sgl_label_set_text_color(main_set_unit, palette->setpoint);

    sgl_rect_set_color(main_power_box, palette->power_bg);
    sgl_rect_set_border_color(main_power_box, palette->power_border);
    sgl_label_set_text_color(main_power_voltage, palette->power);
    sgl_label_set_text_color(main_power_current, palette->power);

    sgl_rect_set_color(main_mode_box, palette->mode_bg);
    sgl_rect_set_border_color(main_mode_box, palette->line);
    sgl_label_set_text_color(main_mode_title, palette->muted);
    sgl_label_set_text_color(main_mode_value, palette->mode_value);

    /* 无工具页：暗顶栏 + 黄色 Warning 胶囊 + 黄边面板 */
    sgl_rect_set_color(no_tool_top_bar, palette->topbar);
    sgl_rect_set_color(no_tool_header_line, palette->header_line);
    sgl_rect_set_color(no_tool_handle_bg, palette->handle_bg);
    sgl_rect_set_border_color(no_tool_handle_bg, palette->handle);
    sgl_label_set_text_color(no_tool_handle_label, palette->handle_text);
    sgl_rect_set_color(no_tool_warn_bg, palette->badge_warn);
    sgl_rect_set_border_color(no_tool_warn_bg, palette->badge_warn);
    sgl_label_set_text_color(no_tool_warn_label, palette->badge_text);
    sgl_rect_set_color(no_tool_panel, palette->panel);
    sgl_rect_set_border_color(no_tool_panel, palette->badge_warn);
    sgl_label_set_text_color(no_tool_title, palette->text);
    sgl_label_set_text_color(no_tool_hint, palette->muted);

    /* 菜单与参数页 */
    sgl_rect_set_color(menu_top_bar, palette->topbar);
    sgl_rect_set_color(menu_header_line, palette->header_line);
    sgl_rect_set_color(menu_handle_bg, palette->handle_bg);
    sgl_rect_set_border_color(menu_handle_bg, palette->handle);
    sgl_label_set_text_color(menu_handle_label, palette->handle_text);
    sgl_rect_set_color(menu_title_bg, palette->edit_bg);
    sgl_rect_set_border_color(menu_title_bg, palette->setpoint);
    sgl_label_set_text_color(menu_title_label, palette->text);
    sgl_label_set_text_color(menu_hint_label, palette->hint);
    /* 行卡片的选中/编辑配色由 refresh_menu 逐行决定，这里只作废缓存 */
    for (i = 0U; i < MENU_VISIBLE_ROWS; i++) {
        row_card_color_valid[i] = 0U;
        row_arrow_color_valid[i] = 0U;
    }

    sgl_rect_set_color(param_top_bar, palette->topbar);
    sgl_rect_set_color(param_accent, palette->accent);
    sgl_label_set_text_color(param_title_label, palette->text);
    sgl_label_set_text_color(param_unit_label, palette->muted);
    sgl_label_set_text_color(param_value_label, palette->text);

    /* 图表颜色由 linechart 自己保存，需要单独同步 */
    sgl_linechart_set_bg_color(param_chart, palette->panel);
    sgl_linechart_set_border_color(param_chart, palette->line);
    sgl_linechart_set_axis_label_color(param_chart, SGL_LINECHART_AXIS_Y,
                                       palette->muted, 255U);
    sgl_linechart_set_axis_grid_color(param_chart, SGL_LINECHART_AXIS_Y,
                                      palette->dim, 100U);

    /* 配色变了，所有"颜色没变就不重设"的缓存都要作废 */
    state_color_valid = 0U;
    preset_color_dirty = 1U;
    last_active_preset = 0xFEU;
    last_param_editing = 0xFEU;
    /* 曲线 series 里的线色/填充色不在控件上，必须重新套一遍样式 */
    apply_chart_style();
}

/**
 * @brief 设置温度曲线样式
 * @note  原机的"系统风格"（标准/系统/曲线）已删除，曲线固定为单线；
 *        但线色属于 series 内部字段，换主题时必须重新套一遍。
 */
static void apply_chart_style(void)
{
    sgl_linechart_t *chart_data = (sgl_linechart_t *)param_chart;
    sgl_linechart_series_t *series = &chart_data->series[0];

    series->style_bits.show_line = 1U;
    series->style_bits.show_points = 0U;
    series->style_bits.fill_under = 0U;
    series->style_bits.point_shape = SGL_LINECHART_POINT_SHAPE_CIRCLE;
    series->line_color = palette->accent;
    series->line_width = 1U;
    sgl_obj_set_dirty(param_chart);
}

/* ---- 状态文本 ---- */

/** 手柄型号徽标只放 ASCII，避免 56 px 徽标里塞三个汉字 */
static const char *get_tip_code(void)
{
    if (!is_tip_present()) {
        return "----";
    }
    switch (station.tip_channel) {
    case 0U: return "T12";
    case 1U: return "C245";
    case 2U: return "C210";
    default: return "----";
    }
}

/**
 * @brief 把状态翻译成 2~3 个字，并带回徽标底色
 * @param badge 输出：状态徽标底色
 * @note  徽标宽度只有 52 px，超过 3 个汉字会被裁掉
 */
static const char *get_state_text(sgl_color_t *badge)
{
    uint16_t state = station.state;

    /* 优先级从安全到正常：休眠 > 待机 > 联锁 > 无工具 > 换芯 > 到温 > 加热 */
    if (0U != (state & FLAG_SLEEP)) {
        *badge = palette->badge_sleep;
        return "休眠";
    }
    if (0U != (state & FLAG_STANDBY)) {
        *badge = palette->badge_stby;
        return "待机";
    }
    if (0U != (state & FLAG_JBC_SW)) {
        *badge = palette->badge_warn;
        return "停止";
    }
    if (!is_tip_present()) {
        *badge = palette->badge_warn;
        return "无工具";
    }
    if (0U != (state & FLAG_DETECT)) {
        *badge = palette->badge_aux;
        return "换芯中";
    }
    /* "已到温"要用 FLAG_TEMP_REACHED，且优先于 HEATING：
     * 恒温时 PID 仍会给出很小的占空比，而 FLAG_HEATING 的语义只是
     * "本轮有输出"（原机 ST_HEATING 0x4000 同样只是"本轮 PID 有效"的提示位）。
     * 若让 HEATING 抢先，到达设定温度后徽标会永远停在"加热中"。
     * 注意不能用 FLAG_READY：那是"已识别、可进 PID"的闸门，不是到温标志。
     *
     * **v9.22：读 ready_shown 而不是直接读 state 位。**
     * 该位的滞回带只有 1℃，温度在 ±2℃ 摆动时会反复置位/清除，
     * 直接显示就是徽标闪。ready_shown 由 update_ready_display()
     * 每帧更新一次（带 300 ms 撤销确认），这里只读、不改状态，
     * 所以 get_state_text/get_mode_text 都是纯函数。 */
    if (0U != ready_shown) {
        *badge = palette->badge_ready;
        return "已到温";
    }
    if (0U != (state & FLAG_HEATING)) {
        *badge = palette->badge_heat;
        return "加热中";
    }
    *badge = palette->badge_sleep;
    return "正常";
}

/** MODE 框只放 3~4 个字母，和 HTML 的 HOLD 对齐 */
static const char *get_mode_text(void)
{
    uint16_t state = station.state;

    if (0U != (state & FLAG_SLEEP)) {
        return "SLP";
    }
    if (0U != (state & FLAG_STANDBY)) {
        return "STBY";
    }
    if (0U != (state & FLAG_JBC_SW)) {
        return "STOP";
    }
    if (!is_tip_present()) {
        return "----";
    }
    if (0U != (state & FLAG_DETECT)) {
        return "DET";
    }
    /* 与状态徽标同一优先级：到温后显示 HOLD，不再被 HEAT 抢占。
     * v9.22：同样读 ready_shown（纯读），避免 HOLD 与 HEAT 之间闪。 */
    if (0U != ready_shown) {
        return "HOLD";
    }
    if (0U != (state & FLAG_HEATING)) {
        return "HEAT";
    }
    return "IDLE";
}

/* ---- 页面创建 ------------------------------------------------------ */

static bool create_main_page(void)
{
    uint8_t i;

    main_page = sgl_obj_create(NULL);
    /* 根页面都建不出来，后面的控件一定也建不出来 */
    if (NULL == main_page) {
        return false;
    }
    sgl_page_set_color(main_page, palette->bg);

    /* 顶栏：整条深色底，型号/主页/状态都挂在上面 */
    main_top_bar = create_rect(main_page, 0, 0, SCREEN_WIDTH, 30,
                               palette->topbar, palette->topbar, 0U, 0U);
    /* 顶栏下沿 1px 分隔线，和 HTML 的 header border-bottom 对齐 */
    main_header_line = create_rect(main_page, 0, 30, SCREEN_WIDTH, 1,
                                   palette->header_line, palette->header_line,
                                   0U, 0U);
    /* 型号胶囊：深底 + 紫边 + 白字（HTML 的 .badge-handle） */
    main_handle_bg = create_rect(main_page, 9, 4, 56, 22, palette->handle_bg,
                                 palette->handle, 1U, 5U);
    main_handle_label = create_label(main_page, 9, 4, 56, 22, &ui13, "C245",
                                     palette->handle_text, SGL_ALIGN_CENTER);
    /* 页面名是金色胶囊（HTML 的 .badge-gold），不是灰色小字。
     * 主页固定 2 个字，用短档 CHIP_GOLD_SHORT_W（= 64，与菜单标题短档同长） */
    main_page_bg = create_rect(main_page, CHIP_X, 4, CHIP_GOLD_SHORT_W, 22,
                               palette->edit_bg, palette->setpoint, 1U, 5U);
    /* 设计稿 .badge 是 13px：胶囊塞 16px 中文会顶到边缘，所以放宽到 64
     * 并仍用 cjk16（中文一律微米黑 Mono，NK57 没有汉字）。 */
    main_page_label = create_label(main_page, CHIP_X, 4, CHIP_GOLD_SHORT_W, 22,
                                   &cjk16, "主页", palette->text,
                                   SGL_ALIGN_CENTER);
    main_state_bg = create_rect(main_page, 260, 4, 52, 22, palette->badge_stby,
                                palette->badge_stby, 0U, 5U);
    main_state_label = create_label(main_page, 260, 4, 52, 22, &cjk16, "待机",
                                    palette->badge_text, SGL_ALIGN_CENTER);

    /* 温度大面板：青色 2 px 边框。
     * 两个标签都用 LEFT_TOP 对齐：SGL 的 TOP_LEFT 把行盒顶端压到标签
     * 顶端，Y 就完全由宏决定，不去猜字高/居中舍入。 */
    main_temp_panel = create_rect(main_page, MAIN_PANEL_X, MAIN_PANEL_Y,
                                  MAIN_PANEL_W, MAIN_PANEL_H, palette->panel,
                                  palette->accent, 2U, 6U);
    main_temp_label = create_label(main_page, MAIN_TEMP_X_3, MAIN_TEMP_DIGIT_Y,
                                   (int16_t)(3 * NUM90_ADV), NUM90_FONT_HEIGHT,
                                   &num90, "--", palette->text,
                                   SGL_ALIGN_TOP_LEFT);
    main_temp_unit = create_label(main_page, MAIN_TEMP_X_3, MAIN_TEMP_UNIT_Y,
                                  (int16_t)(2 * NUM28_ADV), NUM28_FONT_HEIGHT,
                                  &num28, "°C", palette->text,
                                  SGL_ALIGN_TOP_LEFT);
    main_duty_bg = create_rect(main_page, 24, DUTY_BAR_Y, DUTY_BAR_WIDTH,
                               DUTY_BAR_HEIGHT, palette->dim, palette->dim,
                               0U, 3U);
    main_duty_fill = create_rect(main_page, 24, DUTY_BAR_Y, 1, DUTY_BAR_HEIGHT,
                                 palette->accent, palette->accent, 0U, 3U);

    /* 右上设定框：黄色 1 px 边框 + 黄色数字 */
    main_set_box = create_rect(main_page, RIGHT_PANEL_X, SET_BOX_Y, 98,
                               SET_BOX_H, palette->set_bg, palette->setpoint,
                               1U, 6U);
    main_set_title = create_label(main_page, RIGHT_PANEL_X, 42, 98, 20,
                                  &cjk16, "设定", palette->faint,
                                  SGL_ALIGN_CENTER);
    /* 数字与单位是"墨迹底边对齐组"：两个标签都用 TOP_LEFT，Y 由宏算死；
     * 水平位置在 refresh 里按位数查 3 个常量（1/2/3 位），运行时不量宽度。 */
    main_set_digits = create_label(main_page, SET_DIGITS_X_3, SET_DIGITS_Y,
                                    (int16_t)(3 * NUM32_ADV), NUM32_FONT_HEIGHT,
                                    &num32, "320", palette->setpoint,
                                    SGL_ALIGN_TOP_LEFT);
    main_set_unit = create_label(main_page,
                                 (int16_t)(SET_DIGITS_X_3 + 3 * NUM32_ADV +
                                           SET_GROUP_GAP),
                                 SET_UNIT_Y,
                                 (int16_t)(2 * NUM16_ADV), NUM16_FONT_HEIGHT,
                                 &num16, "°C", palette->setpoint,
                                 SGL_ALIGN_TOP_LEFT);

    /* 电压电流框：橙色 1 px 边框 + 两行橙色读数 */
    main_power_box = create_rect(main_page, RIGHT_PANEL_X, POWER_BOX_Y, 98,
                                 POWER_BOX_H, palette->power_bg,
                                 palette->power_border, 1U, 6U);
    /* 16 px 字库行高是 20 px，标签框必须同高；
     * 原来的 18 px 会把上下各裁掉一行像素（U 形字会缺底边） */
    main_power_voltage = create_label(main_page, RIGHT_PANEL_X, 110, 98, 20,
                                      &ui14, "0.0V", palette->power,
                                      SGL_ALIGN_CENTER);
    main_power_current = create_label(main_page, RIGHT_PANEL_X, 130, 98, 20,
                                      &ui14, "0.0A", palette->power,
                                      SGL_ALIGN_CENTER);

    /* MODE 框：暗色底 + 蓝色值（标题用 Sb、值用 Rg，对应 HTML 的 400/600） */
    main_mode_box = create_rect(main_page, RIGHT_PANEL_X, MODE_BOX_Y, 98,
                                MODE_BOX_H, palette->mode_bg, palette->line,
                                1U, 6U);
    main_mode_title = create_label(main_page, 220, MODE_BOX_Y, 40, MODE_BOX_H,
                                   &ui11s, "MODE", palette->muted,
                                   SGL_ALIGN_LEFT_MID);
    main_mode_value = create_label(main_page, 260, MODE_BOX_Y, 48, MODE_BOX_H,
                                   &ui11, "HOLD", palette->mode_value,
                                   SGL_ALIGN_RIGHT_MID);

    /* 底部三个快捷温度：选中项用青色卡片 */
    for (i = 0U; i < 3U; i++) {
        int16_t x = (int16_t)(PRESET_X0 + (i * PRESET_GAP));

        main_preset_bg[i] = create_rect(main_page, x, PRESET_TOP, PRESET_W,
                                        PRESET_H, palette->card,
                                        palette->card_border, 1U, 6U);
        main_preset_label[i] = create_label(main_page, x, PRESET_TOP, PRESET_W,
                                            PRESET_H, &ui14s, "---°C",
                                            palette->card_text,
                                            SGL_ALIGN_CENTER);
        /* 任意一个快捷温度建不出来，后面的刷新就会踩空指针 */
        if ((NULL == main_preset_bg[i]) || (NULL == main_preset_label[i])) {
            return false;
        }
    }

    /* 任一控件没建出来，后面的刷新就会踩空指针；统一在这里判死 */
    if ((NULL == main_top_bar) || (NULL == main_header_line) ||
        (NULL == main_handle_bg) ||
        (NULL == main_handle_label) || (NULL == main_page_bg) ||
        (NULL == main_page_label) ||
        (NULL == main_state_bg) || (NULL == main_state_label) ||
        (NULL == main_temp_panel) || (NULL == main_temp_label) ||
        (NULL == main_temp_unit) || (NULL == main_duty_bg) ||
        (NULL == main_duty_fill) || (NULL == main_set_box) ||
        (NULL == main_set_title) || (NULL == main_set_digits) ||
        (NULL == main_set_unit) || (NULL == main_power_box) ||
        (NULL == main_power_voltage) || (NULL == main_power_current) ||
        (NULL == main_mode_box) || (NULL == main_mode_title) ||
        (NULL == main_mode_value)) {
        return false;
    }
    return true;
}

static bool create_no_tool_page(void)
{
    no_tool_page = sgl_obj_create(NULL);
    /* 告警页根对象失败说明堆已经不够 */
    if (NULL == no_tool_page) {
        return false;
    }
    sgl_page_set_color(no_tool_page, palette->bg);

    /* 顶栏和其它页面一致：暗底 + 型号胶囊 + 黄色 Warning 胶囊 */
    no_tool_top_bar = create_rect(no_tool_page, 0, 0, SCREEN_WIDTH, 30,
                                  palette->topbar, palette->topbar, 0U, 0U);
    no_tool_header_line = create_rect(no_tool_page, 0, 30, SCREEN_WIDTH, 1,
                                      palette->header_line, palette->header_line,
                                      0U, 0U);
    /* 这一页只在"没识别到烙铁头"时显示，型号固定是 ---- */
    no_tool_handle_bg = create_rect(no_tool_page, 9, 4, 56, 22,
                                    palette->handle_bg, palette->handle,
                                    1U, 5U);
    no_tool_handle_label = create_label(no_tool_page, 9, 4, 56, 22, &ui13,
                                        "----", palette->handle_text,
                                        SGL_ALIGN_CENTER);
    no_tool_warn_bg = create_rect(no_tool_page, CHIP_X, 4, 76, 22,
                                  palette->badge_warn, palette->badge_warn,
                                  1U, 5U);
    no_tool_warn_label = create_label(no_tool_page, CHIP_X, 4, 76, 22, &ui13,
                                      "Warning", palette->badge_text,
                                      SGL_ALIGN_CENTER);

    /* 内容面板：从顶栏下 8 px 起，黄边 */
    no_tool_panel = create_rect(no_tool_page, 8, 38, 304, 196,
                                palette->panel, palette->badge_warn, 2U, 8U);
    /* 52 px 字形：无工具三个字撑满告警页 */
    no_tool_title = create_label(no_tool_page, 8, 76, 304, 76, &cjk52,
                                 "无工具", palette->text, SGL_ALIGN_CENTER);
    /* 文案固定：不再区分"没插"和"断偶"，两种情况的处理动作本来就一样 */
    no_tool_hint = create_label(no_tool_page, 8, 172, 304, 24, &cjk16,
                                "请插入工具或检查烙铁头", palette->muted,
                                SGL_ALIGN_CENTER);

    if ((NULL == no_tool_top_bar) || (NULL == no_tool_header_line) ||
        (NULL == no_tool_handle_bg) || (NULL == no_tool_handle_label) ||
        (NULL == no_tool_warn_bg) || (NULL == no_tool_warn_label) ||
        (NULL == no_tool_panel) || (NULL == no_tool_title) ||
        (NULL == no_tool_hint)) {
        return false;
    }
    return true;
}

static bool create_menu_page(void)
{
    uint8_t i;

    menu_page = sgl_obj_create(NULL);
    /* 菜单页根对象失败说明堆已经不够 */
    if (NULL == menu_page) {
        return false;
    }
    sgl_page_set_color(menu_page, palette->bg);

    menu_top_bar = create_rect(menu_page, 0, 0, SCREEN_WIDTH, MENU_HEADER_H,
                               palette->topbar, palette->topbar, 0U, 0U);
    menu_header_line = create_rect(menu_page, 0, MENU_HEADER_H, SCREEN_WIDTH, 1,
                                   palette->header_line, palette->header_line,
                                   0U, 0U);
    /* 型号胶囊：和主界面同款，让用户知道当前是哪个手柄在配置 */
    menu_handle_bg = create_rect(menu_page, 9, CHIP_Y, 56, CHIP_H,
                                 palette->handle_bg, palette->handle,
                                 1U, 6U);
    menu_handle_label = create_label(menu_page, 9, CHIP_Y, 56, CHIP_H, &ui13,
                                     "C245", palette->handle_text,
                                     SGL_ALIGN_CENTER);
    /* 金色标题胶囊：显示当前页名（设置/工具设置/参数调整……）。
     * 长度分两档，短档 64 与主页金色胶囊完全一致；默认先按短档建，
     * 进到 4 个字的子页时由 refresh_menu() 换成 CHIP_GOLD_LONG_W。 */
    menu_title_bg = create_rect(menu_page, CHIP_X, CHIP_Y, CHIP_GOLD_SHORT_W,
                                CHIP_H, palette->edit_bg, palette->setpoint,
                                1U, 6U);
    menu_title_label = create_label(menu_page, CHIP_X, CHIP_Y,
                                    CHIP_GOLD_SHORT_W, CHIP_H, &cjk16, "设置",
                                    palette->text, SGL_ALIGN_CENTER);
    /* 提示行：固定文案，长按返回等操作说明 */
    menu_hint_label = create_label(menu_page, MENU_ROW_LEFT, MENU_HINT_Y,
                                   MENU_ROW_WIDTH, MENU_HINT_H, &cjk16,
                                   "旋转选择  短按确认  长按返回",
                                   palette->hint, SGL_ALIGN_CENTER);

    /* 一屏 5 行：每行是"卡片 + 名称 + 数值 + 选中箭头" */
    for (i = 0U; i < MENU_VISIBLE_ROWS; i++) {
        int16_t y = (int16_t)(MENU_ROW_TOP + (i * MENU_ROW_PITCH));

        /* 卡片必须先建，才能画在文字下面 */
        menu_row_card[i] = create_rect(menu_page, MENU_ROW_LEFT, y,
                                       MENU_ROW_WIDTH, MENU_ROW_HEIGHT,
                                       palette->row_bg, palette->row_border,
                                       1U, 6U);
        menu_row_name[i] = create_label(menu_page, MENU_NAME_X, y, MENU_NAME_W,
                                        MENU_ROW_HEIGHT, &cjk16, "",
                                        palette->item_name, SGL_ALIGN_LEFT_MID);
        menu_row_value[i] = create_label(menu_page, MENU_VALUE_X, y,
                                         MENU_VALUE_W, MENU_ROW_HEIGHT, &cjk16,
                                         "", palette->item_value,
                                         SGL_ALIGN_RIGHT_MID);
        /* 选中箭头用 NK57 的实心三角，只有当前行才显示 */
        menu_row_arrow[i] = create_label(menu_page, MENU_ARROW_X, y,
                                         MENU_ARROW_W, MENU_ROW_HEIGHT,
                                         &arrow16, "▶", palette->accent,
                                         SGL_ALIGN_CENTER);
        /* 任意一行建不出来，后面 refresh 就会踩空指针 */
        if ((NULL == menu_row_card[i]) || (NULL == menu_row_name[i]) ||
            (NULL == menu_row_value[i]) || (NULL == menu_row_arrow[i])) {
            return false;
        }
    }

    /* 顶栏两个胶囊、提示行、标题缺一个都不行 */
    if ((NULL == menu_top_bar) || (NULL == menu_header_line) ||
        (NULL == menu_handle_bg) || (NULL == menu_handle_label) ||
        (NULL == menu_title_bg) || (NULL == menu_title_label) ||
        (NULL == menu_hint_label)) {
        return false;
    }
    return true;
}

static bool create_param_page(void)
{
    sgl_linechart_t *chart_data;

    param_page = sgl_obj_create(NULL);
    /* 参数页根对象失败说明堆已经不够 */
    if (NULL == param_page) {
        return false;
    }
    sgl_page_set_color(param_page, palette->bg);

    param_top_bar = create_rect(param_page, 0, 0, SCREEN_WIDTH, 30,
                                palette->topbar, palette->topbar, 0U, 0U);
    param_title_label = create_label(param_page, 12, 0, 240, 30, &cjk16,
                                     "参数调整", palette->text,
                                     SGL_ALIGN_LEFT_MID);
    param_accent = create_rect(param_page, 0, 30, SCREEN_WIDTH, 2,
                               palette->accent, palette->accent, 0U, 0U);
    param_name_label = create_label(param_page, 20, 38, 280, 24, &cjk16, "",
                                    palette->muted, SGL_ALIGN_LEFT_MID);
    param_value_label = create_label(param_page, 20, 64, 150, 48, &num32, "0",
                                     palette->text, SGL_ALIGN_LEFT_MID);
    param_unit_label = create_label(param_page, 176, 80, 120, 24, &cjk16, "",
                                    palette->muted, SGL_ALIGN_LEFT_MID);

    /* 曲线是参数页的主体，建不出来就整体失败 */
    param_chart = sgl_linechart_create(param_page);
    if (NULL == param_chart) {
        return false;
    }
    sgl_obj_set_pos(param_chart, 8, 122);
    sgl_obj_set_size(param_chart, 304, 110);
    sgl_obj_set_border_width(param_chart, 1U);
    sgl_linechart_set_bg_color(param_chart, palette->panel);
    sgl_linechart_set_border_color(param_chart, palette->line);

    /* series 数组用静态存储：运行时 sgl_malloc 在 24 KB 堆边缘很容易失败，
     * 失败的表现是曲线只有边框没有数据。 */
    chart_data = (sgl_linechart_t *)param_chart;
    (void)memset(&chart_series, 0, sizeof(chart_series));
    chart_data->series = &chart_series;
    chart_data->series_count = 1U;
    chart_data->series[0].line_color = palette->accent;
    chart_data->series[0].line_width = 1U;
    chart_data->series[0].style_bits.show_line = 1U;

    sgl_linechart_set_series_y_array(param_chart, 0U, curve_values,
                                     CURVE_POINT_COUNT);

    /* X 轴就是 60 个采样点，不画标签和网格，省得和曲线抢地方 */
    sgl_linechart_set_axis_range(param_chart, SGL_LINECHART_AXIS_X, 0,
                                 CURVE_POINT_COUNT - 1);
    sgl_linechart_enable_axis_labels(param_chart, SGL_LINECHART_AXIS_X, false);
    sgl_linechart_enable_axis_grid(param_chart, SGL_LINECHART_AXIS_X, false);
    sgl_linechart_enable_axis_ticks(param_chart, SGL_LINECHART_AXIS_X, false);

    /* Y 轴 0…500℃，每 250℃ 一条线，方便一眼看出当前温度 */
    sgl_linechart_set_axis_range(param_chart, SGL_LINECHART_AXIS_Y, 0, 500);
    sgl_linechart_set_axis_step(param_chart, SGL_LINECHART_AXIS_Y, 250);
    sgl_linechart_enable_axis_labels(param_chart, SGL_LINECHART_AXIS_Y, true);
    sgl_linechart_enable_axis_grid(param_chart, SGL_LINECHART_AXIS_Y, true);
    sgl_linechart_enable_axis_ticks(param_chart, SGL_LINECHART_AXIS_Y, true);
    sgl_linechart_set_axis_label_font(param_chart, SGL_LINECHART_AXIS_Y,
                                      &cjk16);
    sgl_linechart_set_axis_label_color(param_chart, SGL_LINECHART_AXIS_Y,
                                       palette->muted, 255U);
    sgl_linechart_set_axis_grid_color(param_chart, SGL_LINECHART_AXIS_Y,
                                      palette->dim, 100U);
    sgl_linechart_set_plot_area_rel(param_chart, 34, 4, 258, 94);

    apply_chart_style();
    reset_curve();

    /* 参数页的标题/名称/值/单位缺一个都不行 */
    if ((NULL == param_top_bar) || (NULL == param_title_label) ||
        (NULL == param_accent) || (NULL == param_name_label) ||
        (NULL == param_value_label) || (NULL == param_unit_label)) {
        return false;
    }
    return true;
}

/* ---- 温度曲线 ------------------------------------------------------ */

static int32_t get_curve_value(void)
{
    int32_t value;

    /* 没工具或断偶时画 0，别把 0℃ 当成真实温度画上去 */
    value = is_tip_present() ? station.temperature_int : 0;

    /* 曲线坐标只画 0…500 ℃，超范围会画到框外 */
    if (value < 0) {
        return 0;
    }
    if (value > 500) {
        return 500;
    }
    return value;
}

static void reset_curve(void)
{
    int32_t value = get_curve_value();
    uint8_t i;

    /* 整条曲线先铺成当前温度，避免刚从主界面切过来时是一条斜线 */
    for (i = 0U; i < CURVE_POINT_COUNT; i++) {
        curve_values[i] = value;
    }
    curve_sample_ms = get_tick_ms();
    curve_needs_reset = 0U;
}

static void sample_curve(void)
{
    uint32_t now = get_tick_ms();
    uint8_t i;

    /* 刚进参数页时先把整条曲线铺平，避免出现一条斜线 */
    if (0U != curve_needs_reset) {
        reset_curve();
    }
    /* 500 ms 一个点：既看得出升温斜率，也不至于把 CPU 都花在曲线重绘上 */
    if ((now - curve_sample_ms) < 500UL) {
        return;
    }
    curve_sample_ms = now;

    /* 整体左移一格，把最老的点挤掉 */
    for (i = 0U; i < (CURVE_POINT_COUNT - 1U); i++) {
        curve_values[i] = curve_values[i + 1U];
    }
    curve_values[CURVE_POINT_COUNT - 1U] = get_curve_value();

    /* 编辑参数时不重绘曲线，否则编码器操作会被图表刷新拖慢；
     * 数据照常记录，确认/退出编辑后下一拍就恢复。 */
    if (0U == station.menu_editing) {
        sgl_linechart_update(param_chart);
    }
}

/* ---- 主界面刷新 ---------------------------------------------------- */

/* "数字 + 单位"两段的起点只取决于数字位数（1/2/3），因为两套字体都是等宽。
 * 下标 0 对应 1 位数，以此类推；第 4 项给越界的脏数据兜底（按 3 位处理）。
 * 这样每帧只做一次查表 + 一次比较，不再调 sgl_font_get_string_width()。 */
static const int16_t temp_group_x[3] = {
    MAIN_TEMP_X_1, MAIN_TEMP_X_2, MAIN_TEMP_X_3,
};
static const int16_t set_group_x[3] = {
    SET_DIGITS_X_1, SET_DIGITS_X_2, SET_DIGITS_X_3,
};

/**
 * @brief 按数字位数查表摆温度组（数字 + °C），不再运行时量字宽
 * @param text  当前数字文本（"--"、"0"…"999"）
 * @retval 实际参与排版的字数
 * @note  位置只在位数变化时才 set_pos：同坐标 set_pos 也会标脏重绘。
 */
static uint8_t place_temp_group(const char *text)
{
    uint8_t digits = 0U;
    int16_t start;
    int16_t unit_x;

    /* "--" 占位符也按自己的字数参与居中，否则无工具时整组会偏左 */
    while ('\0' != text[digits]) {
        digits++;
    }
    /* 字数只可能是 1…3（温度钳在 0…999，占位符是 2 个 '-'） */
    if ((0U == digits) || (digits > 3U)) {
        digits = 3U;
    }
    start = temp_group_x[digits - 1U];
    unit_x = (int16_t)(start + (digits * NUM90_ADV) + TEMP_GROUP_GAP);

    /* 位置没变就不动控件，否则每帧 set_pos 会把整个标签标脏 */
    if (start != temp_label_x) {
        temp_label_x = start;
        sgl_obj_set_pos(main_temp_label, start, MAIN_TEMP_DIGIT_Y);
    }
    if (unit_x != temp_unit_x) {
        temp_unit_x = unit_x;
        sgl_obj_set_pos(main_temp_unit, unit_x, MAIN_TEMP_UNIT_Y);
    }
    return digits;
}

/**
 * @brief 按数字位数查表摆设定框里的"数字 + °C"组
 * @note  98 px 框里 3 位数是 3×16 + 4 + 2×8 = 68，两侧各留 15，永远放得下。
 */
static void place_set_group(const char *text)
{
    uint8_t digits = 0U;
    int16_t start;
    int16_t unit_x;

    while ('\0' != text[digits]) {
        digits++;
    }
    /* 字数只可能是 1…3（设定值钳在 0…999） */
    if ((0U == digits) || (digits > 3U)) {
        digits = 3U;
    }
    start = set_group_x[digits - 1U];
    unit_x = (int16_t)(start + (digits * NUM32_ADV) + SET_GROUP_GAP);

    /* 位数没变就不重摆，省掉一次整标签重绘 */
    if (start != set_digits_x) {
        set_digits_x = start;
        sgl_obj_set_pos(main_set_digits, start, SET_DIGITS_Y);
    }
    if (unit_x != set_unit_x) {
        set_unit_x = unit_x;
        sgl_obj_set_pos(main_set_unit, unit_x, SET_UNIT_Y);
    }
}

/** 当前生效的快捷温度槽位；手动调温到某个预设值时也点亮它 */
static uint8_t get_active_preset(void)
{
    uint8_t i;

    if (station.preset_index < PRESET_COUNT) {
        return station.preset_index;
    }
    for (i = 0U; i < PRESET_COUNT; i++) {
        if (station.preset_temperature[i] == station.set_temperature) {
            return i;
        }
    }
    return 0xFFU;
}

/**
 * @brief 把"向零截断的温度整数"做显示侧平滑，消除末位闪动
 * @param raw_int app_heat.c 算出的 station.temperature_int（截断值）
 * @retval 应该显示给用户的整数温度
 *
 * 为什么需要：`temperature_int` 是截断取整。300℃ 附近真实值在
 * 299.6~300.4 之间摆动时，截断结果就在 299/300 之间跳，
 * 用户看到末位数字闪 —— 这就是"±2℃ 波动"最直观的表现形式。
 *
 * 两个阈值（v9.26，针对实测跨度 4℃ 的摆动）：
 *   · 偏差 >= TEMP_JUMP_THRESHOLD（10）→ 立刻跟随；
 *   · 偏差 <= TEMP_HOLD_STEPS（9）→ 按住，持续满 800 ms 才跟随。
 *   两者之间由 HOLD_STEPS = JUMP - 1 保证不会出现空缺。
 *   余量 6℃ 是为了覆盖"首帧采纳值随机落在摆动边缘"（见常量说明）。
 *
 * 为什么用"上次显示值"而不是滑动平均：
 *   显示侧要做的是"抑制闪烁"，不是"跟踪变化"。滑动平均会把
 *   真实的快速升温也拖慢（看起来迟钝）；而"阶跃式死区"
 *   对真实变化依然是一次到位，只滤掉 1℃ 级的抖动。
 *
 * 注意这是**纯显示**：不回写 station.temperature_int，
 * 不影响 PID、不影响到温判定、不影响提示音。
 */
static int16_t stabilize_display_temp(int16_t raw_int)
{
    int16_t delta;
    uint32_t now;

    /* 还没算过（换页/换主题/换手柄后的第一帧）：直接采纳。
     * 用 temp_valid 判断而不是 `shown_temp_int < 0` —— 后者会把
     * 合法的负温度误当成"未初始化"，见变量声明处的说明。 */
    if (0U == temp_valid) {
        shown_temp_int = raw_int;
        temp_valid = 1U;
        temp_hold_ms = 0U;
        return shown_temp_int;
    }

    delta = (int16_t)(raw_int - shown_temp_int);
    if (delta < 0) {
        delta = (int16_t)(-delta);
    }

    /* 偏差 >= TEMP_JUMP_THRESHOLD：真变化，立刻跟随（一次到位，绝不逐步爬） */
    if (delta >= TEMP_JUMP_THRESHOLD) {
        shown_temp_int = raw_int;
        temp_hold_ms = 0U;
        return shown_temp_int;
    }

    /* 偏差 == 0：显示值就等于真实值，**必须清计时**。
     * 不清的话，下次再出现小偏差会沿用这次留下的旧起点，
     * 直接判成"已持续 500 ms"而立刻跳变 —— 慢跟就失效了。 */
    if (0 == delta) {
        temp_hold_ms = 0U;
        return shown_temp_int;
    }

    /* 偏差在 1 ~ TEMP_HOLD_STEPS（1~5）：**不立刻跟随，但开始计时**。
     *
     * 为什么既要"慢跟"又要"跟"：
     *   只保持不跟的话，真实温度长期停在小偏差处时显示会永远差那几度
     *   —— 用户会觉得"显示不准"。
     *   立刻跟的话，±2℃ 抖动就会让末位每帧闪 —— 这就是要修的毛病。
     *   折中：小偏差先按住，**持续满 500 ms 才接受**。
     *   抖动通常几十毫秒就回摆，够不到 500 ms，所以不闪；
     *   而真实偏移会稳定存在，500 ms 后自然跟上去。
     *
     * 副作用（用户已确认接受）：改设定值后若实际温度变化在 1~9℃，
     * 显示会在 800 ms 后才跟上；≥10℃ 立刻到位。
     *
     * **分支覆盖说明（不留歧义）**：
     *   走到这里时必有 0 < delta < TEMP_JUMP_THRESHOLD（10），
     *   即 delta ∈ {1..9} —— **全部都走慢跟，没有"保持不动"的分支**。
     *   也就是说 delta==9 一定是"800 ms 后接受"，不是"永远不跟"。
     *   （真实温度已经偏了 5℃ 就该跟上，只是延后 500 ms 确认它是稳态。） */
    now = get_tick_ms();
    if (0UL == temp_hold_ms) {
        temp_hold_ms = now;                /* 首次出现 1℃ 偏差：开始计时 */
        return shown_temp_int;
    }
    if ((now - temp_hold_ms) < TEMP_SLOW_FOLLOW_MS) {
        return shown_temp_int;             /* 还没到 500 ms：继续按住 */
    }

    /* 持续满 500 ms：接受新值 */
    shown_temp_int = raw_int;
    temp_hold_ms = 0U;
    return shown_temp_int;
}

/**
 * @brief 每帧更新一次"已到温"的显示状态（写入 ready_shown）
 *
 * 为什么必须**每帧只调用一次**（v9.22 修正）：
 *   最初写成"有状态的查询函数"，`get_state_text()` 和 `get_mode_text()`
 *   各调用一次 —— 同一帧内第二次调用会看到第一次已经改过的
 *   `ready_change_ms`，撤销计时的起点被反复刷新，导致
 *   徽标与 MODE 框行为不一致（一个刚显示已到温、一个还是加热中）。
 *
 * 现在拆成"更新"和"读取"两件事：
 *   · `update_ready_display()` 每帧在 refresh_main() 开头调一次，
 *     负责推进计时、写入 ready_shown；
 *   · `get_state_text()` / `get_mode_text()` 只**读** ready_shown，
 *     纯函数、无副作用、调用几次都一样。
 *
 * 为什么需要保持：app_heat.c 的 `FLAG_TEMP_REACHED` 滞回带只有 1℃
 * （进入 >= 设定值-1、退出 < 设定值-2），而 300℃ 时 temperature_int
 * 本身在 ±2℃ 摆动 → 该位反复置位/清除 → 直接显示就是闪。
 *
 * **置位立即、撤销延迟 300 ms**：
 *   两个方向都延迟的话，到温瞬间会先"加热中"再跳"已到温"，反而成为
 *   新的闪烁源。所以只在"该撤销"时等 300 ms 确认。
 *   300 ms ≈ 10 个 UI 周期（10 ms/帧），能吞掉 1℃ 级抖动又不觉得迟钝。
 *
 * 只影响徽标与 MODE 的文字/配色，**不修改 station.state**；
 * 到温提示音归 app_heat.c 管，UI 不介入。
 */
static void update_ready_display(void)
{
    uint8_t want = (0U != (station.state & FLAG_TEMP_REACHED)) ? 1U : 0U;
    uint32_t now;

    /* 期望值与当前显示一致：撤销计时作废，什么都不用做 */
    if (want == ready_shown) {
        ready_pending = 0U;
        return;
    }

    /* 期望"置位"：立即生效（到温要立刻反馈） */
    if (0U != want) {
        ready_shown = 1U;
        ready_pending = 0U;
        return;
    }

    /* 期望"撤销"：等到确认满 300 ms 才真的撤。
     *
     * **用 ready_pending 判"是否已在等待"，不要用 ready_change_ms == 0**
     * （get_tick_ms() 可能返回 0，那样两种语义无法区分，见声明处说明）。
     * 也不能每帧重置起点 —— 否则计时永远走不满 300 ms，
     * 撤销就永远不发生（等于卡在"已到温"）。 */
    now = get_tick_ms();
    if (0U == ready_pending) {
        ready_pending = 1U;                /* 首次发现要撤销：开始计时 */
        ready_change_ms = now;
        return;
    }
    if ((now - ready_change_ms) < READY_HOLD_DISPLAY_MS) {
        return;                            /* 还没确认满：继续显示已到温 */
    }
    ready_shown = 0U;                      /* 确认满：真的掉出到温带 */
    ready_pending = 0U;
}

static void update_state_badge(void)
{
    sgl_color_t badge;
    const char *text = get_state_text(&badge);

    (void)snprintf(main_state_text, sizeof(main_state_text), "%s", text);
    update_text(main_state_label, main_state_shown, sizeof(main_state_text),
                main_state_text);

    /* 徽标底色跟着状态变；颜色没变就不重设 */
    if ((0U == state_color_valid) || (badge.full != state_color_cache.full)) {
        state_color_valid = 1U;
        state_color_cache = badge;
        sgl_rect_set_color(main_state_bg, badge);
        sgl_rect_set_border_color(main_state_bg, badge);
    }
}

static void update_preset_cards(uint8_t active)
{
    uint8_t i;

    if (0U == preset_color_dirty) {
        return;
    }
    preset_color_dirty = 0U;

    for (i = 0U; i < 3U; i++) {
        uint8_t selected = (i == active) ? 1U : 0U;

        sgl_rect_set_color(main_preset_bg[i],
                           (0U != selected) ? palette->selected_bg
                                            : palette->card);
        sgl_rect_set_border_color(main_preset_bg[i],
                                  (0U != selected) ? palette->selected_border
                                                   : palette->card_border);
        sgl_label_set_text_color(main_preset_label[i],
                                 (0U != selected) ? palette->selected_text
                                                  : palette->card_text);
    }
}

static void update_duty_bar(void)
{
    uint32_t duty = station.heat_duty;
    uint32_t width;

    /* 占空比偶尔会因 PID 超调超过 250，先钳住再换算 */
    if (duty > DUTY_FULL_SCALE) {
        duty = DUTY_FULL_SCALE;
    }
    width = (duty * DUTY_BAR_WIDTH) / DUTY_FULL_SCALE;

    /* 0% 直接隐藏填充条，避免残留 1 px 的亮线 */
    if (0UL == width) {
        /* 已经隐藏过就不再改，避免每帧都标脏 */
        if (0U != duty_fill_visible) {
            duty_fill_visible = 0U;
            sgl_obj_set_hidden(main_duty_fill);
        }
        return;
    }

    /* 从 0% 恢复输出时要把填充条重新显示出来 */
    if (1U != duty_fill_visible) {
        duty_fill_visible = 1U;
        sgl_obj_set_visible(main_duty_fill);
    }
    /* 宽度变了才改尺寸：每帧改一次同样会触发重绘 */
    if ((int16_t)width != duty_fill_width) {
        duty_fill_width = (int16_t)width;
        sgl_obj_set_size(main_duty_fill, (int16_t)width, DUTY_BAR_HEIGHT);
    }
}

static void format_power_texts(void)
{
    /* 先放大 10 倍取整，再拆成整数/小数，避免浮点 printf 拖进 12 KB 库 */
    int32_t voltage_tenth = (int32_t)(station.input_voltage * 10.0F);
    int32_t current_tenth = (int32_t)(station.heat_current * 10.0F);

    /* 采样噪声可能让换算结果为负，显示上不能出现 -0.1V */
    if (voltage_tenth < 0) {
        voltage_tenth = 0;
    }
    if (current_tenth < 0) {
        current_tenth = 0;
    }
    (void)snprintf(main_voltage_text, sizeof(main_voltage_text), "%d.%dV",
                   (int)(voltage_tenth / 10), (int)(voltage_tenth % 10));
    (void)snprintf(main_current_text, sizeof(main_current_text), "%d.%dA",
                   (int)(current_tenth / 10), (int)(current_tenth % 10));
}

static void refresh_main(void)
{
    uint16_t setpoint;
    uint8_t active;
    uint8_t i;

    /* **每帧先把"已到温"的显示状态推进一次**（v9.22）
     *
     * 必须在这里、且整个页面只调一次：
     * get_state_text() / get_mode_text() 只读 ready_shown，
     * 若让它们各自推进计时，同一帧内第二次调用会看到
     * 第一次改过的 ready_change_ms，徽标与 MODE 框行为会不一致。 */
    update_ready_display();

    /* 顶栏：型号 / 状态（"主页"是固定文案，创建时已写好） */
    (void)snprintf(main_handle_text, sizeof(main_handle_text), "%s",
                   get_tip_code());
    update_text(main_handle_label, main_handle_shown,
                sizeof(main_handle_text), main_handle_text);

    update_state_badge();

    /* 温度大数字：无工具/断偶/休眠时显示 --，不把 0℃ 当真实读数。
     *
     * **v9.22：显示前先过一层稳定化**（stabilize_display_temp）。
     * station.temperature_int 是向零截断的整数，300℃ 附近真实值在
     * 299.6~300.4 摆动时整数就在 299/300 跳 —— 这就是"末位闪"。
     * 稳定化只在偏差 ≤1℃ 时保持原值（吃抖），**偏差 ≥2℃ 直接跳到新值**
     * （真实升温/降温一次到位，不会被拖慢）。纯显示，不回写 station。 */
    if ((!is_tip_present()) ||
        (0U != (station.state & (FLAG_TIP_FAULT | FLAG_SLEEP)))) {
        /* **显示 "--" 期间必须作废温度记忆与慢跟计时**（v9.23 / v9.24）
         *
         * 为什么作废 shown_temp_int：进入休眠/断偶后温度可能大幅变化
         * （休眠时降温、换手柄后是另一支头的温度），但记忆还留着进入前的值。
         * 退出后第一帧会拿旧值当基准比较 —— 若新旧值恰好相差 ≤1℃，
         * 显示就"卡"在旧值上不跟着更新（v9.23 已修）。
         *
         * 为什么还要清 temp_hold_ms（v9.24）：若不作废计时，休眠期间
         * 留下的 1℃ 偏差起点会在退出休眠后立刻满足"持续 500 ms"，
         * 导致第一帧就跳变 —— 慢跟本该有的"先按住"效果完全失效。
         * 两个一起清，退出后才是一次干净的重来。 */
        temp_valid = 0U;
        shown_temp_int = 0;
        temp_hold_ms = 0U;
        (void)snprintf(main_temp_text, sizeof(main_temp_text), "--");
    } else {
        int32_t value = stabilize_display_temp(station.temperature_int);

        if (value < 0) {
            value = 0;
        }
        if (value > 999) {
            value = 999;
        }
        (void)snprintf(main_temp_text, sizeof(main_temp_text), "%d",
                       (int)value);
    }
    update_text(main_temp_label, main_temp_shown, sizeof(main_temp_text),
                main_temp_text);
    (void)place_temp_group(main_temp_text);

    /* 设定值：休眠显示 0，待机显示待机温度，正常显示工作温度 */
    setpoint = (0U != (station.state & FLAG_SLEEP)) ? 0U : get_setpoint();
    if (setpoint > 999U) {
        setpoint = 999U;
    }
    (void)snprintf(main_set_text, sizeof(main_set_text), "%u",
                   (unsigned)setpoint);
    update_text(main_set_digits, main_set_shown, sizeof(main_set_text),
                main_set_text);
    place_set_group(main_set_text);

    /* 电压电流 */
    format_power_texts();
    update_text(main_power_voltage, main_voltage_shown,
                sizeof(main_voltage_text), main_voltage_text);
    update_text(main_power_current, main_current_shown,
                sizeof(main_current_text), main_current_text);

    /* MODE */
    (void)snprintf(main_mode_text, sizeof(main_mode_text), "%s",
                   get_mode_text());
    update_text(main_mode_value, main_mode_shown, sizeof(main_mode_text),
                main_mode_text);

    /* 三个快捷温度 */
    for (i = 0U; i < 3U; i++) {
        uint16_t value = station.preset_temperature[i];

        if (0U == value) {
            (void)snprintf(main_preset_text[i], sizeof(main_preset_text[i]),
                           "---");
        } else {
            (void)snprintf(main_preset_text[i], sizeof(main_preset_text[i]),
                           "%u°C", (unsigned)value);
        }
        update_text(main_preset_label[i], main_preset_shown[i],
                    sizeof(main_preset_text[i]), main_preset_text[i]);
    }
    /* 选中的快捷温度变了才重刷三张卡片的配色 */
    active = get_active_preset();
    if (active != last_active_preset) {
        last_active_preset = active;
        preset_color_dirty = 1U;
    }
    update_preset_cards(active);
    update_duty_bar();
}

/* ---- 菜单刷新 ------------------------------------------------------ */

/**
 * @brief 让选中项始终留在可视窗口里
 * @note  规则照抄 HTML 设计稿：选中第一项无条件回顶部，选中最后一项
 *        无条件到底部，中间项才做"滚出视野就滚回来"。这里是按行滚，
 *        不是按像素滚 —— 卡片等高，按行滚不会出现半行残影。
 */
static void update_menu_scroll(void)
{
    uint8_t count = get_page_item_count(station.ui_mode);
    uint8_t scroll;

    if (count <= MENU_VISIBLE_ROWS) {
        scroll = 0U;
    } else if (0U == station.menu_selected) {
        scroll = 0U;
    } else if (station.menu_selected >= (uint8_t)(count - 1U)) {
        scroll = (uint8_t)(count - MENU_VISIBLE_ROWS);
    } else {
        scroll = menu_scroll;
        /* menu_scroll 还是初值（0xFF）时按 0 处理，别让下标飞出去 */
        if (scroll > (uint8_t)(count - MENU_VISIBLE_ROWS)) {
            scroll = 0U;
        }
        if (station.menu_selected < scroll) {
            scroll = station.menu_selected;
        }
        if (station.menu_selected >= (uint8_t)(scroll + MENU_VISIBLE_ROWS)) {
            scroll = (uint8_t)(station.menu_selected -
                               (MENU_VISIBLE_ROWS - 1U));
        }
    }
    menu_scroll = scroll;
}

/**
 * @brief 菜单切页时按页面类型把 5 行控件摆好
 * @note  关于页是纯文本，没有卡片/数值/箭头；其它页每行都是卡片，
 *        但只有"有值"的行才显示右侧数值。
 */
static void apply_menu_layout(uint8_t mode)
{
    uint8_t i;

    if (MODE_ABOUT == mode) {
        /* 关于页：四行说明文字，隐藏卡片、数值和箭头 */
        for (i = 0U; i < MENU_VISIBLE_ROWS; i++) {
            sgl_obj_set_hidden(menu_row_card[i]);
            sgl_obj_set_hidden(menu_row_value[i]);
            sgl_obj_set_hidden(menu_row_arrow[i]);
            row_value_visible[i] = 0U;
            row_arrow_visible[i] = 0U;
            if (i < 4U) {
                sgl_obj_set_visible(menu_row_name[i]);
                sgl_obj_set_pos(menu_row_name[i], MENU_NAME_X,
                                (int16_t)(ABOUT_LINE_TOP +
                                          (i * ABOUT_LINE_PITCH)));
                sgl_obj_set_size(menu_row_name[i], MENU_ROW_WIDTH, 28);
            } else {
                sgl_obj_set_hidden(menu_row_name[i]);
            }
        }
        return;
    }

    /* 普通页：卡片、名称、数值、箭头全部归位，具体显隐由 refresh 决定 */
    for (i = 0U; i < MENU_VISIBLE_ROWS; i++) {
        int16_t y = (int16_t)(MENU_ROW_TOP + (i * MENU_ROW_PITCH));

        sgl_obj_set_pos(menu_row_card[i], MENU_ROW_LEFT, y);
        sgl_obj_set_size(menu_row_card[i], MENU_ROW_WIDTH, MENU_ROW_HEIGHT);
        sgl_obj_set_pos(menu_row_name[i], MENU_NAME_X, y);
        sgl_obj_set_size(menu_row_name[i], MENU_NAME_W, MENU_ROW_HEIGHT);
        sgl_obj_set_pos(menu_row_value[i], MENU_VALUE_X, y);
        sgl_obj_set_size(menu_row_value[i], MENU_VALUE_W, MENU_ROW_HEIGHT);
        sgl_obj_set_pos(menu_row_arrow[i], MENU_ARROW_X, y);
        sgl_obj_set_size(menu_row_arrow[i], MENU_ARROW_W, MENU_ROW_HEIGHT);
        /* 三角是"可进入"标记，右对齐到数值列右边缘（不是选中光标） */
        sgl_label_set_text_align(menu_row_arrow[i], SGL_ALIGN_RIGHT_MID);
        /* 显隐缓存全部作废，交给 refresh_menu_rows 重新判定 */
        row_value_visible[i] = 0xFFU;
        row_arrow_visible[i] = 0xFFU;
        row_card_visible_cache[i] = 0xFFU;
        row_name_visible_cache[i] = 0xFFU;
        row_card_color_valid[i] = 0U;
    }
}

static void set_row_name_color(uint8_t index, sgl_color_t color)
{
    /* 颜色没变就不重设，标签颜色一变整行都要重绘 */
    if ((0U == row_name_color_valid[index]) ||
        (color.full != row_name_color_cache[index].full)) {
        row_name_color_valid[index] = 1U;
        row_name_color_cache[index] = color;
        sgl_label_set_text_color(menu_row_name[index], color);
    }
}

static void set_row_value_color(uint8_t index, sgl_color_t color)
{
    /* 值列同样做颜色缓存，选中/编辑行才需要改色 */
    if ((0U == row_value_color_valid[index]) ||
        (color.full != row_value_color_cache[index].full)) {
        row_value_color_valid[index] = 1U;
        row_value_color_cache[index] = color;
        sgl_label_set_text_color(menu_row_value[index], color);
    }
}

static void set_row_arrow_color(uint8_t index, sgl_color_t color)
{
    /* 三角就是"可进入"标记，配色跟随同行数值（选中变色，普通用 item_value） */
    if ((0U == row_arrow_color_valid[index]) ||
        (color.full != row_arrow_color_cache[index].full)) {
        row_arrow_color_valid[index] = 1U;
        row_arrow_color_cache[index] = color;
        sgl_label_set_text_color(menu_row_arrow[index], color);
    }
}

/** 行卡片配色：未选中 / 选中 / 编辑中三态，颜色没变就不碰控件 */
static void set_row_card_color(uint8_t index, sgl_color_t fill,
                               sgl_color_t border)
{
    if ((0U == row_card_color_valid[index]) ||
        (fill.full != row_card_fill_cache[index].full) ||
        (border.full != row_card_border_cache[index].full)) {
        row_card_color_valid[index] = 1U;
        row_card_fill_cache[index] = fill;
        row_card_border_cache[index] = border;
        sgl_rect_set_color(menu_row_card[index], fill);
        sgl_rect_set_border_color(menu_row_card[index], border);
    }
}

static void set_row_visible(sgl_obj_t *object, uint8_t *cache, uint8_t want)
{
    /* 显隐状态没变就不调用 set_hidden/set_visible，避免重复标脏 */
    if (want == *cache) {
        return;
    }
    *cache = want;
    if (0U != want) {
        sgl_obj_set_visible(object);
    } else {
        sgl_obj_set_hidden(object);
    }
}

static void refresh_about(void)
{
    static const char *const about_lines[] = {
        "JBC 焊台控制器",
        "HC32F460  200MHz",
        "SGL + cjk16/num32",
        "固件重构 v1.0",
    };
    uint8_t i;

    (void)snprintf(menu_hint_text, sizeof(menu_hint_text), "短按或长按返回");
    update_text(menu_hint_label, menu_hint_shown, sizeof(menu_hint_text),
                menu_hint_text);

    /* 四行固定说明文字，按 HTML 设计稿一律用正文色，不做高亮 */
    for (i = 0U; i < 4U; i++) {
        update_text(menu_row_name[i], menu_row_name_shown[i],
                    sizeof(menu_row_name_text[i]), about_lines[i]);
        set_row_name_color(i, palette->text);
    }
}

/**
 * @brief 刷新一屏菜单行
 * @note  第 slot 行显示的是"第 menu_scroll+slot 项"。
 *        右侧只有一个位置：**可进入的行**画 NK57 实心三角 ▶（取代以前的
 *        ">" 文本），其它行画数值；三角跟选中无关，只是"这里能进"的提示。
 */
static void refresh_menu_rows(uint8_t mode, uint8_t count)
{
    uint8_t slot;
    uint8_t item;
    uint8_t selected = station.menu_selected;
    uint8_t editing = station.menu_editing;
    uint8_t enterable;
    const char *value_text;
    char scratch[24];

    for (slot = 0U; slot < MENU_VISIBLE_ROWS; slot++) {
        item = (uint8_t)(menu_scroll + slot);

        /* 这一槽没有对应项：整行藏起来 */
        if (item >= count) {
            set_row_visible(menu_row_card[slot], &row_card_visible_cache[slot],
                            0U);
            set_row_visible(menu_row_name[slot], &row_name_visible_cache[slot],
                            0U);
            set_row_visible(menu_row_value[slot], &row_value_visible[slot], 0U);
            set_row_visible(menu_row_arrow[slot], &row_arrow_visible[slot], 0U);
            continue;
        }

        set_row_visible(menu_row_card[slot], &row_card_visible_cache[slot], 1U);
        set_row_visible(menu_row_name[slot], &row_name_visible_cache[slot], 1U);

        value_text = format_item_value(mode, item, scratch, sizeof(scratch));
        enterable = is_item_enterable(mode, item) ? 1U : 0U;

        update_text(menu_row_name[slot], menu_row_name_shown[slot],
                    sizeof(menu_row_name_text[slot]), get_item_name(mode, item));

        /* 三态配色：编辑中 > 选中 > 普通；数值与三角共用同一套颜色 */
        if (item == selected) {
            set_row_card_color(slot,
                               (0U != editing) ? palette->edit_bg
                                               : palette->selected_bg,
                               (0U != editing) ? palette->edit_border
                                               : palette->selected_border);
            set_row_name_color(slot, palette->text);
            set_row_value_color(slot, (0U != editing) ? palette->setpoint
                                                      : palette->selected_text);
            set_row_arrow_color(slot, palette->selected_text);
        } else {
            set_row_card_color(slot, palette->row_bg, palette->row_border);
            set_row_name_color(slot, palette->item_name);
            set_row_value_color(slot, palette->item_value);
            set_row_arrow_color(slot, palette->item_value);
        }

        /* 可进入的行：右侧画三角，不显示数值（两者互斥，同一个位置） */
        if (0U != enterable) {
            set_row_visible(menu_row_arrow[slot], &row_arrow_visible[slot], 1U);
            set_row_visible(menu_row_value[slot], &row_value_visible[slot], 0U);
            continue;
        }
        set_row_visible(menu_row_arrow[slot], &row_arrow_visible[slot], 0U);

        /* 没有值的行直接隐藏，别留一个空标签占位 */
        if ('\0' == value_text[0]) {
            set_row_visible(menu_row_value[slot], &row_value_visible[slot], 0U);
            continue;
        }
        set_row_visible(menu_row_value[slot], &row_value_visible[slot], 1U);

        /* 关键：format_item_value 写的是栈上的 scratch，而 sgl_label_set_text
         * 只保存指针。必须先复制到静态 menu_row_value_text[slot]，否则函数一
         * 返回，屏幕显示的就是栈上的垃圾数据。 */
        (void)snprintf(menu_row_value_text[slot],
                       sizeof(menu_row_value_text[slot]), "%s", value_text);
        update_text(menu_row_value[slot], menu_row_value_shown[slot],
                    sizeof(menu_row_value_text[slot]),
                    menu_row_value_text[slot]);
    }
}

/**
 * @brief 按标题字数在长短两档之间切换金色胶囊长度
 * @param text 当前页名（"设置"、"基本设置"……）
 * @note  用 UTF-8 字节数判字数：每个汉字 3 B。2 字（6 B）用短档 64，
 *        4 字（12 B）用长档 96。长档的 96 不是拍脑袋——4×16 = 64 px 字宽
 *        两侧各留 16 px 内边距，正好塞得下，也不比短档显得空。
 *        宽度没变就不碰控件：set_size 会把 rect 与 label 一起标脏。
 */
static void resize_title_chip(const char *text)
{
    uint32_t length = 0U;
    int16_t want;

    while ('\0' != text[length]) {
        length++;
    }
    /* 超过 2 个汉字就用长档；菜单页标题最长 4 个字（"基本设置"） */
    want = (length > CHIP_GOLD_WIDE_UTF8) ? (int16_t)CHIP_GOLD_LONG_W
                                          : (int16_t)CHIP_GOLD_SHORT_W;
    if (want == title_chip_width) {
        return;
    }
    title_chip_width = want;
    sgl_obj_set_size(menu_title_bg, want, CHIP_H);
    sgl_obj_set_size(menu_title_label, want, CHIP_H);
}

static void refresh_menu(void)
{
    uint8_t mode = station.ui_mode;
    uint8_t count = get_page_item_count(mode);

    (void)snprintf(menu_title_text, sizeof(menu_title_text), "%s",
                   get_page_title(mode));
    update_text(menu_title_label, menu_title_shown, sizeof(menu_title_text),
                menu_title_text);
    /* 标题变了胶囊也要跟着变长短：4 个字的子页用长档，2 个字的用短档 */
    resize_title_chip(menu_title_text);

    /* 型号胶囊跟着烙铁头走，和主界面保持一致 */
    (void)snprintf(menu_handle_text, sizeof(menu_handle_text), "%s",
                   get_tip_code());
    update_text(menu_handle_label, menu_handle_shown,
                sizeof(menu_handle_text), menu_handle_text);

    /* 关于页只有说明文字，没有可编辑项 */
    if (MODE_ABOUT == mode) {
        refresh_about();
        return;
    }

    (void)snprintf(menu_hint_text, sizeof(menu_hint_text),
                   (0U != station.menu_editing) ? "旋转调值  短按确认  长按取消"
                                                : "旋转选择  短按确认  长按返回");
    update_text(menu_hint_label, menu_hint_shown, sizeof(menu_hint_text),
                menu_hint_text);

    update_menu_scroll();
    refresh_menu_rows(mode, count);
}

/* ---- 参数页刷新 ---------------------------------------------------- */

static void refresh_param(void)
{
    uint8_t selected = station.menu_selected;
    char scratch[24];

    (void)snprintf(param_name_text, sizeof(param_name_text), "%s",
                   get_item_name(MODE_PARAM, selected));
    (void)snprintf(param_unit_text, sizeof(param_unit_text), "%s",
                   get_param_unit(selected));
    (void)snprintf(param_value_text, sizeof(param_value_text), "%s",
                   format_item_value(MODE_PARAM, selected, scratch,
                                     sizeof(scratch)));

    update_text(param_name_label, param_name_shown, sizeof(param_name_text),
                param_name_text);
    update_text(param_value_label, param_value_shown, sizeof(param_value_text),
                param_value_text);
    update_text(param_unit_label, param_unit_shown, sizeof(param_unit_text),
                param_unit_text);

    /* 名称在编辑态变青色，数值在编辑态变黄色；非编辑态用普通文字色。
     * 只在编辑状态变化时重设颜色，否则每帧 set_text_color 都会标脏。 */
    if (last_param_editing != station.menu_editing) {
        last_param_editing = station.menu_editing;
        sgl_label_set_text_color(param_name_label,
                                 (0U != station.menu_editing)
                                     ? palette->selected_text
                                     : palette->muted);
        sgl_label_set_text_color(param_value_label,
                                 (0U != station.menu_editing)
                                     ? palette->setpoint
                                     : palette->text);
    }

    sample_curve();
}

/* ---- 页面切换 ------------------------------------------------------ */

static page_id_t resolve_page(void)
{
    /* 没插烙铁头（或断偶）时**任何 ui_mode 都只显示告警页**。
     * 输入层已经把 ui_mode 收回主页了（见 task_indev），这里再判一次：
     * 屏幕正确性不该依赖"另一层有没有记得清理"。
     * 这条同时保证"在菜单里拔掉工具"不会卡在菜单上。 */
    if (!is_tip_present()) {
        return PAGE_NO_TOOL;
    }
    if (MODE_MAIN == station.ui_mode) {
        return PAGE_MAIN;
    }
    if (MODE_PARAM == station.ui_mode) {
        return PAGE_PARAM;
    }
    return PAGE_MENU;
}

/**
 * @brief 把"已到温"的显示状态直接同步成当前真实状态（页切换时用）
 *
 * 为什么只读一次、不做 300 ms 确认：
 *   300 ms 撤销确认是为了滤掉**运行中**的 1℃ 级抖动（每秒都在变）。
 *   而页切换是一次**离散事件**，此刻的真实状态就是该显示的状态，
 *   再等 300 ms 只是白闪一下。
 *
 * 为什么必须同步而不是留给 update_ready_display() 自己收敛：
 *   `invalidate_text_cache()` 会把 ready_shown 清成 0。若切回来时
 *   `FLAG_TEMP_REACHED` 仍是 1，按 update_ready_display() 的规则
 *   （want == ready_shown 才动作）只能等下一次置位事件 ——
 *   而该位若一直保持，就**永远等不到变化**，界面会一直停在"加热中"。
 */
static void sync_ready_display(void)
{
    ready_shown = (0U != (station.state & FLAG_TEMP_REACHED)) ? 1U : 0U;
    ready_pending = 0U;
    ready_change_ms = 0UL;
}

static void load_page(page_id_t page)
{
    if (PAGE_MAIN == page) {
        sgl_screen_load(main_page);
        sync_ready_display();
    } else if (PAGE_NO_TOOL == page) {
        sgl_screen_load(no_tool_page);
        sync_ready_display();
    } else if (PAGE_PARAM == page) {
        sgl_screen_load(param_page);
        curve_needs_reset = 1U;
    } else {
        sgl_screen_load(menu_page);
        menu_layout_mode = 0xFEU;
    }
}

/* ---- 对外接口 ------------------------------------------------------ */

bool init_ui(void)
{
    select_palette();
    last_theme = station.theme;

    /* 四个页面任一失败都让 main 停机，绝不带着半套界面跑 */
    if (!create_main_page()) {
        return false;
    }
    if (!create_no_tool_page()) {
        return false;
    }
    if (!create_menu_page()) {
        return false;
    }
    if (!create_param_page()) {
        return false;
    }

    /* 创建时已按 palette 上色；这里再统一同步一次，保证主题与控件一致 */
    apply_palette();
    invalidate_text_cache();

    /* 上电时烙铁头通常还没识别完，先直接进无工具页，避免主界面闪一帧 */
    if (is_tip_present()) {
        sgl_screen_load(main_page);
        last_page = PAGE_MAIN;
    } else {
        sgl_screen_load(no_tool_page);
        last_page = PAGE_NO_TOOL;
    }
    return true;
}

void task_ui(void)
{
    page_id_t page;

    /* 主题切换：换调色板 → 重刷所有静态控件 → 作废文本/颜色缓存 */
    select_palette();
    if (last_theme != station.theme) {
        last_theme = station.theme;
        apply_palette();
        invalidate_text_cache();
    }

    /* 换页才重新加载屏幕并清缓存，避免每 10 ms 重建一次页面 */
    page = resolve_page();
    if (page != last_page) {
        last_page = page;
        /* 同步 ready_shown 的逻辑放在 load_page() 里（见该函数），
         * 这样"加载页面"和"页面相关状态初始化"在一处，不会漏。 */
        load_page(page);
        invalidate_text_cache();
    }

    if (PAGE_MAIN == page) {
        refresh_main();
        return;
    }
    if (PAGE_NO_TOOL == page) {
        /* 整页文案固定，创建时就写好了，没有需要刷新的东西 */
        return;
    }
    if (PAGE_PARAM == page) {
        refresh_param();
        return;
    }

    /* 同一页只做一次布局；页内刷新只改文字，不动控件位置 */
    if (station.ui_mode != menu_layout_mode) {
        menu_layout_mode = station.ui_mode;
        apply_menu_layout(station.ui_mode);
        invalidate_text_cache();
    }
    refresh_menu();
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
