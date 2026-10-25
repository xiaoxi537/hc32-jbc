/**
 *******************************************************************************
 * @file  app_heat.c
 * @brief 加热控制：测温窗口 + PID + 软件 PWM
 *
 * 对应原机 heat_cycle(0x21E48) / pid_compute(0x1F5A4) /
 * pwm_output_update(0x2209C) / tip_detect_and_config(0x21D18)。
 *
 * 一轮的时序（与原机一致）：
 *   TMRA6 脉冲结束（中断置完成）→ 测温 5 ms（加热丝已断电）
 *   → 取平均/电压/电流 → 断偶/识别/PID → 写占空比
 *   → 启动下一个 251 周期脉冲（≈25.3 ms）
 *******************************************************************************
 */
#include "app_heat.h"
#include "app_station.h"
#include "bsp_adc.h"
#include "bsp_buzzer_backlight.h"
#include "bsp_clk.h"
#include "bsp_heater.h"

/* ---- 测温窗口（v9.21 回滚到 v9.17） ----
 *
 * 统一 5 ms。v9.19 曾按型号区分（C210=2ms / 其他=5ms），
 * v9.21 按用户要求整体回滚到 v9.17 的状态。
 *
 * 5 ms 是热电偶断电后稳定所需的时间；短了读数会偏低。 */
#define HEAT_MEASURE_MS             (5U)

#define HEAT_RAW_OPEN_THRESHOLD     (3501U)     /* 断偶/无工具门限 */
#define HEAT_TEMP_MAX               (470.0F)
#define HEAT_PREHEAT_CYCLES         (15U)
#define HEAT_DUTY_PREHEAT           (50U)
#define HEAT_DUTY_MAX               (250U)

/* ---- 温度滤波：控制侧与显示侧分开（v9.12） ----
 *
 * 原机是**两套滤波**（逆向报告 §7.2 / §11、analysis/safety_state_machine.md §3.3）：
 *   · `ctx+0x20` 快滤波（上升沿一阶，滞后 30 ms）→ 只喂 PID
 *   · `ctx+0xA4` **10 点滑动平均** → 只喂显示与"到温"判定（temperature_filter_update）
 * v9.10 把两者都做成 10 点平均 → 控制侧滞后 137 ms，1 Hz 极限环（v9.11 已修）；
 * v9.11 又都改成 3 点 → 控制侧对了，但**显示侧跟着变成 30 ms 带宽**，
 * 100℃ 待机时热电偶信号小、噪声占比大，读数就在 98~102 之间跳。
 *
 * 所以这里恢复原机的分工：
 *   · 控制侧 3 点（滞后 30 ms）—— PID 要快，慢了会振荡，一行不动
 *   · 显示侧 10 点（滞后 ~137 ms）—— 只给人看，慢一点完全无感
 *
 * 两者都用**环形缓冲 + 增量求和（O(1)）**：每拍只做
 *     sum -= 最旧;  sum += 最新;  avg = sum / N;
 * 绝不在每拍遍历数组求和（那样每拍 10~32 次加法，纯属浪费 CPU）。
 * 除法只在最后换算时做一次，除数是编译期常量，编译器会转成乘 + 移位。 */
/* **v9.21：回滚到 3 点**（v9.17 状态；v9.18 曾改 5 点）
 *
 * 实机仍波动，说明 3 点的降噪（√3≈1.7 倍）不够。
 * 5 点降噪 √5≈2.24 倍，配合 ADC 侧硬件平均 4→16 次（再降 2 倍），
 * 合计噪声约降到原来的 1/4.5。
 *
 * 滞后代价：(5-1)/2 = 2 拍 ≈ 60 ms。v9.11 的教训是"滞后=测量误差"，
 * 满功率升温 135℃/s 时 60 ms 对应 8.1℃ 偏差；但那个结论是在
 * **10 点（137 ms）** 的前提下得出的。5 点（60 ms）仍在
 * "可接受"区间，且这次优先解决的是**稳态波动**（升温速率接近 0，
 * 滞后几乎不产生误差），两者不冲突。
 *
 * 若实机升温和稳态仍难两全，可考虑：升温时用 3 点、进入死区后用 5 点
 * （即"变窗口"，代价是多一套状态）。 */
#define HEAT_TEMP_FILTER_LEN        (3U)

/* 显示侧窗口：与原子 ctx+0xA4 同为 10 点。
 * 滞后 (10-1)/2 = 4.5 拍 × 30.34 ms ≈ 137 ms —— 对人眼读取数字完全无感，
 * 但降噪 √10 ≈ 3.2 倍，正好把 100℃ 时那 ±2℃ 的跳动压到 ±0.6℃ 以内。
 * 注意它**不参与 PID**，所以不会重新引入 v9.11 修掉的 1 Hz 振荡。 */
#define HEAT_DISPLAY_FILTER_LEN     (10U)

/* ---- 到温显示：滞回 + 500 ms 保持 ----
 *
 * 需求（用户 2026-10-24 指定）：
 *   · 温度达到设定值后，必须**保持 500 ms** 才显示"已到温"/MODE=HOLD
 *   · 温度掉到 **设定值-2℃ 以下** 才切回"加热中"/HEAT
 *
 * 为什么要 500 ms 保持：恒温时读数仍有 ±1℃ 残余波动，瞬时判定会让
 * 徽标与 MODE 框每拍翻转、看着一直闪。保持 500 ms 等于加了一个
 * "确认窗口"，短暂擦过设定值不会被当真。
 * 注意保持窗口内如果又掉下去，计时要重新开始 —— 只认"连续满足"。 */
