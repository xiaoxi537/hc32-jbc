#!/usr/bin/env bash
#
# 重新生成全部界面字库（tools/gen_cjk_font.py 的批处理封装）
#
# 为什么要有这个脚本：字库文件是生成物，改字号/字重/文案后必须整套重跑。
# 逐个手敲 12 条命令容易漏掉某一个，漏了就会出现"英文正常、某个数字变成空白"。
#
# 字重约定（对应 docs/ui/sgl_new_full_UI.html 里的 font-weight）：
#   HTML 400/500 → NK57 Cd Sb（usWeightClass 600）
#   HTML 600     → NK57 Cd Rg（usWeightClass 700）
#   HTML 700     → 中文走文泉驿微米黑 Mono（NK57 没有汉字）
#
# 中文字体为什么是 SourceHanSansHWSC-Bold.otf：
#   HW = Half Width，汉字前进严格 16 px（与微米黑 Mono 同为定值），
#   宽度可以离线算死，因此 app_ui.c 的居中全部是常量，运行时不量字宽。
#   Bold 字重在 320×240 小屏上比微米黑更清晰 —— v9.8 实机确认。
#   为什么用 2bpp：实机上 2bpp 与 4bpp 的差异肉眼基本不可见，
#   2bpp 省一半点阵（9.3 KB vs 18.6 KB），故最终定稿 2bpp。
#
# cjk16 为什么是"混排"字库：
#   SGL 一个标签只能有一种字体，但菜单数值是 "100°C" / "30分" 这类中英混排。
#   唯一能同时做到"英文用 NK57、汉字用思源黑体、只占一个标签"的办法，
#   就是生成期把两套字形合并进同一个 sgl_font_t：
#     --ttf      = 思源黑体（汉字）
#     --latin-ttf = NK57 Cd Rg （ASCII 与 ° % 等符号）
#   行盒度量必须全库统一，取 NK57 的 20/4 —— 它能容纳全部字形
#   （汉字 asc 14/desc 2，NK57 符号 asc 16/desc 4），取 19/4 会裁掉 {}[]| 的顶边。
#
# 字符集都只放"界面真正会渲染的字符"：数字字库多放一个字符就是
# 白占 ROM，而少放一个字符会在运行时查表失败（可能 HardFault）。
#
# 用法：tools/gen_fonts.sh
set -euo pipefail
cd "$(dirname "$0")/.."

GEN=tools/gen_cjk_font.py
NK57=fonts
RG=$NK57/nk57-monospace.cdrg-bold.otf      # 700：中粗，数字/读数/徽标
SB=$NK57/nk57-monospace.cdsb-regular.otf   # 600：常规，快捷卡片/MODE 标题
# 思源黑体 半宽简体中文 Bold（等宽 16 px，中文用这一档）
CJK=$NK57/SourceHanSansHWSC-Bold.otf
CJK_FACE=0

# 字体名:字号:字体文件:face:字符集文件
FONTS=(
    "num90:90:$RG:0:tools/ui_num90_chars.txt"
    "num32:32:$RG:0:tools/ui_num32_chars.txt"
    "num28:28:$RG:0:tools/ui_num28_chars.txt"
    "num16:16:$RG:0:tools/ui_num16_chars.txt"
    "ui14:16:$RG:0:tools/ui_readout14_chars.txt"
    "ui14s:16:$SB:0:tools/ui_preset14_chars.txt"
    "ui13:16:$RG:0:tools/ui_badge13_chars.txt"
    "ui11:16:$RG:0:tools/ui_mode11_chars.txt"
    "ui11s:16:$SB:0:tools/ui_modetitle11_chars.txt"
    "arrow16:16:$RG:0:tools/ui_arrow16_chars.txt"
    "cjk16:16:$CJK:$CJK_FACE:tools/ui_charset.txt"
    "cjk52:52:$CJK:$CJK_FACE:tools/ui_cjk52_chars.txt"
)

for entry in "${FONTS[@]}"; do
    IFS=: read -r name size ttf face charset <<< "$entry"
    printf '%-8s %2spx  %s (face %s)\n' "$name" "$size" "$(basename "$ttf")" "$face"
    if [ "$name" = "cjk16" ]; then
        # 混排档：汉字走微米黑 Mono，ASCII 与符号走 NK57
        python3 "$GEN" --ttf "$ttf" --face-index "$face" --size "$size" --bpp 2 \
            --charset "$charset" --name "$name" --out "User/Src/font_$name.c" \
            --latin-ttf "$RG" --latin-face-index 0 --metrics-from-latin
    else
        python3 "$GEN" --ttf "$ttf" --face-index "$face" --size "$size" --bpp 2 \
            --charset "$charset" --name "$name" --out "User/Src/font_$name.c"
    fi
done

echo "全部字库已重新生成（User/Src/font_*.c）"
echo "排版常量：python3 tools/font_metrics.py"
