#!/usr/bin/env python3
"""
HC32F460 时钟初始化 —— 静态（不上机）最小验证

在没有 J-Link 的情况下，通过反汇编 init_clock 检查生成的机器码是否真的按
原机 JBC.bin 的顺序、写入了正确的寄存器与数值。

用法:
    cd hc32f460 && make -j4 && python3 tools/verify_clk.py

不变量（全部来自 docs/时钟配置实机验证.md 的实机结论）:
    CMU_PLLCFGR = 0x11103101
    CMU_SCFGR   = 0x00112210   (掩码 0x07777777)
    CMU_CKSWR   = 5
    EFM_FRMC.FLWT = 5          (EFM_WaitCycle 参数 0x50)
    SRAM_WTCR   = 0x11001111   (0xF->1wait, 0x4->0wait)
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "hc32_test.elf")
OBJDUMP = "arm-none-eabi-objdump"

# 期望在 init_clock 里出现的字面量
EXPECTED_LITERALS = {
    0x11103101: "CMU_PLLCFGR",
    0x00112210: "CMU_SCFGR 分频域",
    0x07777777: "SCFGR 分频掩码",
}

# 期望的调用顺序（关键子序列）
EXPECTED_CALL_ORDER = [
    "SRAM_SetWaitCycle",
    "EFM_SetWaitCycle",
    "CLK_XtalStructInit",
    "CLK_XtalInit",
    "CLK_SetClockDiv",
    "CLK_PLLStructInit",
    "CLK_PLLInit",
    "CLK_SetSysClockSrc",
]

# 关键调用的立即数参数: 符号 -> [(寄存器, 值), ...]（须至少匹配其中一组）
EXPECTED_ARGS = {
    "EFM_SetWaitCycle": [{0: 0x50}],
    "CLK_SetSysClockSrc": [{0: 0x05}],
    "SRAM_SetWaitCycle": [{0: 0x0F, 1: 1, 2: 1}, {0: 0x04, 1: 0, 2: 0}],
}

ok = 0
bad = 0


def check(cond, msg, detail=""):
    global ok, bad
    if cond:
        ok += 1
        print("  [PASS] %s" % msg)
    else:
        bad += 1
        print("  [FAIL] %s %s" % (msg, detail))


def main():
    if not os.path.isfile(ELF):
        print("找不到 %s，请先 make" % ELF)
        return 2

    out = subprocess.run(
        [OBJDUMP, "-d", "-M", "force-thumb", "--disassemble=init_clock", ELF],
        capture_output=True, text=True,
    )
    if out.returncode != 0 or "init_clock" not in out.stdout:
        print("反汇编失败或被 --gc-sections 移除")
        print(out.stderr[:400])
        return 2
    text = out.stdout

    print("=" * 72)
    print("HC32F460 时钟初始化 —— 静态验证 (init_clock)")
    print("=" * 72)

    # --- 1. 常量 ---
    lits = {int(m, 16) for m in re.findall(r"\.word\s+0x([0-9a-fA-F]{8})", text)}
    print("\n[1] 字面量池")
    for val, name in EXPECTED_LITERALS.items():
        check(val in lits, "0x%08X (%s)" % (val, name),
              "-> 实际池: %s" % ", ".join("0x%08X" % v for v in sorted(lits)))

    # --- 2. 调用顺序 ---
    calls = re.findall(r"bl\s+\w+\s+<([A-Za-z_][\w]*)>", text)
    print("\n[2] 调用顺序")
    print("     实际: %s" % " -> ".join(calls))
    idx = -1
    order_ok = True
    for sym in EXPECTED_CALL_ORDER:
        try:
            i = calls.index(sym, idx + 1)
        except ValueError:
            order_ok = False
            break
        idx = i
    check(order_ok, "关键调用按 RM §7.4.1 顺序出现（先等待周期，后提频）")

    # --- 3. 关键参数 ---
    print("\n[3] 关键调用参数")
    # 把每条指令切开，便于向前收集立即数
    lines = [ln.strip() for ln in text.splitlines()]
    bl_sites = [(i, m.group(1)) for i, ln in enumerate(lines)
                for m in [re.search(r"bl\s+\w+\s+<([A-Za-z_]\w*)>", ln)] if m]

    for sym, arg_sets in EXPECTED_ARGS.items():
        sites = [i for i, s_ in bl_sites if s_ == sym]
        for want in arg_sets:
            found = False
            for i in sites:
                regs = {}
                # 目标函数可能把立即数放在 r8..r12 里再 mov 回 r0，
                # 所以窗口放宽到 80 行，并接受 mov / movs / mov.w 三种写法。
                for j in range(max(0, i - 80), i):
                    ln = lines[j]
                    m = re.search(r"movs?(\.w)?\s+r(\d+),\s+#(\d+)", ln)
                    if m:
                        regs[int(m.group(2))] = int(m.group(3))
                        continue
                    m = re.search(r"movs?(\.w)?\s+r(\d+),\s+r(\d+)", ln)
                    if m and regs.get(int(m.group(3))) is not None:
                        regs[int(m.group(2))] = regs.get(int(m.group(3)))
                if all(regs.get(r) == v for r, v in want.items()):
                    found = True
                    break
            check(found, "%s(%s)" % (sym, ", ".join(
                "r%d=0x%X" % (r, v) for r, v in sorted(want.items()))))

    # --- 4. 写保护解锁/回锁 ---
    print("\n[4] 写保护序列")
    check("0xa500" in text.lower() or "#42240" in text, "PWC_FPRC 解锁键 0xA5xx")
    check(re.search(r"#291\b", text) is not None and re.search(r"#12816\b", text) is not None,
          "EFM_FAPRT 解锁键 0x0123 / 0x3210")
    # 编译器可能用 r1/r2/r3 任意一个装立即数，这里不绑定寄存器号
    check(re.search(r"movs?(\.w)?\s+r\d+,\s+#119", text) is not None and
          re.search(r"movs?(\.w)?\s+r\d+,\s+#118", text) is not None,
          "SRAM WTPR/CKPR 解锁 0x77 / 回锁 0x76")

    # --- 5. 源码级编译期断言 ---
    print("\n[5] 编译期断言")
    src = os.path.join(ROOT, "User", "Src", "bsp_clk.c")
    c = open(src, encoding="utf-8", errors="ignore").read()
    check(c.count("_Static_assert") == 2, "bsp_clk.c 只保留 %d 条 _Static_assert（全工程共 3 条）"
          % c.count("_Static_assert"))

    # --- 6. 裸机时基（替代 FreeRTOS tick） ---
    print("\n[6] 裸机 SysTick 时基")
    out2 = subprocess.run(
        [OBJDUMP, "-d", "-M", "force-thumb", "--disassemble=init_tick", ELF],
        capture_output=True, text=True).stdout
    check("init_tick" in out2, "init_tick 存在")
    check("udiv" in out2, "LOAD = SystemCoreClock/freq 运行时计算")
    check(re.search(r"\[r\d+, #20\]", out2) is not None, "写 SysTick LOAD (0xE000E014)")
    check(re.search(r"\[r\d+, #16\]", out2) is not None and
          re.search(r"movs?(\.w)?\s+r\d+,\s+#7\b", out2) is not None,
          "写 SysTick CTRL = 0x07 (ENABLE|TICKINT|CLKSOURCE=HCLK)")

    print("\n" + "=" * 72)
    print("PASS=%d  FAIL=%d" % (ok, bad))
    print("=" * 72)
    if bad == 0:
        print("\n静态验证全部通过。")
        print("注意：这证明“生成的代码写的是对的”，不等于“芯片上真的跑通了”。")
        print("上机后还需要读寄存器确认：init_clock() 内部已回读，失败会停在 main 的死循环。")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
