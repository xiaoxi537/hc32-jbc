/**
 *******************************************************************************
 * @file  app_check.c
 * @brief 电源状态机：PA3 联锁、PA9 在位、自动关机、待机、休眠
 *
 * 对应原机 sleep_standby_timer（0x219FE）。
 * 1 ms 节拍跑一次；所有开关都做 16 次连续采样去抖（16 ms），
 * 因为机械开关和换芯座在动作瞬间会有几十次抖动。
 *******************************************************************************
 */
#include "app_check.h"
#include "app_station.h"
#include "bsp_buzzer_backlight.h"
#include "bsp_clk.h"
#include "bsp_heater.h"
#include "bsp_input.h"

/* 16 位全 1 = 连续 16 ms 拉低；全 0 = 连续 16 ms 抬起 */
#define DEBOUNCE_ALL_LOW    (0xFFFFU)
#define DEBOUNCE_ALL_HIGH   (0x0000U)

static uint16_t sw_history;         /* PA3 / JBC_SW */
static uint16_t sleep_history;      /* PA4 / JBC_SLE */

/**
 * PA4 待机判定抑制标志（v9.13）
 *
 * 为什么要它：用户坐在座子上/手柄仍压着座子时按键长按唤醒，
 * 若下一毫秒 PA4 仍是低电平，下面的 update_standby_input() 会立刻
 * 把 FLAG_STANDBY 重新置回去 —— 表现为"菜单开了但温度显示 0、加热锁死"。
 *
 * 置位时机：唤醒唤醒（由 app_station 的 wake 入口置位）。
 * 清除时机：PA4 真正抬起过一次（连续 16 ms 高电平）。
 * 这样"手柄一直没离开座子"时不会每毫秒被打回待机，
 * 而"用户真的把烙铁放回去了"时待机判定照常生效。
 */
static bool standby_suppressed;
static uint32_t standby_suppress_ms;    /* 抑制起点，用于超时兜底 */

/** 每 1 ms 把两个开关的当前电平移进历史寄存器 */
static void sample_switch_inputs(void)
{
    sw_history = (uint16_t)((sw_history << 1U) | (is_jbc_sw_low() ? 1U : 0U));
    sleep_history = (uint16_t)((sleep_history << 1U) | (is_jbc_sle_low() ? 1U : 0U));
}

/**
 * @brief  PA3 安全联锁：持续拉低就禁止输出并退出菜单
 * @note   换芯时手柄座断开，此时还加热会烧到手指或烧坏手柄。
 */
static void update_sw_interlock(void)
{
    /* 只有连续 16 ms 都低才认账，避免换芯瞬间的抖动误触发 */
    if (DEBOUNCE_ALL_LOW == sw_history) {
        /* 已经置位就别重复关输出/关菜单，这 1 ms 只做一次动作 */
        if (0U == (station.state & FLAG_JBC_SW)) {
            station.state |= FLAG_JBC_SW;
            stop_heat_output();
            /* 菜单里正在改参数时也要退出去，安全优先 */
            if (MODE_MAIN != station.ui_mode) {
                close_menu();
            }
        }
        return;
    }

    /* 只有确认回到高电平才解除联锁，中间态保持"禁止输出" */
    if (DEBOUNCE_ALL_HIGH == sw_history) {
        station.state &= (uint16_t)(~FLAG_JBC_SW);
    }
}

/**
 * @brief  PA9：进出待机的一个输入（原机 SLEEP，两种角色按型号切换）
 *
 * 原机 `ctx+0x5A+ch = {1, 0, 0}`（逆向报告 §10.2、analysis/input_storage.md §3.2）：
 *   · **T12 (ch0 = 1)**：PA9 是手柄内的**滚珠开关** —— T12 没有休眠座，
 *     闲置满"待机时间"后进待机，**拿起手柄（滚珠动作）= 唤醒**。
 *   · **C245/C210 (ch1/ch2 = 0)**：PA9 只作在手柄在位镜像，
 *     它们的进出待机由**休眠座** PA4 负责（见 update_standby_input）。
 *
 * 本工程用 `auto_off_enabled[ch]` 承载这个"每头开关"的语义
 * （默认 {1,0,0} 与原机完全一致；名字历史遗留，值是对的）。
 *
 * 为什么必须在这里唤醒 T12：v9.13 之前只把 PA9 当"在位镜像"、
 * 拿它重装计时器，**从不唤醒** —— T12 闲置进待机后就再也不会醒，
 * 只有按键能救，与实机行为不符（用户 2026-10-24 反馈）。
 */
