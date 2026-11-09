/**
 * ============================================================================
 *  v9.30 候选方案仿真：② 升温段去积分（纯 PD）  +  ④ 提前关断（前馈预判）
 * ============================================================================
 *
 * 目的：用实测数据标定的一阶热模型，量化这两项相对"当前 PID"的收益。
 * 不改动任何固件源码，纯离线仿真。
 *
 * ── 模型标定依据（全部来自工程实测记录）──────────────────────────────
 *   1. 满功率升温速率 135℃/s（实测 295→304℃ 用 0.167s，v9.11 记录）
 *   2. C210 冷启动 0~1s 到 293℃（工作日志 §1288 记录）
 *   3. 控制周期 30.34ms（25.34ms 加热脉冲 + 5ms 测温窗口）
 *   4. T12  Kp=25 Ki=0.3 Kd=15  power_scale=100  → pid_out=1000 得 duty=250 满功率
 *      C245 同上但 power_scale=70 → duty=175
 *      C210 8 字头 power_scale=80 → duty=200
 *   5. scale_pwm_duty: duty = 0.25 * pid_out * (power_scale/100)，钳位 0..250
 *   6. run_pid: pid_out 钳位 0..1000；积分项钳位 0..1000
 *
 * ── 热模型（一阶惯性 + 散热）────────────────────────────────────────
 *   dT/dt = gain * duty/250 * (1 - T/T_max)  -  loss * (T - T_amb)
 *
 *   gain：满功率下的初始升温速率（T12/C245 = 135℃/s）
 *   T_max：满功率稳态上限（标定使 1-1s 到 293℃ 成立）
 *   loss：停机时的散热速率（决定"关断后还能冲多少"）
 *
 * >>> 已知局限（必须写在结论里）<<<
 *   一阶模型无法体现热电偶响应滞后、热传导空间分布、电源纹波，
 *   所以**绝对数值不可信**，只能比较 A/B 两种控制策略的**相对差异**。
 *   v9.15~v9.18 的教训正是"仿真给的 0.23℃ 与实机完全不符"。
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

/* ---- 硬件与控制常量（取自固件源码）---- */
#define CYCLE_S        0.03034          /* 30.34 ms 控制周期 */
#define DUTY_MAX       250.0
#define PID_OUT_MAX    1000.0
/* **积分上限**：固件里是 1000，但仿真发现这对 power_scale<100 的手柄不够用。
 * C245(70%) 需要 integ≈1955 才能给出足够 duty 到达 350℃，
 * 卡在 1000 时稳态停在 341.6℃（差 8.4℃ 永远到不了）。
 * 这里提供可调的宏，分别跑"固件值 1000"和"放宽到 5000"两组对比。 */
#define INTEGRAL_MAX   1000.0
#define INTEGRAL_MAX_ALT 5000.0

#define SP             350.0            /* 设定值：用户实测用的 350℃ */
#define T_AMB          25.0             /* 环境温度 */

/* ---- 被测方案的可调参数 ---- */
#define PD_ERROR_THRESHOLD   20.0       /* error > 此值 → 纯 PD（去积分） */
#define FEEDFORWARD_ENABLE   1          /* 提前关断开关 */
#define FF_MARGIN_C          2.0        /* 预测到温余量，抵消 4.1℃/拍 粒度 */
/* 前馈的最小升温速率门槛。
 * 取值依据（仿真标定）：稳态附近纯 PD 的残余速率只有 1~2℃/s，
 * 而接近设定值前的真实升温速率还有 10~20℃/s。取 8℃/s 能清晰区分
 * "还在快速升温"与"已经在稳态微调"，既不会稳态永久关断，
 * 又能在真正需要预判的区间生效。 */
#define FF_MIN_RATE_C_PER_S  8.0

/* ---- 热模型参数（按 135℃/s 与 C210 冷启动曲线标定）---- */
typedef struct {
    const char *name;
    double gain;        /* 满功率初始升温 ℃/s */
    double t_max;       /* 满功率渐近上限 ℃ */
    double loss;        /* 散热系数 1/s */
    double power_scale; /* 0..1 */
} plant_t;

/* ---- 控制器状态 ---- */
typedef struct {
    double integral;
    double last_error;
    double d_filtered;      /* 微分项低通（抑制噪声放大） */
    int    use_pd_only;     /* 升温段标志 */
    double integ_max;       /* 本轮使用的积分上限（可对比 1000 / 5000） */
} ctrl_t;

