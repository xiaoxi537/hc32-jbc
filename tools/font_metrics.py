#!/usr/bin/env python3
"""
离线打印字库度量，用来手算 app_ui.c 里的静态排版常量。

为什么不把这个逻辑写进固件：
  固件里只需要几个整数（前进宽度、行高、Y 偏移），完全没有必要为了
  排版在 MCU 上跑一遍字符串宽度测量 —— 既费 CPU 又要在运行时反复量。
  这里把数字一次算清楚，人肉抄进 app_ui.c 的宏，运行时就只剩加法。

对齐公式（与 sgl_draw_character 一致）：
  字形墨迹底边（相对整行行盒顶端） = font_height + bbox_bottom - base_line
  两个不同字号要"底边平齐"，单位字体的行盒必须比数字字体下移：
      dy = (fh_digit + b_digit - bl_digit) - (fh_unit + b_unit - bl_unit)

用法：
  python3 tools/font_metrics.py
"""

import os
import sys

try:
    from PIL import ImageFont
except ImportError:
    sys.exit("需要 Pillow：pip install pillow")

# 与 tools/gen_fonts.sh 里的字号严格一致；改字号必须两处一起改
NK57 = "fonts/nk57-monospace.cdrg-bold.otf"
NK57_SB = "fonts/nk57-monospace.cdsb-regular.otf"
WQY = "fonts/wqy-microhei.ttc"
WQY_MONO_FACE = 1          # WenQuanYi Micro Hei Mono，等宽

# 字体名 -> (汉字字体, face, 字号, 拉丁字体/None, 是否用拉丁度量)
# cjk16 是混排档：汉字走微米黑 Mono、ASCII 与符号走 NK57，
# 行盒度量统一取 NK57（见 tools/gen_fonts.sh 的说明）。
FONTS = [
    ("num90", NK57, 0, 90, None, False),
    ("num32", NK57, 0, 32, None, False),
    ("num28", NK57, 0, 28, None, False),
    ("num16", NK57, 0, 16, None, False),
    ("ui14", NK57, 0, 16, None, False),
    ("ui14s", NK57_SB, 0, 16, None, False),
    ("ui13", NK57, 0, 16, None, False),
    ("ui11", NK57, 0, 16, None, False),
    ("ui11s", NK57_SB, 0, 16, None, False),
    ("arrow16", NK57, 0, 16, None, False),
    ("cjk16", WQY, WQY_MONO_FACE, 16, NK57, True),
    ("cjk52", WQY, WQY_MONO_FACE, 52, None, False),
]


def probe(path, face, size, glyphs, latin=None, metrics_from_latin=False):
    """返回 {字形: (bbox, 前进宽度像素)} 以及 (font_height, base_line)

    与 tools/gen_cjk_font.py 的混排规则保持一致：汉字和主字体缺的字
    走主字体，其余 ASCII/符号走 latin 字体。
    """
    from fontTools.ttLib import TTFont

    cjk_font = ImageFont.truetype(path, size, index=face)
    latin_font = ImageFont.truetype(latin, size, index=0) if latin else None
    latin_cmap = TTFont(latin).getBestCmap() if latin else {}
    metric_font = latin_font if metrics_from_latin else cjk_font
    ascent, descent = metric_font.getmetrics()
    font_height = ascent + descent
    base_line = descent
    out = {}
    for ch in glyphs:
        is_cjk = 0x2E80 <= ord(ch) <= 0x9FFF
        if is_cjk:
            font, tag = cjk_font, "cjk"
        elif latin_font is not None and ord(ch) in latin_cmap:
            font, tag = latin_font, "nk57"
        else:
            font, tag = cjk_font, "cjk"
        box = font.getbbox(ch, anchor="ls")
        # adv_w 存的是 1/16 像素，SGL 画字时用 (adv_w + 8) >> 4 还原成整数像素
        adv_px = (int(round(font.getlength(ch) * 16)) + 8) >> 4
        out[ch] = (box, adv_px, tag)
    return out, font_height, base_line


