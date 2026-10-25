/* 本文件由 tools/gen_cjk_font.py 自动生成，请勿手工修改 */
/* 源字体: nk57-monospace.cdrg-bold.otf (face 0) */
/* 字号: 16px  bpp: 2  字形数: 1  行高: 20  基线: 4 */

#include <sgl.h>

/* 字形点阵：行优先，MSB 优先，2 位/像素 */
static const uint8_t arrow16_bitmap[] = {
    0x08,0x00,0x0F,0x40,0x0F,0xE0,0x0F,0xFC,0x0F,0xE0,0x0F,0x40,0x08,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
};

/* 度量表：下标 0 为空白字形 */
static const sgl_font_table_t arrow16_table[2] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* reserved */,
    {.bitmap_index = 0, .adv_w = 124, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0} /* U+25B6 */,
};

/* 码点列表：相对 32 的升序偏移 */
static const uint16_t arrow16_list[1] = {
    9622,
};

static const sgl_font_unicode_t arrow16_unicode = {
    .offset = 32,
    .tab_offset = 1,
    .list = arrow16_list,
    .len = 1,
};

const sgl_font_t arrow16 = {
    .bitmap = arrow16_bitmap,
    .table = arrow16_table,
    .font_table_size = 2,
    .font_height = 20,
    .base_line = 4,
    .bpp = 2,
    .unicode = &arrow16_unicode,
    .unicode_num = 1,
};
