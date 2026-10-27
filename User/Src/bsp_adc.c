/**
 *******************************************************************************
 * @file  bsp_adc.c
 * @brief ADC1 + DMA2 连续扫描采样
 *
 * 配置对齐原机 adc1_dma_init（0x63F8）：
 *   - ADC1 序列 A 连续扫描 ch5..ch8，采样时间 0xFF（最长，给高阻信号源留余量）
 *   - 硬件 4 次平均，压掉开关噪声
 *   - AOS 事件 EVT_SRC_ADC1_EOCA(448) 触发 DMA2_CH0
 *   - DMA 块大小 4、传输次数 0（连续），源/目标地址递增，重复模式
 *   - DMA2_BTC0 → INT039，优先级 5（要能打断主循环，但别抢 SysTick）
 *******************************************************************************
 */
#include "bsp_adc.h"
#include "bsp_heater.h"
#include "hc32_ll.h"

#define ADC_UNIT                CM_ADC1
#define ADC_SEQ                 ADC_SEQ_A
#define ADC_CH_ID               ADC_CH5
#define ADC_CH_CURRENT          ADC_CH6
#define ADC_CH_TEMP             ADC_CH7
#define ADC_CH_VIN              ADC_CH8

#define ADC_DMA_UNIT            CM_DMA2
#define ADC_DMA_CH              DMA_CH0
#define ADC_DMA_BLOCK           (4U)

/* 热电偶环形缓冲深度（v9.12：10 → 32）
 *
 * 为什么从 10 加到 32：ADC 一次完整扫描（4 通道）约 69 µs，温度通道
 * 每 ~69 µs 出一个样，5 ms 测温窗口能采到 ~72 个点。原来只留 10 点，
 * 相当于只平均了窗口**最后 0.7 ms** —— 对整窗噪声几乎没有压制作用，
 * 却要关中断做 10 次加法。32 点覆盖窗口末尾 ~2.2 ms，仍在热电偶
 * "断电后已稳定"的区间内（不会把刚开始放电的低读数算进来），
 * 降噪 √3.2 ≈ 1.8 倍，且加成是纯粹的（不引入任何滞后）。
 *
 * 为什么不到 72（整窗）：测温窗口开头的样本还没稳定，混进来会把
 * 读数整体拉低（代码里 5 ms 的注释就是这个物理约束）。 */
#define TEMP_RING_SIZE          (32U)

/* 电流原始值超过 2999（约 23 A）就认定短路，立刻关输出 */
#define CURRENT_RAW_LIMIT       (2999U)

volatile uint16_t adc_raw[4];
volatile uint32_t adc_isr_count;
volatile bool adc_over_current;

static volatile uint8_t sample_phase = ADC_PHASE_IDLE;
static volatile uint32_t sum_current;
static volatile uint32_t sum_vin;
static volatile uint32_t sum_count;
static volatile uint16_t temp_ring[TEMP_RING_SIZE];
static volatile uint8_t temp_ring_index;
static volatile uint32_t temp_ring_sum;     /* 32 个点的滚动和，O(1) 维护 */
static volatile uint8_t temp_ring_fill;     /* 窗口填满前的有效点数 */

/**
 * @brief  DMA2 块完成中断：按相位累加
 * @note   加热相位累加电流/电压并做过流保护，测温相位填温度环形缓冲
 */
