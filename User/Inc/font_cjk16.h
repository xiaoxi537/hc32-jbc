/**
 *******************************************************************************
 * @file  font_cjk16.h
 * @brief 16px 中英混排小字（汉字=思源黑体 HW Bold，ASCII/符号=NK57 Cd Rg）
 *
 * 由 tools/gen_fonts.sh 调用 tools/gen_cjk_font.py 生成，勿手改。
 * 字符集文件：tools/ui_charset.txt（只含界面真正渲染的 143 字）
 *
 * 这是**混排字库**：汉字字形取自 SourceHanSansHWSC-Bold.otf（思源黑体
 * 半宽简体中文 Bold），ASCII 与 ° % . 等符号取自 NK57 Cd Rg。
 * 两套字形在生成期合并，
 * 因此一个标签就能渲染 "100°C" / "30分" 这类中英混排（SGL 没有字体回退）。
 * 前进宽度：汉字 16 px、ASCII/符号 8 px；行盒统一 20/4（取自 NK57）。
 * 字符集只含界面真正渲染的 143 字（汉字 103 + 拉丁 40），
 * bpp=2（实机 2bpp 与 4bpp 肉眼无差，2bpp 省一半点阵）。
 * 排版常量见 tools/font_metrics.py。
 *******************************************************************************
 */
#ifndef __FONT_CJK16_H__
#define __FONT_CJK16_H__

#include <sgl.h>

extern const sgl_font_t cjk16;

#endif /* __FONT_CJK16_H__ */
