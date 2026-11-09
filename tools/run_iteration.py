#!/usr/bin/env python3
"""迭代流程一键工具：抓基线 / 自检 / 对比。

对应 docs/迭代流程规范.md 的第 3 节流程。所有子命令都强制校验
"本次采集的固件版本"，防止拿错版本的数据做对比。

子命令：
  baseline  抓一次基线（自动带元数据 json）
  capture   抓改动后的数据（同 baseline，但输出到新文件）
  check     版本自检：板载固件 == 本地构建产物
  compare   对比两次采集，输出判定
  branch    开迭代分支（从 master 拉出，禁止在 master 上直接改）

必须用 pyocd 的 venv 解释器运行：
  PY=/home/bunny233/.local/share/pipx/venvs/pyocd/bin/python
  $PY tools/run_iteration.py baseline --tag v9.29-baseline --tip c210 --temp 350
"""

import argparse
import csv
import hashlib
import json
import os
import statistics
import subprocess
import sys
import time
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PROBE = "3983ABCD"
TARGET = "hc32f460xe"
BIN = os.path.join(ROOT, "build/hc32_test.bin")
ELF = os.path.join(ROOT, "build/hc32_test.elf")

# 地址从当前 elf 动态读取，**不要硬编码**。
#
# 为什么：变量地址是编译期定的，改代码后地址会变。交接说明 §41.5 坑 A
# 就是拿旧 elf 的地址读新固件，读出恒为 0，差点误判"积分从未累积"。
# 本轮（v9.31 方法 B）实测 pid_integral 又从 0x1FFF81B8 挪到了 0x1FFF81C0，
# 硬编码必然会踩。
def _symbol_addr(name):
    if not os.path.exists(ELF):
        sys.exit(f"缺少 {ELF}，先 make full")
    out = subprocess.run(["arm-none-eabi-nm", "-n", ELF],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    sys.exit(f"elf 里找不到符号 {name}（可能被内联或改名）")


STATION = _symbol_addr("station")
PID_INTEGRAL = _symbol_addr("pid_integral")
SYSTICK_MS = _symbol_addr("systick_ms")
OFF_STATE = 0x08
OFF_PID_OUT = 0x0A
OFF_SET_TEMP = 0x0C
OFF_TEMPERATURE = 0x1C
OFF_VIN = 0x20
OFF_CURRENT = 0x24
OFF_CAL_RAW = 0x28
OFF_TEMP_INT = 0x2C
OFF_HEAT_DUTY = 0x2E
OFF_TIP_CHANNEL = 0x2F
OFF_OVER_CURRENT = 0x30
SPAN_BEGIN = STATION
SPAN_LEN = (SYSTICK_MS + 4) - STATION

FLAG_JBC_SW = 0x0010
FLAG_SLEEP = 0x0400
FLAG_STANDBY = 0x0800
FLAG_READY = 0x1000
FLAG_TIP_FAULT = 0x0004


# ---------------------------------------------------------------- 基础设施

def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def connect():
    from pyocd.core.helpers import ConnectHelper
    s = ConnectHelper.session_with_chosen_probe(
        unique_id=PROBE, target_override=TARGET,
        options={"auto_unlock": True})
    if s is None:
        sys.exit("连不上探针")
    s.open()
    # pyocd 连接默认 halt 内核，不 resume 会采到全冻结的假数据
    s.target.resume()
    return s


def git_describe():
    try:
        return subprocess.run(["git", "describe", "--tags", "--always", "--dirty"],
                              cwd=ROOT, capture_output=True, text=True).stdout.strip()
    except Exception:
        return "unknown"


def git_branch():
    try:
        return subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                              cwd=ROOT, capture_output=True, text=True).stdout.strip()
    except Exception:
        return "unknown"


# ---------------------------------------------------------------- 采集

FIELDS = ["t_ms", "systick_ms", "state", "tip_channel", "set_temp",
          "temperature", "temp_int", "pid_out", "pid_integral",
          "heat_duty", "input_voltage", "heat_current",
          "cal_raw_at_350", "over_current_latch", "read_sec"]


