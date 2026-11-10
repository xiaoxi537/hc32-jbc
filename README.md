# HC32F460 焊台固件（JBC C210 / C245 / T12）

> 在 HC32F460JETA 上从零重建的恒温焊台固件：**裸机（无 RTOS）**、
> 200 MHz、ST7789V2 彩屏、SGL 图形库、PID 闭环控温。
> 支持 JBC C210 / C245 与 T12 三种烙铁头，自动识别、自动校准。

**当前版本：v9.35** · 固件 80 788 B · `text 80224 / data 148 / bss 59764`

---

## 特性

### 控温性能（C210 / 24 V / 350℃，69 Hz 实测）

| 指标 | 值 |
|---|---|
| 恒温噪声 σ | **0.14 ℃** |
| 恒温波动幅度 | **1.7 ℃** |
| 升温过冲 | **+24.9 ℃** |
| 待机 100 ℃ 波动 | 4.6 ℃ |

控温链路的三个关键设计：

- **占空比 Sigma-Delta 抖动**：硬件 PWM 只收整数占空比，而 350 ℃ 平衡点
  只有 5～6 格（满量程 250）。用一阶 Σ-Δ 把小数抖成整数，长期平均严格
  等于目标值，等效分辨率从 1 格提升到约 1/33 格。
- **积分分离**：误差 >20 ℃ 时不累加积分。大偏差时 P 项已远超输出上限，
  积分再累加只会在到温时无法及时收手。
- **双环滤波**：控制侧 3 点（低滞后，喂 PID）/ 显示侧 10 点（平滑，喂 UI），
  两个环完全独立。都是 O(1) 环形缓冲 + 增量求和。

### 安全机制

> 一次真实事故后的加固：调试器 halt 内核时加热关断中断不执行，
> 功率引脚被冻结在高电平，导致满功率持续导通。

三层防护：

| 层 | 机制 |
|---|---|
| 1 | **烧录前强制拉低加热引脚**（`tools/flash_pyocd.sh` 自动执行，失败拒绝烧录） |
| 2 | 温度合理性终审：>470 ℃ / >600 ℃ / <−50 ℃ / 过流 → 物理关断 |
| 3 | SWDT 独立看门狗（专用 10 kHz 振荡器，超时 819 ms） |

⚠ **软件保护无法覆盖「CPU 不运行」的场景**（烧录擦除、调试器 halt）。
量产建议加装硬件过温开关（KSD 温控器串联加热回路）。

编译开关 `SWDT_FREEZE_ON_HALT`（`User/Inc/bsp_swdt.h`）：
`1` = 调试优先（halt 时看门狗暂停，当前默认）；`0` = 安全优先（halt 也复位）。

### 界面

- 320×240 横屏，深/浅双主题
- 主界面 / 无工具告警 / 菜单（一屏 5 行可滚动）/ 参数曲线页
- 数字与英文用 **NK57 Monospace**，中文用 **思源黑体 HW Bold**（等宽，2bpp）
- 排版全部离线计算成常量，运行时不量字符串宽度

### 其他

- 手柄自动识别（T12 / C245 / C210），断电测温窗口 5 ms
- 设置掉电保存（Flash sector 31，CRC 校验）
- 联锁：无工具屏蔽输入、进菜单禁止加热

---

## 硬件

| 项 | 配置 |
|---|---|
| MCU | HC32F460JETA（Cortex-M4F） |
| 主频 | 200 MHz（XTAL 16 MHz → MPLL 400 MHz → /2） |
| 显示屏 | ST7789V2 320×240，SPI3 25 MHz + DMA1/AOS |
| 加热 | PA10 + TMRA6 软件 PWM（251 拍 × 101 µs ≈ 25.3 ms/周期） |
| 采样 | ADC1 + DMA2/AOS（热电偶、电流、输入电压、手柄 ID） |
| 输入 | EC11 编码器、按键、休眠座、滚珠开关 |
| 声光 | 蜂鸣器、背光 PWM（TMRA4） |

**加热功率级**：PA10 → R2 1 kΩ → Q1(MMBT2222A) → Q3(G65P06D5 P-MOS)
→ 24 V 母线。R3 10 kΩ 下拉保证引脚悬空时 Q1 截止。

---

## 构建

需要 `arm-none-eabi-gcc` 工具链。

```bash
# 全功能版（含加热）
make full                     # → build/hc32_full.bin

# 安全版（强制 0 占空比，用于调试 UI/ADC）
make safe                     # → build/hc32_safe.bin

# 打印调试用的全局变量地址
make symbols

# 内存占用摘要（不用翻 600 KB 的 map）
make map
```

