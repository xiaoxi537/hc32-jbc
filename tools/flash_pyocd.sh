#!/usr/bin/env bash
##############################################################################
# pyocd + DAPLink 烧写脚本（HC32F460）
#
# 为什么换到 pyocd：
#   J-Link 硬件损坏后改用 DAPLink（CMSIS-DAP）。pyocd 烧写更快，
#   且不会出现 J-Link 那种"卡死需要给目标板断电"的问题。
#
# 用法（必须在 hc32f460/ 目录下运行）：
#   ./tools/flash_pyocd.sh            # 烧全功能版 build/hc32_full.bin
#   ./tools/flash_pyocd.sh safe       # 烧安全版 build/hc32_safe.bin
#   ./tools/flash_pyocd.sh <文件>      # 烧指定镜像
#
# 前提：
#   1. 已 make full / make safe 生成镜像
#   2. DAPLink 已插好（pyocd list 能看到探针）
#
# 注意：
#   - 可用环境变量 PYOCD_UID 指定探针 UID（多探针时必须指定）。
#     单探针可省略，脚本会自动探测。
#   - 烧完不会自动复位运行？pyocd load 默认会复位运行（见 --help）。
#     若需烧后停在复位状态，加 --no-reset。
##############################################################################

set -eu

# ---- 定位仓库根（脚本在 tools/ 下） ----
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

TARGET="${PYOCD_TARGET:-hc32f460xe}"
BASE_ADDR="0x00000000"

# ---- 选镜像 ----
case "${1:-}" in
    "")
        IMAGE="build/hc32_full.bin"
        ;;
    safe)
        IMAGE="build/hc32_safe.bin"
        ;;
    *)
        IMAGE="$1"
        ;;
esac

if [ ! -f "$IMAGE" ]; then
    echo "错误：镜像不存在 $IMAGE" >&2
    echo "      先运行 make full 或 make safe" >&2
    exit 1
fi

# ---- 打印镜像信息（便于事后核对烧的是哪一版） ----
SIZE=$(stat -c%s "$IMAGE")
HASH=$(sha256sum "$IMAGE" | cut -d' ' -f1)
echo "镜像   : $IMAGE"
echo "大小   : $SIZE B"
echo "SHA-256: $HASH"
echo "目标   : $TARGET @ $BASE_ADDR"
echo

# ---- 探针检查 ----
if ! pyocd list | grep -qi "cmsis-dap\|daplink"; then
    echo "错误：未检测到 DAPLink 探针（pyocd list 无 CMSIS-DAP）" >&2
    echo "      检查 USB 连接，或换一根线/换一个 USB 口" >&2
    exit 1
fi

# ---- 【安全步骤】烧录前强制把 PA10（加热 PWM）拉低 ----
#
# 为什么必须做：2026-11-09 出现过"烧录完成瞬间烙铁烧红"事故。
# 加热关断依赖 TMRA6 中断里的 set_heat_pin(false)，而 pyocd 连接会
# halt 内核、擦除又会清掉中断向量 → 中断不执行 → PA10 冻结在最后电平。
# duty=80 时高电平占 31.2%，一旦冻结为高，加热就从 31% 变成 100% 持续导通。
#
# 这里在烧录前主动把 PA10 配成"输出低电平"，Q1 必然截止，
# 即使随后 halt/擦除/复位，物理上也无法加热。
#
# 硬件依据：PA10 --R2 1kΩ-- Q1(MMBT2222A) 基极，R3 10kΩ 下拉 GND；
#           Q1 截止 → Q3(G65P06D5 P沟道) 栅极被 R1 10kΩ 拉到 VIN
#           → Vgs=0 → Q3 关断 → 24V 不进烙铁芯。
# 确认加热回路已物理断开时可设 SKIP_HEAT_SAFE=1 跳过。
if [ "${SKIP_HEAT_SAFE:-0}" != "1" ]; then
    echo "---- 安全步骤：强制 PA10 低电平 ----"
    PYOCD_PY="/home/bunny233/.local/share/pipx/venvs/pyocd/bin/python"
    if [ -x "$PYOCD_PY" ]; then
        if ! "$PYOCD_PY" "$SCRIPT_DIR/safe_heat_pin.py"; then
            echo "错误：无法把 PA10 拉低，为防止加热失控已中止烧录" >&2
            echo "      确认加热回路已断开后可设 SKIP_HEAT_SAFE=1 跳过" >&2
            exit 1
        fi
    else
        echo "警告：找不到 $PYOCD_PY，跳过安全步骤" >&2
        echo "⚠ 此时烧录可能导致加热失控，请确认加热回路已断开" >&2
        sleep 3
    fi
    echo
fi

# ---- 烧写 ----
# -u : 指定探针 UID（未设置则用默认/唯一探针）
# -t : 目标型号
if [ -n "${PYOCD_UID:-}" ]; then
    UID_ARGS="-u $PYOCD_UID"
else
    UID_ARGS=""
fi

# shellcheck disable=SC2086
pyocd load $UID_ARGS -t "$TARGET" "$IMAGE" --base-address "$BASE_ADDR"

echo
echo "烧写完成。建议回读校验（实测可用，注意是 savemem 不是 save）："
echo "  pyocd commander ${UID_ARGS} -t $TARGET -c \"savemem 0x0 $SIZE build/readback.bin\""
echo "  sha256sum build/readback.bin    # 应与上面镜像 SHA-256 一致"
echo
echo "注意：文件名不要用下划线（pyocd 会吃掉并报 invalid argument）。"
