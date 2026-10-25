#ifndef __HC32F460_IT_H
#define __HC32F460_IT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Cortex-M4 内核异常 */
void NMI_Handler(void);
void HardFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

/* 外设中断（用到时再取消注释） */
/* void IRQ039_Handler(void); */   /* DMA2_BTC0 */
/* void IRQ082_Handler(void); */   /* TMRA6_OVF */
/* void IRQ085_Handler(void); */   /* TMRA4_OVF */

#ifdef __cplusplus
}
#endif

#endif /* __HC32F460_IT_H */