static void update_presence(void)
{
    bool level = is_handle_present();
    bool was_level = (0U != (station.state & FLAG_PRESENT));
    uint8_t channel = get_active_channel();

    /* 电平没变就什么都不做，避免每 1 ms 都重装计时/重复唤醒 */
    if (level == was_level) {
        return;
    }

    /* 记录最新电平，供状态镜像与去重用 */
    if (level) {
        station.state |= FLAG_PRESENT;
    } else {
        station.state &= (uint16_t)(~FLAG_PRESENT);
    }

    /* 只有"改用 PA9 作为休眠输入"的通道（T12）才拿它做唤醒。
     * C245/C210 的滚珠/休眠座走 PA4，PA9 变化对它们只是插拔信息。 */
    if (0U == station.auto_off_enabled[channel]) {
        reload_timers();
        return;
    }

    /* T12：电平变化 = 用户动了手柄（滚珠开关）。
     *
     * 唤醒方向不区分高/低：滚珠是**双掷**的，拿起与放下都可能翻转电平；
     * 原机 `read_pa9_input` 也只做状态跟踪，不判方向。
     * 已经在待机就退出待机（回工作温度），并在下面重装计时器，
     * 这样"放下 → 闲置满时间 → 再拿起"才是完整的一轮。 */
    if (0U != (station.state & FLAG_STANDBY)) {
        station.state &= (uint16_t)(~FLAG_STANDBY);
        if (0U != station.sound_enabled) {
            beep_buzzer(30U);
        }
    }
    /* 休眠中不在这里唤醒：休眠是"挂起"，必须按键（见 handle_short_press），
     * 避免手柄轻微晃动就把挂起态弄醒、白耗电。 */

    reload_timers();
}

/**
 * @brief  自动关机倒计时（只对启用了自动关机的通道）
 * @note   倒计时到 0 先进待机而不是直接关死，用户碰一下就能继续用。
 */
static void update_auto_off(void)
{
    uint8_t channel = get_active_channel();

    /* 该通道没开自动关机，计时器就不该动 */
    if (0U == station.auto_off_enabled[channel]) {
        return;
    }
    /* 已经在待机，等休眠计时接管 */
    if (0U != (station.state & FLAG_STANDBY)) {
        return;
    }
    /* 0 表示关机时间设为"从不"，别再往下减 */
    if (0UL == station.off_timer_ms) {
        return;
    }

    station.off_timer_ms--;

    /* 到点：切待机、装休眠计时、立刻断输出 */
    if (0UL == station.off_timer_ms) {
        station.state |= FLAG_STANDBY;
        station.sleep_timer_ms = (uint32_t)station.sleep_minutes[channel] * 60000UL;
        stop_heat_output();
    }
}

/**
 * @brief  PA4 休眠座开关（未启用自动关机的通道走这条路）
 * @note   手柄放回座子 → 待机；拿起来 → 回工作温度并响一声确认。
 */