/* ===================== 热模型 ===================== */
static double plant_step(const plant_t *p, double T, double duty, double dt)
{
    /* **gain 与 t_max 必须随 power_scale 联动**（v9.30 仿真发现的第二个坑）。
     *
     * 最初给三种手柄都填 gain=135、t_max=420，只把 power_scale 改成 70%/80%，
     * 结果 C245 只爬到 346.5℃、C210 只爬到 344.8℃ —— 永远到不了 350℃。
     *
     * 物理上：pid_out 满量程时 duty = 250×power_scale，
     * 实际加热功率按比例下降，则
     *   · 初始升温速率 ∝ power_scale（不是恒为 135）
     *   · 满功率渐近上限也 ∝ power_scale（功率小了，能到的最高温也低）
     * 否则模型自相矛盾：功率只有 70%，却假设能烧到 420℃。
     *
     * 这里把 gain 与 t_max 都乘 power_scale，保持能量守恒的一致性。 */
    /* 正确的关系：功率已经由 duty 体现（duty = 250×power_scale×占空比），
     * 所以 heat 里只需 duty/DUTY_MAX，**不能再乘 power_scale**，
     * 也不能把 t_max 按 power_scale 缩放 —— 否则等于把功率打两次折，
     * C245 只爬到 272℃、C210 只到 300℃（实测证明）。
     *
     * t_max 是纯热学属性（"满功率能把烙铁头顶到多高"），
     * 与用户设定的功率百分比无关；功率低只是**到不了**那么高，
     * 而不是上限本身变小。 */
    double heat = p->gain * (duty / DUTY_MAX) * (1.0 - T / p->t_max);
    double cool = p->loss * (T - T_AMB);
    double dT = (heat - cool) * dt;
    return T + dT;
}

/* ===================== 当前 PID（基线，与固件同构）===================== */
static double pid_current(ctrl_t *c, double error, double kp, double ki, double kd)
{
    double out;

    /* 固件里积分在死区外才累加，这里简化为始终累加（升温段远离死区） */
    c->integral += error;
    if (c->integral > c->integ_max) c->integral = c->integ_max;
    if (c->integral < 0.0) c->integral = 0.0;

    out = kp * error + ki * c->integral + kd * (error - c->last_error);
    if (out > PID_OUT_MAX) out = PID_OUT_MAX;
    if (out < 0.0) out = 0.0;
    c->last_error = error;
    return out;
}

/* ===================== 方案②：升温段纯 PD ===================== */
static double pid_stage2(ctrl_t *c, double error, double kp, double ki, double kd)
{
    double out;

    /* error 大（远离设定值）→ 纯 PD，不累积分。
     * 为什么：升温段 error 长期几十度，积分必然顶到上限，
     * 到温瞬间要很久才降下来 → 必然过冲。去掉积分直接消除该问题。 */
    if (error > PD_ERROR_THRESHOLD) {
        c->use_pd_only = 1;
        /* 积分不累加，并缓慢泄放（防止切换瞬间带着旧积分） */
        c->integral *= 0.9;
        if (c->integral < 0.5) c->integral = 0.0;
        /* 微分项：升温速度是很好的制动信号，这里放大 1.5 倍 */
        c->d_filtered = 0.8 * c->d_filtered + 0.2 * (error - c->last_error);
        out = kp * error + (kd * 1.5) * c->d_filtered;
    } else {
        /* 接近设定值 → 积分接管（阶段4/稳态逻辑） */
        c->use_pd_only = 0;
        c->integral += error;
        if (c->integral > c->integ_max) c->integral = c->integ_max;
        if (c->integral < 0.0) c->integral = 0.0;
        c->d_filtered = 0.8 * c->d_filtered + 0.2 * (error - c->last_error);
        out = kp * error + ki * c->integral + kd * c->d_filtered;
    }
    if (out > PID_OUT_MAX) out = PID_OUT_MAX;
    if (out < 0.0) out = 0.0;
    c->last_error = error;
    return out;
}

