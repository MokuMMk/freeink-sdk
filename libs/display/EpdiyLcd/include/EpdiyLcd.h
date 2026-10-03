/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Board glue for the vendor epdiy LCD fork (LGPL-3.0-or-later).
// Local highlevel changes retain the baseline on draw failure, check allocations,
// and release owned buffers; see test/host/test_transactions.py.
// Linked only on raw parallel targets. Panel VCOM remains the factory PMU value;
// the epdiy set_vcom callback never writes it.

#include <Arduino.h>

namespace freeink {

/// 板级电源钩子。与 LgfxEpdPowerHooks 同形，但定义在这里，避免本库反向依赖
/// FreeInkDisplay（那会成环）。任一可为 nullptr。
/// / Board power hooks. Same shape as LgfxEpdPowerHooks but defined here so this
/// library does not depend back on FreeInkDisplay. powerOn must report successful rail bring-up.
struct EpdiyLcdPowerHooks {
  bool (*prepare)();   ///< 上电前：引脚归安全电平，轨全部关。/ Park pins, rails down.
  bool (*powerOn)();   ///< VCOM 校验通过后才升轨。/ Raise rails after the VCOM check.
  void (*powerOff)();  ///< 掉轨。/ Drop rails.
  /// 面板温度，用来选波形的温度档。nullptr 或返回值不在 0..50 时按 20 °C 处理。
  /// 本板波形只有一个 0–50 °C 档，所以这个值不改变选表结果。
  /// / Panel temperature, used to pick the waveform's temp range. nullptr, or a
  /// value outside 0..50, falls back to 20 °C — this board's table has a single
  /// 0–50 °C range, so the value does not change which phases run.
  float (*getTemperature)();
};

/// 一行四段 + CKV，单位是像素钟个数（ckv 是 0.1µs）。对应 epdiy 的
/// LcdLineTiming_t。行长 = leHighTime + lineFrontPorch + lineData + lineEnd，
/// lineData 由面板宽度和总线宽度推出，不在配置里。
/// / One scan line in four segments plus CKV, in pixel clocks (CKV in 0.1 µs).
/// `lineData` is derived from the panel width / bus width, so it is not here.
struct EpdiyLcdLineTiming {
  int leHighTime;
  int lineFrontPorch;
  int lineEnd;
  int ckvHighTime01us;
};

/// 面板总线接线与扫描参数。几何尺寸不在这里，来自 ACTIVE BoardProfile。
/// / Panel bus wiring and scan parameters. Geometry is not here; it comes from
/// the ACTIVE BoardProfile, like every other driver.
struct EpdiyLcdConfig {
  int8_t dataPins[16];   ///< D0..D15。-1 = 未接（BoardConfig::PIN_UNASSIGNED）。
  int8_t pinClock;       ///< XCL，外部像素钟（LCD_CAM pclk 输出）。
  int8_t pinCkv;         ///< CKV，栅极钟（RMT 产生）。
  int8_t pinStartPulse;  ///< XSTL，接 LCD 的 DE 信号。
  int8_t pinLeh;         ///< XLE，接 LCD 的 HSYNC 信号。
  int8_t pinStv;         ///< SPV，帧起始脉冲。
  uint8_t busWidth;      ///< 8 或 16。
  int pclkMhz;           ///< 像素钟 MHz（本板 18）。
  EpdiyLcdLineTiming line;
  uint8_t prefillLines;  ///< 每相开扫前的预填行数，决定帧间隙。
  EpdiyLcdPowerHooks power;