static void handle_adc_block(void)
{
    uint16_t current;
    uint16_t temp;
    uint16_t vin;
    uint8_t phase;

    DMA_ClearTransCompleteStatus(ADC_DMA_UNIT, DMA_FLAG_BTC_CH0);
    adc_isr_count++;

    /* 先整体快照，避免读到一半时 ADC 又更新了 adc_raw */
    current = adc_raw[1];
    temp = adc_raw[2];
    vin = adc_raw[3];
    phase = sample_phase;

    /* 只有加热相位才累加电流/电压；测温相位的电流没有参考价值 */
    if (ADC_PHASE_HEAT == phase) {
        sum_current += current;
        sum_vin += vin;
        sum_count++;
        /* 硬件级过流保护放在中断里：主循环再快也可能被刷屏阻塞，
         * 短路电流必须在这一个 DMA 块就切断。 */
        if (current > CURRENT_RAW_LIMIT) {
            adc_over_current = true;
            stop_heat_output();
        }
    } else if (ADC_PHASE_MEASURE == phase) {
        /* **环形缓冲 + 增量求和（O(1)）**
         * 不是"每拍遍历整个数组求和"——那样每拍 32 次加法还要关中断。
         * 这里只做两件事：把最旧的点从和里减掉、把最新的点加进去。
         * 窗口内始终是最近 TEMP_RING_SIZE 个样本的和，除法只在读取时做一次。
         *
         * **减法必须带"窗口已填满"的条件**（v9.12 踩坑）：
         * 窗口没填满时 `temp_ring[index]` 还是上次运行/上一支烙铁头的旧值
         * （reset 故意不清数组以省掉循环）。此时若无条件执行
         * `sum -= 旧值`，而 sum ≈ 0，就会出现**无符号下溢**，
         * 均值瞬间跳到 42 亿，读数直接显示垃圾。
         * 这就是上电/换头后头几拍温度乱跳的根因。 */
        if (temp_ring_fill >= TEMP_RING_SIZE) {
            temp_ring_sum -= temp_ring[temp_ring_index];
        }
        temp_ring[temp_ring_index] = temp;
        temp_ring_sum += temp;
        temp_ring_index++;
        /* 到末尾就绕回 0，窗口始终是最新的那一段 */
        if (temp_ring_index >= TEMP_RING_SIZE) {
            temp_ring_index = 0U;
        }
        /* 上电头几拍窗口还没填满，记录有效点数，读取时不能除以 32 */
        if (temp_ring_fill < TEMP_RING_SIZE) {
            temp_ring_fill++;
        }
    }
    /* 空闲相位什么都不做：此时输出刚切换，数据还没稳定 */
}

