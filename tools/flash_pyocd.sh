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
