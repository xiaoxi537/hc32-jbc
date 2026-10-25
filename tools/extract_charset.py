#!/usr/bin/env python3
"""
从 User/Src/*.c 与 User/Inc/*.h 的字符串字面量里提取界面实际用到的字符，
生成 tools/ui_charset.txt（cjk16 的字符集）。

为什么要单独写一个提取器：手写字符集早晚会漏字，漏了的表现是"界面上某个字
显示成空白"，而且只有切到那个页面才看得见。这里用"剥注释 + 取字面量"的方式
自动提取，再强制并入 ASCII 全区间 —— 菜单里很多数字是运行时 snprintf 出来的
（例如 "320°C"），字面量里看不到，必须保证 0-9 一定在字库里。

用法：
  python3 tools/extract_charset.py            # 打印统计与将被删掉的字符
  python3 tools/extract_charset.py --write     # 写回 tools/ui_charset.txt
"""

import argparse
import glob
import os
import re
import sys

SOURCES = ["User/Src/*.c", "User/Inc/*.h"]
OUT_PATH = "tools/ui_charset.txt"

# 运行时格式化出来的字符（字面量里查不到，必须显式补上）：
#   0-9  —— 菜单数值、参数值、温度全是 snprintf 生成的
#   ° %  —— "%u°C"、参数功率单位 "%"
#   .    —— Ki 显示成 "3.0"
#   °    —— 温度单位
# 注意：**不再**并入全 ASCII。界面用 cjk16 渲染的 ASCII 只有下面这些，
# 和菜单名称里出现过的字母（Kp/Ki/Kd、raw、MHz 等）合起来才 40 个；
# 把 95 个可见 ASCII 全塞进去等于白占 ROM（v9.8 实测省 63 个字形）。
MUST_HAVE = "0123456789°%."


def strip_comments(text):
    """去掉 C 注释与纯编译期消息，避免把它们当成界面文案收进字库

    `_Static_assert(cond, "message")` 的 message 只在编译失败时打印到
    主机终端，永远不上屏；不剔掉会把一堆字母收进 cjk16（实测多收 14 个）。
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"_Static_assert\s*\([^;]*?\)\s*;", " ", text, flags=re.S)
    return text


def string_literals(text):
    """取出所有 "..." 字面量（含转义），本工程的界面文案都在这里面

    要先剔掉两类"看起来像文案、其实永远不上屏"的字符串：
      1. `#include "xxx.h"` 头文件名 —— 提取器的正则会把它们当文案收进来，
         实测多收了 28 个字母，等于白占 ROM；
      2. `%d` / `%s` / `%u` 这类纯格式串 —— 它们只用于 sprintf，
         真正上屏的是格式化后的数字（已由 MUST_HAVE 的 0-9 覆盖）。
    """
    text = re.sub(r'^\s*#\s*include\s*"[^"]*"', " ", text, flags=re.M)
    out = []
    for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', text):
        # 纯格式串（去掉 % 及其修饰符后什么都不剩）不算文案
        if re.fullmatch(r'(?:%[-+ #0-9.*lhjzt]*[a-zA-Z%]|\s)*', lit):
            continue
        out.append(lit)
    return out


# 由 NK57 专用字库渲染的文案，绝不能进 cjk16（否则白占 ROM）。
# 对应 tools/ui_mode11_chars.txt / ui_badge13_chars.txt / ui_readout14_chars.txt
# / ui_preset14_chars.txt / ui_arrow16_chars.txt 这几份字符集。
NK57_ONLY_LITERALS = {
    "MODE", "HOLD", "STBY", "STOP", "SLP", "DET", "HEAT", "IDLE",   # ui11 MODE 值
    "Warning", "T12", "C245", "C210", "----",                        # ui13 型号/告警
    "0.0V", "0.0A", "---°C", "--", "---",                            # ui14 / ui14s
    "%d.%dV", "%d.%dA",                                              # ui14 电压/电流
    "▶",                                                             # arrow16
}


def collect():
    chars = set(MUST_HAVE)
    for pattern in SOURCES:
        for path in sorted(glob.glob(pattern)):
            with open(path, encoding="utf-8") as f:
                text = strip_comments(f.read())
            for literal in string_literals(text):
                # NK57 专用字库的文案跳过：它们不进 cjk16
                if literal in NK57_ONLY_LITERALS:
                    continue
                chars.update(literal)
    return set(c for c in chars if c >= " ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="写回 tools/ui_charset.txt")
    ap.add_argument("--out", default=OUT_PATH)
    args = ap.parse_args()

    chars = collect()
    old = set()
    if os.path.isfile(args.out):
        with open(args.out, encoding="utf-8") as f:
            old = set(c for c in f.read() if c >= " ")

    added = sorted(chars - old)
    removed = sorted(old - chars)
    print("提取到 %d 个字符（原字符集 %d 个）" % (len(chars), len(old)))
    print("新增: %s" % ("".join(added) if added else "无"))
    print("删除: %s" % ("".join(removed) if removed else "无"))

    # 删除前必须确认这些字确实不在任何字面量里；打出来方便人工复核
    if removed:
        print("被删字符所属的旧文案请自行确认已不存在于界面")

    if args.write:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write("".join(sorted(chars)))
        print("已写入 %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
