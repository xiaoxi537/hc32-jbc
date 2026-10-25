#!/usr/bin/env python3
"""
C210 温控 PID 参数仿真：复现"焊接大焊盘→提起烙铁→回温振荡"。

为什么要有这个脚本：
  实机调 PID 一次要烧录 + 焊接测试，一轮下来十几分钟。
  先在这里把固件的 PID 逻辑与最简热学模型跑一遍，
  把参数扫一遍再上机，能省掉大部分盲试。

**功率上限说明（重要）**
  C210: duty = pid_out/10 * (power_scale/100)
  其中 power_scale 默认 80%（原闭源固件的安全默认值），但**用户可以手动改**
  （菜单"参数调整 → 输出功率"，范围 10~100%）。
  默认 80% 时 pid_out 满 1000 -> duty=80，仅硬件满量程 250 的 32%。

  因此调参有两个杠杆：**PID 三个参数 + 输出功率百分比**。
  power_scale 调高能直接放大有效功率（缓解"到不了设定值"），
  但会让小烙铁头更容易过热/烧红 —— 默认 80% 是安全值，往上调要谨慎。

**仿真严格复刻固件逻辑**（app_heat.c 的 run_pid + scale_pwm_duty）：
    error = setpoint - temperature
    pid_integral = clamp(pid_integral + error, 0, 1000)    # 按采样次数累加，不带 dt
    if error >= -20: out = Kp*e + Ki*integral + Kd*(e - e_prev)
    else:            out = 0
    pid_out = clamp(out, 0, 1000)
    # C210: duty = pid_out / 10 * power_scale，再钳 250

热学模型（一阶集总，够用即可）：
    C_eff * dT/dt = P_heat - (T - T_amb)/R - P_load
  其中 P_heat 与 duty 成正比，P_load 模拟焊盘吸热。

  注意：模型参数是从 C210 的物理常识标定的**相对值**，
  不是实测数据。它只能用来比较参数的**相对好坏**，
  绝对升温时间/超调量不能当真实值看。

用法：
  python3 tools/sim_pid_c210.py              # 跑默认参数 + 推荐参数对比
  python3 tools/sim_pid_c210.py --sweep      # 扫参数网格
"""

import argparse
import sys

# ---- 固件常量（与 app_heat.c / bsp_heater.h 一致）----
CYCLE_MS = 30.34          # 加热脉冲 25.34 ms + 测温窗口 5 ms
DUTY_MAX = 250.0          # 硬件占空比满量程
TEMP_MAX = 470.0          # 过温硬限
PID_INTEGRAL_MAX = 1000.0
HEAT_TEMP_FILTER_LEN = 10     # 与 app_heat.c 一致，10 点滑动平均
ERROR_CUTOFF = -20.0      # 冲过设定值 20℃ 以上直接关输出


class Plant:
    """C210 烙铁头 + 焊盘 的一阶热模型"""

    def __init__(self, ambient=25.0):
        self.temp = ambient
        self.ambient = ambient

        # 空载：小烙铁头，热容小、散热快
        self.c_heat = 0.55       # 等效热容（相对单位）
        # 空载散热时间常数。这个值必须让"32% 功率上限"能维持 300℃：
        #   duty 上限 = 80/250 = 32%，稳态温度 = 25 + p_heat*--tau
        #   取 tau=6 s 时维持 300℃ 需要 pid_out≈716，落在 1000 上限内 ✓
        # （第一版取 3.5 s，结果满功率只能到 249℃，所有 PID 参数都没区别）
        self.tau_cool = 6.0      # 空载散热时间常数 s

        # 满功率（duty=250）下的净升温速率。
        # 标定依据：满功率稳态温度必须远高于 300℃，否则系统根本到不了设定值、
        # PID 参数也就完全不起作用（第一版标成 45 ℃/s，稳态只有 75℃，
        # 所有参数算出来的结果都一样 —— 这就是标定过低的典型症状）。
        # 这里取 520 ℃/s：满功率稳态 ≈ 25 + 520*3.5 = 1845℃（远超量程），
        # 保证 PID 有充足调节余量，只受 pid_out 上限约束。
        # 标定依据：C210 从室温升到 350℃ 约 4 秒（限功率后保守估计），
        # 平均净升温 ≈ 81 ℃/s；满功率留 2.5 倍余量 -> 约 200 ℃/s（duty=250）。
        # 这样"维持 300℃"所需的 pid_out 与积分都能落在固件上限内。
        self.max_heat_rate = 200.0   # ℃/s（duty=250 时）

        # 焊盘负载状态
        self.load = 0.0          # 吸热强度（℃/s 当量）
        self.load_c = 0.0        # 额外热容倍数

    def set_welding(self, on):
        """焊接大焊盘：吸热强、热容大；提起后瞬间消失"""
        self.load = 26.0 if on else 0.0
        self.load_c = 6.0 if on else 0.0

    def step(self, duty, dt):
        """推进 dt 秒，duty 是 0…250 的硬件占空比"""
        ctot = self.c_heat * (1.0 + self.load_c)
        p_heat = self.max_heat_rate * (duty / DUTY_MAX)
        p_cool = (self.temp - self.ambient) / self.tau_cool
        dtemp = (p_heat - p_cool) / (1.0 + self.load_c) - self.load
        self.temp += dtemp * dt