def show(name, path, face, size, glyphs, latin=None, metrics_from_latin=False):
    metrics, font_height, base_line = probe(path, face, size, glyphs, latin,
                                            metrics_from_latin)
    src = "混排: 汉字 %s / 拉丁 %s" % (
        "微米黑 Mono", os.path.basename(latin)) if latin else ""
    print("%-8s %-28s face=%d %2dpx  font_height=%-3d base_line=%-2d  %s"
          % (name, os.path.basename(path), face, size, font_height, base_line, src))
    for ch, (box, adv, tag) in metrics.items():
        left, top, right, bottom = box
        print("         %r bbox=(%d,%d,%d,%d)  bbox_bottom=%-3d  box=%dx%d  adv=%-3d [%s]"
              % (ch, left, top, right, bottom, bottom, right - left, bottom - top,
                 adv, tag))
    return metrics, font_height, base_line



def main():
    results = {}
    for name, path, face, size, latin, from_latin in FONTS:
        # 中文档额外探一个 ASCII，用来确认混排后 ASCII 确实走的是 NK57
        glyphs = {"num32": "0°C", "num16": "0°C",
                  "num90": "0°C", "num28": "0°C",
                  "cjk16": "0设主", "cjk52": "0设主"}.get(name, "0M设")
        results[name] = show(name, path, face, size, glyphs, latin, from_latin)
        print()

    # 设定框：num32 数字 + num16 单位，底边平齐所需下移量
    d_metrics, d_fh, d_bl = results["num32"]
    u_metrics, u_fh, u_bl = results["num16"]
    b_d = d_metrics["0"][0][3]
    b_u = u_metrics["°"][0][3]
    dy = (d_fh + b_d - d_bl) - (u_fh + b_u - u_bl)
    print("设定框 SET_UNIT_Y_OFFSET = (%d + %d - %d) - (%d + %d - %d) = %d"
          % (d_fh, b_d, d_bl, u_fh, b_u, u_bl, dy))
    print("num32 每字前进 %d px，num16 每字前进 %d px"
          % (d_metrics["0"][1], u_metrics["°"][1]))
    print()

    # 主界面温度：num90 数字 + num28 单位
    t_metrics, t_fh, t_bl = results["num90"]
    s_metrics, s_fh, s_bl = results["num28"]
    b_t = t_metrics["0"][0][3]
    b_s = s_metrics["°"][0][3]
    dy2 = (t_fh + b_t - t_bl) - (s_fh + b_s - s_bl)
    print("温度 MAIN_TEMP_UNIT_DY = (%d + %d - %d) - (%d + %d - %d) = %d"
          % (t_fh, b_t, t_bl, s_fh, b_s, s_bl, dy2))
    print("num90 每字前进 %d px，num28 每字前进 %d px"
          % (t_metrics["0"][1], s_metrics["°"][1]))
    print()

    # 居中用的静态 X 表：数字位数 1..3
    def centres(base_x, box_w, digit_adv, unit_adv, gap, shift=0):
        rows = []
        for digits in (1, 2, 3):
            width = digits * digit_adv + gap + unit_adv
            start = base_x + (box_w - width) // 2 + shift
            rows.append((digits, start, start + digits * digit_adv + gap))
        return rows

    print("设定框（框 x=%d 宽 98，间隔 4）: 位数 -> (数字X, 单位X)" % 214)
    for digits, dx, ux in centres(214, 98, d_metrics["0"][1], u_metrics["°"][1] * 2, 4):
        print("    %d 位: 数字 x=%d, 单位 x=%d" % (digits, dx, ux))
    print()

    print("温度组（面板 x=8 宽 198，居中偏移 -10，间隔 4）: 位数 -> (数字X, 单位X)")
    for digits, dx, ux in centres(8, 198, t_metrics["0"][1], s_metrics["°"][1] * 2, 4, -10):
        print("    %d 位: 数字 x=%d, 单位 x=%d" % (digits, dx, ux))
    print()

    # 菜单文字：cjk16 每字前进宽度（中文与 ASCII 不同，说明它不是纯等宽）
    c_metrics, c_fh, c_bl = results["cjk16"]
    print("cjk16 中文前进 %d px / ASCII 前进 %d px，行高 %d"
          % (c_metrics["设"][1], c_metrics["0"][1], c_fh))
    return 0


if __name__ == "__main__":
    sys.exit(main())
