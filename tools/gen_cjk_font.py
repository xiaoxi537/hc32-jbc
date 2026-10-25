#!/usr/bin/env python3
"""
从 TTF/TTC 生成 SGL 格式的点阵字库（.c 文件）

SGL 字库由三部分组成：
  1. bitmap[]  —— 所有字形按行优先、MSB 优先、bpp 位/像素 打包成连续字节流
  2. table[]   —— 每个字形的度量 {bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y}
                  bitmap_index 是【字节】偏移；adv_w 单位是 1/16 像素
  3. unicode[] —— 区间表，用于把码点映射到 table 下标
                  单区间方案：offset = 基准码点，list = 升序的【相对码点】，
                  tab_offset = 1（下标 0 保留给空白字形）

垂直定位（与 sgl_draw_character 的公式对齐）：
  baseline_row = font_height - base_line
  字形底行（相对整行顶端） = baseline_row + (bbox_bottom - 1)
  公式给出的底行          = font_height - ofs_y - base_line - 1
  两式相等 => ofs_y = -bbox_bottom          （PIL 基线锚定的下边界，开区间）

重要前提：
  本工程 User/Inc/sgl_config.h 必须设置 CONFIG_SGL_FONT_SMALL_TABLE = 0。
  默认值 1 会把 table[].bitmap_index 定义成 uint16_t（最大 65535），
  点阵超过 64 KB 的中文字形索引会被截断，现象是"英文正常、中文碎片"。

用法示例：
  python3 gen_cjk_font.py --charset gb2312-l1 --size 16 --bpp 2 \
      --name cjk16 --out ../User/Src/font_cjk16.c
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow：pip install pillow")


DEFAULT_TTF = "fonts/wqy-microhei.ttc"   # face 1 = WenQuanYi Micro Hei Mono


def gb2312_level1():
    """GB2312 一级汉字区（0xB0A1~0xF7FE 按区排列），解码成字符列表"""
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def gb2312_level2():
    out = []
    for hi in range(0xD8, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


SYMBOLS = "°℃Ω±×÷µμ√∞≤≥≠≈∑∫"


def build_charset(name):
    if name == "ascii":
        return [chr(c) for c in range(0x20, 0x7F)]
    if name == "gb2312-l1":
        return [chr(c) for c in range(0x20, 0x7F)] + list(SYMBOLS) + gb2312_level1()
    if name == "gb2312-l1l2":
        chars = [chr(c) for c in range(0x20, 0x7F)] + list(SYMBOLS)
        chars += gb2312_level1() + gb2312_level2()
        return sorted(set(chars))
    if os.path.isfile(name):
        with open(name, encoding="utf-8") as f:
            text = f.read()
        return sorted(set(ch for ch in text if ch >= " "))
    raise SystemExit(f"未知的字符集: {name}")


def pack_pixels(values, bpp):
    """把整个字形的像素序列按 MSB 优先打包成连续位流

    注意：SGL 的解码是【跨行连续】的
        pixel_index = rel_y * box_w + rel_x
        byte_index  = pixel_index >> (3 - log2(bpp)) ...
    也就是"整块点阵当作一条位流"，**不是每行补齐到字节**。
    若按行补齐，box_w 不是每字节像素数整数倍时，逐行会累积错位，
    显示出来就是"每行横移一点"的剪切感。这里必须连续打包。
    """
    out = bytearray()
    per_byte = 8 // bpp
    mask = (1 << bpp) - 1
    acc = 0
    nbits = 0
    for v in values:
        acc = (acc << bpp) | (v & mask)
        nbits += bpp
        if nbits == 8:
            out.append(acc)
            acc = 0
            nbits = 0
    if nbits > 0:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def is_cjk(ch):
    """判断是不是汉字（CJK 统一表意文字及扩展区）"""
    cp = ord(ch)
    return (0x2E80 <= cp <= 0x9FFF) or (0xF900 <= cp <= 0xFAFF) or (0x20000 <= cp)


def pick_font(ch, primary, fallback, primary_cmap):
    """按字符选字体：汉字与主字体缺字一律走 fallback

    为什么按字符挑而不是整个字符串挑：SGL 一个标签只能有一种字体，
    但菜单数值是 "100°C"、"30分" 这种中英混排。唯一能同时满足
    "英文用 NK57、汉字用微米黑、且只用一个标签"的办法，
    就是在【生成期】把两套字形合并进同一个 sgl_font_t。
    """
    if is_cjk(ch):
        return fallback
    if primary is not None and ord(ch) in primary_cmap:
        return primary
    return fallback


def generate(ttf, face_index, size, bpp, chars, name, out_path,
             latin_ttf=None, latin_face_index=0, metrics_from_latin=False):
    """生成一个 sgl_font_t 点阵字库

    latin_ttf 非空时进入"混排模式"：
      - 汉字（以及主字体缺的字）从 ttf/face_index 取字形
      - 其余 ASCII/符号从 latin_ttf 取字形（本工程就是 NK57）
      - 整个字库只能有一组 font_height/base_line，否则 SGL 的行盒算不对；
        默认取主字体的度量，metrics_from_latin=True 时取 latin 字体的度量。
        哪个能容纳全部字形就用哪个（见 tools/font_metrics.py 的 asc/desc）。
    """
    from fontTools.ttLib import TTFont

    cjk_font = ImageFont.truetype(ttf, size, index=face_index)
    latin_font = (ImageFont.truetype(latin_ttf, size, index=latin_face_index)
                  if latin_ttf else None)
    latin_cmap = (TTFont(latin_ttf, fontNumber=latin_face_index).getBestCmap()
                  if latin_ttf else {})

    # 行盒度量必须全库统一：挑能容纳所有字形的那一组
    metric_font = latin_font if metrics_from_latin else cjk_font
    ascent, descent = metric_font.getmetrics()
    font_height = ascent + descent
    base_line = descent
    max_val = (1 << bpp) - 1

    bitmap = bytearray()
    entries = []          # (cp, bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y)
    # 下标 0 保留：空白字形
    entries.append((None, 0, 0, 0, 0, 0, 0))

    # 统计每个字符实际用了哪套字体，写进文件头方便事后核对
    used = {"latin": 0, "cjk": 0}

    for ch in chars:
        cp = ord(ch)
        font = pick_font(ch, latin_font, cjk_font, latin_cmap)
        used["latin" if font is latin_font else "cjk"] += 1
        try:
            l, t, r, b = font.getbbox(ch, anchor="ls")
            adv_px = font.getlength(ch)
        except Exception:
            continue
        if l is None:
            continue

        box_w = max(0, r - l)
        box_h = max(0, b - t)
        adv_w = int(round(adv_px * 16))
        ofs_x = int(l)
        ofs_y = -int(b)

        bmp_index = len(bitmap)
        if box_w > 0 and box_h > 0:
            img = Image.new("L", (box_w, box_h), 0)
            ImageDraw.Draw(img).text((-l, -t), ch, font=font, anchor="ls", fill=255)
            px = img.load()
            pixels = []
            for y in range(box_h):
                for x in range(box_w):
                    pixels.append(round(px[x, y] * max_val / 255))
            bitmap += pack_pixels(pixels, bpp)
        entries.append((cp, bmp_index, adv_w, box_w, box_h, ofs_x, ofs_y))

    # 码点必须升序，且要重排 entries 以保持一一对应
    head, body = entries[0], entries[1:]
    body.sort(key=lambda e: e[0])
    entries = [head] + body

    base_cp = 0x20
    rel = [e[0] - base_cp for e in entries[1:]]
    assert all(0 <= v <= 0xFFFF for v in rel), "相对码点超出 uint16"

    sym = name
    L = []
    w = L.append
    w("/* 本文件由 tools/gen_cjk_font.py 自动生成，请勿手工修改 */")
    if latin_ttf:
        w("/* 字形来源: 汉字 %s (face %d) / ASCII 与符号 %s (face %d) */"
          % (os.path.basename(ttf), face_index,
             os.path.basename(latin_ttf), latin_face_index))
        w("/*   汉字 %d 字 + 拉丁 %d 字；行盒度量取自 %s */"
          % (used["cjk"], used["latin"],
             os.path.basename(latin_ttf) if metrics_from_latin
             else os.path.basename(ttf)))
    else:
        w("/* 源字体: %s (face %d) */" % (os.path.basename(ttf), face_index))
    w("/* 字号: %dpx  bpp: %d  字形数: %d  行高: %d  基线: %d */"
      % (size, bpp, len(entries) - 1, font_height, base_line))
    w("")
    w('#include <sgl.h>')
    w("")
    w("/* 字形点阵：行优先，MSB 优先，%d 位/像素 */" % bpp)
    w("static const uint8_t %s_bitmap[] = {" % sym)
    for i in range(0, len(bitmap), 16):
        chunk = bitmap[i:i + 16]
        w("    " + ",".join("0x%02X" % v for v in chunk) + ",")
    w("};")
    w("")
    w("/* 度量表：下标 0 为空白字形 */")
    w("static const sgl_font_table_t %s_table[%d] = {" % (sym, len(entries)))
    for idx, (cp, bi, aw, bw, bh, ox, oy) in enumerate(entries):
        note = "/* reserved */" if cp is None else "/* U+%04X */" % cp
        w("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d} %s,"
          % (bi, aw, bw, bh, ox, oy, note))
    w("};")
    w("")
    w("/* 码点列表：相对 %d 的升序偏移 */" % base_cp)
    w("static const uint16_t %s_list[%d] = {" % (sym, len(rel)))
    for i in range(0, len(rel), 12):
        w("    " + ",".join("%d" % v for v in rel[i:i + 12]) + ",")
    w("};")
    w("")
    w("static const sgl_font_unicode_t %s_unicode = {" % sym)
    w("    .offset = %d," % base_cp)
    w("    .tab_offset = 1,")
    w("    .list = %s_list," % sym)
    w("    .len = %d," % len(rel))
    w("};")
    w("")
    w("const sgl_font_t %s = {" % sym)
    w("    .bitmap = %s_bitmap," % sym)
    w("    .table = %s_table," % sym)
    w("    .font_table_size = %d," % len(entries))
    w("    .font_height = %d," % font_height)
    w("    .base_line = %d," % base_line)
    w("    .bpp = %d," % bpp)
    w("    .unicode = &%s_unicode," % sym)
    w("    .unicode_num = 1,")
    w("};")
    w("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(L))

    return dict(font_height=font_height, base_line=base_line,
                glyphs=len(entries) - 1, bitmap=len(bitmap),
                total=len(bitmap) + len(entries) * 12 + len(rel) * 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", default=DEFAULT_TTF)
    ap.add_argument("--face-index", type=int, default=1)
    ap.add_argument("--size", type=int, default=16)
    ap.add_argument("--bpp", type=int, default=2, choices=[1, 2, 4])
    ap.add_argument("--charset", default="gb2312-l1")
    ap.add_argument("--name", default="cjk16")
    ap.add_argument("--out", required=True)
    # 混排模式：ASCII/符号另取一套拉丁字体（本工程用 NK57 Monospace）
    ap.add_argument("--latin-ttf", default=None,
                    help="非空则进入混排模式：汉字用 --ttf，ASCII/符号用这个字体")
    ap.add_argument("--latin-face-index", type=int, default=0)
    ap.add_argument("--metrics-from-latin", action="store_true",
                    help="行盒度量取自拉丁字体（默认取汉字字体）")
    a = ap.parse_args()

    chars = build_charset(a.charset)
    st = generate(a.ttf, a.face_index, a.size, a.bpp, chars, a.name, a.out,
                  latin_ttf=a.latin_ttf, latin_face_index=a.latin_face_index,
                  metrics_from_latin=a.metrics_from_latin)

    print("字体高度 %d，基线 %d" % (st["font_height"], st["base_line"]))
    print("字形数 %d，点阵 %d B，估算 ROM 占用 %.1f KB"
          % (st["glyphs"], st["bitmap"], st["total"] / 1024.0))
    print("已写入 %s" % a.out)


if __name__ == "__main__":
    main()