void init_adc(void)
{
    stc_adc_init_t adc_init;
    stc_dma_init_t dma_init;
    stc_dma_repeat_init_t dma_repeat;
    stc_irq_signin_config_t irq_sign;
    stc_gpio_init_t gpio_init;

    /* DMA2/AOS 属于 FCG0，默认写保护；ADC1 属于 FCG3 */
    PWC_FCG0_REG_Unlock();
    FCG_Fcg0PeriphClockCmd(FCG0_PERIPH_DMA2 | FCG0_PERIPH_AOS, ENABLE);
    FCG_Fcg3PeriphClockCmd(FCG3_PERIPH_ADC1, ENABLE);

    /* 四个脚改模拟输入，断开数字输入缓冲，避免漏电流影响采样 */
    (void)GPIO_StructInit(&gpio_init);
    gpio_init.u16PinAttr = PIN_ATTR_ANALOG;
    (void)GPIO_Init(GPIO_PORT_A, GPIO_PIN_05 | GPIO_PIN_06 | GPIO_PIN_07, &gpio_init);
    (void)GPIO_Init(GPIO_PORT_B, GPIO_PIN_00, &gpio_init);

    /* 序列 A 连续扫描、12 bit 右对齐：和原机完全一致 */
    (void)ADC_StructInit(&adc_init);
    adc_init.u16ScanMode = ADC_MD_SEQA_CONT;
    adc_init.u16Resolution = ADC_RESOLUTION_12BIT;
    adc_init.u16DataAlign = ADC_DATAALIGN_RIGHT;
    (void)ADC_Init(ADC_UNIT, &adc_init);

    ADC_ChCmd(ADC_UNIT, ADC_SEQ, ADC_CH_ID, ENABLE);
    ADC_ChCmd(ADC_UNIT, ADC_SEQ, ADC_CH_CURRENT, ENABLE);
    ADC_ChCmd(ADC_UNIT, ADC_SEQ, ADC_CH_TEMP, ENABLE);
    ADC_ChCmd(ADC_UNIT, ADC_SEQ, ADC_CH_VIN, ENABLE);

    /* 采样时间给到最大：热电偶和分压后的 VIN 都是高阻源 */
    ADC_SetSampleTime(ADC_UNIT, ADC_CH_ID, 0xFFU);
    ADC_SetSampleTime(ADC_UNIT, ADC_CH_CURRENT, 0xFFU);
    ADC_SetSampleTime(ADC_UNIT, ADC_CH_TEMP, 0xFFU);
    ADC_SetSampleTime(ADC_UNIT, ADC_CH_VIN, 0xFFU);

    /* ID 通道不做硬件平均：ID 是直流电平，平均反而拖慢识别。
     *
     * **硬件平均次数：v9.20 曾试过 16，已回滚为 4**（v9.21 回滚到 v9.17）
     *
     * 为什么在硬件里加而不是在软件里加：ADC 的 AVCNT 是**转换期间自动累加**，
     * 完全由硬件完成，不占 CPU、不引入软件滤波的额外滞后 ——
     * 这是"降噪但不增加延迟"的唯一手段。
     *
     * 次数与效果（噪声按 1/√N 下降）：
     *   · 4 次（原）：降噪 2 倍
     *   · **16 次：降噪 4 倍**（比原来再降一倍）
     * 代价：单通道转换时间 ×4。采样时间已设 0xFF（最长），
     * 16 次平均后一次完整扫描 4 通道仍远小于 5 ms 测温窗口（约 0.28 ms），
     * 不会压缩测温窗口的有效采样点数。
     *
     * 为什么不用 32/64：AVCNT 越大单次转换越久，32 次就要 ~0.56 ms，
     * 会明显减少窗口内的样本数量；16 次是"降噪明显且不牺牲样本数"的平衡点。 */
    ADC_ConvDataAverageConfig(ADC_UNIT, ADC_AVG_CNT4);
    ADC_ConvDataAverageChCmd(ADC_UNIT, ADC_CH_CURRENT, ENABLE);
    ADC_ConvDataAverageChCmd(ADC_UNIT, ADC_CH_TEMP, ENABLE);
    ADC_ConvDataAverageChCmd(ADC_UNIT, ADC_CH_VIN, ENABLE);

    /* DMA2_CH0：从 ADC1_DR5 起连续搬 4 个结果到 adc_raw */
    (void)DMA_StructInit(&dma_init);
    dma_init.u32IntEn = DMA_INT_ENABLE;
    dma_init.u32SrcAddr = (uint32_t)(&ADC_UNIT->DR5);
    dma_init.u32DestAddr = (uint32_t)adc_raw;
    dma_init.u32DataWidth = DMA_DATAWIDTH_16BIT;
    dma_init.u32BlockSize = ADC_DMA_BLOCK;
    dma_init.u32TransCount = 0U;                 /* 0 = 无限连续传输 */
    dma_init.u32SrcAddrInc = DMA_SRC_ADDR_INC;
    dma_init.u32DestAddrInc = DMA_DEST_ADDR_INC;
    (void)DMA_Init(ADC_DMA_UNIT, ADC_DMA_CH, &dma_init);

    /* 源/目标都要循环回起点，否则第二轮就搬到 RAM 外面去了 */
    dma_repeat.u32Mode = DMA_RPT_BOTH;
    dma_repeat.u32SrcCount = ADC_DMA_BLOCK;
    dma_repeat.u32DestCount = ADC_DMA_BLOCK;
    (void)DMA_RepeatInit(ADC_DMA_UNIT, ADC_DMA_CH, &dma_repeat);

    /* ADC 序列 A 结束事件触发 DMA，不占用 CPU */
    AOS_SetTriggerEventSrc(AOS_DMA2_0, EVT_SRC_ADC1_EOCA);

    irq_sign.enIntSrc = INT_SRC_DMA2_BTC0;
    irq_sign.enIRQn = INT039_IRQn;
    irq_sign.pfnCallback = &handle_adc_block;
    (void)INTC_IrqSignIn(&irq_sign);
    DMA_ClearTransCompleteStatus(ADC_DMA_UNIT, DMA_FLAG_BTC_CH0);
    NVIC_ClearPendingIRQ(INT039_IRQn);
    NVIC_SetPriority(INT039_IRQn, 5U);
    NVIC_EnableIRQ(INT039_IRQn);

    DMA_Cmd(ADC_DMA_UNIT, ENABLE);
    DMA_ChCmd(ADC_DMA_UNIT, ADC_DMA_CH, ENABLE);

    ADC_Start(ADC_UNIT);
}

