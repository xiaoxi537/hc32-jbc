# tools 目录说明

**当前工程**：`hc32f460/`（裸机，200 MHz，SGL + 206 字子集，中文字体为微米黑 Mono 等宽版）

| 路径 | 用途 | 是否在用 |
|---|---|---|
| `verify_clk.py` | 反汇编比对 `init_clock`，16 项静态检查 | ✅ |
| `gen_cjk_font.py` | 从 TTF/TTC 生成 `font_*.c` 点阵字库；支持 `--latin-ttf` 混排（汉字一套字体、ASCII/符号另一套） | ✅ |
| `gen_fonts.sh` | 一次生成全部 12 个字库（改字号/字重/文案后跑它） | ✅ |
| `font_metrics.py` | **离线打印排版常量**（`font_height`/`base_line`/`bbox_bottom`/`adv`、底边对齐偏移、居中 X 表），改字号后把输出抄进 `app_ui.c` 的宏 | ✅ |
| `extract_charset.py` | 从字符串字面量提取 `cjk16` 字符集（`--write` 写回） | ✅ |
| `ui_charset.txt` | `cjk16` 字符集（206 字；汉字走微米黑 Mono、ASCII 与符号走 NK57） | ✅ |
| `ui_num90_chars.txt` | `num90` 字符集（0-9 -） | ✅ |
| `ui_num32_chars.txt` | `num32` 字符集（0-9 . -） | ✅ |
| `ui_num28_chars.txt` | `num28` 字符集（°C） | ✅ |
| `ui_num16_chars.txt` | `num16` 字符集（°C） | ✅ |
| `ui_readout14_chars.txt` | `ui14` 字符集（0-9 . V A） | ✅ |
| `ui_preset14_chars.txt` | `ui14s` 字符集（0-9 - °C） | ✅ |
| `ui_badge13_chars.txt` | `ui13` 字符集（型号/告警） | ✅ |
| `ui_mode11_chars.txt` / `ui_modetitle11_chars.txt` | `ui11` / `ui11s` 字符集 | ✅ |
| `ui_arrow16_chars.txt` | `arrow16` 字符集（▶） | ✅ |
| `ui_cjk52_chars.txt` | `cjk52` 字符集（无/工/具） | ✅ |
| `ui_preview/` | PC 离屏渲染 `app_ui.c`，输出 320×240 PPM，对照 `docs/ui/*.html` | ✅ |
| `flash_safe.jlink` | 烧安全版 `build/hc32_safe.bin`（1000 kHz，不加热） | ✅ |
| `flash_full.jlink` | 烧全功能版 `build/hc32_full.bin`（1000 kHz，含加热） | ✅ |
| `flash_original.jlink` | 烧原机 `../JBC.bin`（447 KB，只烧不自动运行） | 对比用 |
| `read_original_ctx.jlink` | 读取原机 `ctx=0x20014A4C` 状态/温度/PID/占空比 | 对比用 |
| `read_original_curve.jlink` | 原机 C210 80→300 ℃ 升温曲线采样 | 对比用 |
| `capture_original_insert.jlink` | 原机固件自动采 15 s（插入窗口不足，暂停使用） | 暂停 |
| `capture_v4_insert.jlink` | v4 全功能版同样自动采 15 s（暂停使用） | 暂停 |
| `flash_lcd.jlink` | 烧当前构建 `build/hc32_test.bin`（兼容旧流程） | 兼容 |
| `verify_onboard.jlink` | 读回 `systick_ms`/ADC/加热计数、时钟寄存器、SysTick；地址已按 v7 更新 | ✅ |
| `dwt_meas.jlink` | DWT `CYCCNT` 双窗口测主频 | ✅ |
| `fw/` | **历史固件留档 v1–v7**（勿删，见 `fw/README.md`） | 留档 |
| `logs/` | J-Link 日志与数据 dump（证据，见 `logs/README.md`） | 留档 |
| `legacy/` | 一次性诊断脚本（早期调试用，见 `legacy/README.md`） | 仅留档 |

## 常用命令

```bash
cd hc32f460

# 全功能版
make full                 # build/hc32_full.bin

# 安全版（强制 0 占空比）
make safe                 # 安全版：build/hc32_safe.bin

# 打印 J-Link 需要读取的全局量地址
make symbols

# 静态验证时钟树
python3 tools/verify_clk.py

# 离屏渲染 UI（不需要 SDL，输出 PPM 到 docs/ui/preview）
make -C tools/ui_preview
./tools/ui_preview/build/ui_preview docs/ui/preview

# 烧写
JLinkExe -CommanderScript tools/flash_safe.jlink
JLinkExe -CommanderScript tools/flash_full.jlink

# 读回启动状态
JLinkExe -CommanderScript tools/verify_onboard.jlink
```

> `make safe` / `make full` 都会先 `make clean`，`build/` 里只保留最后一次
> 生成的那份命名镜像；要同时留两份，先把自己需要的那份拷走。

> **J-Link 注意事项**
> - 同一时间只能开一个 J-Link 工具；J-Link GUI 与命令行并存会抢 USB/复位。
> - 卡住时先给**目标板**断电 5 s，再重插 J-Link；不要只重插 J-Link。
> - 烧录脚本固定 1000 kHz，`erase` 后显式 `halt` 再 `loadbin`。
> - 不要开 J-Flash 的 “Verify after programming” 与脚本并行。