  // 可选：快速扫描档（本板 10µs/行 = 7.01ms/帧；FULL 是 16µs/行 = 11.22ms/帧）。
  // 错相揭页用它 —— 21ms 的每拍是围绕 7ms 扫描设计的（1/3 驱动 + 2/3 停留），眼睛才看
  // 得清每一带的台阶。
  //
  // `fastPrefillLines` 不能沿用 FULL 的值：一行只有 10µs 时预填 32 行会欠载
  // （EPD_DRAW_EMPTY_LINE_QUEUE，后续帧不再输出），厂商实测要 64 行。
  //
  // `fastLine.leHighTime == 0` 表示这块板子没有快速档，切换请求会被忽略、继续用 FULL。
  // / Optional fast scan profile (this board 10 µs/line = 7.01 ms/frame against FULL's
  // 16 µs/line = 11.22 ms). The phase-offset reveal uses it: its 21 ms tick is built around
  // a 7 ms scan (1/3 drive, 2/3 hold) so each band's step reads clearly.
  //
  // `fastPrefillLines` cannot reuse the FULL value -- at 10 µs/line a 32-line prefill
  // underruns (EPD_DRAW_EMPTY_LINE_QUEUE, later frames stop outputting) and the vendor
  // measured 64. `fastLine.leHighTime == 0` means the board has no fast profile and switch
  // requests are ignored.
  EpdiyLcdLineTiming fastLine = {};
  uint8_t fastPrefillLines = 0;
};

/// 刷新档位，映射到 epdiy 的 MODE_*。/ Refresh profile, mapped onto epdiy MODE_*.
///
/// TextTurn 与 Half 同为 GL16，但用 E0470_TEXTTURN_WAVEFORM：对角线全保持，未变化的
/// 像素完全不驱动。原地重推一个黑像素会先擦白再推黑，那正是翻页可见的白闪；抗锯齿
/// 文字页的常规翻页用它，静止内容的刷新交给周期性 GC16。
/// / TextTurn is GL16 like Half but drives E0470_TEXTTURN_WAVEFORM, whose diagonal is
/// entirely held so unchanged pixels are not driven at all. Re-driving a black pixel in
/// place erases it white first, which is the white flash a turn shows; ordinary
/// anti-aliased text turns use it, and static content is refreshed by the periodic GC16.
enum class EpdiyLcdRefresh : uint8_t { Full, Half, Fast, TextTurn };

/// 初始化总线并挂上波形。width/height 是面板扫描尺寸（本板 1216x684）。
///
/// `blackIsOne` 在这里给定一次，说明调用方的 1bpp 帧缓冲里 1 代表黑还是白；内部
/// 需要同一个约定去建开机白场，所以不再每次推帧重复传。
///
/// 失败返回 false（例如 PSRAM 里的 4bpp 帧缓冲分配不出来）。
/// / Bring up the bus and attach the waveform. Returns false on failure (e.g. the
/// 4 bpp framebuffer could not be allocated in PSRAM). `blackIsOne` fixes the
/// caller's 1 bpp bit convention once, because the internal boot-time white field
/// must be built with that same convention.
bool epdiyLcdBegin(const EpdiyLcdConfig& cfg, uint16_t width, uint16_t height, bool blackIsOne);

/// 释放总线与帧缓冲。/ Release the bus and the framebuffer.
void epdiyLcdEnd();

// Borrow the existing front framebuffer. No allocation. Commit uses GL16 and
// promotes an unknown baseline to GC16; cancellation never changes the glass.
uint8_t* epdiyLcdBeginGrayscale16();
bool epdiyLcdCommitGrayscale16(const uint8_t* bwProxy);
void epdiyLcdCancelGrayscale16();

/// 推一帧。`fb` 是 1bpp、MSB 在前、每行 width/8 字节、height 行；位约定由
/// epdiyLcdBegin 的 `blackIsOne` 给定。
/// / Push one frame. `fb` is 1 bpp MSB-first, width/8 bytes per row, height rows;
/// the bit convention is the one handed to epdiyLcdBegin.
bool epdiyLcdDraw(const uint8_t* fb, EpdiyLcdRefresh mode, bool turnOff);

/// 错相揭页：把 `fb` 合成进新页后，用 e0470_page_turn() 逐带揭开而不是一次差分推送。
/// `dir` 是 e0470_turn_dir_t（LTR/RTL/TTB/BTT），逻辑方向，引擎按当前旋转映射。
///
/// 走**快速扫描档**（本板 10µs/行 = 7.01ms/帧），调用期间临时切换、返回前还原 ——
/// 21ms 的每拍是围绕 7ms 扫描设计的（1/3 驱动 + 2/3 停留），那个"停留"才是条带台阶
/// 看得清的原因。首帧或上次推送失败时没有可揭的底图，退化为普通推送。
/// / Phase-offset page reveal: compose `fb` into the new page, then stagger the reveal
/// across 16 bands through e0470_page_turn() instead of one difference push. `dir` is an
/// e0470_turn_dir_t in logical coordinates; the engine maps it through the rotation.
///
/// Runs the **fast scan profile** (10 µs/line = 7.01 ms/frame on this board), switched for
/// the duration of the call and restored before returning: the engine's 21 ms tick is built
/// around a 7 ms frame (1/3 drive, 2/3 hold) and that hold is what makes each band's step
/// legible. Falls back to a plain push when there is no valid front frame to reveal over.
bool epdiyLcdPageTurn(const uint8_t* fb, int dir, bool turnOff);

/// 切换面板扫描档位。FAST 只给错相揭页用（见 epdiyLcdPageTurn），其余路径都走 FULL ——
/// FULL 的行周期垫到波形标定的帧周期，驱动量才与波形表一致。两块板子没有快速档时忽略
/// （`fastLine.leHighTime == 0`）：档位是提示，不是契约。
/// / Switch the panel scan profile. FAST is only for the phase-offset reveal (see
/// epdiyLcdPageTurn); every other path runs FULL, whose line period is padded to the
/// waveform's calibrated frame period so the drive matches the table. Ignored on boards
/// without a fast profile (fastLine.leHighTime == 0): the profile is a hint, not a contract.
void epdiyLcdUseScan(bool fast);

/// 只把这一页留作底图，不推屏。给「底图与灰阶合并成一次波形」的宿主用：宿主随后调用
/// epdiyLcdDrawGray()，由它用这张底图合成整页并只推一次。
/// / Stash this page as the base WITHOUT presenting it. For hosts that combine the
/// base and the grey planes into one waveform: they then call epdiyLcdDrawGray(),
/// which composes the whole page from this base and presents it once.
void epdiyLcdStashBase(const uint8_t* fb);

/// 推一帧中间灰：以最近一次 epdiyLcdDraw() 的黑白页为底，再用 LSB/MSB 选择平面对
/// 被选中的像素做中间灰覆盖。
///
/// 注意 `fb` 不在这里：anti-aliasing 提交时，调用方手里那个缓冲装的是**最后写入的
/// 选择平面**，不是页面（平面背景 0、灰标记 1，与页面互补），把它当页面推上去得到
/// 的是负片。LgfxEpdDriver.cpp:187-201 为同一个坑留过注释，这里照它的结论保存底图。
///
/// English: Push a mid-gray frame: start from the B/W page of the last
/// epdiyLcdDraw() and overlay the pixels the LSB/MSB selector planes pick. `fb` is
/// deliberately absent — at anti-aliasing commit time the caller's buffer holds the
/// LAST SELECTOR PLANE, not the page (plane background 0, gray marks 1, i.e. the
/// complement of the page), so pushing it yields a negative. LgfxEpdDriver.cpp:187-201
/// documents the same trap; this keeps a base image for the same reason.
bool epdiyLcdDrawGray(const uint8_t* lsb, const uint8_t* msb, EpdiyLcdRefresh mode, bool turnOff);

/// 进入低功耗：掉轨并释放 LCD_CAM/GDMA/RMT。
/// / Enter low power: drop the rails and release LCD_CAM/GDMA/RMT.
void epdiyLcdDeepSleep();

/// 上一帧是否真的推到了面板（调试/对账用）。
/// / Whether the last frame actually reached the panel (diagnostics).
bool epdiyLcdReady();

}  // namespace freeink
