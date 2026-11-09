#!/usr/bin/env python3
"""采集 v9.29 实时控制变量，存成 CSV，供积分/稳态分析。

为什么用单条 savemem 而不是多次 read32：
  交接说明 §41.5 踩过两个坑 ——
  坑 B：一条 pyocd 命令串多个 read32 时输出格式不一致，批量解析会错位；
  坑 C：高频 SWD 访问（45 点/秒）两分钟就会挂掉调试口。
  单条 savemem 一次读一大块连续 RAM，一个采样点只发一条命令，两个坑都绕开。

地址必须来自与板载固件哈希一致的 elf，否则读出来的是错位数据
（交接说明 §41.5 坑 A：曾用前馈版 elf 读 v9.29 板子，积分地址错位、
  读出恒为 0，差点误判"积分从未累积"）。

用法：
  python3 tools/capture_baseline.py --seconds 240 --interval 1.0 \
          --out docs/baseline/v929_350c.csv
"""

import argparse
import csv
import hashlib
import os
import struct
import subprocess
import sys
import time

# ---- 全部地址由 arm-none-eabi-nm 从 build/hc32_test.elf 取得（勿手数） ----
STATION = 0x1FFF80B8          # station_t station
PID_INTEGRAL = 0x1FFF81B8     # static float pid_integral
DEADBAND_DUTY = 0x1FFF8154    # static uint8_t deadband_duty
SYSTICK_MS = 0x1FFF8848       # volatile uint32_t systick_ms

# ---- station_t 字段偏移（arm-none-eabi-readelf --debug-dump=info 读出） ----
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

# 一次读完整跨度的 RAM：station 起点 → systick_ms 末尾
SPAN_BEGIN = STATION
SPAN_END = SYSTICK_MS + 4
SPAN_LEN = SPAN_END - SPAN_BEGIN

DEFAULT_PROBE = "3983ABCD"
DEFAULT_TARGET = "hc32f460xe"
EXPECTED_SHA = "e7c9d85c2f69a7547863698104d51f8fd5c821d886d3c543d62df06e9ef8622a"
ELF = "build/hc32_test.elf"
BIN = "build/hc32_test.bin"


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pyocd_savemem(probe, target, addr, length, timeout=120):
    """执行一条 savemem 并把结果读回来。地址/长度都是字节。"""
    tmp = f"/tmp/_snap_{os.getpid()}.bin"
    if os.path.exists(tmp):
        os.remove(tmp)
    r = subprocess.run(
        ["pyocd", "cmd", "-u", probe, "-t", target,
         "-c", f"savemem 0x{addr:X} {length} {tmp}"],
        capture_output=True, text=True, timeout=timeout)
    if not os.path.exists(tmp):
        raise RuntimeError(f"savemem 失败：\n{r.stdout}\n{r.stderr}")
    data = open(tmp, "rb").read()
    os.remove(tmp)
    if len(data) != length:
        raise RuntimeError(f"长度异常：期望 {length}，实际 {len(data)}")
    return data


def check_board_matches_source(probe, target):
    """先确认板载固件与本地构建产物同一个版本。

    为什么必须先验：变量地址是编译期定的，固件版本不同就会挪位置，
    拿错地址采集会得到"恒为 0"的假数据，而且看起来很像真的。
    """
    if not os.path.exists(ELF) or not os.path.exists(BIN):
        sys.exit(f"缺少 {ELF} 或 {BIN}，先 make full")
    local = sha256_of(BIN)
    size = os.path.getsize(BIN)
    board = hashlib.sha256(pyocd_savemem(probe, target, 0x0, size, timeout=300)).hexdigest()
    print(f"板载 SHA-256: {board}")
    print(f"本地 SHA-256: {local}")
    if board != local:
        sys.exit("板载固件与本地构建产物不一致，拒绝采集（地址会错位）")
    note = "" if board == EXPECTED_SHA else "  ⚠ 非文档基线版本"
    print(f"✅ 版本一致，可以采集（{size} B）{note}")


def read_counters(probe, target):
    """把一块连续 RAM 解析成一行采样。"""
    data = pyocd_savemem(probe, target, SPAN_BEGIN, SPAN_LEN)

    def at(addr):
        return addr - SPAN_BEGIN

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
        "systick_ms": u32(SYSTICK_MS),
        "state": u16(STATION + OFF_STATE),
        "tip_channel": u8(STATION + OFF_TIP_CHANNEL),
        "set_temp": u16(STATION + OFF_SET_TEMP),
        "temperature": round(f32(STATION + OFF_TEMPERATURE), 3),
        "temp_int": i16(STATION + OFF_TEMP_INT),
        "pid_out": u16(STATION + OFF_PID_OUT),
        "pid_integral": round(f32(PID_INTEGRAL), 3),
        "heat_duty": u8(STATION + OFF_HEAT_DUTY),
        "deadband_duty": u8(DEADBAND_DUTY),
        "input_voltage": round(f32(STATION + OFF_VIN), 3),
        "heat_current": round(f32(STATION + OFF_CURRENT), 3),
        "cal_raw_at_350": round(f32(STATION + OFF_CAL_RAW), 1),
        "over_current_latch": u8(STATION + OFF_OVER_CURRENT),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", default=DEFAULT_PROBE)
    ap.add_argument("--target", default=DEFAULT_TARGET)
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--interval", type=float, default=1.0,
                    help="采集间隔秒；§41.5 坑 C 建议 ≤1 点/秒")
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip-version-check", action="store_true")
    args = ap.parse_args()

    if args.interval < 0.5:
        print("⚠ 间隔 < 0.5 s 可能挂掉 SWD（§41.5 坑 C）")

    if not args.skip_version_check:
        check_board_matches_source(args.probe, args.target)

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    fields = ["t_ms", "systick_ms", "state", "tip_channel", "set_temp",
              "temperature", "temp_int", "pid_out", "pid_integral",
              "heat_duty", "deadband_duty", "input_voltage", "heat_current",
              "cal_raw_at_350", "over_current_latch"]

    t0 = time.time()
    n = int(args.seconds / args.interval) + 1
    print(f"采集 {n} 点，间隔 {args.interval}s → {args.out}")
    count = 0
    # 逐点落盘：长采集被中断也不丢已采数据
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        f.flush()
        try:
            for i in range(n):
                delay = (t0 + i * args.interval) - time.time()
                if delay > 0:
                    time.sleep(delay)
                try:
                    row = read_counters(args.probe, args.target)
                except Exception as e:
                    print(f"  [{i}] 读取失败，重试一次：{e}")
                    time.sleep(0.3)
                    row = read_counters(args.probe, args.target)
                row["t_ms"] = int((time.time() - t0) * 1000)
                w.writerow(row)
                f.flush()
                count += 1
                if i % 10 == 0 or i == n - 1:
                    print(f"  [{i:4d}] {row['temperature']:7.2f}℃ "
                          f"duty={row['heat_duty']:3d} "
                          f"integ={row['pid_integral']:7.1f} "
                          f"st=0x{row['state']:04X}", flush=True)
        except KeyboardInterrupt:
            print("\n用户中断，已采数据已落盘")

    if count == 0:
        sys.exit("没有采到数据")
    print(f"\n已保存 {count} 点到 {args.out}")


if __name__ == "__main__":
    main()