class OnePoleSensor:
    """复刻原机的"上升沿一阶平滑"（findings_core.md 步骤 3）

        if (t_new > t_old) t = (t_old + t_new) / 2   # 只在上行时取中点
        else              t = t_new                  # 下降沿直接跟随

    滞后只有 1 个周期（约 30 ms），是我们 10 点平均（303 ms）的 1/10。
    """

    def __init__(self):
        self.value = None

    def update(self, sample):
        if self.value is None:
            self.value = sample
        elif sample > self.value:
            self.value = (self.value + sample) * 0.5
        else:
            self.value = sample
        return self.value


class Sensor:
    """复刻固件的 10 点滑动平均（app_heat.c update_temperature）

    这一层很关键：10 点 × 30.34 ms = **303 ms 滞后**。
    PID 用的是这个滞后后的温度，而真实烙铁头温度已经跑在前面，
    于是"该刹车时还没看到温度"——这正是回温过冲振荡的重要成因。
    上一版仿真漏了这一层，导致 Kd 完全不起作用、也复现不出振荡。
    """

    def __init__(self, length=10):
        self.buf = [0.0] * length
        self.pos = 0
        self.fill = 0

    def update(self, sample):
        self.buf[self.pos] = sample
        self.pos = (self.pos + 1) % len(self.buf)
        if self.fill < len(self.buf):
            self.fill += 1
        return sum(self.buf) / self.fill


class Pid:
    """逐行复刻 app_heat.c 的 run_pid()"""

    def __init__(self, kp, ki, kd):
        self.kp = kp
        self.ki = ki
        self.kd = kd
        self.integral = 0.0
        self.last_error = 0.0

    def compute(self, setpoint, temperature):
        e = setpoint - temperature
        # 与固件一致：无条件累加（只有菜单期间才冻结），按次数而非时间
        self.integral = min(max(self.integral + e, 0.0), PID_INTEGRAL_MAX)

        if e >= ERROR_CUTOFF:
            out = self.kp * e + self.ki * self.integral + \
                  self.kd * (e - self.last_error)
            out = min(max(out, 0.0), 1000.0)
        else:
            out = 0.0
        self.last_error = e
        return out


def duty_from_pid(pid_out, power_scale=80.0, c210_high_voltage=True):
    """复刻 scale_pwm_duty() 的 C210 分支"""
    scaled = pid_out / 10.0 if c210_high_voltage else pid_out * 0.25
    duty = (power_scale / 100.0) * scaled
    return min(max(duty, 0.0), DUTY_MAX)


def run_scenario(kp, ki, kd, setpoint=300.0, verbose=False, power_scale=80.0,
                 filter_mode='fir10'):
    """跑完整场景，返回指标字典

    时间线（秒）：
      0–8    升温到设定值并稳定
      8–16   焊接大焊盘（持续吸热）
      16     提起烙铁（负载瞬间消失）→ 观察回温振荡
      16–40  观察是否稳定
    """
    plant = Plant()
    sensor = (OnePoleSensor() if filter_mode == 'onepole'
              else Sensor(HEAT_TEMP_FILTER_LEN))
    pid = Pid(kp, ki, kd)
    dt = CYCLE_MS / 1000.0

    # 时间线：先给足升温时间（C210 限功率，升温慢），再焊接、再提起
    t_weld_on, t_weld_off, t_end = 12.0, 20.0, 50.0
    welding = False
    t = 0.0

    peak_after = plant.ambient     # 提起后的最高温
    settled_at = None              # 首次进入 ±2℃ 且之后不再离开的时间
    hist = []

    # 记录"提起后"是否再次跌破 -3℃（说明发生过冲后回落 = 振荡）
    dip_after_peak = 0.0
    peak_seen = 0.0
    crossings = 0          # 提起后穿越设定值(±1℃)的次数 = 振荡次数
    side = 0

    while t < t_end:
        if (not welding) and t >= t_weld_on:
            plant.set_welding(True)
            welding = True
        if welding and t >= t_weld_off:
            plant.set_welding(False)

        # 固件的顺序：先按 ADC 采样算温度（经滑动平均），再喂给 PID，
        # 最后用这份"感知温度"决定本拍输出 —— 滞后就在这里产生。
        sensed = sensor.update(plant.temp)
        out = pid.compute(setpoint, sensed)
        duty = duty_from_pid(out, power_scale)
        plant.step(duty, dt)
        t += dt
        hist.append((t, plant.temp, duty, pid.integral))

        if t >= t_weld_off:
            # 统计穿越设定值的次数（每穿越一次 = 一次来回）
            if plant.temp > setpoint + 1.0 and side <= 0:
                crossings += 1
                side = 1
            elif plant.temp < setpoint - 1.0 and side >= 0:
                crossings += 1
                side = -1
            peak_seen = max(peak_seen, plant.temp)
            peak_after = peak_seen
            # 记录峰值之后的最大回落
            dip_after_peak = max(dip_after_peak, peak_seen - plant.temp)
            # 稳定判定：进入 ±2℃ 带之后不再离开
            if settled_at is None and abs(plant.temp - setpoint) <= 2.0:
                settled_at = t
            elif settled_at is not None and abs(plant.temp - setpoint) > 3.0:
                settled_at = None       # 又跑出去了，重新计时

    # 过冲量：提起后的峰值 - 设定值
    overshoot = peak_after - setpoint
    settle_time = (settled_at - t_weld_off) if settled_at else float('inf')

    result = dict(kp=kp, ki=ki, kd=kd,
                  overshoot=overshoot,
                  dip=dip_after_peak,
                  crossings=crossings,
                  settle_time=settle_time,
                  final=plant.temp,
                  integral_end=pid.integral)
    if verbose:
        result['hist'] = hist
    return result