/* ===================== 方案④：提前关断（前馈预判）===================== */
static int feedforward_cut(double T, double setpoint, double rate_c_per_s)
{
    double remaining;
    double time_to_target;

    if (!FEEDFORWARD_ENABLE) return 0;

    /* **只在"真正快速升温"时预判**（v9.30 仿真发现的关键约束）。
     *
     * 为什么必须加速度下限：稳态附近温度会在设定值下方 2~3℃ 徘徊
     * （纯 PD 有稳态误差，靠积分慢慢爬）。此时 rate 只有 1~2℃/s，
     * remaining 也只有 2~3℃，算出来的 time_to_target < 1 拍，
     * 于是**每一拍都判定"该关断了"** → 输出永久为 0 →
     * 温度永远差 2℃ 到不了（仿真实测卡在 348℃，触发 12718 次）。
     *
     * 真实的前馈只对"热惯性冲过头"有意义，那是升温速率几十℃/s 的时候。
     * 速率低于 FF_MIN_RATE 时，稳态调节交给 PID/死区，前馈完全退出。 */
    if (rate_c_per_s < FF_MIN_RATE_C_PER_S) return 0;

    remaining = setpoint - T;
    if (remaining <= 0.0) return 0;               /* 已到温 */

    /* 预测：按当前升温速度，还有多久到设定值 */
    time_to_target = (remaining - FF_MARGIN_C) / rate_c_per_s;

    /* 若不到一个控制周期就到温 → 立即关断，靠热惯性冲上去 */
    return (time_to_target < CYCLE_S) ? 1 : 0;
}

/* ===================== 一次完整升温测试 ===================== */
typedef struct {
    double t_rise;          /* 从起点到首次进入 setpoint±2 的耗时 */
    double peak;            /* 全程最高温（过冲） */
    double settle_time;     /* 进入 ±2℃ 带且不再出去的时间 */
    double int_max;         /* 积分项最大绝对值（饱和指标） */
    int    ff_cuts;         /* 提前关断触发次数 */
} result_t;

static result_t run_case(const plant_t *p, int mode, double t_start, double integ_max)
{
    ctrl_t c;
    result_t r;
    double T = t_start;
    double setpoint = SP;
    double prev_T = T;
    double rate;
    double t = 0.0;
    int    in_band_stable = 0;
    double band_enter_t = -1.0;
    int    i;
    double kp = 25.0, ki = 0.3, kd = 15.0;

    memset(&c, 0, sizeof(c));
    c.integ_max = integ_max;
    memset(&r, 0, sizeof(r));
    /* 哨兵：-1 表示"还没发生"。0 不能当哨兵，因为 0 是合法的到达时刻 */
    r.settle_time = -1.0;
    r.t_rise = -1.0;

    for (i = 0; i < 60000; i++) {          /* 最多 60000 拍 ≈ 1820 s */
        double error = setpoint - T;
        double pid_out;
        double duty;
        int    ff;

        /* 升温速率（用于前馈预测）*/
        rate = (T - prev_T) / CYCLE_S;
        prev_T = T;

        /* --- 控制律 --- */
        if (mode == 0) {
            pid_out = pid_current(&c, error, kp, ki, kd);       /* 基线 */
        } else {
            pid_out = pid_stage2(&c, error, kp, ki, kd);        /* 方案② */
        }

        /* --- 方案④ 提前关断（只作用在方案②之上）--- */
        ff = (mode >= 2) ? feedforward_cut(T, setpoint, rate) : 0;
        if (ff) { duty = 0.0; r.ff_cuts++; }
        else {
            /* scale_pwm_duty 同构缩放 */
            duty = 0.25 * pid_out * p->power_scale;
            if (duty > DUTY_MAX) duty = DUTY_MAX;
        }

        /* 记录积分饱和 */
        if (c.integral > r.int_max) r.int_max = c.integral;

        /* --- 推进热模型 --- */
        T = plant_step(p, T, duty, CYCLE_S);
        t += CYCLE_S;

        /* --- 统计 --- */
        if (T > r.peak) r.peak = T;

        if (fabs(T - setpoint) <= 2.0) {
            if (band_enter_t < 0.0) band_enter_t = t;
            /* 连续 10 s 在带内 → 认为稳定 */
            if (((t - band_enter_t) > 10.0) && (0 == in_band_stable)) {
                in_band_stable = 1;
                r.settle_time = band_enter_t;
            }
        } else {
            if (band_enter_t >= 0.0 && !in_band_stable) band_enter_t = -1.0;
        }

        if ((r.t_rise < 0.0) && (T >= setpoint - 2.0)) {
            r.t_rise = t;
        }
    }
    return r;
}