static void update_standby_input(void)
{
    uint8_t channel = get_active_channel();

    /* 启用自动关机的通道由倒计时管理，不看 PA4，避免两套逻辑打架 */
    if (0U != station.auto_off_enabled[channel]) {
        return;
    }

    /* PA4 连续 16 ms 拉低 = 手柄放回座子 */
    if (DEBOUNCE_ALL_LOW == sleep_history) {
        /* 用户刚主动唤醒、手柄还没真正离开座子：这段时间内不许再打进待机。
         * 否则长按唤醒的下一毫秒就又被置回待机，菜单看起来是开的、
         * 实际温度显示 0℃ 且加热锁死（v9.13 实机反馈的根因）。
         *
         * 解除有两条路（缺一不可）：
         *   ① PA4 真正抬起过（见下面的 DEBOUNCE_ALL_HIGH 分支）
         *   ② **超时兜底**：手柄一直被压着不抬起时，抑制也不能永久生效，
         *      否则待机/自动关机永久失效、手柄一直加热（费电且不安全） */
        if (standby_suppressed) {
            if ((get_tick_ms() - standby_suppress_ms) <
                APP_CHECK_STANDBY_SUPPRESS_MS) {
                return;
            }
            /* 超时：放弃抑制，走正常待机判定 */
            standby_suppressed = false;
        }
        /* 已经在待机就别重复装计时，否则休眠时间被不断续命 */
        if (0U == (station.state & FLAG_STANDBY)) {
            station.state |= FLAG_STANDBY;
            station.sleep_timer_ms = (uint32_t)station.sleep_minutes[channel] * 60000UL;
            stop_heat_output();
        }
        return;
    }

    /* 确认抬起才唤醒；中间态不动，避免半按状态反复进出待机 */
    if (DEBOUNCE_ALL_HIGH == sleep_history) {
        /* 手柄真正离开座子了：解除"唤醒后抑制待机"，
         * 之后用户再放回去就该正常进待机 */
        standby_suppressed = false;

        /* 只有真的在待机才需要唤醒，否则会白响一声 */
        if (0U != (station.state & FLAG_STANDBY)) {
            station.state &= (uint16_t)(~FLAG_STANDBY);
            reload_timers();
            /* 静音设置下不响，但状态照样恢复 */
            if (0U != station.sound_enabled) {
                beep_buzzer(30U);
            }
        }
    }
}

/** 待机超时 → 休眠（设定值 0，只能靠按键唤醒） */
static void update_sleep_timer(void)
{
    /* 不在待机就没有休眠计时可走 */
    if (0U == (station.state & FLAG_STANDBY)) {
        return;
    }
    /* 休眠时间设为"从不"时计时器是 0，不能让它立刻进休眠 */
    if (0UL == station.sleep_timer_ms) {
        return;
    }

    station.sleep_timer_ms--;

    /* 到点：退出待机、进入休眠，设定值变 0 */
    if (0UL == station.sleep_timer_ms) {
        station.state = (uint16_t)((station.state & (uint16_t)(~FLAG_STANDBY)) | FLAG_SLEEP);
        stop_heat_output();
    }
}

void suppress_standby(void)
{
    /* 用户主动操作后先压住待机判定；解除见 update_standby_input 的两个出口
     * （PA4 抬起 / 超时兜底）。这里记下起点给超时用。 */
    standby_suppressed = true;
    standby_suppress_ms = get_tick_ms();
}

void init_check(void)
{
    sw_history = 0U;
    sleep_history = 0U;
    standby_suppressed = false;
    standby_suppress_ms = 0UL;
}

void task_check(void)
{
    /* 休眠时不再跑任何计时，否则睡一会儿就被自己的倒计时弄醒 */
    if (0U != (station.state & FLAG_SLEEP)) {
        task_buzzer();
        return;
    }

    /* 先采样再判断，所有分支用的都是同一时刻的电平 */
    sample_switch_inputs();
    update_sw_interlock();
    update_presence();

    /* 菜单打开期间跳过"自动关机 / 待机 / 休眠"三条判定（v9.13）。
     *
     * 为什么：菜单本身已经把加热锁死（FLAG_MENU），这三条判定唯一的作用
     * 就是改设定值和置待机/休眠标志，在菜单里做只会制造混乱 ——
     * 用户翻着菜单突然被判待机，设定值跳成待机温度、温度显示变 0；
     * 而 PA4 抬起的分支还会顺手把菜单关掉。
     * 用户在操作就是"有人在用"，本来就不该超时。
     *
     * 注意联锁（PA3）和手柄在位（PA9）**不能**跳过 —— 那是安全逻辑。 */
    if (0U == (station.state & FLAG_MENU)) {
        update_auto_off();
        update_standby_input();
        update_sleep_timer();
    }

    task_buzzer();
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