#define HEAT_READY_HOLD_MS          (500U)
#define HEAT_REACH_ENTER_C          (1)     /* >= 设定值-1℃ 才算到达（容差） */
#define HEAT_REACH_EXIT_C           (2)     /* 掉到 设定值-2℃ 以下才撤销 */

/* ---- 加热死区 ----
 *
 * **v9.21：回滚到 ±2.0℃**（v9.17 状态）
 *
 * v9.18 曾放宽到 ±4℃，v9.19 删掉微调只留纯冻结，实机崩溃。
 * 现整体回滚到 v9.17：死区 ±2.0℃ + 固定步长 1 的微调（见下）。
 *
 * 需求（用户 2026-10-24 指定）：误差在死区内时
 *   · 停止积分累加（防止恒温时积分继续爬、造成缓慢漂移）
 *   · **保持上一次的占空比**，不要完全关断
 *
 * 为什么保持而不是关断：完全关断会让温度掉下去、下一拍又满功率补回来，
 * 形成"开-关"振荡（继电器效应）。保持占空比才能稳在小火上煨着。
 *
 * **为什么从 ±1.5℃ 放宽到 ±2℃（v9.15）**：
 *   实机稳态仍有 ±2℃ 慢速振荡（设定 300℃ 在 298~302 晃）。
 *   ±1.5℃ 的死区太窄，烙铁头热惯性还没消化完功率变化就出了死区，
 *   于是"进死区保持 → 出死区 PID 补 → 再进死区"来回切。
 *   放宽到 ±2℃ 先把整个振荡幅度包进死区，给物理惯性留出余地。 */
#define HEAT_DEADBAND_C             (2.0F)
/* 判据直接用 station.temperature（控制侧 3 点快滤波）比较，
 * 不再强转成整数 —— 强转会把 299.6 和 300.4 都变成 300，
 * 丢掉死区内本该用于"偏高就减、偏低就加"的方向信息。 */

/* ---- 死区微调步长（v9.21 回滚到 v9.17 状态） ----
 *
 * v9.17 的状态：**固定步长 1，带 1…5 严格钳位**。
 *   · 死区内按温度偏差方向，每拍把占空比挪一格
 *     （偏高 -1、偏低 +1），等价于一个极慢的积分作用
 *   · 步长固定 1 —— 33 Hz 下每秒最多挪 33 格（满量程 13%），
 *     对 ±2℃ 的死区微调已经绰绰有余
 *
 * 历史（勿重蹈）：
 *   v9.16 改成自适应 duty/10、v9.18 改两档 >15 用 2 —— 都是基于
 *   被"微分踢动 bug"污染的数据做的决定，实机反而更差（v9.18 崩溃到 9℃/13℃）。
 *   v9.19 干脆全删只留纯冻结 —— 失去了微调，又引入 0 占空比死锁。
 *   最终回到 v9.17 这个"最小可用"的形态。
 *
 * 钳位两个方向都必须夹住：
 *   下限 1：步长 0 会让死的占空比永久停住（温度再也拉不回来）
 *   上限 5：防止异常大的偏差一步跳太多造成冲击（当前步长恒 1，留作防御） */
#define HEAT_TRIM_STEP_BASE        (1U)
#define HEAT_TRIM_STEP_MAX         (5U)

/* 台架首测可以 -DHEAT_OUTPUT_ENABLE=0 强制占空比 0，识别/测温/UI 照常跑 */
#ifndef HEAT_OUTPUT_ENABLE
#define HEAT_OUTPUT_ENABLE          (1)
#endif

static float pid_integral;
static float pid_last_error;
static uint32_t measure_start_ms;
static uint32_t detect_start_ms;
static uint32_t recheck_start_ms;
static bool measuring;
static bool detect_pending;
static bool recheck_c245;
static uint8_t preheat_cycles;
static volatile bool heat_tick_enabled;

/* 3 点滑动平均：**整数环形缓冲 + 增量求和**
 *
 * 对"原始 ADC 值"做整数平均，而不是对浮点温度做平均：
 *   · 纯整数加减，Cortex-M4 上单周期指令，无浮点、无除法循环
 *   · raw 最大 3501（断偶门限），3 个相加最多 10503，uint32 远远够
 *   · 换算成温度放在平均之后做一次，浮点运算只剩 1 次乘 1 次除
 *   · 每拍开销：2 次整数加/减 + 1 次乘法 + 1 次除法（除数是常量，编译器转乘法） */
static uint32_t temp_ring[HEAT_TEMP_FILTER_LEN];
static uint32_t temp_ring_sum;
static uint8_t temp_ring_pos;
static uint8_t temp_ring_fill;

/* 显示侧 10 点环（只喂 station.temperature_int，不进 PID）
 *
 * 与上面控制侧的环**完全独立**：控制侧要低滞后、显示侧要平滑，
 * 两者指标相反，共用一个窗口必然只能二选一（v9.10/v9.11 就是这么来回翻车的）。 */