def show(r, label=""):
    st = r['settle_time']
    st_s = "未稳定" if st == float('inf') else f"{st:.1f}s"
    print(f"  {('['+label+']') if label else ''}"
          f"Kp={r['kp']:>3} Ki={r['ki']:>4.1f} Kd={r['kd']:>3} | "
          f"过冲={r['overshoot']:>+6.1f}℃  振荡={r['crossings']}次  "
          f"稳定={st_s:>7}  末温={r['final']:6.1f}  残差={r['final']-300.0:+.1f}")


def score(r, setpoint=300.0):
    """越小越好：过冲 + 回落 + 稳定时间 + 终值偏差"""
    st = r['settle_time'] if r['settle_time'] != float('inf') else 60.0
    return (max(r['overshoot'], 0) * 3.0 +      # 过冲最要紧
            r['crossings'] * 8.0 +               # 振荡次数（本次要解决的）
            r['dip'] * 1.0 +                     # 振荡幅度
            st * 0.4 +                           # 稳定快慢
            abs(r['final'] - setpoint) * 1.5)    # 稳态残差


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sweep', action='store_true', help='扫参数网格')
    a = ap.parse_args()

    SETPOINT = 300.0

    print("=" * 78)
    print(f"C210 温控仿真（设定 {SETPOINT:.0f}℃，场景：焊接大焊盘 8s → 提起 → 观察）")
    print("=" * 78)
    print("注意：热模型为相对标定，只用来看参数之间的相对优劣，绝对值不可当实测。\n")

    print("当前出厂参数（C210: kp=25 ki=0.3 kd=15）:")
    base = run_scenario(25, 0.3, 15, SETPOINT)
    show(base, "当前")
    print(f"      评分={score(base):.1f}\n")

    # 候选参数覆盖大 Ki 区间：C210 限功率后需要更大 Ki 才能让积分
    # 在 1000 上限内撑起稳态输出（见仿真脚本顶部注释）
    print("候选参数（含大 Ki 区间，C210 限功率需要更大 Ki）:")
    cands = [
        (25, 0.3, 15), (25, 0.3, 25),                       # 小 Ki 对照组
        (25, 0.5, 25), (25, 0.6, 25), (25, 0.8, 25), (25, 1.0, 25),
        (20, 0.6, 25), (20, 0.8, 30), (20, 1.0, 25),
        (18, 0.8, 25), (18, 1.0, 30),
        (15, 1.0, 25), (25, 1.2, 25), (25, 1.5, 25),
    ]
    results = []
    for kp, ki, kd in cands:
        r = run_scenario(kp, ki, kd, SETPOINT)
        results.append(r)
        show(r)
    print()

    if a.sweep:
        # 扫描区间聚焦大 Ki：32% 功率天花板下，Ki 太小会让积分撑不住稳态
        print("参数网格扫描（Kp 12–30, Ki 4–18, Kd 10–40）:")
        best = []
        for kp in (12, 15, 18, 22, 25, 28, 30):
            for ki10 in (4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18):
                for kd in (10, 15, 20, 25, 30, 35, 40):
                    r = run_scenario(kp, ki10 / 10.0, kd, SETPOINT)
                    best.append((score(r), r))
        best.sort(key=lambda x: x[0])
        print("  最优 8 组:")
        for s, r in best[:8]:
            show(r, f"{s:.1f}")
        print()

    results.sort(key=lambda r: score(r))
    print("推荐（按评分排序，取前 3）:")
    for r in results[:3]:
        show(r, f"{score(r):.1f}")
        print(f"       母头设置：比例 Kp={r['kp']}  积分 Ki={int(round(r['ki']*10))}"
              f"  微分 Kd={r['kd']}")
    print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