### 烧录

用 [pyocd](https://github.com/pyocd/pyocd) + DAPLink：

```bash
./tools/flash_pyocd.sh          # 全功能版
./tools/flash_pyocd.sh safe     # 安全版
```

> **必须用这个脚本，不要裸跑 `pyocd load`。**
> 脚本会在烧录前把加热引脚强制拉低，防止烧录/擦除期间误加热。

烧录后建议回读校验：

```bash
pyocd cmd -t hc32f460xe -c "savemem 0x0 80788 build/readback.bin"
sha256sum build/readback.bin build/hc32_full.bin
```

---

## 数据采集与调参

改控制参数前，必须先用当前固件采一份基线数据，改完再采一份对比。
本工程带了一套工具支撑这个流程：

```bash
PY=/path/to/pyocd/venv/bin/python

# 采集（自动校验固件版本、等待就绪、逐点落盘）
$PY tools/capture_safe.py --out docs/baseline/x.csv --seconds 120 --interval 0.015

# 一键迭代：抓基线 / 开分支 / 对比判定
$PY tools/run_iteration.py baseline --tag v9.35 --tip c210 --temp 350
$PY tools/run_iteration.py branch --name iter/v9.36-<摘要>
$PY tools/run_iteration.py compare <基线.csv> <改动后.csv>
```

- **采样率**：控温相关必须 ≤50 ms。0.3 s 会漏掉高频分量，测出的幅度是假的。
- **符号地址**：一律从 elf 动态读取，不要硬编码（改代码后地址会漂）。
- **固件校验**：采集前必须确认板载 SHA-256 与本地构建产物一致。

---

## 目录结构

```text
hc32f460/
├── User/
│   ├── Inc/            应用与板级接口头文件、字库声明、配置
│   └── Src/
│       ├── main.c          上电引导 + 主循环
│       ├── app.c           初始化 + 四个状态机调度
│       ├── app_station.c   全局状态 / 默认参数 / 设置 CRC
│       ├── app_check.c     联锁 / 在位 / 关机 / 待机 / 休眠
│       ├── app_indev.c     按键 / 编码器 / 菜单 / 参数调整
│       ├── app_heat.c      测温 / 识别 / PID / PWM
│       ├── app_ui.c        四个页面与刷新
│       ├── bsp_*.c         时钟 / 屏 / 输入 / ADC / 加热 / 声光 / 设置 / 看门狗
│       ├── sgl_port_hc32.c SGL 移植层
│       └── font_*.c        字库（生成物，勿手改）
├── Library/            厂商 DDL + SGL 图形库
├── CMSIS/ Startup/     CMSIS 与启动文件
├── docs/
│   ├── 分析进度与交接说明.md   当前交接点（§42 为最新）
│   ├── 迭代流程规范.md        改动前抓基线 / 分叉 / 最小改动 / 先仿真
│   ├── 架构与编码规范.md      硬性代码约束与审查清单
│   ├── iterations/           每轮迭代的详细记录
│   ├── baseline/             实测基线数据（CSV + 元数据）
│   └── ui/                   界面版式、字体表、离屏预览
├── tools/              采集 / 迭代 / 字库生成 / 烧录 / 固件归档
└── Makefile
```

---

## 代码约定

- **寄存器访问只允许出现在 `bsp_*.c`**，`app_*.c` 只调函数、不含 DDL 头文件
- **零动态分配**，全部静态；SGL 的 24 KB 静态堆是库自带
- **不用函数指针表**（唯一例外：SGL 移植层，由库强制要求）
- 数据用二维常量表 / 并列常量数组，不为数据造结构体
- 变量名无匈牙利前缀，函数动词开头，注释只写「为什么」
- 错误处理「启动即死」：哪步失败就 `while (1)` 停在哪，看 PC 定位

详见 [`docs/架构与编码规范.md`](docs/架构与编码规范.md)。

---

## 开源协议

本项目采用多协议：

| 部分 | 协议 | 文件 |
|---|---|---|
| 本工程原创代码 | MIT | [`LICENSES/MIT-License.txt`](LICENSES/MIT-License.txt) |
| HC32F460 DDL（`Library/hc32_ll_lib/`） | BSD-3-Clause | [`LICENSES/HC32-BSD-3-Clause.txt`](LICENSES/HC32-BSD-3-Clause.txt) |
| SGL 图形库（`Library/sgl/`） | MIT | [`LICENSES/SGL-MIT.txt`](LICENSES/SGL-MIT.txt) |

字体：NK57 Monospace（CC0）、思源黑体 Source Han Sans（SIL OFL 1.1）。