static uint32_t display_ring[HEAT_DISPLAY_FILTER_LEN];
static uint32_t display_ring_sum;
static uint8_t display_ring_pos;
static uint8_t display_ring_fill;

/* "已到温"显示：滞回锁存 + 500 ms 保持确认 */
static bool ready_latched;
static uint32_t ready_reach_ms;     /* 首次连续满足条件的时刻 */

/* 加热死区：误差在 ±HEAT_DEADBAND_C 内时按偏差微调占空比（步长自适应） */
static uint8_t deadband_duty;       /* 上次真正下发的占空比 */
static bool deadband_active;        /* 当前是否处于死区保持状态 */

static float clamp_float(float value, float low, float high)
{
    /* 先压下限再压上限，两个方向都要管 */
    if (value < low) {
        return low;
    }
    /* 高于上限就压到上限 */
    if (value > high) {
        return high;
    }
    return value;
}

/**
 * @brief  位置式 PID，输出 0…1000
 * @note   误差低于 -20℃ 时输出 0（已经冲过头，先停手等自然降温）
 */
static uint16_t run_pid(uint16_t setpoint)
{
    uint8_t channel = get_active_channel();
    float kp = (float)station.kp[channel];
    float ki = (float)station.ki_tenths[channel] / 10.0F;
    float kd = (float)station.kd[channel];
    float error = (float)setpoint - station.temperature;
    float output = 0.0F;

    /* 积分限幅 0…1000：不封顶的话长时间到不了温会积到几万，
     * 一旦到温要很久才能把占空比降下来，表现就是大幅过冲。 */
    /* 设置菜单里加热被禁止（见 task_heat 的 FLAG_MENU 闸），
     * 烙铁头会自然降温、误差不断变大；这时候再累加积分，退出菜单的瞬间
     * 会带着一个顶到上限的积分去给功率，表现就是"一退菜单就猛冲"。
     * 所以菜单期间只冻结积分，pid_last_error 照常更新 —— 微分项才不会跳。
     *
     * 死区：误差在 ±HEAT_DEADBAND_C（v9.18 起为 ±4℃）内时**冻结积分**。
     * 恒温时误差本来就贴着 0 抖，不停累加的话积分会按"零均值噪声"慢慢
     * 漂移，造成温度缓慢爬升/下沉；冻结后积分锁在平衡点附近，稳得住。
     * （死区里的占空比由 run_ready_phase **纯冻结**，不做任何微调。
     *   边界必须与那里用同一个常量，否则会出现"积分在动但占空比在冻结"
     *   的不一致状态。） */
    if ((0U == (station.state & FLAG_MENU)) &&
        ((error > HEAT_DEADBAND_C) || (error < -HEAT_DEADBAND_C))) {
        pid_integral = clamp_float(pid_integral + error, 0.0F, 1000.0F);
    }

    /* 冲过设定值 20℃ 以上就输出 0，先让烙铁头自然降温 */
    if (error >= -20.0F) {
        output = (kp * error) + (ki * pid_integral) + (kd * (error - pid_last_error));
        output = clamp_float(output, 0.0F, 1000.0F);
    }
    pid_last_error = error;

    return (uint16_t)output;
}

/**
 * @brief  PID 输出换算成 0…250 的硬件占空比
 * @note   防护条件和原机 pwm_output_update 一致：
 *         过流闩锁 / 未允许输出 / 休眠 / 联锁 / 断偶 / 识别中 → 一律 0
 */
static uint8_t scale_pwm_duty(uint16_t pid_out)
{
    uint8_t channel = station.tip_channel;
    float scaled;
    float duty;

    /* 过流闩锁只有重新识别烙铁头才清，中间任何情况都不许再输出 */
    if (0U != station.over_current_latch) {
        return 0U;
    }
    /* ARMED 必须置位，且不能处在休眠或手柄座联锁状态 */
    if ((0U == (station.state & FLAG_ARMED)) ||
        (0U != (station.state & (FLAG_JBC_SW | FLAG_SLEEP)))) {
        return 0U;
    }
    /* 断偶或还在预热验证时也不给输出 */
    if (0U != (station.state & (FLAG_TIP_FAULT | FLAG_DETECT))) {
        return 0U;
    }
    /* 没识别出通道就没有功率曲线可查 */
    if (channel >= CHANNEL_COUNT) {
        return 0U;
    }

    /* C210 在高压下只有十分之一功率，硬按 T12 的系数会烧红小烙铁头 */
    if ((2U == channel) && (0U != (station.state & FLAG_TIP_TYPE_B))) {
        scaled = (float)pid_out / 10.0F;
    } else {
        scaled = (float)pid_out * 0.25F;
    }

    /* 再乘用户设的输出功率百分比，最后钳到硬件上限 */
    duty = ((float)station.power_scale[channel] / 100.0F) * scaled;
    duty = clamp_float(duty, 0.0F, (float)HEAT_DUTY_MAX);

    return (uint8_t)duty;
}

/**
 * @brief 清空温度滑动平均窗口与"到温"滞回
 * @note  换头/断偶/重新识别时必须调，否则新烙铁头会用上一支的
 *        历史样本做平均，"到温"还会保持锁存，界面直接显示错的状态。
 */