void set_adc_phase(uint8_t phase)
{
    sample_phase = phase;
}

uint16_t read_tip_id(void)
{
    return adc_raw[0];
}

/**
 * @brief  读温度通道的原始均值（防脉冲干扰平均滤波）
 * @retval 去极值后的平均值，单位是 ADC 原始码（0~4095）
 *
 * 一句话概括（规范 §9.7 第 5 条）：把测温窗口里的 32 个 ADC 样点抄到栈上，
 * 掐掉最大最小的各 2 个（丢掉电磁尖峰），中间 28 个求平均给控制环用。
 *
 * **为什么要去极值**：开关电源和 PWM 加热线会在热电偶上耦合出偶发尖峰。
 * 简单的求和平均里，一个 +200 码的尖峰会被 32 除，看起来只抬高 6 码 ——
 * 但温度是按 ~0.146 ℃/码 折算的（350℃ / 2400 码），6 码就是一度的假跳。
 * 尖峰若是连续两三个，误差更大。去极值能把这几个点**整个扔掉**而不是摊薄。
 *
 * **为什么用插入排序**：每轮周期只挪动一个点（旧的走、新的来），
 * 数组天然接近有序，插入排序在这种数据上是近似 O(n)，
 * 比 qsort 快得多且不需要函数指针（规范 §1.2 禁止函数指针）。
 */