def parse_snapshot(data, t_ms, read_sec):
    import struct

    def at(a):
        return a - SPAN_BEGIN

    def u8(a):
        return data[at(a)]

    def u16(a):
        return struct.unpack_from("<H", data, at(a))[0]

    def i16(a):
        return struct.unpack_from("<h", data, at(a))[0]

    def u32(a):
        return struct.unpack_from("<I", data, at(a))[0]

    def f32(a):
        return struct.unpack_from("<f", data, at(a))[0]

    return {
        "t_ms": t_ms,
        "systick_ms": u32(SYSTICK_MS),
        "state": u16(STATION + OFF_STATE),
        "tip_channel": u8(STATION + OFF_TIP_CHANNEL),
        "set_temp": u16(STATION + OFF_SET_TEMP),
        "temperature": round(f32(STATION + OFF_TEMPERATURE), 3),
        "temp_int": i16(STATION + OFF_TEMP_INT),
        "pid_out": u16(STATION + OFF_PID_OUT),
        "pid_integral": round(f32(PID_INTEGRAL), 3),
        "heat_duty": u8(STATION + OFF_HEAT_DUTY),
        "input_voltage": round(f32(STATION + OFF_VIN), 3),
        "heat_current": round(f32(STATION + OFF_CURRENT), 3),
        "cal_raw_at_350": round(f32(STATION + OFF_CAL_RAW), 1),
        "over_current_latch": u8(STATION + OFF_OVER_CURRENT),
        "read_sec": round(read_sec, 4),
    }


def do_capture(out_path, seconds, interval, tag, tip, temp, skip_check=False):
    if not os.path.exists(BIN):
        sys.exit(f"缺少 {BIN}，先 make full")

    session = connect()
    try:
        target = session.target

        # 硬性校验：板载固件必须与本地构建产物一致
        local = sha256_of(BIN)
        if not skip_check:
            size = os.path.getsize(BIN)
            board = hashlib.sha256(bytes(target.read_memory_block8(0, size))).hexdigest()
            if board != local:
                sys.exit(f"❌ 板载固件与本地构建产物不一致，拒绝采集\n"
                         f"   板载 {board}\n   本地 {local}\n"
                         f"   地址会错位，采出来的数据是假的")
            print(f"✅ 版本一致 {local[:16]}…")

        # 活性校验：内核必须在跑
        a = target.read32(SYSTICK_MS)
        time.sleep(0.5)
        b = target.read32(SYSTICK_MS)
        if target.is_halted() or (b - a) < 300:
            sys.exit(f"❌ 固件没在跑（halted={target.is_halted()}, "
                     f"systick {a}→{b}）")

        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        n = int(seconds / interval) + 1
        print(f"采集 {n} 点 @{interval}s → {out_path}", flush=True)
        t0 = time.time()
        count = 0
        last_tick = None
        stale = 0
        with open(out_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
            w.writeheader()
            f.flush()
            try:
                for i in range(n):
                    delay = (t0 + i * interval) - time.time()
                    if delay > 0:
                        time.sleep(delay)
                    ts = time.time()
                    data = bytes(target.read_memory_block8(SPAN_BEGIN, SPAN_LEN))
                    read_sec = time.time() - ts
                    row = parse_snapshot(data, int((ts - t0) * 1000), read_sec)
                    # 冻结检测：冻结数据的特征是 systick 不动而温度"极稳"
                    if last_tick is not None and row["systick_ms"] == last_tick:
                        stale += 1
                        if stale >= 5:
                            f.flush()
                            sys.exit(f"\n❌ 连续 {stale} 点节拍不变，固件疑似 halt")
                    else:
                        stale = 0
                    last_tick = row["systick_ms"]
                    w.writerow(row)
                    f.flush()
                    count += 1
                    if i % 200 == 0 or i == n - 1:
                        print(f"  [{i:5d}] T={row['temperature']:7.2f} "
                              f"duty={row['heat_duty']:3d} "
                              f"I={row['pid_integral']:7.1f} "
                              f"st=0x{row['state']:04X}", flush=True)
            except KeyboardInterrupt:
                print("\n中断，已采数据已落盘")
        print(f"✅ 已保存 {count} 点")

        # 元数据：没有它，三个月后没人知道这份数据是什么条件下采的
        meta = {
            "file": os.path.basename(out_path),
            "captured_at": datetime.now().isoformat(timespec="seconds"),
            "firmware_sha256": local,
            "firmware_size": os.path.getsize(BIN),
            "git_tag": tag,
            "git_branch": git_branch(),
            "git_describe": git_describe(),
            "probe": PROBE,
            "tip": tip,
            "set_temp": temp,
            "interval_sec": interval,
            "points": count,
            "note": "采集期间不得开菜单/转编码器/拔插手柄",
        }
        meta_path = os.path.splitext(out_path)[0] + ".json"
        with open(meta_path, "w") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)
        print(f"✅ 元数据 → {meta_path}")
        return meta
    finally:
        session.close()


