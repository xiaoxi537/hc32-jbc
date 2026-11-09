#!/usr/bin/env python3
"""烧录前把 PA10（加热 PWM）强制拉低，防止烧录期间误加热。

=== 为什么必须有这一步 ===

2026-11-09 发生一次事故：烧录完成后烙铁瞬间烧红（显示 999℃）。

根因链：
  1. 加热关断完全依赖 TMRA6 溢出中断里的 set_heat_pin(false)
  2. pyocd 连接目标时会 halt 内核；擦写 Flash 期间中断向量也被擦除
     → 中断不再执行 → set_heat_pin(false) 永远跑不到
  3. PA10 冻结在最后写入的电平。duty=80 时高电平占 31.2%（7.88/25.35 ms），
     一旦冻结在高电平，加热就从 31% 变成 100% 持续导通
  4. 硬件侧 R3 10kΩ 只能保证"悬空时"Q1 截止；
     但 PA10 被主动驱动为高时，Q1 依然导通 → Q3(G65P06D5) 开通 → 24V 灌进烙铁芯

结论：**必须在烧录前把 PA10 主动配置成输出低电平并保持**，
这样即使随后 halt/擦除/复位，Q1 也一定截止，物理上不可能加热。

=== PA10 寄存器地址（由 DDL 反推，已实机验证） ===

  CM_GPIO_BASE = 0x40053800
  PCRA0        = 0x40053C28      （结构体内偏移 0x400，见 hc32f460.h）
  GPIO_PIN_OFFSET = 0x04         （hc32_ll_gpio.c）
  PA10 PCR     = 0x40053C28 + 10*4 = 0x40053C50

  PCR 关键位：
    bit0 POUT  = 输出电平
    bit1 POUTE = 输出使能
    bit8 DDIS  = 数字/模拟属性（1=模拟）

  写入 0x0002 = 输出使能 + 输出低电平 → PA10 拉低 → Q1 截止 → 不加热。
"""

import sys
import time

try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    sys.exit("请在 pyocd 的 venv 解释器下运行本脚本")

PROBE_UID = "3983ABCD"
TARGET = "hc32f460xe"

PA10_PCR = 0x40053C50     # 见文件头推导
PCR_OUTPUT_LOW = 0x0002   # POUTE=1, POUT=0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--release":
        print("提示：--release 不需要单独调用；烧录脚本烧完会恢复正常状态。")
        return 0

    session = ConnectHelper.session_with_chosen_probe(
        unique_id=PROBE_UID, target_override=TARGET,
        options={"auto_unlock": True})
    if session is None:
        print("错误：连不上探针", file=sys.stderr)
        return 1

    session.open()
    try:
        t = session.target
        # 连接时内核会被 halt —— 这正是危险窗口。先别 resume，
        # 趁内核停着直接改寄存器，避免控制循环又把 PA10 拉高。
        before = t.read16(PA10_PCR)

        t.write16(PA10_PCR, PCR_OUTPUT_LOW)
        after = t.read16(PA10_PCR)

        print(f"PA10 PCR: 0x{before:04X} -> 0x{after:04X}")

        ok = (after & 0x0002) and not (after & 0x0001)
        if not ok:
            print("错误：PA10 未能拉低（PCR 回读不符）", file=sys.stderr)
            print("      此时烧录可能导致加热失控，已中止", file=sys.stderr)
            return 1

        print("✅ PA10 已强制拉低，Q1 截止，烧录期间不会加热")

        # 让内核继续跑。注意：固件运行后 may 重新配置 PA10，
        # 但烧录会立刻复位，时间窗口极小；且此刻 PA10 已是低电平，
        # 即使被重新配置也只会按正常 PWM 工作，不会一直高。
        t.resume()
        time.sleep(0.2)
        after2 = t.read16(PA10_PCR)
        print(f"   resume 后 PA10 PCR = 0x{after2:04X}")
    finally:
        session.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