uint16_t read_temp_raw_avg(void)
{
    /* 栈上快照：32 个 uint16 = 64 字节，M4 的 8 KB 栈完全够用。
     * 规范要求局部数组用栈变量，不用 malloc，也不动静态区。 */
    uint16_t snapshot[TEMP_RING_SIZE];
    uint32_t sum = 0U;
    uint8_t fill;
    uint8_t count;
    uint8_t i;
    uint8_t cut;
    uint32_t primask;

    /* 关中断把整个环形缓冲抄到栈上。
     *
     * 为什么必须关中断：DMA 中断随时会写入 temp_ring，抄到一半被打断
     * 就会得到"一半旧一半新"的数组 —— 去极值后仍是个假值。
     * 旧实现只抄一个 sum 也要关中断，这里抄 32 个字，临界区变长约 32 倍，
     * 但仍只有几十个指令周期；DMA 块完成间隔约 69 µs，不会被漏掉。 */
    primask = __get_PRIMASK();
    __disable_irq();
    fill = temp_ring_fill;
    for (i = 0U; i < TEMP_RING_SIZE; i++) {
        snapshot[i] = temp_ring[i];
    }
    /* 只有原本开着中断才恢复，别把别人关的中断打开 */
    if (0UL == primask) {
        __enable_irq();
    }

    /* 一个样点都没有（刚上电、还没进过测温相位）：
     * 直接返回 0，避免下面做除法时除以 0。 */
    if (0U == fill) {
        return 0U;
    }

    /* 样本不足 8 个时不排序、不去极值，直接求均值。
     *
     * 为什么定 8：去极值要扔掉 4 个（最小 2 + 最大 2），
     * 剩不下几个点时"去掉极值"就没有统计意义，反而更容易被
     * 本来就偏低的早期样本带跑（测温窗口开头的点还没稳定）。
     * 8 个样点至少留 4 个中间值，且只在上电最初几拍出现。 */
    if (fill < 8U) {
        for (i = 0U; i < fill; i++) {
            sum += snapshot[i];
        }
        return (uint16_t)(sum / (uint32_t)fill);
    }

    /* 插入排序，从小到大。
     *
     * 为什么插入排序而不是快排：窗口每轮只换一个点（旧的走、新的来），
     * 数组天然近乎有序，插入排序在这种数据上内层几乎不进，
     * 且实现短、无递归、无函数指针，符合规范 §1.2。
     *
     * **j 用 uint8_t，内层条件是 `j > 0`（不是 `j >= 0`）**：
     * j 递减到 0 时循环立刻退出，**不会**再减一次回绕到 255，
     * 所以不会越界读 snapshot[-1]。
     *
     * 这里刻意不用 `int8_t j = (int8_t)i - 1`（实测踩过）：
     * i=129 时它算出 j=-128，while 虽被跳过，但循环体后的
     * `snapshot[j+1] = key` 仍会执行，**写到 snapshot[-127]** ——
     * 越界写，实测直接 SIGSEGV。
     * 触发条件是 fill >= 130（当前 TEMP_RING_SIZE=32 够不到，
     * 但属于"埋着等以后有人调大窗口就爆"的类型）。
     * 换成无符号 + `j > 0` 后，扫遍 fill=0..256 均无越界。 */
    for (i = 1U; i < fill; i++) {
        uint16_t key = snapshot[i];
        uint8_t j = i;

        while ((j > 0U) && (snapshot[j - 1U] > key)) {
            snapshot[j] = snapshot[j - 1U];
            j--;
        }
        snapshot[j] = key;
    }

    /* 掐头去尾：扔掉最小 2 个、最大 2 个，固定不变。
     *
     * 为什么是 2 而不是 1：单个尖峰常带"主峰 + 相邻次峰"两个点，
     * 只扔 1 个会留下次峰；扔 2 个能覆盖这种成对干扰。
     * 为什么固定而不按比例：32 个点里剔除 4 个最极端的，
     * 剩 28 个求平均已足够压制 PWM 突发尖峰，按比例反而让
     * 短窗口（fill 刚过 8）剩不下几个有效点、均值更噪。
     *
     * 这里不需要"剩余点数是否够除"的防御：上面已经拦掉 fill < 8，
     * 所以 fill >= 8 → count = fill-4 >= 4，永远不会除以 0。 */
    cut = 2U;
    count = (uint8_t)(fill - (cut * 2U));

    for (i = cut; i < (uint8_t)(fill - cut); i++) {
        sum += snapshot[i];
    }

    return (uint16_t)(sum / (uint32_t)count);
}

void read_heat_averages(uint16_t *current_raw, uint16_t *vin_raw)
{
    uint32_t count;
    uint32_t current;
    uint32_t vin;
    uint32_t primask;

    /* 快照并清零必须是原子的：DMA 中断一插进来就会把新样本算到旧窗口里 */
    primask = __get_PRIMASK();
    __disable_irq();
    count = sum_count;
    current = sum_current;
    vin = sum_vin;
    sum_count = 0UL;
    sum_current = 0UL;
    sum_vin = 0UL;
    /* 只有原本开着中断才恢复 */
    if (0UL == primask) {
        __enable_irq();
    }

    /* 本轮还没采到样本（比如启动第一拍）就返回 0，让上层按"没电流"处理 */
    if (0UL == count) {
        *current_raw = 0U;
        *vin_raw = 0U;
        return;
    }
    *current_raw = (uint16_t)(current / count);
    *vin_raw = (uint16_t)(vin / count);
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
