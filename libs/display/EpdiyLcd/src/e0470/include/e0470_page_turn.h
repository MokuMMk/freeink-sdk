/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase-offset page-reveal engine, ported from the Read Pico vendor example
 * (components/e0470_page_turn). The vendor released it for reuse on the condition
 * that MindReset is credited; that credit lives here, in the implementation, and in
 * the pull request that brought it in.
 *
 * 错相揭页引擎。公开接口仅为本头文件；实现细节（条带划分、LUT 布局）不是调用契约。
 * / The public interface is this header only; the .c's band split and LUT layout are
 * / implementation detail, not a contract.
 */

#pragma once

#include "epd_highlevel.h"
#include "epdiy.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 逻辑屏幕上的揭页方向（与握持方向一致，库内按当前旋转映射到 framebuffer）。
/// / Reveal direction in logical screen coordinates; mapped to the framebuffer through
/// / the current rotation internally.
typedef enum {
    E0470_TURN_LTR = 0,
    E0470_TURN_RTL = 1,
    E0470_TURN_TTB = 2,
    E0470_TURN_BTT = 3,
} e0470_turn_dir_t;

#define E0470_TURN_DEFAULT_TICK_US 21000

const char* e0470_turn_dir_name(e0470_turn_dir_t dir);

/// 每拍目标时长。扫描约 7ms，余量用来看清条带边界；默认 21ms × 52 ≈ 1.1s。
/// / Target duration per tick. A scan is ~7 ms; the remainder is what makes the band
/// / boundaries legible. 21 ms × 52 ≈ 1.1 s by default.
void e0470_page_turn_set_tick_us(int us);
int e0470_page_turn_tick_us(void);

/// `area` 是逻辑坐标。无可用 GL16 时返回 `EPD_DRAW_NO_PHASES_AVAILABLE`，不刷屏。
/// 调用方负责 FAST 扫描与 HV 轨保活。
/// / `area` is in logical coordinates. Returns EPD_DRAW_NO_PHASES_AVAILABLE without
/// / touching the panel when no GL16 table is usable. The caller owns the FAST scan
/// / selection and keeping the HV rails alive across the whole call.
enum EpdDrawError e0470_page_turn(EpdiyHighlevelState* hl, EpdRect area, e0470_turn_dir_t dir);

#ifdef __cplusplus
}
#endif
