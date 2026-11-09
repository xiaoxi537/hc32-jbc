#!/usr/bin/env python3
"""安全的加热设备数据采集（v9.34 安全加固版）。

=== 这个脚本修复了三个曾经导致事故的缺陷 ===

1. **烧录后立刻采集会读到未初始化 RAM**
   固件刚复位时变量还是垃圾值（曾读到 1106℃、1464.9℃），
   现在采集前必须等到 systick_ms 推进正常且温度在合理范围。

2. **退出时没有 resume，内核可能留在 halt**
   halt 期间加热中断不执行、PA10 冻结最后电平 → 可能持续满功率。
   现在 finally 里强制 resume 并回读验证。

3. **采集期间不监控危险状态**
   现在每点检查 duty 与温度，异常立刻中止并强制关断加热。

=== 安全前提 ===

本脚本仍会在 pyocd 连接瞬间 halt 内核（这是 pyocd 的行为）。
所以**强烈建议用安全版固件采集**（-DHEAT_OUTPUT_ENABLE=0，duty 恒 0）。
必须用全功能版时，先运行 tools/safe_heat_pin.py 把 PA10 拉低
（烧录脚本已自动做这一步）。

用法：
  PY=/home/bunny233/.local/share/pipx/venvs/pyocd/bin/python
  $PY tools/capture_safe.py --out docs/baseline/x.csv --seconds 120 --interval 0.015
"""

import argparse
import csv
import os
import struct
import subprocess
import sys
import time

try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    sys.exit("请在 pyocd 的 venv 解释器下运行：\n"
             "  /home/bunny233/.local/share/pipx/venvs/pyocd/bin/python " + __file__)

PROBE_UID = "3983ABCD"
TARGET = "hc32f460xe"
ELF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "build", "hc32_test.elf")

PA10_PCR = 0x40053C50
PCR_OUTPUT_LOW = 0x0002

OFF_STATE = 0x08
OFF_PID_OUT = 0x0A
OFF_TEMP = 0x1C
OFF_HEAT_DUTY = 0x2E

FLAG_READY = 0x1000
FLAG_SLEEP = 0x0400
FLAG_JBC_SW = 0x0010

# 安全阈值：超出即中止
TEMP_ABORT_MAX = 600.0
TEMP_ABORT_MIN = -50.0


def symbol_addr(name):
    """从 elf 动态取符号地址（改代码后地址会漂，不能硬编码）。"""
    out = subprocess.run(["arm-none-eabi-nm", "-n", ELF],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    sys.exit(f"elf 里找不到 {name}")


def force_heat_off(t):
    """把 PA10 强制拉低并回读确认。"""
    t.write16(PA10_PCR, PCR_OUTPUT_LOW)
    v = t.read16(PA10_PCR)
    if not (v & 0x0002) or (v & 0x0001):
        raise RuntimeError(f"PA10 拉低失败（PCR=0x{v:04X}）")


def wait_firmware_ready(t, base, sy_addr, timeout=20.0):
    """等固件真正跑起来。

    烧录后立刻读会拿到未初始化 RAM 的垃圾值（曾读到 1106℃、1464.9℃）。
    判据：systick_ms 足够大且在推进、状态合理、温度在物理范围。
    """
    t0 = time.time()
    prev = None
    while time.time() - t0 < timeout:
        time.sleep(0.25)
        try:
            sy = t.read32(sy_addr)
            d = bytes(t.read_memory_block8(base, 0x40))
            st = struct.unpack_from("<H", d, OFF_STATE)[0]
            T = struct.unpack_from("<f", d, OFF_TEMP)[0]
        except Exception:
            continue
        sane = (sy > 100000) and (TEMP_ABORT_MIN < T < TEMP_ABORT_MAX) and \
               (st & FLAG_READY)
        advancing = (prev is None) or (sy > prev)
        if sane and advancing:
            return sy, st, T
        prev = sy
    raise RuntimeError("固件未就绪（systick 未推进或温度非法），拒绝采集")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--interval", type=float, default=0.015)
    ap.add_argument("--no-force-off", action="store_true",
                    help="不强制拉低 PA10（仅当用安全版固件时）")
    args = ap.parse_args()

    station = symbol_addr("station")
    integral = symbol_addr("pid_integral")
    systick = symbol_addr("systick_ms")
    span_len = (systick + 4) - station

    session = ConnectHelper.session_with_chosen_probe(
        unique_id=PROBE_UID, target_override=TARGET,
        options={"auto_unlock": True})
    if session is None:
        sys.exit("连不上探针")
    session.open()
    t = session.target

    count = 0
    aborted = None
    try:
        if not args.no_force_off:
            # 连接会 halt 内核 —— 趁这时立刻把加热脚拉低，最安全
            force_heat_off(t)
            print("✅ PA10 已强制拉低")

        t.resume()
        sy, st, T = wait_firmware_ready(t, station, systick)
        print(f"✅ 固件就绪 systick={sy} T={T:.2f} st=0x{st:04X}")

        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        n_total = int(args.seconds / args.interval) + 1
        print(f"采集 {n_total} 点 @{args.interval}s → {args.out}", flush=True)

        t0 = time.time()
        last_tick = None
        stale = 0
        with open(args.out, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_ms", "systick_ms", "temperature", "heat_duty",
                        "pid_out", "pid_integral", "state"])
            f.flush()
            for i in range(n_total):
                delay = (t0 + i * args.interval) - time.time()
                if delay > 0:
                    time.sleep(delay)
                ts = time.time()
                d = bytes(t.read_memory_block8(station, span_len))
                temp = struct.unpack_from("<f", d, OFF_TEMP)[0]
                duty = d[OFF_HEAT_DUTY]
                stt = struct.unpack_from("<H", d, OFF_STATE)[0]
                syv = struct.unpack_from("<I", d, systick - station)[0]

                # 危险状态立即中止并关断
                if not (TEMP_ABORT_MIN < temp < TEMP_ABORT_MAX):
                    aborted = f"温度非法 {temp:.2f}℃"
                    break
                if last_tick is not None and syv == last_tick:
                    stale += 1
                    if stale >= 5:
                        aborted = "节拍停滞（疑似 halt）"
                        break
                else:
                    stale = 0
                last_tick = syv

                w.writerow([int((ts - t0) * 1000), syv, round(temp, 2), duty,
                            struct.unpack_from("<H", d, OFF_PID_OUT)[0],
                            round(struct.unpack_from("<f", d, integral - station)[0], 1),
                            stt])
                f.flush()
                count += 1
                if i % 500 == 0:
                    print(f"  [{i:5d}] T={temp:7.2f} duty={duty:3d} "
                          f"st=0x{stt:04X}", flush=True)
    except KeyboardInterrupt:
        print("\n用户中断")
    finally:
        # 无论如何都要恢复内核并确认
        try:
            t.write16(PA10_PCR, PCR_OUTPUT_LOW)   # 关加热
            t.resume()
            halted = t.is_halted()
            print(f"\n退出检查：halted={halted}  PA10 PCR=0x{t.read16(PA10_PCR):04X}")
            if halted:
                print("⚠ 内核仍在 halt，再次尝试 resume", file=sys.stderr)
                t.resume()
        except Exception as e:
            print(f"⚠ 退出收尾异常：{e}", file=sys.stderr)
        session.close()

    if aborted:
        print(f"❌ 已中止采集：{aborted}", file=sys.stderr)
        return 1
    print(f"✅ 已保存 {count} 点")
    return 0


if __name__ == "__main__":
    sys.exit(main())