static void reset_temperature_filter(void)
{
    /* 只需要把"和"与"下标/有效点数"归零，**不必清数组本身**：
     * 增量求和里 `sum` 是唯一的累加器 —— 窗口未满时只累加真实推入的值，
     * 填满后减掉的 `ring[pos]` 必然是 TEMP_FILTER_LEN 次推入前的真实值。
     * 数组里的旧内容永远不会进入 `sum`，所以清数组是纯多余的工作
     * （而且会引入一个本该避免的循环）。 */
    temp_ring_sum = 0U;
    temp_ring_pos = 0U;
    temp_ring_fill = 0U;

    /* 显示侧环同理 */
    display_ring_sum = 0U;
    display_ring_pos = 0U;
    display_ring_fill = 0U;

    /* 到温显示与死区也要一起复位，否则换头后还挂着旧状态 */
    ready_latched = false;
    ready_reach_ms = 0U;
    deadband_active = false;
    deadband_duty = 0U;
}

/** 识别失败：清型号、禁止输出，等用户重新插好烙铁头 */
static void enter_tip_fault(void)
{
    station.tip_channel = CHANNEL_NONE;
    station.state |= (FLAG_ARMED | FLAG_TIP_FAULT);
    station.state &= (uint16_t)(~(FLAG_READY | FLAG_DETECT | FLAG_HEATING));
    station.temperature_int = 0;
    station.temperature = 0.0F;
    station.heat_duty = 0U;
    detect_pending = false;
    recheck_c245 = false;
    reset_temperature_filter();
}

/** 识别成功：装载该通道参数，进入预热验证 */
static void enter_tip_detected(uint8_t channel)
{
    load_channel(channel);
    station.tip_channel = channel;

    /* 上一轮的故障标志先清掉，否则新烙铁头也会被当成断偶 */
    station.state &= (uint16_t)(~(FLAG_TIP_FAULT | FLAG_TIP_TYPE_B));

    /* C210 接 24 V 时功率过大，标记成低功率型号 */
    if ((2U == channel) && (station.input_voltage > 14.0F)) {
        station.state |= FLAG_TIP_TYPE_B;
    }

    station.state |= (FLAG_ARMED | FLAG_ANNOUNCE | FLAG_DETECT);
    station.state &= (uint16_t)(~FLAG_READY);
    /* 新烙铁头从冷态开始，"到温"必须重新判定 */
    ready_latched = false;
    station.over_current_latch = 0U;
    adc_over_current = false;
    station.pid_out = 0U;
    pid_integral = 0.0F;
    pid_last_error = 0.0F;
    preheat_cycles = 0U;
    detect_pending = false;
    recheck_c245 = false;
}

/**
 * @brief  按 ID 原始值判断烙铁头型号
 * @param  id  烙铁头 ID 通道原始值
 * @param  vin 本轮输入电压（V）
 * @retval 0 T12 / 1 C245 / 2 C210 / CHANNEL_NONE 不认识
 * @note   门限来自 docs/人工勘误.md §8.12~8.14 的实机结论：
 *         T12 1001…1599，仅要求 VIN<32；C245 ≥3901，仅要求 VIN<32；
 *         C210 ≤799，必须 10V<VIN<32V（否则就是 ID 脚浮空）。
 */
static uint8_t classify_tip(uint16_t id, float vin)
{
    /* T12：ID 1001…1599，只卡 VIN 上限（不接主供电也能识别） */
    if ((id >= 1001U) && (id <= 1599U)) {
        return (vin < 32.0F) ? 0U : CHANNEL_NONE;
    }
    /* C245：ID ≥3901，同样只卡 VIN 上限 */
    if (id >= 3901U) {
        return (vin < 32.0F) ? 1U : CHANNEL_NONE;
    }
    /* C210：ID ≤799，必须落在 10…32 V 窗口，否则是 ID 脚浮空 */
    if (id <= 799U) {
        return ((vin > 10.0F) && (vin < 32.0F)) ? 2U : CHANNEL_NONE;
    }
    return CHANNEL_NONE;
}

/**
 * @brief  识别烙铁头（分两次读，不能阻塞）
 * @retval true 识别成功；false 还在等稳定或识别失败
 * @note   已移到 SysTick 里执行，所以用状态机分两次读，不能 delay。
 */