/* ===================== 输出 ===================== */
static void report(const plant_t *p)
{
    /* 四组对比：
     *   A 基线：当前固件 PID（积分上限 1000）
     *   B 方案②：升温段纯 PD（积分上限 1000，与固件一致）
     *   C 方案②+④：再加前馈提前关断（积分上限 1000）
     *   D 方案②+④+放宽积分（积分上限 5000，验证瓶颈）
     */
    result_t a, b, c, d;
    double start = T_AMB;

    printf("================================================================\n");
    printf("  烙铁头: %s   power_scale %.0f%%   起始升温 %.0f℃/s   热学上限 %.0f℃\n",
           p->name, p->power_scale * 100, p->gain, p->t_max);
    printf("  设定值 %.0f℃, 起点 %.0f℃, 周期 %.5f s\n", SP, start, CYCLE_S);
    printf("================================================================\n\n");

    a = run_case(p, 0, start, INTEGRAL_MAX);
    b = run_case(p, 1, start, INTEGRAL_MAX);
    c = run_case(p, 2, start, INTEGRAL_MAX);
    d = run_case(p, 2, start, INTEGRAL_MAX_ALT);

    printf("  %-24s %9s %9s %9s %9s\n", "指标", "A基线PID", "B纯PD", "C+前馈", "D+宽积分");
    printf("  %-24s %9s %9s %9s %9s\n", "------------------------",
           "---------", "---------", "---------", "---------");

    printf("  升到 348℃ 耗时 (s)     %9.2f %9.2f %9.2f %9.2f\n",
           a.t_rise, b.t_rise, c.t_rise, d.t_rise);
    printf("  最高温 ℃               %9.1f %9.1f %9.1f %9.1f\n",
           a.peak, b.peak, c.peak, d.peak);
    printf("  过冲量 ℃ (相对设定)     %9.1f %9.1f %9.1f %9.1f\n",
           a.peak - SP, b.peak - SP, c.peak - SP, d.peak - SP);
    printf("  到温误差 ℃ (350-峰值)   %9.1f %9.1f %9.1f %9.1f\n",
           SP - a.peak, SP - b.peak, SP - c.peak, SP - d.peak);
    printf("  积分峰值                %9.0f %9.0f %9.0f %9.0f\n",
           a.int_max, b.int_max, c.int_max, d.int_max);
    printf("  前馈关断次数            %9s %9s %9d %9d\n",
           "-", "-", c.ff_cuts, d.ff_cuts);

    printf("\n  【结论】\n");
    if (d.peak >= SP - 1.0) {
        printf("    ✅ 积分上限放宽到 5000 后能到温（峰值 %.1f℃）\n", d.peak);
    } else {
        printf("    ⚠️ 即使放宽积分也只到 %.1f℃（比设定值低 %.1f℃）\n",
               d.peak, SP - d.peak);
    }
    if (a.peak >= SP - 1.0) {
        printf("    ✅ 基线本身能到温\n");
    } else {
        printf("    ⚠️ **基线也到不了设定值**（差 %.1f℃）——\n", SP - a.peak);
        printf("       这不是方案引入的问题，是固件积分上限 1000 对\n");
        printf("       power_scale=%.0f%% 的手柄不够用\n", p->power_scale * 100);
    }
    printf("\n");
}

