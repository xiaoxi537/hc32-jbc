/**
 *******************************************************************************
 * @file  bsp_settings.h
 * @brief 设置项掉电保存（内部 Flash 最后一个扇区）
 *
 * 只负责"按字节读写一块数据"，数据格式和 CRC 由 app_station.c 负责。
 * 地址：0x0003E000（sector 31）。当前固件约 76 KB（v7 全功能版 text+data 77 756 B），不会重叠。
 *******************************************************************************
 */
#ifndef __BSP_SETTINGS_H__
#define __BSP_SETTINGS_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief  从 Flash 读出保存区
 * @param  buffer 目标缓冲，长度至少 size
 * @param  size   字节数；空片读出来是全 0xFF，由上层校验 magic 判断有效性
 * @note   内部 Flash 是直接映射到地址空间的，这里就是一次 memcpy
 */
void read_settings(void *buffer, uint32_t size);

/**
 * @brief  擦除设置扇区并写入
 * @param  buffer 源数据
 * @param  size   字节数，必须是 4 的倍数（EFM 只能按字编程）
 * @retval true 写入完成；false 长度不合法或擦除/编程失败
 * @note   写之前强制关加热：Flash 操作会停 CPU 取指，不能让加热丝保持通电
 */
bool write_settings(const void *buffer, uint32_t size);

#endif /* __BSP_SETTINGS_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