static bool detect_tip(void)
{
    uint16_t id;
    uint8_t channel;

    /* 插入后先强制关加热等 100 ms：让 HEAT- 放电、识别电平稳定。
     * C210 的 ID 脚接电源负极，加热后的残留电平会把 ID 抬到 C245 区间；
     * 原机也只在刚上电（未加热）时才识别 C210。 */
    if (!detect_pending) {
        stop_heat_output();
        (void)take_pulse_done();
        detect_pending = true;
        recheck_c245 = false;
        detect_start_ms = get_tick_ms();
        return false;
    }
    /* 100 ms 放电窗口没到就继续等；这里不能 delay，会在 SysTick 里卡死 */
    if ((get_tick_ms() - detect_start_ms) < 100UL) {
        return false;
    }

    id = read_tip_id();

    /* C245 候选：再关加热 50 ms 重读一次，排除 C210 的 HEAT- 残留高电平 */
    if (id >= 3901U) {
        /* 第一次读到 C245 区间：先关加热 50 ms 再重读，排除 C210 残压 */
        if (!recheck_c245) {
            stop_heat_output();
            (void)take_pulse_done();
            recheck_c245 = true;
            recheck_start_ms = get_tick_ms();
            return false;
        }
        /* 50 ms 没到就等下一拍，绝不能在中断上下文里忙等 */
        if ((get_tick_ms() - recheck_start_ms) < 50UL) {
            return false;
        }
        id = read_tip_id();
        recheck_c245 = false;
    }

    /* 注意：VIN 必须用本轮刚读出的值。这里若再调一次 read_heat_averages，
     * 测温窗口不累加，会读到 0 V，导致永远识别失败。 */
    channel = classify_tip(id, station.input_voltage);

    /* C245 的 ID 和"空载浮空"重叠，只有热电偶不是断偶才认 */
    if ((1U == channel) && (read_temp_raw_avg() >= HEAT_RAW_OPEN_THRESHOLD)) {
        channel = CHANNEL_NONE;
    }

    /* ID 不在任何窗口，或 C245 断偶：按"没有工具"处理 */
    if (CHANNEL_NONE == channel) {
        enter_tip_fault();
        return false;
    }

    enter_tip_detected(channel);
    return true;
}

/**
 * @brief  热插拔：已识别 T12/C245 时，ID 跳到 C210 区间就重新识别
 * @retval true 已处理（本轮不要再算温度/输出）
 * @note   C210 自身不参与，避免它加热时的 HEAT- 残压把自己误判成 C245。
 */
static bool handle_tip_replacement(void)
{
    /* 只对已识别的 T12/C245 有效 */
    if ((0U == (station.state & FLAG_READY)) || (station.tip_channel >= 2U)) {
        return false;
    }
    /* ID 还在原区间就说明没换头 */
    if (read_tip_id() > 799U) {
        return false;
    }

    station.tip_channel = CHANNEL_NONE;
    station.state = (uint16_t)((station.state &
        (uint16_t)(~(FLAG_READY | FLAG_DETECT | FLAG_HEATING |
                     FLAG_ANNOUNCE | FLAG_TIP_TYPE_B))) |
        FLAG_ARMED | FLAG_TIP_FAULT);
    station.heat_duty = 0U;
    station.pid_out = 0U;
    detect_pending = false;
    stop_heat_output();
    (void)take_pulse_done();
    return true;
}

/** 断偶/没插工具：原始值顶到满量程，继续加热等于把全功率加在开路 */
static void handle_open_tip(void)
{
    /* C245 空载时清掉型号，UI 顶栏显示"无工具" */
    if (1U == station.tip_channel) {
        station.tip_channel = CHANNEL_NONE;
    }
    station.state |= FLAG_TIP_FAULT;
    station.state &= (uint16_t)(~(FLAG_READY | FLAG_DETECT | FLAG_HEATING));
    station.temperature = 0.0F;
    station.temperature_int = 0;
    station.heat_duty = 0U;
    station.pid_out = 0U;
    detect_pending = false;
    stop_heat_output();
    reset_temperature_filter();
}

/**
 * @brief 单点 350℃ 校准 + 双环滤波（控制侧 3 点 / 显示侧 10 点）
 * @param raw 热电偶原始 ADC 值（0…3501）
 *
 * 控制侧为什么是 3 点（v9.11 结论，保持不动）：
 *   实机实测 C210 空载恒温时读数以 **约 1 Hz** 在 295~304℃ 振荡，
 *   根因是 10 点平均带来 137 ms 滞后 —— 满功率升温 135℃/s 时，
 *   PID 看到的温度比真实低 **18.5℃**，一路冲过头才反应。
 *   3 点平均等效延迟只有 1 拍（30 ms），误差降到 4.1℃，
 *   与原子"上升沿一阶"持平。
 *
 * 显示侧为什么单独再做一个 10 点（v9.12）：
 *   v9.11 只留 3 点时，**显示也走这条 30 ms 带宽**，100℃ 待机时
 *   热电偶信号小、噪声占比大，读数在 98~102 之间跳（用户实测）。
 *   原机把两者分开：`ctx+0x20` 快滤波给 PID、`ctx+0xA4` 10 点给显示。
 *   显示侧滞后 137 ms 对人眼完全无感，且**不进 PID**，
 *   不会重新引发 v9.11 修掉的 1 Hz 振荡。
 *
 * 两个环都是**整数环形缓冲 + 增量求和（O(1)）**：
 *   每拍只做"减最旧、加最新"，除法只在求平均时做一次，
 *   除数是编译期常量，编译器会转成乘 + 移位。
 *   **绝不遍历数组求和**，全程无浮点循环、无多余状态。
 */