# ---------------------------------------------------------------- 分析

def load_steady(path, setpoint=350):
    """读取 CSV，返回干净稳态段（剔除待机/休眠/断偶）。"""
    rows = list(csv.DictReader(open(path)))
    out = []
    for r in rows:
        st = int(r["state"])
        if st & FLAG_STANDBY or st & FLAG_SLEEP or st & FLAG_TIP_FAULT:
            continue
        if not (st & FLAG_READY):
            continue
        T = float(r["temperature"])
        # 用设定值附近 ±30℃ 界定稳态，排除升温/降温瞬态
        if abs(T - setpoint) > 30:
            continue
        out.append({
            "t": float(r["t_ms"]) / 1000.0,
            "T": T,
            "duty": int(r["heat_duty"]),
            "I": float(r["pid_integral"]),
            "pid_out": int(r["pid_out"]),
        })
    return out


def metrics(ss):
    if len(ss) < 50:
        return None
    T = [x["T"] for x in ss]
    d = [x["duty"] for x in ss]
    I = [x["I"] for x in ss]

    # 二阶差分估高频噪声
    d2 = [T[i + 1] - 2 * T[i] + T[i - 1] for i in range(1, len(T) - 1)]
    noise = statistics.stdev(d2) / 6 ** 0.5 if len(d2) > 2 else 0.0

    return {
        "n": len(ss),
        "duration": ss[-1]["t"] - ss[0]["t"],
        "t_mean": statistics.mean(T),
        "t_min": min(T),
        "t_max": max(T),
        "amplitude": max(T) - min(T),
        "noise": noise,
        "duty_mean": statistics.mean(d),
        "duty_min": min(d),
        "duty_max": max(d),
        "duty_zero_pct": 100.0 * sum(1 for v in d if v == 0) / len(d),
        "I_mean": statistics.mean(I),
        "I_min": min(I),
        "I_max": max(I),
    }


def compare(base_path, new_path, setpoint=350):
    b = metrics(load_steady(base_path, setpoint))
    n = metrics(load_steady(new_path, setpoint))
    if b is None:
        sys.exit(f"基线数据稳态点不足：{base_path}")
    if n is None:
        sys.exit(f"新数据稳态点不足：{new_path}")

    print("=" * 68)
    print(f"{'指标':<18}{'基线':>14}{'改动后':>14}{'变化':>14}")
    print("=" * 68)

    def row(name, bv, nv, unit="", lower_better=True, pct=True):
        delta = nv - bv
        if pct and bv != 0:
            p = 100.0 * delta / abs(bv)
            ds = f"{delta:+9.2f} ({p:+6.1f}%)"
        else:
            ds = f"{delta:+9.2f}"
        print(f"{name:<18}{bv:>13.2f}{nv:>14.2f}{ds:>14}{unit}")

    row("稳态幅度", b["amplitude"], n["amplitude"], "℃")
    row("温度均值", b["t_mean"], n["t_mean"], "℃")
    row("高频噪声σ", b["noise"], n["noise"], "℃")
    row("占空比均值", b["duty_mean"], n["duty_mean"], "格")
    row("占空比0占比", b["duty_zero_pct"], n["duty_zero_pct"], "%")
    row("积分均值", b["I_mean"], n["I_mean"], "")
    row("积分幅度", b["I_max"] - b["I_min"], n["I_max"] - n["I_min"], "")
    print("=" * 68)

    # 判定标准（见 迭代流程规范.md §4）
    print()
    amp_red = (1 - n["amplitude"] / b["amplitude"]) * 100 if b["amplitude"] else 0
    bias_worse = (abs(n["t_mean"] - setpoint) - abs(b["t_mean"] - setpoint))
    print(f"幅度降低: {amp_red:+.1f}%   （判据：≥20% 才算有效）")
    print(f"平均偏差变化: {bias_worse:+.2f}℃  （判据：恶化不得 >1℃）")
    print()
    if amp_red >= 20 and bias_worse <= 1.0:
        print("✅ 判定：通过（幅度显著降低且偏差未恶化）")
        return 0
    elif amp_red >= 20:
        print("⚠ 判定：幅度降低但偏差恶化，需人工权衡")
        return 2
    else:
        print("❌ 判定：未通过（幅度降低不足 20%，视为噪声范围内）")
        return 1


