# hc32f460 重构工程（裸机）

> 目标：在 HC32F460 新主控板上，按原机 `JBC.bin` 的行为重建焊台固件。
> 架构决策：**不使用 FreeRTOS**，裸机前后台（主循环 + 状态机 + ISR 标志）。
> 详见 [`docs/重构决策-裸机化.md`](docs/重构决策-裸机化.md)。

## 0. 当前状态

> ## ⚠ 迭代流程硬性规范（2026-11-09 起）
>
> **任何功能改动都必须遵守 [`docs/迭代流程规范.md`](docs/迭代流程规范.md)**：
>
> 1. **改动前先抓基线**（50 ms 采样，存档到 `docs/baseline/`）；
> 2. **分叉开发**：`git checkout -b iter/v<版本>-<摘要> master`，
>    **禁止直接在 master 上改功能代码**；v9.29 由 tag `v9.29-baseline` 钉住，随时可回退；
> 3. **最小化改动**：能改一个常量就不要改函数；一次只改一件事；
> 4. **加热控制等核心改动必须先仿真**——且模型必须能复现实测基线，
>    幅度误差 >40% 则模型作废、改用机理分析（详见规范 §2）。
>
> 一键工具：`tools/run_iteration.py`（`baseline` / `capture` / `check` / `compare` / `branch`）。