static void update_temperature(uint16_t raw)
{
    float scale;
    uint32_t average;
    uint32_t display_average;
    uint32_t raw_value = (uint32_t)raw;

    /* ---- 控制侧 3 点环：PID 用的快滤波 ---- */
    if (temp_ring_fill >= HEAT_TEMP_FILTER_LEN) {
        temp_ring_sum -= temp_ring[temp_ring_pos];
    }
    temp_ring[temp_ring_pos] = raw_value;
    temp_ring_sum += raw_value;
    temp_ring_pos++;
    if (temp_ring_pos >= HEAT_TEMP_FILTER_LEN) {
        temp_ring_pos = 0U;
    }
    /* 窗口没填满时按已有样本数求平均，避免上电头几拍从 0 爬升 */
    if (temp_ring_fill < HEAT_TEMP_FILTER_LEN) {
        temp_ring_fill++;
    }

    /* ---- 显示侧 10 点环：只给界面看，不参与控制 ---- */
    if (display_ring_fill >= HEAT_DISPLAY_FILTER_LEN) {
        display_ring_sum -= display_ring[display_ring_pos];
    }
    display_ring[display_ring_pos] = raw_value;
    display_ring_sum += raw_value;
    display_ring_pos++;
    if (display_ring_pos >= HEAT_DISPLAY_FILTER_LEN) {
        display_ring_pos = 0U;
    }
    if (display_ring_fill < HEAT_DISPLAY_FILTER_LEN) {
        display_ring_fill++;
    }

    /* 整数平均：除数是常量，编译器转成乘 + 移位 */
    average = temp_ring_sum / (uint32_t)temp_ring_fill;
    display_average = display_ring_sum / (uint32_t)display_ring_fill;

    /* 换算成温度：t = raw * 350 / cal_raw_at_350（与校准公式一致） */
    scale = (station.cal_raw_at_350 < 1.0F) ? 1.0F : station.cal_raw_at_350;
    station.temperature = ((float)average * 350.0F) / scale;
    /* 显示值走慢环；UI 与"到温"判定都读它，语义和原子 ctx+0xA4 一致 */
    station.temperature_int =
        (int16_t)(((float)display_average * 350.0F) / scale);
}

/**
 * @brief 到温显示判定：滞回 + 500 ms 保持确认
 * @param setpoint 当前设定温度
 *
 * 需求（用户 2026-10-24 指定）：
 *   · 温度达到设定值后，必须**连续保持 500 ms** 才显示"已到温"/MODE=HOLD
 *   · 温度掉到**设定值-2℃ 以下**才切回"加热中"/HEAT
 *
 * 为什么需要 500 ms 确认：恒温时读数仍有 ±1℃ 残余波动，
 * 瞬时判定会让徽标与 MODE 框每拍翻转（看着就是一直闪）。
 * 加确认窗口后，短暂擦过设定值不会被当真；
 * 保持期间只要掉出 -2℃ 就重新计时，只认"连续满足"。
 *
 * 注意**不是** FLAG_READY：那是"已识别、可进 PID"的闸门
 * （原机 ST_READY 0x1000 同义，见 task_heat 的入口判断），
 * 把它当成"已到温"会让 PID 永远进不去。
 */
static void announce_if_reached(uint16_t setpoint)
{
    uint32_t now;

    /* 设定值 ≤50℃（休眠/待机）不算"到温"，也不响 */
    if (setpoint <= 50U) {
        ready_latched = false;
        ready_reach_ms = 0U;
        station.state &= (uint16_t)(~FLAG_TEMP_REACHED);
        return;
    }

    now = get_tick_ms();

    if (!ready_latched) {
        /* 未到温：看是否已到达设定值。到达后先记下起始时刻，
         * 等满 500 ms 再真正置位 —— 这就是"保持 500 ms"的实现。
         *
         * **进入判据必须给容差（v9.10 实测结论，v9.12 一并补齐）**：
         * PID 稳态会把温度压在设定值**下方**，实测 300℃ 时稳定在
         * 299.88~299.98℃，而 raw→温度是**截断**取整，299.9 就是 299。
         * 若判据要求精确 `>= 设定值`，299 永远不成立，"已到温"永远不出。
         * 所以进入放宽到 `设定值 − 1℃`，与退出的 `−2℃` 形成 1℃ 宽的
         * 滞回带 —— 进出不对称，进去稍难、退出更难，状态才不会在
         * 临界点每拍翻转。 */
        if (station.temperature_int >=
            ((int16_t)setpoint - HEAT_REACH_ENTER_C)) {
            if (0U == ready_reach_ms) {
                /* 第一拍满足：开始计时 */
                ready_reach_ms = now;
            } else if ((now - ready_reach_ms) >= HEAT_READY_HOLD_MS) {
                /* 连续满足满 500 ms：确认到温 */
                ready_latched = true;
            }
        } else {
            /* 中途掉下去：计时作废，下次满足要重新数 500 ms */
            ready_reach_ms = 0U;
        }
    } else if (station.temperature_int <
               ((int16_t)setpoint - HEAT_REACH_EXIT_C)) {
        /* 已到温：掉到 设定值-2℃ 以下才撤销 */
        ready_latched = false;
        ready_reach_ms = 0U;
    }

    /* 映射成状态位交给界面 */
    if (ready_latched) {
        station.state |= FLAG_TEMP_REACHED;
    } else {
        station.state &= (uint16_t)(~FLAG_TEMP_REACHED);
    }

    /* 提示音：只在首次确认到温时响一次 */
    if ((0U == (station.state & FLAG_ANNOUNCE)) || (!ready_latched)) {
        return;
    }
    station.state &= (uint16_t)(~FLAG_ANNOUNCE);
    if (0U != station.sound_enabled) {
        beep_buzzer(30U);
    }
}

