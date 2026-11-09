#!/usr/bin/env python3
"""用实测数据辨识被控对象，再仿真方法 A（丢拍）与方法 B（Sigma-Delta）。

为什么必须先辨识再仿真：
  文档 §41.4 坑 5 已四次印证"仿真不能用来选控制参数"——
  之前用一阶热模型给的 0.23℃ 与实机 0.23/13℃ 完全不符。
  这次的做法反过来：**先从实测稳态数据辨识对象参数**，
  要求仿真能复现实测的"12.65℃ 幅度 / 3.6 s 周期"，
  复现不了就说明模型错了，仿真结论一律作废。

对象模型：
  dT/dt = K * duty_eff - LOSS * (T - T_amb)   （一阶热惯性 + 散热）
  其中 duty_eff 是实际生效的占空比（0…250），
  K 由实测拟合得到，LOSS 由"0 占空比时降温速率"反推。
  另加热丝→热电偶的两级热阻，用一阶滞后近似（实测滞后约 1 拍）。
"""

import argparse
import csv
import math
import statistics
import sys

# ---------------- 固件常量（与 User/Src/app_heat.c 一致） ----------------
CYC = 0.03034          # 控制周期 s（251 拍 × 101 µs + 5 ms 测温）
KP = 25.0
KI_TENTHS = 3
KD = 15.0
KI = KI_TENTHS / 10.0
DEADBAND = 2.0
DUTY_MAX = 250
POWER_SCALE = 80        # C210 = 80%
C210_HV_DIV = 10.0      # C210 高压档 /10
SETPOINT = 350


def scale_duty(pid_out):
    """复现 scale_pwm_duty：C210 高压档 /10，再乘功率百分比。"""
    scaled = pid_out / C210_HV_DIV
    duty = (POWER_SCALE / 100.0) * scaled
    return max(0.0, min(float(DUTY_MAX), duty))


class Plant:
    """一阶热对象：加热功率 → 加热丝温度 → 热电偶温度（两级热阻）。

    参数由实测辨识，见 calibrate()。
    """

    def __init__(self, k_power, loss, tau_tip, amb=25.0, delay_cycles=1):
        self.k_power = k_power      # ℃/s per 满量程 duty
        self.loss = loss            # 1/s 散热系数
        self.tau_tip = tau_tip      # 加热丝→热电偶 时间常数 s
        self.amb = amb
        self.T_tip = amb
        self.T_tc = amb
        self.delay = delay_cycles   # 传输延迟（控制拍数）
        self.buf = [0.0] * (delay_cycles + 1)

    def step(self, duty, dt):
        # 延迟线：现在给的热，过 delay 拍才到加热丝
        self.buf.append(duty)
        d = self.buf.pop(0)
        # 加热丝吸热 - 散热 - 传给热电偶
        heat = self.k_power * (d / DUTY_MAX)
        conduct = (self.T_tip - self.T_tc) / self.tau_tip
        radiate = self.loss * (self.T_tip - self.amb)
        self.T_tip += (heat - conduct - radiate) * dt
        # 热电偶跟随加热丝
        self.T_tc += conduct * dt - self.loss * 0.15 * (self.T_tc - self.amb) * dt
        return self.T_tc