> ## v9.29 实机基线（2026-10-27）
>
> **350℃ 恒温实测**（pyocd 采集）：**过冲 +2.95℃、稳态幅度 7.2℃**、
> 积分峰值 877（未饱和）。此前的"过冲 15.6℃"来源不明，以本数据为准。
>
> **前馈方案实验失败并已撤销**：虽把过冲压到 −0.42℃，
> 但稳态幅度恶化到 **25.7℃**（原始 7.2℃）。用稳态换过冲是亏本交易。
> 完整教训（5 个坑）见 [交接说明 §41](docs/分析进度与交接说明.md)。
> 失败代码归档在 `docs/experiments/`。
>
> ---
>
> ## v9.29（2026-10-27）：ADC 去极值平均滤波
>
> `read_temp_raw_avg()` 从简单求和平均改为**防脉冲干扰平均滤波**：
> 抄进栈 → 插入排序 → **掐头去尾各 2 个** → 求均值。
> 干掉 PWM 耦合的电磁尖峰（实测：2 个 +300 尖峰下，简单平均偏 +3.15℃，
> 去极值后 **+0.00℃**）。
>
> 顺带修掉一个**静默失效 + 越界写**：初版排序用 `int8_t`，
> 在 `fill>=130` 时写出 `A(-127)`。已改为 `uint8_t j` + `while (j > 0U)`，
> 扫遍 `fill=0..256` 验证零错误、零越界。
>
> | 项 | 值 |
> |---|---|
> | 镜像 | `build/hc32_full.bin` **80 300 B** |
> | SHA-256 | `e7c9d85c2f69a7547863698104d51f8fd5c821d886d3c543d62df06e9ef8622a` |
> | 状态 | ⏳ 待实机实测（350℃ 过冲与水分） |
>
> 交接点：[`docs/分析进度与交接说明.md`](docs/分析进度与交接说明.md) §40
>
> ---
>
> # 🏁 v9.28 最终稳定版（2026-10-26）
>
> **实机验证：非常稳定** —— 待机 100℃、恒温 350℃ 都稳定，
> 徽标/MODE 不再闪烁，三种手柄（T12/C245/C210）全部正常。
> **加热控制与 UI 显示定档冻结，工程收尾。**
>
> **最后修的问题**：阈值打架 —— `HEAT_DISPLAY_NEAR_C`(5℃) 与
> `HEAT_REACH_EXIT_C`(2℃) 在偏差 2~5℃ 时矛盾，导致 350℃ 降温经过
> 347~350 时数字跳、徽标闪。修法：UI 用**独立 8℃ 撤销容差**，
> 强制 `NEAR(5) < EXIT_TOLERANCE(8)`，让数字与徽标错开 3℃ 变化。
>
> | 项 | 值 |
> |---|---|
> | 镜像 | `build/hc32_full.bin` **80 144 B** |
> | SHA-256 | `c356c47e036b8a76b39c1cd1f811351d2ab57cf3fdde36e4cb9fc155cfaed8c4` |
> | 归档 | `tools/fw/app_v928_final_{full,safe}.bin` |
>
> 交接点：[`docs/分析进度与交接说明.md`](docs/分析进度与交接说明.md) §39
>
> ---
>
> v9.27（2026-10-26）：**加热控制定档冻结于 v9.17**，只改 UI 显示。
> v9.27：**已到温且真实温度在设定值 ±5℃ 内时，大数字直接显示设定值** ——
> 消除稳态偏差造成的"稳住了但显示不准"（实测待机稳在 102 而设定 100）。
> 升温中/大幅掉温时仍显示真实温度，不会被钉住。
>
> v9.26（2026-10-25）：
> **实机已验证（2026-10-25）**：**三种手柄（T12/C245/C210）全部稳定**；
> 待机**稳定显示 102**（不再 99↔103 跳，偏高 2℃ 属控制域）、
> 恒温**稳定显示 300** ✅；**显示更新速度偏慢**（慢跟 800 ms 的固有代价）；
> **顶栏状态胶囊与 MODE 框仍闪烁，已知未修、暂缓**。
> **待办**：工程模式诊断页（关于本机连按 5 次进入）——
> 需求与代码核对见 [`docs/feature_specs/工程模式需求.txt`](docs/feature_specs/工程模式需求.txt)，
> 摘要见 [`交接说明`](docs/分析进度与交接说明.md) §36。**仅记录，未实现**。
>
> v9.26：阈值 **10/9**、慢跟 **800 ms**（治待机 99↔103 单次跳）；
> 「已到温」撤销确认 **300→800 ms**；**`invalidate_text_cache()` 删掉 ready
> 三项重置**（修 v9.23 的顺序 bug：先同步后被清，等于同步从未发生）。
> 承 v9.25：显示阈值改为 **`delta>=6` 立刻跟随 / `1~5` 走 500 ms 慢跟**
> （实测摆幅 4℃，留 2℃ 余量；取 4/3 会因首帧随机落边缘而逐拍跳）。
> 代价：改设定值 1~5℃ 时显示延后 500 ms，≥6℃ 立刻到位。
> 承 v9.24：
> v9.24：温度加 **500 ms 慢跟**（1℃ 偏差按住、持续满 500 ms 才接受；
> 偏差 0 必须清计时）；`temp_valid` **替代负值哨兵**（负温度不再误判）。
> 新增 `tests/` **独立单元测试**（`make -C tests run`，31 项）。
> 承 v9.23：
> v9.23 补两处边界：① 显示 `--` 时**作废温度记忆**（防休眠退出卡旧值）；
> ② 页切换时**立即同步** `ready_shown`（防返回主界面闪 300 ms 旧状态）。
> 承 v9.22：
> ① 温度整数 **±1℃ 死区平滑**（超死区直接跳 raw，不逐步爬）→ 消除末位闪；
> ② 「已到温」**300 ms 撤销确认**（置位立即、撤销延迟）→ 消除徽标/MODE 闪烁。
> 两者都是纯显示，不回写状态；到温提示音仍由 `app_heat.c` 管。
> 当前交接点见 [`docs/分析进度与交接说明.md`](docs/分析进度与交接说明.md) §35。
>
> v9.20（2026-10-25）：C210 积分补偿 ×0.9（周期快 11%，
> 累加项打折拉回等效 Ki；**仅 C210**，T12/C245 系数恒为 1.0）+ 
> **修复死区 0 占空比死锁**（占空比为 0 时不进死区，让 PID 继续跑）。
> 承 v9.19：**死区微调已全部删除**（纯冻结）、测温窗口按型号（C210=2ms / 其他=5ms）。
>
> v9.18（2026-10-25）：**弃用仿真选参，改按实机经验调**。
> 四处改动：① ADC 硬件平均 **4→16 次**（硬件自动累加，**降噪不增加滞后**）；
> ② 死区 **±2→±4℃**（宽到包住稳态波动，减少进出 PID）；
> ③ 死区步长**两档**（占空比>15 用 2，否则 1）；
> ④ 控制侧滤波 **3→5 点**（仍是 O(1) 增量求和，汇编确认无额外运算）。
> 两个热模型仿真**已归档**（结论与实机不符，不再用于选参）。
>
> v9.17（2026-10-25）：修复死区内 `pid_last_error`
> 不更新造成的**微分踢动**（Kd=15 时可达 −60）。修完 300℃ 稳态波动
> **4.48→0.23℃**。**这个 bug 正是 v9.16 误选自适应步长的原因**：
> 修复后重扫发现大步长反而变差，故步长**回退为固定 1**（保留严格钳位 1…5）。
>
> v9.16（2026-10-25）：死区微调改自适应步长 `duty/10`。**该方案已被 v9.17 回退**。
> 固定步长 1 追不上热惯性（300℃ 4.48℃）、固定步长 2 会粗调过猛（100℃ 变差），
> 自适应让 100℃ 自动退化为步长 1、300℃ 偏离时自动加速到 3~4。
> 仿真（真实 Kp=25/Ki=0.3/**Kd=15**）：300℃ 波动 **4.48→1.07℃**，
> **100℃ 保持 0.255℃ 不变**；死区仍 **±2.0℃**。
>
> v9.15（2026-10-25）：修**稳态 ±2℃ 慢速振荡**（死区±2 + 按偏差微调 + 浮点判据）。
> 死区 `HEAT_DEADBAND_C` **±1.5→±2℃**；死区内按偏差对占空比做 **±1 微调**
> （偏高 -1 / 偏低 +1，等价于极慢积分）；判据改用**浮点数**比较
> （强转 int 会丢掉"偏高/偏低"的方向信息）。
> 闭环仿真（真实 PID 参数 + 一阶热模型）：300℃ 稳态波动 **4.40℃ → 0.23℃**。
> **注意**：该收益依赖控制侧低噪声；噪声大时会被噪声主导（详见版本日志 §3.1）。
>
> v9.14（2026-10-24）：按实机**重划"待机 / 休眠"语义**。
> v9.13 用一个条件判两态是错的 —— **待机是保温态，休眠才是挂起**：
> · **待机**（C245/C210 休眠座、T12 待机时间到）：短按**无功能**、
>   转编码器**改待机温度**（黄框那个数）、长按**进菜单**；
> · **休眠**（待机闲置满"休眠时间"，大数字 `--`）：短按唤醒且**回到待机**。
> 另补齐 **T12 滚珠开关（PA9）唤醒**这条一直缺失的路径；
> 待机时间对 C245/C210 **锁定为 0**（原机可调但不生效）。
>
> v9.13（2026-10-24）：修复主页待机/休眠下长按无法进菜单。**注意其中
> "待机需要唤醒"的理解已被 v9.14 纠正**。
>
> v9.12（2026-10-24）：修复**待机 100℃ 读数 98~102 跳动**。
> 根因是 v9.11 把"控制侧"与"显示侧"共用的那一个滤波一起降到 3 点，
> 显示跟着变噪。本次恢复原机**双环滤波**（控制侧 3 点喂 PID / 显示侧 10 点喂 UI，
> 两个环互相独立、都是 **O(1) 环形缓冲 + 增量求和**，禁止每拍遍历数组求和），
> 并把 ADC 采样窗口 10→32（覆盖 0.7→2.2 ms，降噪 3.2→5.7 倍，CPU 反而更省）。
> **加热控制律本轮一行未动**。
>
> v9.8（2026-10-23）：中文字库定稿 **思源黑体 HW Bold + 2bpp**，
> 字符集精简到 **143 字**（实机确认 2bpp 与 4bpp 肉眼无差，2bpp 省一半点阵）。
> v9.7 已完成并上机验证的四件事：
> ① 中文字库换成**文泉驿微米黑 Mono 等宽版**（只留界面用到的 206 字）；
> ② **英文/ASCII/特殊符号改回 NK57**，与汉字在生成期合并成混排 `cjk16`；
> ③ 设定框 `°C` **墨迹底边与数字严格平齐**、顶栏金色胶囊**长短两档自动切换**，
> 两组"数字 + °C"的排版全部改成**静态常量**（运行时不量字宽）；
> ④ 修复**快捷温度三组无法调整**（`open_selected_item()` 漏 `MODE_PRESET` 分支）。

| 项 | 状态 |
|---|---|
| 系统时钟 | 200 MHz（XTAL 16 MHz → MPLL 400 MHz → /2），DWT 实测 200.05 MHz |
| 裸机时基 | SysTick 1 kHz，`LOAD=199999`，与原机 `configTICK_RATE_HZ=1000` 等价 |
| 显示 | ST7789V2 320×240，SPI3 **25 MHz** + DMA1/AOS；IDLNF 等待方向已修正 |
| 图形库 | SGL（行缓冲双缓冲 + 脏区刷新），堆 **24 KB** |
| 字体 | **NK57 Monospace**（10 档，数字/读数/英文/数字/符号）+ **思源黑体 HW Bold**（汉字，`SourceHanSansHWSC-Bold.otf`）；`cjk16` 是**中英混排档**（汉字思源黑体、英文/数字/符号 NK57，**143 字**、2bpp），`cjk52` 无工具大字。全部由 `tools/gen_fonts.sh` 生成 |
| 界面 | 按 **`docs/ui/sgl_new_full_UI.html`** 定稿：主界面 + 无工具告警 + 菜单（**一屏 5 行、超过滚动、可进入的行右侧 `▶`**）+ 快捷温度子页；**参数曲线页版式仍待设计** |
| 联锁 | **没插工具屏蔽整套输入**（插上自动恢复并回主页）；**进设置菜单禁止加热**（退回主页恢复，PID 积分冻结不冲功率）；**菜单等用户主动退出**（无闲置超时）；**菜单里拔工具直接进无工具页**；见 `docs/版本日志.md` v9.2 |
| 四个状态机 | `check` 1 ms / `indev` 5 ms / `heat` SysTick 1 ms 推进 / `ui` 10 ms |
| 输入/输出 | EC11、PA2 按键、PA3/PA4/PA9、蜂鸣器、背光 PWM、ADC1+DMA2、TMRA6 加热 |
| 手柄识别 | T12/C245 仅上界 VIN<32；C210 10–32 V；C245 加热电偶判断 |
| 设置保存 | Flash sector 31 (`0x3E000`)，magic/CRC，回主界面 200 ms 后保存 |
| 代码规范 | **2026-10-19 全工程去匈牙利前缀、函数动词开头、注释讲原因**（见 `docs/架构与编码规范.md` §9） |
| 当前固件 | **v9.29** 全功能 `text 79736 / data 148 / bss 59756`，bin **80 300 B**；安全版 80 200 B |
| SHA-256 (full) | `e7c9d85c2f69a7547863698104d51f8fd5c821d886d3c543d62df06e9ef8622a` |
| SHA-256 (safe) | `f6fe559c82edada041b13d3b86dce18e8b635327bda51bf26912c618c728a7a4` |
| 稳定基线 | 🏁 **v9.28**（`c356c47e…`，实机验证稳定）—— 若 v9.29 有问题可回退 |
| 当前烧入 | **v9.7 full**（2026-10-23 实机烧录，回读 80 432 B 哈希与镜像一致；混排字库、`°C` 底边对齐、胶囊 64/96 切换、快捷温度金色编辑态均已确认） |
| UI 验证 | 主界面 / 无工具页 / 滚动菜单 / 快捷温度子页 / 双主题**均已上机确认**（`docs/ui/hw/`）；v9.7 另用芯片内 `sgl_obj_t.coords` 与 `sgl_rect_t.color` 逐项核对了排版与三态配色 |
| 24 V 测试 | ⏸ **等定制焊台面板到货后装机测试**（届时才接 24 V 主供电） |
| 看门狗 | ⏸ **项目即将重构完成时再设计** |

**2026-10-18 晚**：主控板全部焊接后完成首版功能重构。
**2026-10-19**：修复 C210 误判 C245、加热时编码器漏步、预热周期 off-by-one；
全功能版 C210 12 V 稳定 300 ℃。随后复原原机 5 项主菜单结构，新增参数调整页：
上半屏参数名/数值，下半屏 60 点实时温度曲线（SGL linechart）；补 387 字库，修复参数页 % 字符 HardFault 与 Y 轴标签重叠。
详见 [`docs/进度_2026-10-18_功能重构.md`](docs/进度_2026-10-18_功能重构.md)。

**2026-10-19 晚**：按 [`docs/追加规范.md`](docs/追加规范.md) 完成全工程改名与风格重构：
删除全部 `g_`/`s_`/`u8`/`u16`/`i32` 前缀，函数统一动词开头；删除启动错误位和 `BSP_CLK_Verify()`，
改为"哪步失败停哪步"；`app_heat.c` 拆掉 `goto` 和长函数，`app_indev.c` 菜单读写按页拆分；
全工程只留 3 条 `_Static_assert`。行为与 v6d 等价，详见 [`docs/工作日志.md`](docs/工作日志.md)。

**2026-10-20**：按 [`docs/ui/sgl_main.html`](docs/ui/sgl_main.html) 与
[`docs/ui/sgl_no_tools.html`](docs/ui/sgl_no_tools.html) 重构界面：
主界面改为顶栏 + 青色温度面板 + 黄色设定框 + 橙色电压电流框 + MODE 框 + 三个快捷温度；
新增独立的无工具告警页；菜单/参数页沿用同一配色语言。新增 `num76`/`num28`/`ui11`/`cjk52` 字库，
并提供 PC 离屏预览器 `tools/ui_preview/`，渲染结果见
[`docs/ui/README.md`](docs/ui/README.md)。

**2026-10-21**：v8 定稿后再改两处显示细节 —— 无工具页顶栏文案 `无工具` → `Warning`
（+8 B，本次 `text` 全部增量）、ST7789V2 的 `0xE0`/`0xE1` gamma 表手工校正。
重建后 bin 91 164 B（SHA-256 `61d65a0e…`）。
随后 **J-Link 实机校验通过**：读回整片与构建产物逐字节一致、`systick_ms` 1 s 涨 1001、
PC 采样落在 `task_app`、§4 的 6 项寄存器逐项相符（实测 200 MHz）；
**UI 也已人工验证**（主界面 `12°C`/`设定 320°C`/`加热中`/三个快捷温度、
无工具页 `Warning` + `无工具` + `请插入工具`，照片见 [`docs/ui/hw/`](docs/ui/hw/)）。

本次顺带查清了一个链接约定：`User/Src/hc32f460_it.c` 手工定义的 `_init`/`_fini` 桩
与链接选项 `-nostartfiles` 是**一对** —— `startup_hc32f460.s` 调用 `__libc_init_array`，
后者（newlib `init.c`）引用 `_init`；`-nostartfiles` 又不带 `crti.o`，
所以桩必须自己提供（实测：删 `_init` → `undefined reference to '_init'`）。
反过来去掉 `-nostartfiles` 就会与 `crti.o` 的同名符号**重复定义**。
该选项自 10-19 起就在用（见 `tools/logs/build_ui_v6d_full.log`），并非新加。
详见 [`docs/工作日志.md`](docs/工作日志.md) §1.1–§1.3。

---

## 1. 目录与分层

```text
hc32f460/
├── User/
│   ├── Inc/
│   │   ├── app.h / app_station.h / app_check.h / app_indev.h
│   │   ├── app_heat.h / app_ui.h
│   │   ├── bsp_clk.h / bsp_lcd.h / bsp_input.h
│   │   ├── bsp_adc.h / bsp_heater.h / bsp_buzzer_backlight.h / bsp_settings.h
│   │   ├── sgl_config.h / sgl_port_hc32.h
│   │   ├── font_cjk16.h / font_num32.h
│   │   └── hc32f460_it.h
│   └── Src/
│       ├── main.c             上电引导 + 主循环
│       ├── app.c              初始化 + 四状态机调度 + 延迟保存
│       ├── app_station.c      全局状态/默认参数/设置 CRC
│       ├── app_check.c        1 ms 联锁/在位/关机/待机/休眠
│       ├── app_indev.c        5 ms 按键消抖/原机菜单/参数调整
│       ├── app_heat.c         测温/识别/PID/PWM
│       ├── app_ui.c           主界面/无工具/菜单/参数曲线渲染
│       ├── bsp_clk.c          200 MHz + 1 kHz 时基
│       ├── bsp_lcd.c          SPI3 + DMA1 + 条带送屏
│       ├── bsp_input.c        PA0/1/2/3/4/9 输入 + 正交解码
│       ├── bsp_adc.c          ADC1_CH5..8 + DMA2/AOS
│       ├── bsp_heater.c       PA10 + TMRA6 软件 PWM
│       ├── bsp_buzzer_backlight.c  PB8/PB9 TMRA4 PWM
│       ├── bsp_settings.c     Flash sector 31 设置保存
│       ├── sgl_port_hc32.c    SGL fbdev/flush/tick
│       ├── font_cjk16.c       206 字中文/混排子集（生成物）
│       ├── font_num90.c       90 px 当前温度（NK57，生成物）
│       ├── font_num32/28/16.c 32/28/16 px 数字与 °C（NK57，生成物）
│       ├── font_ui14/14s/13.c 14/13 px 读数、卡片、徽标（NK57，生成物）
│       ├── font_ui11/11s.c    11 px MODE 值/标题（NK57，生成物）
│       ├── font_arrow16.c     16 px 菜单"可进入"三角 ▶（NK57，生成物）
│       ├── font_cjk52.c       52 px 无工具大字（生成物）
│       └── hc32f460_it.c      NMI/HardFault/SysTick
├── CMSIS/ Library/ Startup/   厂商 DDL 与启动文件
├── HC32F460xE.ld              链接脚本（Flash 512 KB / RAM 188 KB）
├── Makefile
├── docs/                      进度、规范、决策、SGL、工作日志
└── tools/
    ├── verify_clk.py          时钟静态验证（16 项）
    ├── gen_cjk_font.py        字库生成器
    ├── ui_charset.txt         当前 UI 字符集（206 字，生成 cjk16 用）
    ├── gen_fonts.sh           一次生成全部界面字库
    ├── extract_charset.py     从字符串字面量重新提取 cjk16 字符集
    ├── flash_safe.jlink       安全版烧录（1000 kHz）
    ├── flash_full.jlink       全功能版烧录（1000 kHz）
    ├── fw/                    历史固件留档（v1–v7）
    ├── logs/                  调试日志
    └── legacy/                一次性诊断脚本
```

**分层约束**：

```
main.c  ->  app.c  ->  app_ui.c        (SGL 控件)
                 \->  bsp_lcd.c        (唯一碰 LCD 寄存器)
                 \->  sgl_port_hc32.c  (唯一允许函数指针：SGL 的 flush_area)
```

* 寄存器访问只允许出现在 `bsp_*.c`
* `app_*` 不访问寄存器、不包含 DDL 头文件
* 不用 `malloc`、不用函数指针表；数据用常量表
* 详见 [`docs/架构与编码规范.md`](docs/架构与编码规范.md)

---

## 2. 编译、烧写与验证

```bash
cd hc32f460

# 全功能版（含加热）
make full                 # build/hc32_full.bin

# 安全版（强制 0 占空比，用于 UI/ADC/识别验证）
make safe                 # 安全版：build/hc32_safe.bin

# 打印调试器（pyocd / gdb）需要读取的全局量地址
make symbols

# 内存占用摘要（从 linker map 里拎出内存区/节区/符号，不用翻 600 KB）
make map
make map MAP_FILE=build/hc32_full.map    # 看指定版本的 map

# 静态验证时钟初始化（16 项）
python3 tools/verify_clk.py

# 烧写（pyocd + DAPLink）
./tools/flash_pyocd.sh           # 全功能版（会自动打印镜像 SHA-256）
./tools/flash_pyocd.sh safe      # 安全版
```

> `make safe` / `make full` 都会先 `make clean`，`build/` 里只保留最后一次
> 生成的那份命名镜像；要同时留两份，先把自己需要的那份拷走。

### 烧写工具链（2026-10-25 变更）

**J-Link 硬件已损坏，改用 pyocd + DAPLink。**

| 项 | J-Link（旧） | pyocd + DAPLink（新） |
|---|---|---|
| 命令 | `JLinkExe -CommanderScript tools/flash_*.jlink` | `pyocd load -t hc32f460xe <bin> --base-address 0x0` |
| 速度 | 1000 kHz | 约 21 kB/s（4.9 s 烧完 79 KB） |
| 稳定性 | 偶发卡死，需断电重启 | 无卡死问题 |

**烧写日志里的 `programmed 80384` 比镜像 79 980 B 多是正常的**：
pyocd 按 4 字节字对齐向上取整（79980/4=19995 → 20096 字 → 80384 B）。

回读校验（建议保留这个习惯）：
```bash
pyocd cmd -t hc32f460xe -c "save build/readback.bin 0x0 79980"
sha256sum build/readback.bin build/hc32_full.bin   # 两者应一致
```

> `tools/*.jlink` 脚本（含 `flash_safe/flash_full/flash_lcd`）**保留作历史参考，
> 不再使用**。详见 [`docs/分析进度与交接说明.md`](docs/分析进度与交接说明.md) §37。
>
> 下面两条 J-Link 注意事项**已失效**，保留仅作记录：

---

## 3. 启动流程

```
复位
 └─ main()
     ├─ init_clock()          200 MHz；失败 → while(1) 停机
     ├─ init_tick(TICK_HZ)    1 kHz SysTick；失败 → while(1) 停机
     ├─ init_app()            失败 → while(1) 停机（背光不亮）
     │   ├─ init_lcd()        GPIO/SPI3/DMA1/复位/命令序列/清屏
     │   ├─ init_sgl_port()   注册 fbdev、初始化 SGL
     │   ├─ init_ui()         创建主界面/无工具/菜单/参数页
     │   ├─ init_inputs / init_buzzer_backlight / set_backlight
     │   ├─ init_adc / init_heater / init_heat
     │   └─ init_check / init_indev
     └─ for(;;) task_app()    1/5/10 ms 状态机 + SGL 渲染 → flush_lcd_rect → DMA
```

**失败即停机**：哪一步失败就停在哪一步的 `while (1)`，不再维护错误位。
若 LCD/SGL/界面初始化失败，背光保持熄灭，便于区分软件错误与屏/接线故障。

---

## 4. 系统时钟

```
XTAL 16 MHz --/2--> PFD 8 MHz --x50--> VCO 400 MHz --/2--> HCLK/PCLK0 = 200 MHz
                                                           PCLK1 = 100 MHz
                                                           PCLK2 =  50 MHz
                                                           PCLK3 =  50 MHz
                                                           PCLK4 = 100 MHz
```

| 寄存器 | 目标值 | 约束 |
|---|---|---|
| `CMU_PLLCFGR` | `0x11103101` | M=/2, N=x50, P=/2, Q=/2, R=/2, SRC=XTAL |
| `CMU_SCFGR` | `0x00112210` | 掩码 `0x07777777` |
| `CMU_CKSWR` | `5` | 系统时钟 = MPLL |
| `EFM_FRMC` | `0x50` → `FLWT=5` | RM 表 7-1：168 MHz < HCLK ≤ 200 MHz |
| `SRAM_WTCR` | `0x11001111` | SRAM1/2、SRAM3、Ret_SRAM=1 wait；SRAMH=0 |
| `SysTick` | `LOAD=199999`、`CTRL=0x00010007` | 1 kHz，`CLKSOURCE=HCLK` |

> **2026-10-21 J-Link 实测**：上表 6 项寄存器在板上一一核对通过
> （`PLLCFGR=0x11103101`、`SCFGR=0x00112210`、`CKSWR=0x05`、`FRMC=0x50`、
> `SRAMC_WTCR=0x11001111`、SysTick `LOAD=0x00030D3F`）；
> 由 `LOAD=199999` 反推 `SystemCoreClock=200 MHz`。脚本与 log 见 `tools/logs/verify_clk.*`、
> `verify_regs.*`。

初始化顺序（RM §7.4.1：先提等待周期，再提主频）：

```
PWC 解锁 → SRAM 等待 → EFM FLWT=5 → XTAL 起振 → SCFGR 分频
→ MPLL → CKSWR=5 → PWC 回锁
```

全工程只保留 3 条 `_Static_assert`：`bsp_clk.c` 锁 `XTAL_VALUE=16 MHz` 和
1 ms 时基（`HCLK/TICK_HZ=200000`），`bsp_lcd.c` 锁条带字节数不超过 DMA 16 位计数。
**忘了 `-DXTAL_VALUE` 会直接编译失败。**

实机验证数据（DWT 200.05 MHz、比值法 200 010 cycles/tick、寄存器回读）
见工作区 [`../docs/时钟配置实机验证.md`](../docs/时钟配置实机验证.md)。

---

## 5. 显示屏（ST7789V2 / SPI3 / DMA1）

### 5.1 硬件连接

| 信号 | 引脚 | 配置 |
|---|---|---|
| TFT_CS | PA15 | GPIO 输出，低有效（需先释放 JTDI） |
| TFT_CLK | PB3 | SPI3_SCK，复用功能 **0x2B**（需先释放 JTDO） |
| TFT_SDA | PB5 | SPI3_MOSI，复用功能 **0x28** |
| TFT_A0 / DC | PB6 | GPIO 输出，0=命令 1=数据 |
| TFT_RST | PB7 | GPIO 输出，低有效 |
| TFT_LED | PB9 | **TMRA4_CH4 PWM**（0–10 级）；R36 20k → Q4 MMBT2222A → LED_K |

### 5.2 软件通路

```
SPI3（3 线制 / 只发送 / 主机 / 8bit / Mode 0，PCLK1÷4 = 25 MHz）
  + DMA1_CH0（AOS 触发 INT_SRC_SPI3_SPTI = 0x136）
  + 320×30 RGB565 条带缓冲（19 200 B）→ 整屏 240 行分 8 段
```

* `init_lcd()`：GPIO → SPI3 → DMA1/AOS → 硬件复位 → 15 条初始化命令 → `MADCTL=0x70` → 清屏
* `flush_lcd_rect()`：任意矩形，SGL 行缓冲直接送入
* `clear_lcd()`：纯色整屏填充
* 背光：`set_backlight(0…10)`，TMRA4_CH4 PWM；`bsp_lcd.c` 不再控制 PB9
* 单次 DMA 传输 ≤ 65 535 B（传输计数 16 位），由 SGL 行缓冲尺寸保证

### 5.3 关键时序与坑

| 结论 | 原因 |
|---|---|
| **25 MHz 必须走 DMA** | CPU 逐字节等 `TDEF` 会把 SPI 饿死（实测全黑）；DMA 由 AOS 稳定供数 |
| **发送模式 DMA 要"关→配→开"** | 停 SPI → 配 DMA → 开通道 → 再开 SPI，才会产生 SPTI |
| **收尾只停 DMA，不能停 SPI** | 下一条带还要靠 CPU 发 `CASET/RASET/RAMWR`，SPI 停了会顶部移位 |
| **抬 CS 前等 `TDEF` → `IDLNF=0` → 再补余量** | `IDLNF=1` 表示传送中；等反会每次 flush 超时 ~50 ms（屏幕极慢） |
| **`DTCTL.BLKSIZE` 只有 10 位** | 块大小必须为 1，写总长度会被截断、通道挂起 |
| **`PWC_FCG0` 默认写保护** | 必须先 `PWC_FCG0_REG_Unlock()`，否则 DMA/AOS 时钟使能被静默丢弃 |

完整排查过程与固件对照见 [`docs/工作日志.md`](docs/工作日志.md) 与
[`tools/fw/README.md`](tools/fw/README.md)。

---

## 6. 图形库与界面

```
SGL 渲染 320×10 行缓冲（双缓冲 12.8 KB）
  → CONFIG_SGL_COLOR16_SWAP=1 就地交换字节序
  → flush_lcd_rect() → DMA1/SPI3 25 MHz → ST7789V2
  → sgl_fbdev_flush_ready()
```

* 只读引用工作区 `../sgl/`，适配代码全在本工程 `User/`
* 行缓冲而非整屏帧缓冲：整屏要 153.6 KB，行缓冲仅 12.8 KB
* 页面底色必须显式设置（SGL 默认白底，白色图元会被吞掉）
* `sgl_label_set_text()` 只存指针：不能传局部变量，否则菜单会卡死
* **字体**：数字/读数用 **NK57 Monospace**（`num90`/`num32`/`num28`/`num16`/
  `ui14`/`ui14s`/`ui13`/`ui11`/`ui11s`/`arrow16`），中文用 `cjk16`（206 字子集）
  与 `cjk52`；不再链接全量 GB2312。字符集只放"界面真正会渲染的字"
  （由 `tools/extract_charset.py` 从字面量提取），全部由 `tools/gen_fonts.sh` 生成
* `CONFIG_SGL_FONT_SMALL_TABLE` 保持 0（`bitmap_index` 用 32 位；子集后续扩容也安全）
* SGL 堆 **24 KB**；`sgl_system` 紧跟内存池，堆溢出会破坏 dirty 状态
* 主界面/无工具/菜单/参数页：`app_ui.c` 中 `init_ui()`；控件静态创建，文本缓冲必须静态
* 版式来源与离屏预览：见 [`docs/ui/README.md`](docs/ui/README.md)（`tools/ui_preview/`）
* SGL 细节与踩坑见 [`docs/SGL接入说明.md`](docs/SGL接入说明.md)

---

## 7. 当前进度

| 阶段 | 状态 |
|---|---|
| M0 时钟 / 时基 / 工程骨架 | ✅ |
| M0.5 代码分层与清理 | ✅（2026-10-18） |
| M0.6 编码规范落地 | ✅（2026-10-19）去前缀 / 动词开头 / 启动即停 / 断言收敛到 3 条 |
| M1 UI 显示 / 菜单 | ✅ v9 按新设计稿定稿（菜单一屏 5 行可滚动 + 快捷温度子页 + 删系统风格层）；**参数曲线页视觉待设计** |
| M2 控制（EC11 + 按键） | ✅ 首版完成 |
| M3 手柄识别 | ✅ 首版完成（T12/C245 无 VIN 下限；C210 电压窗口） |
| M4 测温 | ✅ 首版完成（C245 室温 0 ℃ 与原机一致） |
| M5 加热 / PID / 保护 | ✅ C210 12 V 300 ℃ 稳定；加热状态机由 SysTick 的 `tick_heat` 推进；24 V 长跑待测 |
| 设置掉电保存 | ✅ 首版完成（Flash sector 31） |
| C210/C245 识别 | ✅ C210 冷启动/热切换修复；T12/C245 待换头复测 |
| 编码器抗漏步 | ✅ 扫描移入 SysTick 1 ms；实体走查待确认 |
| 加热调度 | ✅ `tick_heat` 在 SysTick 1 ms 内推进，不再受 UI 渲染阻塞 |

**下一步**：

1. 限流 24 V + C245 闭环：观察温度上升、电流、过流/断偶保护；
2. **v9 上机走查**（滚动菜单、快捷温度子页、无工具页新文案、双主题，清单见 `docs/版本日志.md` §4.2）；
   然后实体编码器/按键/蜂鸣器/参数曲线页完整走查；
3. 补看门狗；
4. 空载 vs C245 硬件 ID 重叠的最终方案（当前用 C245 热电偶判断折中）。

完整路线图： [`docs/重构进度与路线图.md`](docs/重构进度与路线图.md)。
本次详细记录： [`docs/工作日志.md`](docs/工作日志.md)（v7）；
历史功能重构： [`docs/进度_2026-10-18_功能重构.md`](docs/进度_2026-10-18_功能重构.md)（v6d）。

---

## 8. 已知注意事项

1. **源文件已统一为 LF**（`User/` 下我们自己的文件；厂商头文件保持原样）。
2. `XTAL_VALUE` 必须 16 MHz；`bsp_clk.c` 已用 `_Static_assert` 拦住误编译。
3. 本工程**不启用看门狗**；进入 24 V 闭环前建议补 SWDT。
4. RAM `0x1FFF8000` 起 188 KB；当前 bss **59 620 B（约 58.2 KiB）**，其中 SGL 堆 24 KB，栈 8 KB。
5. 无 `printf`（`LL_PRINT_ENABLE=DDL_OFF`），调试靠调试器读全局量
   （2026-10-25 起用 **pyocd + DAPLink**，`pyocd cmd -c "..."` 或 `pyocd gdbserver`）。
6. **SGL 文本生命周期**：`sgl_label_set_text()` 只保存指针，字符串必须是静态/长期有效，
   不能传局部变量；本次声音页卡死就是该问题。
7. **字体是子集**：新增中文界面文字后重跑
   `python3 tools/extract_charset.py --write && tools/gen_fonts.sh`；
   改温度/设定值/无工具大字时，按对应 `tools/ui_*_chars.txt` 重生成，
   否则新字会显示空白/异常。数字字库多放一个字白占 ROM、少放一个字可能 HardFault。
8. 设置区在 `0x3E000`；整片擦除烧录会清掉设置，烧完恢复默认。
9. ~~J-Link 只用命令行 1000 kHz；卡住先给目标板断电~~ —— **已失效**：
   J-Link 硬件损坏，改用 **pyocd + DAPLink**（见 §7「烧写工具链」与交接说明 §37）。
10. **C210 的 ID 脚接 HEAT-**：加热后残留电平会把 ID 抬高；识别前必须先关加热等待。
11. `tick_encoder` 和 `tick_heat` 都在 SysTick 1 ms 内；主循环只做 UI/菜单/设置。

---

## 9. 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/迭代流程规范.md`](docs/迭代流程规范.md) | **【硬性】改动前先抓基线 / 分叉开发 / 核心改动先仿真 / 最小化改动** |
| [`docs/工作日志.md`](docs/工作日志.md) | **按会话累计的工作记录（最新在最上面，接手先看这个）** |
| [`docs/重构进度与路线图.md`](docs/重构进度与路线图.md) | 当前状态、验证方式、优先级与 M1~M5 计划 |
| [`../docs/分析进度与交接说明.md`](../docs/分析进度与交接说明.md) | **总交接说明**；§14 是固件工程的交接点（状态、证据、编译约定、环境坑） |
| [`docs/ui/README.md`](docs/ui/README.md) | 四页版式、字体表、离屏预览与实机照片 |
| [`docs/进度_2026-10-18_功能重构.md`](docs/进度_2026-10-18_功能重构.md) | 四状态机/UI/菜单/识别/设置保存的详细记录 |
| [`docs/架构与编码规范.md`](docs/架构与编码规范.md) | **硬性编码约束与审查清单** |
| [`docs/重构决策-裸机化.md`](docs/重构决策-裸机化.md) | 放弃 FreeRTOS 的决策与等价映射 |
| [`docs/SGL接入说明.md`](docs/SGL接入说明.md) | SGL 数据流、配置、字库与踩坑 |
| [`tools/fw/README.md`](tools/fw/README.md) | 历史固件清单与 SHA-256 |
| [`../docs/时钟配置实机验证.md`](../docs/时钟配置实机验证.md) | 时钟树双向验证 |
| [`../docs/可重构组件文档.md`](../docs/可重构组件文档.md) | 组件划分与原机对应关系 |
| [`../analysis/ui_display.md`](../analysis/ui_display.md) | 原机显示子系统逆向细节 |