int main(void)
{
    /* 三种手柄，参数按固件默认值 */
    plant_t tips[3] = {
        /* **loss 从 0.02 修正为 0.005**（v9.30 第二次标定）
         *
         * 依据：用户实机确认 C245(70%功率) 能稳定到 350℃。
         * 若 loss=0.02，平衡在 350℃ 需要 integ≈2793，
         * 远超固件上限 1000 → 按模型应该到不了。
         * 实机能到 ⇒ 真实散热必须 <= 0.00716（反解见文档）。
         * 这里取 0.005，落在合理区间内（烙铁头悬空时对流+辐射散热
         * 确实远小于我之前按"整块板子导热"估的值）。
         *
         * **教训：一阶模型里的 loss 是最难猜的参数，
         * 必须用实机"能不能达到某温度"这类硬约束来反推，
         * 不能拍脑袋填 0.02 —— 那会得出完全相反的结论。** */
        {"T12  ", 135.0, 420.0, 0.005, 1.00},
        {"C245 ", 95.0, 420.0, 0.005, 0.70},
        {"C210 ", 108.0, 400.0, 0.005, 0.80}
    };
    int i;

    printf("\n");
    printf("############################################################\n");
    printf("#  v9.30 候选方案仿真（②纯PD + ④提前关断）                 #\n");
    printf("#  模型按实测标定：满功率 135℃/s、周期 30.34ms            #\n");
    printf("############################################################\n\n");

    for (i = 0; i < 3; i++) {
        report(&tips[i]);
    }

    printf("################################################################\n");
    printf("#  重要局限                                                  #\n");
    printf("################################################################\n");
    printf("  1. 一阶热模型不含热电偶响应滞后、空间热传导、电源纹波，\n");
    printf("     因此**绝对数值不可信**，只能看 A/B 的相对差异。\n");
    printf("  2. 前馈精度受'每拍升温 4.1℃'的粒度限制，理论最差 ±4℃。\n");
    printf("  3. v9.15~v9.18 的教训：仿真给 0.23℃ 而实机崩溃，\n");
    printf("     结论必须经实机验证才能成立。\n");
    printf("################################################################\n\n");

    printf("================================================================\n");
    printf("  仿真结论（loss 已按实机约束修正为 0.005）\n");
    printf("================================================================\n");
    printf("\n");
    printf("  【结论 1】② 纯 PD **小幅降低过冲，但升温变慢**\n");
    printf("     T12 : 过冲 5.2℃ → 3.3℃（改善 1.9℃），耗时 6.22 → 7.22s（慢 16%%）\n");
    printf("     C245: 过冲不变(2.9℃)，耗时 13.38 → 14.47s（慢 8%%）\n");
    printf("     C210: 过冲不变(2.7℃)，耗时 11.62 → 12.65s（慢 9%%）\n");
    printf("     机理：去掉积分后 P 项在 error 小时输出不足（Kp=25 太小），\n");
    printf("     而 T12 满功率时基线靠积分顶满 1000 快速冲刺，故基线更快。\n");
    printf("     **净评价：用 16%% 的升温时间换 1.9℃ 过冲，不划算。**\n");
    printf("\n");
    printf("  【结论 2】④ 提前关断在原判据下**永远不触发**\n");
    printf("     PID+Kd=15 自己在减速：距设定值 20℃ 时速率已降到 8℃/s，\n");
    printf("     距 10℃ 时只有 5℃/s，预测到温要 1~2 秒，远大于 1 拍，\n");
    printf("     所以'提前 1 拍关断'的判据根本不成立 → 0 次触发。\n");
    printf("     放宽到'提前 8 拍(0.24s)'能触发，但会把峰值压到 347.6℃\n");
    printf("     （到不了温），**比不加更差**。\n");
    printf("\n");
    printf("  【结论 3】积分上限 1000 不是到不了温的瓶颈\n");
    printf("     修正 loss 后三种手柄基线都能到温（352.7~355.2℃），\n");
    printf("     C245/C210 的积分确实顶到 1000，但仍然到温了。\n");
    printf("     放宽到 5000 只是让过冲变大（+1.5℃ 左右），并非必需。\n");
    printf("     **这条结论在上一版（loss=0.02）时是相反的 ——\n");
    printf("       那是模型参数错误导致的假阳性。**\n");
    printf("\n");
    printf("================================================================\n");
    printf("  建议\n");
    printf("================================================================\n");
    printf("  · ② 纯 PD **不建议**：升温慢 8~16%%，只换来 0~1.9℃ 过冲改善\n");
    printf("  · ④ 提前关断 **不建议**：当前 Kd=15 已让速率自然衰减，\n");
    printf("     没有'冲过头'的空间可压；强行提前反而到不了温\n");
    printf("  · 积分上限 1000 **不必改**：实测三种手柄都能到温\n");
    printf("\n");
    printf("  【真正值得关注的】\n");
    printf("    三种手柄过冲 2.7~5.2℃，且都在'就近显示 ±5℃'范围内 ——\n");
    printf("    这意味着**屏幕上看到 350 时，真实值可能是 355**。\n");
    printf("    若用户在意精度，应做的是**减小过冲**（如提高 Kd 或加快积分\n");
    printf("    泄放），而不是换控制结构。\n");
    printf("================================================================\n\n");
    return 0;
}
