/**
 *******************************************************************************
 * @file  bsp_clk.c
 * @brief 时钟初始化：XTAL 16 MHz → MPLL 400 MHz → HCLK 200 MHz
 *
 * 顺序照抄原机 app_hardware_init（已实机核对）：
 *   解除写保护 → SRAM 等待 → Flash FLWT=5 → 分频 → 晶振起振 → MPLL → 切 PLL
 *
 * 顺序依据 RM §7.4.1：先把等待周期加够，再提主频；反过来会在高速下取指出错。
 *******************************************************************************
 */
#include "bsp_clk.h"
#include "hc32_ll.h"

/* 只留 3 条最关键的编译期断言（全工程合计 3 条），防止有人改时钟宏不改实现。 */

/* 板上是 16 MHz 晶振。忘了 -DXTAL_VALUE=16000000UL 时 DDL 会按 8 MHz 算，
 * SystemCoreClock 变成 100 MHz，SysTick 和所有延时都快一倍。 */
_Static_assert(XTAL_VALUE == CLOCK_XTAL_HZ,
               "XTAL_VALUE != 16 MHz, check -DXTAL_VALUE=16000000UL");

/* 1 ms 节拍的 LOAD 必须正好 200000-1。它是加热/测温节拍的基准，
 * 算错会让 PID 周期从 25.3 ms 变成别的值，整定参数全部失效。 */
_Static_assert((CLOCK_HCLK_HZ / TICK_HZ) == 200000UL,
               "SysTick reload must be 199999 to keep the 25.3 ms heat cycle");

/**
 * @brief  回读时钟相关寄存器，确认 DDL 真的写进去了
 * @retval true 全部吻合；false 至少一项不符
 */
static bool check_clock_target(void)
{
    uint32_t scfgr = READ_REG32(CM_CMU->SCFGR) & CLOCK_SCFGR_DIV_MASK;
    uint8_t ckswr = READ_REG8_BIT(CM_CMU->CKSWR, CMU_CKSWR_CKSW);
    uint32_t pllcfgr = READ_REG32(CM_CMU->PLLCFGR);
    uint32_t flash_wait = READ_REG32_BIT(CM_EFM->FRMC, EFM_FRMC_FLWT);
    uint32_t sram_wait = READ_REG32(CM_SRAMC->WTCR);

    /* 任意一项不符都说明宏和芯片不匹配，不能继续往下跑外设 */
    return (CLOCK_SCFGR_DIV == scfgr) &&
           (CLK_SYSCLK_SRC_PLL == ckswr) &&
           (CLOCK_PLLCFGR == pllcfgr) &&
           (CLOCK_FLASH_WAIT == flash_wait) &&
           (CLOCK_SRAM_WTCR == sram_wait) &&
           (CLOCK_HCLK_HZ == SystemCoreClock);
}

bool init_clock(void)
{
    stc_clock_xtal_init_t xtal_init;
    stc_clock_pll_init_t pll_init;

    /* CMU 的 SCFGR/PLLCFGR/CKSWR/XTAL 都在 PWC_UNLOCK_CODE0 后面，
     * 不解锁的写入会被硬件直接丢掉，现象是"代码跑了但频率没变"。 */
    PWC_REG_Unlock(PWC_UNLOCK_CODE0);

    /* 先把 4 块 SRAM 都设成 1 等待，再把 SRAMH 放宽到 0 等待；
     * 漏掉这一步，200 MHz 下读写 SRAM 会偶发错数据。 */
    SRAM_REG_Unlock();
    SRAM_SetWaitCycle(SRAM_SRAM_ALL, SRAM_WAIT_CYCLE1, SRAM_WAIT_CYCLE1);
    SRAM_SetWaitCycle(SRAM_SRAMH, SRAM_WAIT_CYCLE0, SRAM_WAIT_CYCLE0);
    SRAM_REG_Lock();

    /* Flash 等待周期必须在提频之前写好，否则 200 MHz 下取指不可靠 */
    EFM_REG_Unlock();
    /* FLWT 写失败说明写保护没解开，回锁并让上层停机 */
    if (LL_OK != EFM_SetWaitCycle(CLOCK_FLASH_WAIT)) {
        EFM_REG_Lock();
        PWC_REG_Lock(PWC_UNLOCK_CODE0);
        return false;
    }
    EFM_REG_Lock();

    /* 晶振不起振就没有 PLL 输入，后面所有配置都是空转，直接报失败 */
    (void)CLK_XtalStructInit(&xtal_init);
    xtal_init.u8State = CLK_XTAL_ON;
    xtal_init.u8StableTime = CLK_XTAL_STB_2MS;
    /* 晶振不起振就没有 PLL 输入，后面配置全是空转 */
    if (LL_OK != CLK_XtalInit(&xtal_init)) {
        PWC_REG_Lock(PWC_UNLOCK_CODE0);
        return false;
    }

    /* 此时系统时钟还是内部 MRC，改分频不会踩到 PLL 切换限制 */
    CLK_SetClockDiv(CLK_BUS_CLK_ALL, CLOCK_SCFGR_DIV);

    /* PLLCFGR 一次写全 PLLSRC/PLLM/N/R/Q/P；使能后 DDL 会等 XTAL 和 MPLL 稳定 */
    (void)CLK_PLLStructInit(&pll_init);
    pll_init.PLLCFGR = CLOCK_PLLCFGR;
    pll_init.u8PLLState = CLK_PLL_ON;
    /* PLL 锁不住就不能切系统时钟，直接报失败 */
    if (LL_OK != CLK_PLLInit(&pll_init)) {
        PWC_REG_Lock(PWC_UNLOCK_CODE0);
        return false;
    }

    /* 切到 MPLL 后 DDL 会更新 SystemCoreClock，SysTick 的分频就靠这个值 */
    CLK_SetSysClockSrc(CLK_SYSCLK_SRC_PLL);

    PWC_REG_Lock(PWC_UNLOCK_CODE0);

    /* 留一条硬校验：时钟没配对就让 main 停在死循环，
     * 绝不能带着错误时序去驱动 SPI 屏和加热丝。 */
    return check_clock_target();
}

bool init_tick(uint32_t rate_hz)
{
    /* LOAD 只有 24 位，而且本设计的调度周期就是 1 ms，不接受其它值 */
    if ((0UL == rate_hz) || (rate_hz > TICK_HZ)) {
        return false;
    }

    systick_ms = 0UL;

    /* SysTick_Config 内部算 LOAD = SystemCoreClock/rate - 1，
     * 返回值非 0 表示重装值超出 24 位范围。 */
    return 0UL == SysTick_Config(SystemCoreClock / rate_hz);
}

uint32_t get_tick_ms(void)
{
    return systick_ms;
}

void wait_ms(uint32_t ms)
{
    uint32_t start = systick_ms;

    /* 用无符号减法比较，跨过 49.7 天溢出也不会卡死 */
    while ((systick_ms - start) < ms) {
    }
}

void reset_mcu(void)
{
    NVIC_SystemReset();
}

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
