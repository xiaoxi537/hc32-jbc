/**
 *******************************************************************************
 * @file  app_indev.h
 * @brief 按键与菜单状态机（5 ms 节拍）
 *
 * indev = input device，沿用原机任务名；本文件负责按键、编码器和菜单。
 *
 * 操作约定（和原机一致）：
 *   转动 = 选择/调值；短按 = 进入/确认；长按 = 返回/取消
 *   主界面短按 = 循环三个快捷温度；主界面长按 = 打开菜单
 *
 * 联锁：**没插烙铁头时整套输入被屏蔽**（编码器转动与按键都不响应），
 * 并强制回到主页；插上工具后自动恢复。判定用 app_station 的
 * is_tip_present()，与界面显示"无工具"告警页的条件完全一致 ——
 * 两处必须一起改，否则会出现"屏幕说没工具、编码器却还能翻菜单"。
 *******************************************************************************
 */
#ifndef __APP_INDEV_H__
#define __APP_INDEV_H__

#include <stdbool.h>
#include <stdint.h>

/** 初始化按键消抖状态（须在 init_inputs 之后调用） */
void init_indev(void);

/** 5 ms 周期调用：按键事件 → 菜单动作 → 编码器增量 */
void task_indev(void);

/* ---- 供界面渲染查询的菜单描述 ---- */

/** 当前页面有几行可选项 */
uint8_t get_page_item_count(uint8_t mode);

/** 页面标题（静态字符串，可长期持有） */
const char *get_page_title(uint8_t mode);

/** 某一行的名称（静态字符串，可长期持有） */
const char *get_item_name(uint8_t mode, uint8_t index);

/** 参数页的单位；num32 字库不能显示 %，所以单位要单独用 cjk16 画 */
const char *get_param_unit(uint8_t index);

/**
 * @brief  这一行是不是"可进入"的跳转/动作项
 * @retval true  短按会进入下一页或立即执行；界面在右侧画实心三角 ▶
 * @note   界面禁止用 format_item_value() 的返回值去猜（以前返回 ">"，
 *         现在返回空串），要判断就用这个函数。
 */
bool is_item_enterable(uint8_t mode, uint8_t index);

/**
 * @brief  把某一行的当前值写进 buffer 并返回可显示文本
 * @param  buffer 调用方提供的静态缓冲；SGL 只存指针，绝不能传局部变量
 * @param  size   缓冲长度
 * @retval 要显示在右侧的文本；空串表示这一行没有值
 */
const char *format_item_value(uint8_t mode, uint8_t index, char *buffer, uint32_t size);

#endif /* __APP_INDEV_H__ */

/*******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/
