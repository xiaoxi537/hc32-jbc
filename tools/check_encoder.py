#!/usr/bin/env python3
"""
编码器方向自检：把 bsp_input.c 的正交解码逻辑在主机侧重放一遍，
确认"A 相领先"对应加、"B 相领先"对应减，且与逆向报告的状态表一致。

为什么单独做这个脚本：
  编码器方向错了只会在实机上表现成"转的方向和界面变化相反"，
  主机侧 host_test 用的是 take_encoder_delta() 桩函数，
  **测不到 bsp_input.c 里的位序**。这条错误曾经真的发生过一次
  （read_encoder 把 A/B 的 bit 位置写反，两个方向整体对调），
  所以这里把逻辑抄一份独立验证，改动 bsp_input.c 后跑一次即可。

对照基准是逆向报告 analysis/input_storage.md §2.2 的 16 项状态表，
来自原机 encoder_poll(0xA002) 的反汇编：
    idx = (prev<<2) | (A<<1) | B
    0x2814 = {2,4,11,13}  -> 加
    0x4182 = {1,7,8,14}   -> 减

用法：
  python3 tools/check_encoder.py
"""

import sys

# 与 bsp_input.c 完全一致
ENCODER_POSITIVE = 0x2814
ENCODER_NEGATIVE = 0x4182
STEPS_PER_DETENT = 4

# 逆向报告 §2.2 的 16 项状态表：idx -> 累加方向
# '-' 表示非法跳变或状态不变（两相同时变化 / 没有变化）
DOC_TABLE = {
    0: '-',  1: 'NEG', 2: 'POS', 3: '-',
    4: 'POS', 5: '-',  6: '-',  7: 'NEG',
    8: 'NEG', 9: '-', 10: '-', 11: 'POS',
    12: '-', 13: 'POS', 14: 'NEG', 15: 'RESET',
}


def encode_state(pin_a, pin_b):
    """复现 bsp_input.c 的 read_encoder()：**bit0=B、bit1=A**

    这一条就是历史 bug 的位置：曾经写成 bit0=A、bit1=B，
    会让两个转动方向整体对调。改这里必须先改 README 里的说明。
    """
    state = 0
    if pin_b:
        state |= 0x01
    if pin_a:
        state |= 0x02
    return state


def classify(prev_state, cur_state):
    """复现 tick_encoder() 的方向判定"""
    idx = (prev_state << 2) | cur_state
    if (1 << idx) & ENCODER_POSITIVE:
        return 'POS', idx
    if (1 << idx) & ENCODER_NEGATIVE:
        return 'NEG', idx
    if idx == 0xF:
        return 'RESET', idx
    return '-', idx


class Decoder:
    """完整复现 tick_encoder()，含 4 边沿一格与限幅"""

    def __init__(self):
        self.prev = encode_state(0, 0)
        self.acc = 0
        self.delta = 0

    def step(self, pin_a, pin_b):
        cur = encode_state(pin_a, pin_b)
        kind, _ = classify(self.prev, cur)
        if kind == 'POS':
            self.acc += 1
        elif kind == 'NEG':
            self.acc -= 1
        self.prev = cur
        if self.acc >= STEPS_PER_DETENT:
            self.acc = 0
            self.delta += 1
        elif self.acc <= -STEPS_PER_DETENT:
            self.acc = 0
            self.delta -= 1
        return self.delta

    def feed(self, seq):
        """喂一串 (A,B) 采样，返回最终 delta"""
        for a, b in seq:
            self.step(a, b)
        return self.delta


def check_table():
    """16 项状态表必须与逆向报告逐项吻合"""
    bad = []
    for prev in range(4):
        for cur in range(4):
            idx = (prev << 2) | cur
            kind, _ = classify(prev, cur)
            if kind != DOC_TABLE[idx]:
                bad.append((idx, DOC_TABLE[idx], kind))
    return bad


def main():
    fails = 0

    print("== 1. 16 项状态表 vs 逆向报告 §2.2 ==")
    bad = check_table()
    if bad:
        for idx, want, got in bad:
            print(f"  [FAIL] idx={idx:2d} 文档={want} 实际={got}")
        fails += len(bad)
    else:
        print("  [OK  ] 16 项全部吻合")
        print(f"  [OK  ] POS 覆盖 {sorted(i for i in range(16) if ENCODER_POSITIVE >> i & 1)}")
        print(f"  [OK  ] NEG 覆盖 {sorted(i for i in range(16) if ENCODER_NEGATIVE >> i & 1)}")

    # 一个完整四相周期 = 一格。A 领先 = 顺时针，B 领先 = 逆时针
    cw = [(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)]    # (A,B)：A 先动
    ccw = [(0, 0), (0, 1), (1, 1), (1, 0), (0, 0)]   # (A,B)：B 先动

    print("\n== 2. 转动方向 ==")
    d = Decoder().feed(cw * 1)
    ok = (d == 1)
    print(f"  [{'OK  ' if ok else 'FAIL'}] A 相领先（顺时针）一格 -> delta={d}（应为 +1）")
    fails += 0 if ok else 1

    d = Decoder().feed(ccw * 1)
    ok = (d == -1)
    print(f"  [{'OK  ' if ok else 'FAIL'}] B 相领先（逆时针）一格 -> delta={d}（应为 -1）")
    fails += 0 if ok else 1

    # 连续两格、混合方向
    d = Decoder().feed(cw * 2)
    ok = (d == 2)
    print(f"  [{'OK  ' if ok else 'FAIL'}] 连续顺时针两格 -> delta={d}（应为 +2）")
    fails += 0 if ok else 1

    d = Decoder().feed(cw * 3 + ccw * 1)
    ok = (d == 2)
    print(f"  [{'OK  ' if ok else 'FAIL'}] 顺 3 格再逆 1 格 -> delta={d}（应为 +2）")
    fails += 0 if ok else 1

    print("\n== 3. 抖动抑制 ==")
    # 只抖一格边沿（A 跳一下就回来）：不满 4 个边沿，不应产生事件
    d = Decoder().feed([(0, 0), (1, 0), (0, 0)])
    ok = (d == 0)
    print(f"  [{'OK  ' if ok else 'FAIL'}] 单边沿抖动 -> delta={d}（应为 0）")
    fails += 0 if ok else 1

    if fails:
        print(f"\n存在失败（{fails} 项）")
        return 1
    print("\n全部通过（失败 0 项）")
    print("提示：A 相领先 => 加（顺时针）；若实机相反，先查 read_encoder 的 A/B 位序，"
          "不要直接交换 POS/NEG 掩码表 —— 那张表是原机反汇编验证过的。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