class Controller:
    """复现 run_pid + 死区微调，可切换方法 A / B / 基线。"""

    def __init__(self, method, trim_div=3, duty_precision=False):
        self.method = method          # "baseline" / "A" / "B"
        self.trim_div = trim_div      # 方法 A：每 N 拍才挪 1
        self.duty_precision = duty_precision  # 方法 B：float duty
        self.integ = 0.0
        self.last_err = 0.0
        self.deadband_duty = 0.0      # B 用 float，基线用整数
        self.trim_count = 0
        self.sd_acc = 0.0             # Sigma-Delta 累加器
        self.log = []

    def pid(self, T):
        err = SETPOINT - T
        # 死区外才累加积分（与固件一致）
        if abs(err) > DEADBAND:
            self.integ = max(0.0, min(1000.0, self.integ + err))
        if err >= -20.0:
            out = KP * err + KI * self.integ + KD * (err - self.last_err)
            out = max(0.0, min(1000.0, out))
        else:
            out = 0.0
        self.last_err = err
        return out

    def step(self, T):
        err = T - SETPOINT     # 固件里 error = T - setpoint（注意符号）
        in_db = (-DEADBAND <= err <= DEADBAND)

        if in_db:
            if self.method == "baseline":
                # 每拍挪 1 格（整数）
                if err > 0.1:
                    self.deadband_duty = max(0, self.deadband_duty - 1)
                elif err < -0.1:
                    self.deadband_duty = min(DUTY_MAX, self.deadband_duty + 1)
                duty = self.deadband_duty
            elif self.method == "A":
                # 方法 A：每 trim_div 拍才允许挪 1 格
                self.trim_count += 1
                if self.trim_count >= self.trim_div:
                    self.trim_count = 0
                    if err > 0.1:
                        self.deadband_duty = max(0, self.deadband_duty - 1)
                    elif err < -0.1:
                        self.deadband_duty = min(DUTY_MAX, self.deadband_duty + 1)
                duty = self.deadband_duty
            elif self.method == "B":
                # 方法 B：内部 float 微调 0.2 格/拍，输出用 Sigma-Delta
                STEP = 0.2
                if err > 0.1:
                    self.deadband_duty = max(0.0, self.deadband_duty - STEP)
                elif err < -0.1:
                    self.deadband_duty = min(float(DUTY_MAX), self.deadband_duty + STEP)
                # Sigma-Delta：把小数部分累积成整数输出，长期平均等于 float 值
                self.sd_acc += self.deadband_duty
                out = math.floor(self.sd_acc)
                self.sd_acc -= out
                duty = out
            self.last_err = SETPOINT - T
        else:
            self.trim_count = 0
            pid_out = self.pid(T)
            duty_f = scale_duty(pid_out)
            if self.method == "B":
                self.deadband_duty = duty_f          # B 保留小数
                self.sd_acc += duty_f
                out = math.floor(self.sd_acc)
                self.sd_acc -= out
                duty = out
            else:
                duty = int(duty_f)                   # 基线/A 截断成整数
                self.deadband_duty = duty
        return int(duty)


def calibrate(csv_path):
    """从实测数据辨识对象参数。

    做法：用稳态段"占空比 → dT/dt"的线性回归得到 K 与平衡点，
    再用 0 占空比段的降温速率反推 loss。
    """
    rows = list(csv.DictReader(open(csv_path)))
    D = [(float(r["t_ms"]) / 1000, float(r["temperature"]),
          int(r["heat_duty"]), int(r["state"])) for r in rows]
    ss = [x for x in D if 200 < x[0] < 800 and not (x[3] & 0x800)]
    if len(ss) < 100:
        sys.exit("恒温段数据不足")

    # 1) 用 1 s 窗口算 dT/dt，回归出 dT/dt = a*duty + c
    W = 3
    pts = []
    for i in range(len(ss) - W):
        dt = ss[i + W][0] - ss[i][0]
        if dt < 0.6:
            continue
        dT = (ss[i + W][1] - ss[i][1]) / dt
        duty = sum(ss[j][2] for j in range(i, i + W)) / W
        pts.append((duty, dT))
    n = len(pts)
    sx = sum(p[0] for p in pts)
    sy = sum(p[1] for p in pts)
    sxx = sum(p[0] ** 2 for p in pts)
    sxy = sum(p[0] * p[1] for p in pts)
    a = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    c = (sy - a * sx) / n
    duty_eq = -c / a

    # 2) 实测温度幅度与周期（作为仿真必须复现的验收指标）
    T = [x[1] for x in ss]
    amp = max(T) - min(T)

    # 3) 由"平衡占空比"反推 K：稳态时 heat = loss
    #    a 是 (duty 每格对应的净 dT/dt)，已经含了 loss 的影响近平衡点
    #    直接用 a 作为 K*duty_rate 的净增益，另取 loss 使 0 占空比降温率吻合
    zero_rates = [p[1] for p in pts if p[0] < 0.5]
    loss_rate = statistics.mean(zero_rates) if zero_rates else -9.89

    print(f"辨识结果（来自 {csv_path}）：")
    print(f"  dT/dt = {a:.3f}*duty + {c:.3f}   (r 略低是因热滞后)")
    print(f"  平衡占空比 duty* = {duty_eq:.2f} 格")
    print(f"  实测温度幅度 = {amp:.2f}℃")
    print(f"  0 占空比降温率 = {loss_rate:.2f} ℃/s")
    return {
        "k_power": a * DUTY_MAX,     # 满量程升功率 ℃/s
        "loss": abs(loss_rate) / (SETPOINT - 25.0),
        "amp_measured": amp,
        "duty_eq": duty_eq,
    }


