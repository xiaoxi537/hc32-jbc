/**
 *******************************************************************************
 * @file  app.c
 * @brief 应用入口：初始化顺序 + 裸机调度
 *
 * 原机 FreeRTOS 四个任务 → 本工程裸机调度：
 *   check 任务  1 ms    task_check()      （主循环）
 *   indev 任务  5 ms    task_indev()      （主循环）
 *   ui 任务     10 ms   task_ui()         （主循环）
 *   heat 任务   1 ms    tick_heat()       （SysTick，等价原机最高优先级）
 *******************************************************************************
 */
#include "app.h"
#include "app_check.h"
#include "app_heat.h"
#include "app_indev.h"
#include "app_station.h"
#include "app_ui.h"
#include "bsp_adc.h"
#include "bsp_buzzer_backlight.h"
#include "bsp_swdt.h"
#include "bsp_clk.h"
#include "bsp_heater.h"
#include "bsp_input.h"
#include "bsp_lcd.h"
#include "sgl_port_hc32.h"

static uint32_t last_check_ms;
static uint32_t last_indev_ms;
static uint32_t last_ui_ms;
static uint32_t last_save_ms;

bool init_app(void)
{
    uint32_t now;

    /* 最优先：让 SWDT 在内核 halt 时暂停。
     *
     * 为什么放在所有初始化之前：调试器连上目标会立刻 halt 内核，
     * 若此时 SWDT 还在计数，约 819 ms 后芯片就被复位，断点根本停不住。
     * 这一步只写一两条寄存器指令，放在最前面最安全。 */
    init_debug_freeze();

    /* 先建状态：后面每个模块都要读 station 里的用户参数 */
    init_station();

    /* 显示链路：屏 → 图形库 → 页面。任何一步失败都直接返回，
     * 此时背光还没打开，现场看到"屏全黑但背光不亮"就是软件没起来。 */
    init_lcd();
    /* SGL 注册失败基本只有一种原因：24 KB 静态堆不够，界面注定起不来 */
    if (!init_sgl_port()) {
        return false;
    }
    /* 页面没建出来时背光还没打开，现场"屏全黑"就是初始化失败 */
    if (!init_ui()) {
        return false;
    }

    /* 输入与声光：背光按用户设置开，蜂鸣器初始化时是静音的 */
    init_inputs();
    init_buzzer_backlight();
    set_backlight(station.brightness);

    /* 采样和加热：必须先配好 ADC/定时器，再让加热状态机跑起来 */
    init_adc();
    init_heater();
    init_heat();

    /* 两个状态机：check 负责电源状态，indev 负责按键/菜单 */
    init_check();
    init_indev();

    now = get_tick_ms();
    last_check_ms = now;
    last_indev_ms = now;
    last_ui_ms = now;
    last_save_ms = now;

    /* 上电提示音放在最后：前面都成功了才响，听到响声说明初始化完成 */
    if (0U != station.sound_enabled) {
        beep_buzzer(200U);
    }

    return true;
}

void task_app(void)
{
    uint32_t now = get_tick_ms();

    /* check：每个新毫秒都跑一次，休眠/待机计时靠它 */
    if (now != last_check_ms) {
        last_check_ms = now;
        task_check();
    }

    /* indev：5 ms 一次，按键消抖和编码器取数都要这个节奏 */
    if ((now - last_indev_ms) >= 5UL) {
        last_indev_ms = now;
        task_indev();
    }

    /* ui：10 ms 一次，够 100 fps，又不会把 SPI 占满 */
    if ((now - last_ui_ms) >= 10UL) {
        last_ui_ms = now;
        task_ui();
    }

    /* 设置延迟保存：回到主界面 200 ms 后再统一擦写一次 Flash。
     * 不能在 5 ms 按键任务里写，擦扇区要十几毫秒，会把编码器和蜂鸣器卡住。 */
    if ((0U != station.settings_dirty) && (MODE_MAIN == station.ui_mode) &&
        ((now - last_save_ms) >= 200UL)) {
        station.settings_dirty = 0U;
        (void)save_settings();
        last_save_ms = get_tick_ms();
    }

    /* SGL 任务：把脏区渲染进行缓冲并通过 SPI3/DMA1 送屏。
     * 漏掉这一句的现象是"背光亮、屏幕全黑"，因为 task_ui 只改文本和标脏。 */
    task_sgl_port();

    /* 喂狗：放在主循环最末尾，意味着"一整轮调度都跑完了"才算数。
     *
     * 为什么不能放中断里：中断照常在跑时喂狗，即使主循环已经卡死，
     * 看门狗也永远不会复位，等于没装。
     * 为什么不会误复位：SWDT 超时 819 ms，而本函数最快每毫秒被调用一次，
     * 即使某个分支（如写 Flash 扇区约十几 ms）耗时较长，
     * 余量仍有 50 倍以上。 */
    feed_swdt();
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
