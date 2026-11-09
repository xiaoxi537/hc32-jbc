#!/usr/bin/env python3
"""采集焊台实时控制变量，存 CSV，用于积分泄放与稳态分析。

必须用 pyocd 自带 venv 的解释器运行（它装在 pipx 独立环境里）：
  /home/bunny233/.local/share/pipx/venvs/pyocd/bin/python \
      tools/capture_baseline.py --seconds 600 --interval 0.3 --out docs/baseline/x.csv

为什么复用会话而不是每点起一个 pyocd 进程：
  CLI 方式单点 0.635 s，其中绝大部分是解释器+进程启动开销。
  复用 DebugProbe 会话后单点约 0.1 s，才能达到原基线 0.35 s/点 以下的
  采样率 —— 稳态 7.2℃ 的摆动和 +2.95℃ 的过冲峰都只有秒级宽度，
  采样慢了会直接混叠掉，测出来的"幅度"是假的。

为什么一次读一整块连续 RAM：
  交接说明 §41.5 坑 B（多地址批量 read32 输出错位）与
  坑 C（高频 SWD 访问挂掉调试口）都出在"多次零散读"上。
  一次读一块连续内存，命令数等于点数，风险最低。

地址必须来自与板载固件哈希一致的 elf（§41.5 坑 A）。
"""

import argparse
import csv
import hashlib
import os
import struct
import sys
import time

try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    sys.exit("请在 pyocd 的 venv 解释器下运行：\n"
             "  /home/bunny233/.local/share/pipx/venvs/pyocd/bin/python " + __file__)

# ---- 地址由 arm-none-eabi-nm 从 build/hc32_test.elf 取得 ----
STATION = 0x1FFF80B8
PID_INTEGRAL = 0x1FFF81B8
DEADBAND_DUTY = 0x1FFF8154
SYSTICK_MS = 0x1FFF8848

# ---- station_t 字段偏移（readelf --debug-dump=info 读出） ----
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

# 一次读完整跨度：station 起点 → systick_ms 末尾
SPAN_BEGIN = STATION
SPAN_LEN = (SYSTICK_MS + 4) - STATION

PROBE_UID = "3983ABCD"
TARGET = "hc32f460xe"
EXPECTED_SHA = "e7c9d85c2f69a7547863698104d51f8fd5c821d886d3c543d62df06e9ef8622a"
BIN = "build/hc32_test.bin"

FIELDS = ["t_ms", "probe_ms", "systick_ms", "state", "tip_channel",
          "set_temp", "temperature", "temp_int", "pid_out", "pid_integral",
          "heat_duty", "deadband_duty", "input_voltage", "heat_current",
          "cal_raw_at_350", "over_current_latch", "read_sec"]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def connect():
    session = ConnectHelper.session_with_chosen_probe(
        unique_id=PROBE_UID,
        target_override=TARGET,
        options={"auto_unlock": True},
    )
    if session is None:
        sys.exit("连不上探针，检查连线")
    session.open()
    # pyocd 连接后默认把内核停在 halt。不 resume 的话读到的全是冻结值：
    # systick_ms 不变、温度不变，数据看上去"很稳"，实际固件根本没在跑。
    # 这个坑曾让 434 个采样点全部作废。
    session.target.resume()
    return session


def check_tick_alive(session):
    """确认固件真的在运行。

    为什么要验：halt 状态下所有变量都是静止的，采样出来会呈现
    "零噪声完美稳定"的假象，比真实数据更"好看"，最容易骗过分析。
    """
    t = session.target
    a = t.read32(SYSTICK_MS)
    time.sleep(0.5)
    b = t.read32(SYSTICK_MS)
    if t.is_halted() or (b - a) < 300:
        sys.exit(f"固件没有在跑（halted={t.is_halted()}，"
                 f"systick {a}→{b}）。先 resume 再采，否则数据全废。")
    print(f"✅ 固件在运行（systick 0.5 s 走了 {b - a}）")


def check_version(session):
    """确认板载固件与本地构建产物同版本。

    地址是编译期定的，版本不同就会挪位置；拿错地址会读出"恒为 0"的
    假数据，而且看起来很像真的（§41.5 坑 A 差点误判积分从未累积）。
    """
    if not os.path.exists(BIN):
        sys.exit(f"缺少 {BIN}，先 make full")
    local = sha256_of(BIN)
    size = os.path.getsize(BIN)
    raw = session.target.read_memory_block8(0x0, size)
    board = hashlib.sha256(bytes(raw)).hexdigest()
    print(f"板载 SHA-256: {board}")
    print(f"本地 SHA-256: {local}")
    if board != local:
        sys.exit("板载固件与本地构建产物不一致，拒绝采集（地址会错位）")
    print(f"✅ 版本一致（{size} B）" +
          ("" if board == EXPECTED_SHA else "  ⚠ 非文档基线版本"))


def parse(data, t_ms, probe_ms, read_sec):
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
        "probe_ms": probe_ms,
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
        "read_sec": round(read_sec, 4),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--interval", type=float, default=0.3)
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip-version-check", action="store_true")
    args = ap.parse_args()

    session = connect()
    try:
        if not args.skip_version_check:
            check_version(session)
        check_tick_alive(session)
        target = session.target

        out_dir = os.path.dirname(args.out)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)

        n = int(args.seconds / args.interval) + 1
        print(f"采集 {n} 点，间隔 {args.interval}s → {args.out}", flush=True)
        t0 = time.time()
        count = 0
        last_tick = None
        stale = 0
        # 逐点落盘：长采集被打断也不丢已采数据
        with open(args.out, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
            w.writeheader()
            f.flush()
            try:
                for i in range(n):
                    delay = (t0 + i * args.interval) - time.time()
                    if delay > 0:
                        time.sleep(delay)
                    ts = time.time()
                    raw = target.read_memory_block8(SPAN_BEGIN, SPAN_LEN)
                    data = bytes(raw)
                    read_sec = time.time() - ts
                    row = parse(data, int((ts - t0) * 1000),
                                int((ts - t0) * 1000), read_sec)

                    # 每点校验节拍在推进。冻结数据的特征就是 systick 不动，
                    # 而温度还会显得"极其稳定"，不校验就会当成好数据。
                    if last_tick is not None and row["systick_ms"] == last_tick:
                        stale += 1
                        if stale >= 5:
                            f.flush()
                            sys.exit(f"\n❌ 连续 {stale} 点节拍不变"
                                     f"(systick={row['systick_ms']})，"
                                     f"固件疑似被 halt，采集中止")
                    else:
                        stale = 0
                    last_tick = row["systick_ms"]

                    w.writerow(row)
                    f.flush()
                    count += 1
                    if i % 20 == 0 or i == n - 1:
                        print(f"  [{i:5d}] T={row['temperature']:7.2f} "
                              f"duty={row['heat_duty']:3d} "
                              f"I={row['pid_integral']:7.1f} "
                              f"st=0x{row['state']:04X} "
                              f"({read_sec*1000:.0f}ms)", flush=True)
            except KeyboardInterrupt:
                print("\n用户中断，已采数据已落盘", flush=True)
        if count == 0:
            sys.exit("没有采到数据")
        print(f"\n已保存 {count} 点到 {args.out}", flush=True)
    finally:
        session.close()


if __name__ == "__main__":
    main()