/** READY 阶段：过温硬限 → 提示音 → PID → 占空比 */
static void run_ready_phase(void)
{
    uint16_t setpoint;
    uint8_t duty;
    uint8_t step;           /* 死区微调步长（固定 1，见 HEAT_TRIM_STEP_BASE） */
    float error;            /* 控制侧温度 - 设定值（浮点，保留方向信息） */

    /* 超过 470℃ 立刻断输出：热电偶可能脱焊，再加热会烧红烙铁头 */
    if (station.temperature > HEAT_TEMP_MAX) {
        station.heat_duty = 0U;
        station.pid_out = 0U;
        station.state &= (uint16_t)(~FLAG_HEATING);
        return;
    }

    setpoint = get_setpoint();
    announce_if_reached(setpoint);

    /* ---- 死区：保证占空比 + 按偏差微调（v9.21 回滚到 v9.17） ----
     *
     * 死区内**保持上次 PID 算出的占空比**，不重新算 PID（避免震荡），
     * 但按温度偏差方向每拍微调一格（偏高 -1、偏低 +1）。
     *
     * 为什么不能只冻结：烙铁头是纯热惯性系统，冻结的占空比几乎不可能
     * 刚好等于当时的散热功率 —— 多给一点就慢慢爬，少给一点就慢慢掉。
     * 微调等价于一个极慢的积分作用，把平衡点找回来。
     *
     * 步长固定 1（不是自适应）：v9.16/v9.18 试过自适应与两档，
     * 都是基于被 bug 污染的数据，实机更差。33 Hz 下每秒最多挪 33 格，
     * 对 ±2℃ 的死区已经足够。
     *
     * 判据用**浮点** station.temperature（控制侧 3 点快滤波），
     * 强转 int 会丢掉"偏高还是偏低"的方向信息。 */
    error = station.temperature - (float)setpoint;
    if ((error <= HEAT_DEADBAND_C) && (error >= -HEAT_DEADBAND_C) &&
        (0U == (station.state & FLAG_MENU))) {
        /* 进入死区：在保持值基础上按偏差方向挪一格 */
        deadband_active = true;

        /* 步长固定 1，两端钳位（见 HEAT_TRIM_STEP_BASE 的说明） */
        step = HEAT_TRIM_STEP_BASE;
        if (step < 1U) {
            step = 1U;                          /* 步长 0 会让占空比永久停住 */
        }
        if (step > HEAT_TRIM_STEP_MAX) {
            step = HEAT_TRIM_STEP_MAX;          /* 防止一步跳太多 */
        }

        /* 偏差要越过 0.1℃ 才动，否则平衡点附近会因浮点噪声每拍翻转 */
        if (error > 0.1F) {
            /* 偏高：往下减，不能减穿 0（减到 0 会被 PID 接管，见下） */
            if (deadband_duty >= step) {
                deadband_duty = (uint8_t)(deadband_duty - step);
            } else {
                deadband_duty = 0U;
            }
        } else if (error < -0.1F) {
            /* 偏低：往上加，不能加穿硬件上限 */
            if (((uint16_t)deadband_duty + step) <= HEAT_DUTY_MAX) {
                deadband_duty = (uint8_t)(deadband_duty + step);
            }
        }

        duty = deadband_duty;
        station.heat_duty = duty;

        /* **同步 D 项历史**：run_pid() 只在死区外调用，不同步的话
         * 离开死区时微分项会拿"很久以前的误差"算，产生虚假踢动
         * （Kd=15、差 4℃ 时约 −60）。
         *
         * 这里用 `setpoint - temperature`（与 run_pid() 完全同一表达式），
         * 而不是 `-error`。两者数学等价，但前者不依赖"error 是
         * temperature-setpoint"这个隐含前提，不会被后人读错符号。 */
        pid_last_error = (float)setpoint - station.temperature;
    } else {
        /* 离开死区：正常跑 PID，并记下本拍占空比供下次进死区时用 */
        deadband_active = false;
        station.pid_out = run_pid(setpoint);
        duty = scale_pwm_duty(station.pid_out);
        station.heat_duty = duty;
        deadband_duty = duty;
    }

    /* UI 靠 HEATING 位显示红色"加热中"。
     * 注意：死区保持占空比时如果 duty>0，仍然算"有输出"，
     * 但到温显示由 FLAG_TEMP_REACHED 优先（见 app_ui.c 的优先级），
     * 所以恒温时会正确显示"已到温"而不是"加热中"。 */
    if (0U != duty) {
        station.state |= FLAG_HEATING;
    } else {
        station.state &= (uint16_t)(~FLAG_HEATING);
    }
}

/** 预热验证：给固定占空比跑 15 个周期，温度合理才转 READY */
static void run_preheat_verify(void)
{
    /* 联锁/休眠/过流时预热也必须停，但周期照数，避免卡在识别阶段 */
    if ((0U != (station.state & (FLAG_JBC_SW | FLAG_SLEEP))) ||
        (0U != station.over_current_latch)) {
        station.heat_duty = 0U;
    } else {
        station.heat_duty = HEAT_DUTY_PREHEAT;
    }

    preheat_cycles++;

    /* 15 个周期（约 380 ms）后温度还没飞掉，才认为烙铁头真实存在 */
    if ((preheat_cycles >= HEAT_PREHEAT_CYCLES) && (station.temperature <= 500.0F)) {
        station.state &= (uint16_t)(~FLAG_DETECT);
        station.state |= (FLAG_READY | FLAG_ARMED | FLAG_ANNOUNCE);
    }
}