# ---------------------------------------------------------------- 命令

def cmd_check(args):
    if not os.path.exists(BIN):
        sys.exit("缺少构建产物，先 make full")
    session = connect()
    try:
        local = sha256_of(BIN)
        size = os.path.getsize(BIN)
        board = hashlib.sha256(bytes(session.target.read_memory_block8(0, size))).hexdigest()
        print(f"本地: {local}")
        print(f"板载: {board}")
        if local != board:
            print("❌ 不一致：板载固件不是这个构建产物，先烧录")
            return 1
        a = session.target.read32(SYSTICK_MS)
        time.sleep(0.5)
        b = session.target.read32(SYSTICK_MS)
        print(f"✅ 一致；固件在跑（systick +{b - a}）")
        return 0
    finally:
        session.close()


def cmd_baseline(args):
    out = args.out or os.path.join(
        ROOT, "docs/baseline",
        f"{args.tag.replace('-', '')}_{args.tip}_{args.temp}c.csv")
    do_capture(out, args.seconds, args.interval, args.tag, args.tip, args.temp)
    return 0


def cmd_capture(args):
    do_capture(args.out, args.seconds, args.interval,
               args.tag, args.tip, args.temp, args.skip_check)
    return 0


def cmd_compare(args):
    return compare(args.base, args.new, args.setpoint)


def cmd_branch(args):
    """从 master 拉出迭代分支。禁止在 master 上直接改功能代码。"""
    cur = git_branch()
    if args.name.startswith("master"):
        sys.exit("分支名不能是 master")
    r = subprocess.run(["git", "checkout", "-b", args.name, "master"],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"开分支失败：{r.stderr}")
    print(f"✅ 已从 master 拉出 {args.name}（原分支 {cur}）")
    print("   记住：一次只改一件事；核心功能改动先仿真。")
    return 0


def main():
    ap = argparse.ArgumentParser(description="迭代流程一键工具")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("baseline", help="抓基线")
    p.add_argument("--tag", required=True, help="版本标签，如 v9.29-baseline")
    p.add_argument("--tip", default="c210")
    p.add_argument("--temp", default="350")
    p.add_argument("--seconds", type=float, default=150)
    p.add_argument("--interval", type=float, default=0.05,
                   help="加热控制必须 0.05")
    p.add_argument("--out")
    p.set_defaults(func=cmd_baseline)

    p = sub.add_parser("capture", help="抓改动后数据")
    p.add_argument("--out", required=True)
    p.add_argument("--tag", required=True)
    p.add_argument("--tip", default="c210")
    p.add_argument("--temp", default="350")
    p.add_argument("--seconds", type=float, default=150)
    p.add_argument("--interval", type=float, default=0.05)
    p.add_argument("--skip-check", action="store_true")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("check", help="版本自检")
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("compare", help="对比两次采集")
    p.add_argument("base")
    p.add_argument("new")
    p.add_argument("--setpoint", type=float, default=350)
    p.set_defaults(func=cmd_compare)

    p = sub.add_parser("branch", help="开迭代分支")
    p.add_argument("--name", required=True, help="如 iter/v9.30-c210-scale")
    p.set_defaults(func=cmd_branch)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