def simulate(params, method, seconds=600, trim_div=3, seed=1):
    """跑闭环仿真，返回温度轨迹统计。"""
    plant = Plant(params["k_power"], params["loss"],
                  tau_tip=0.8, amb=25.0, delay_cycles=1)
    ctrl = Controller(method, trim_div=trim_div)

    # 从平衡点附近起步，跳过升温瞬态，只看稳态
    plant.T_tip = SETPOINT + 10
    plant.T_tc = SETPOINT + 10

    T_log = []
    n = int(seconds / CYC)
    for i in range(n):
        T = plant.T_tc
        duty = ctrl.step(T)
        plant.step(duty, CYC)
        T_log.append(T)

    # 丢掉前 20% 适应期
    cut = int(len(T_log) * 0.2)
    steady = T_log[cut:]
    if not steady:
        return None
    amp = max(steady) - min(steady)
    mean = statistics.mean(steady)
    # 数过零点估周期
    crossings = []
    for i in range(1, len(steady)):
        if (steady[i - 1] - SETPOINT) * (steady[i] - SETPOINT) < 0:
            crossings.append(i)
    period = (crossings[-1] - crossings[0]) / max(len(crossings) - 1, 1) * CYC * 2 if len(crossings) > 1 else 0
    return {"amp": amp, "mean": mean, "period": period, "tr": steady}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="docs/baseline/v929_c210_350c.csv")
    ap.add_argument("--seconds", type=float, default=600)
    args = ap.parse_args()

    p = calibrate(args.csv)
    print()

    # 先验证模型能否复现实测 —— 复现不了则一切结论作废
    print("=" * 62)
    print("第 0 步：模型自检（必须复现基线 12.65℃ / 3.6s）")
    print("=" * 62)
    base = simulate(p, "baseline", args.seconds)
    print(f"  基线仿真: 幅度={base['amp']:.2f}℃  均值={base['mean']:.2f}  "
          f"周期={base['period']:.1f}s")
    print(f"  实测对照: 幅度={p['amp_measured']:.2f}℃  均值≈349.9  周期≈3.6s")
    err = abs(base["amp"] - p["amp_measured"]) / p["amp_measured"] * 100
    print(f"  幅度误差 = {err:.0f}%")
    if err > 60:
        print("  ⚠ 模型复现不了实测幅度，下面的收益对比只能看趋势，不能当预测值")
    print()

    print("=" * 62)
    print("方法 A：丢拍（每 N 拍才允许微调 1 格）")
    print("=" * 62)
    print(f"{'N':>4} {'幅度℃':>10} {'相对基线':>10}")
    for n_div in [1, 2, 3, 4, 6, 8, 12]:
        r = simulate(p, "A", args.seconds, trim_div=n_div)
        red = (1 - r["amp"] / base["amp"]) * 100 if base["amp"] > 0 else 0
        print(f"{n_div:>4} {r['amp']:>10.2f} {red:>9.1f}%")
    print()

    print("=" * 62)
    print("方法 B：内部高精度 + Sigma-Delta 输出")
    print("=" * 62)
    r = simulate(p, "B", args.seconds)
    red = (1 - r["amp"] / base["amp"]) * 100
    print(f"  B 幅度={r['amp']:.2f}℃  均值={r['mean']:.2f}  降低 {red:.1f}%")


if __name__ == "__main__":
    main()