/** 未识别/识别中：先识别，成功后给一轮预热占空比 */
static void start_or_continue_detect(void)
{
    /* 还在等稳定或识别失败：本轮一定不给输出 */
    if (!detect_tip()) {
        station.heat_duty = 0U;
        station.pid_out = 0U;
        return;
    }

    station.state |= FLAG_DETECT;
    preheat_cycles = 0U;
    station.pid_out = 0U;
    station.heat_duty = HEAT_DUTY_PREHEAT;
}

/** 一个测温窗口结束后的完整控制计算 */
static void update_heating(void)
{
    uint16_t current_raw;
    uint16_t vin_raw;
    uint16_t raw;

    /* ADC 中断已经拉过流闩锁：这一轮绝不能再给占空比 */
    if (0U != adc_over_current) {
        station.over_current_latch = 1U;
        stop_heat_output();
    }

    read_heat_averages(&current_raw, &vin_raw);
    /* 分压比与采样电阻的换算系数取自原机，改一个数电流/电压显示就全错 */
    station.input_voltage = (float)vin_raw / 110.0F;
    station.heat_current = (float)current_raw / 130.0F;

    raw = read_temp_raw_avg();

    /* 换头优先处理：先停输出重识别，这一轮不再按旧型号算温度 */
    if (handle_tip_replacement()) {
        return;
    }

    /* 原始值顶到满量程 = 热电偶开路，再加热就是全功率灌进开路 */
    if (raw >= HEAT_RAW_OPEN_THRESHOLD) {
        handle_open_tip();
        return;
    }

    update_temperature(raw);

    /* 还没识别或正在识别：识别成功后本轮只给预热占空比 */
    if (0U == (station.state & (FLAG_READY | FLAG_DETECT))) {
        start_or_continue_detect();
        return;
    }

    /* 识别成功后的 15 周期预热验证 */
    if (0U != (station.state & FLAG_DETECT)) {
        run_preheat_verify();
        return;
    }

    run_ready_phase();
}

void init_heat(void)
{
    pid_integral = 0.0F;
    pid_last_error = 0.0F;
    measuring = false;
    preheat_cycles = 0U;
    detect_pending = false;
    recheck_c245 = false;
    measure_start_ms = 0UL;
    /* 上电时平均窗口和滞回都必须是干净的，否则会用 0 填充的窗口
     * 把前 10 个周期的温度拉低，显示从 0℃ 慢慢爬上来 */
    reset_temperature_filter();

    station.adc_phase = ADC_PHASE_HEAT;
    set_adc_phase(ADC_PHASE_HEAT);

    /* 从这一刻起 SysTick 才允许推进加热；
     * 显示初始化期间 ADC/加热还没配好，不能让它跑。 */
    heat_tick_enabled = true;
}

void tick_heat(void)
{
    /* 初始化没完成时加热状态机不能跑：ADC/定时器可能还没配好 */
    if (heat_tick_enabled) {
        task_heat();
    }
}

void task_heat(void)
{
    uint32_t now;

    /* 脉冲结束：加热丝已断电，立刻开 5 ms 测温窗口 */
    if (take_pulse_done()) {
        set_adc_phase(ADC_PHASE_MEASURE);
        station.adc_phase = ADC_PHASE_MEASURE;
        measure_start_ms = get_tick_ms();
        measuring = true;
        return;
    }

    /* 不在测温窗口，什么都不用算 */
    if (!measuring) {
        return;
    }

    now = get_tick_ms();
    /* 5 ms 是热电偶和 ADC 稳定的最小窗口，短了读数会偏低 */
    if ((now - measure_start_ms) < HEAT_MEASURE_MS) {
        return;
    }

    measuring = false;
    set_adc_phase(ADC_PHASE_IDLE);
    station.adc_phase = 0xFFU;

    update_heating();
    station.heat_cycles++;

#if (HEAT_OUTPUT_ENABLE != 0)
    /* 设置菜单打开期间禁止加热：用户翻菜单/调参数时不该继续给功率。
     * 这是"写硬件之前的最后一道闸"，识别/预热/正常控制三种相位都会被挡住，
     * 所以不必在 scale_pwm_duty 里再判一次。
     * 挡住识别相位是安全的：识别只看 ID 脚和热电偶读数，不依赖加热，
     * 而预热周期数照数（见 run_preheat_verify），提前退出菜单后照常转 READY。
     * 退回主页时 close_menu() 清掉 FLAG_MENU，下一拍自动恢复加热。 */
    if (0U != (station.state & FLAG_MENU)) {
        station.heat_duty = 0U;
        station.pid_out = 0U;
        station.state &= (uint16_t)(~FLAG_HEATING);
    }
    start_heat_pulse(station.heat_duty);
#else
    /* 安全版：物理输出和 UI 一起归零，避免屏幕上还显示"加热中" */
    station.heat_duty = 0U;
    station.pid_out = 0U;
    station.state &= (uint16_t)(~FLAG_HEATING);
    start_heat_pulse(0U);
#endif

    station.adc_phase = ADC_PHASE_HEAT;
    set_adc_phase(ADC_PHASE_HEAT);
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
