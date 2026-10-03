/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 错相揭页引擎实现。16 带、每像素一次完整 GL16；差分只算一次，
 * 每拍只换各带对应相的 1K LUT。
 *
 * Ported from the Read Pico vendor example (components/e0470_page_turn). The vendor
 * released it for reuse on the condition that MindReset is credited; that credit is
 * this notice, e0470_page_turn.h, and the pull request that brought it in. The body
 * below is the vendor's implementation unmodified.
 */

#include "e0470_page_turn.h"

#include <stdbool.h>
#include <string.h>

#include "e0470_epaper_waveform.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char* TAG = "e0470_turn";

#define TURN_BANDS 16
#define TURN_PHASE_CAP 40
#define TURN_LINE_MAX 2048

static uint8_t s_lut[TURN_PHASE_CAP][1024] DRAM_ATTR __attribute__((aligned(16)));
static const uint8_t* s_lut_ptr[TURN_PHASE_CAP];
static int s_band0[TURN_BANDS];
static int s_band1[TURN_BANDS];
static int8_t s_band_phase[TURN_BANDS];
static int8_t s_line_phase[TURN_LINE_MAX] DRAM_ATTR;
static const EpdWaveformPhases* s_lut_src;
static int s_lut_n;
static int s_tick_us = E0470_TURN_DEFAULT_TICK_US;

const char* e0470_turn_dir_name(e0470_turn_dir_t dir) {
    switch (dir) {
        case E0470_TURN_LTR: return "ltr";
        case E0470_TURN_RTL: return "rtl";
        case E0470_TURN_TTB: return "ttb";
        case E0470_TURN_BTT: return "btt";
        default: return "?";
    }
}

void e0470_page_turn_set_tick_us(int us) {
    if (us < 0) us = 0;
    s_tick_us = us;
}

int e0470_page_turn_tick_us(void) {
    return s_tick_us;
}

static void clip_rect(EpdRect* r, int w, int h) {
    if (r->x < 0) {
        r->width += r->x;
        r->x = 0;
    }
    if (r->y < 0) {
        r->height += r->y;
        r->y = 0;
    }
    if (r->x + r->width > w) r->width = w - r->x;
    if (r->y + r->height > h) r->height = h - r->y;
}

static EpdRect rotate_to_fb(EpdRect rect) {
    const int pw = epd_width();
    const int ph = epd_height();
    const int lx0 = rect.x;
    const int ly0 = rect.y;
    const int lx1 = rect.x + rect.width - 1;
    const int ly1 = rect.y + rect.height - 1;
    int ax, ay, bx, by;

    switch (epd_get_rotation()) {
        case EPD_ROT_LANDSCAPE:
            ax = lx0;
            ay = ly0;
            bx = lx1;
            by = ly1;
            break;
        case EPD_ROT_PORTRAIT:
            ax = pw - ly0 - 1;
            ay = lx0;
            bx = pw - ly1 - 1;
            by = lx1;
            break;
        case EPD_ROT_INVERTED_LANDSCAPE:
            ax = pw - lx0 - 1;
            ay = ph - ly0 - 1;
            bx = pw - lx1 - 1;
            by = ph - ly1 - 1;
            break;
        default:
            ax = ly0;
            ay = ph - lx0 - 1;
            bx = ly1;
            by = ph - lx1 - 1;
            break;
    }

    const int x0 = ax < bx ? ax : bx;
    const int x1 = ax < bx ? bx : ax;
    const int y0 = ay < by ? ay : by;
    const int y1 = ay < by ? by : ay;
    return (EpdRect){ .x = x0, .y = y0, .width = x1 - x0 + 1, .height = y1 - y0 + 1 };
}

static void phys_to_logical(int px, int py, int* lx, int* ly) {
    const int pw = epd_width();
    const int ph = epd_height();
    switch (epd_get_rotation()) {
        case EPD_ROT_LANDSCAPE:
            *lx = px;
            *ly = py;
            break;
        case EPD_ROT_PORTRAIT:
            *lx = py;
            *ly = pw - px - 1;
            break;
        case EPD_ROT_INVERTED_LANDSCAPE:
            *lx = pw - px - 1;
            *ly = ph - py - 1;
            break;
        default:
            *lx = ph - py - 1;
            *ly = px;
            break;
    }
}

static bool dir_is_lr(e0470_turn_dir_t dir) {
    return dir == E0470_TURN_LTR || dir == E0470_TURN_RTL;
}

static bool dir_is_reverse(e0470_turn_dir_t dir) {
    return dir == E0470_TURN_RTL || dir == E0470_TURN_BTT;
}

// 错相轴若落在物理 y 上，整行共用一个相位；落在物理 x 上则一行内分段。
static bool uses_line_phase(e0470_turn_dir_t dir) {
    const bool lr = dir_is_lr(dir);
    switch (epd_get_rotation()) {
        case EPD_ROT_LANDSCAPE:
        case EPD_ROT_INVERTED_LANDSCAPE:
            return !lr;
        default:
            return lr;
    }
}

static int band_of(int coord, int origin, int span, int bands, bool reverse) {
    int band = (coord - origin) * bands / span;
    if (band < 0) band = 0;
    if (band >= bands) band = bands - 1;
    return reverse ? bands - 1 - band : band;
}

static void clear_bands(int* b0, int* b1, int n) {
    for (int i = 0; i < n; i++) {
        b0[i] = 0;
        b1[i] = 0;
    }
}

static void assign_bands(
    int* b0,
    int* b1,
    int bands,
    int from,
    int to,
    int step,
    bool along_x,
    int origin,
    int span,
    bool lr,
    bool reverse
) {
    clear_bands(b0, b1, bands);
    int cur = -1;
    int run = from;
    for (int p = from; p < to; p += step) {
        int lx, ly;
        if (along_x) phys_to_logical(p, 0, &lx, &ly);
        else phys_to_logical(0, p, &lx, &ly);
        const int coord = lr ? lx : ly;
        const int band = band_of(coord, origin, span, bands, reverse);
        if (band != cur) {
            if (cur >= 0) {
                b0[cur] = run;
                b1[cur] = p;
            }
            cur = band;
            run = p;
        }
    }
    if (cur >= 0) {
        b0[cur] = run;
        b1[cur] = to;
    }
}

static void build_luts(const EpdWaveformPhases* gl) {
    if (s_lut_src == gl && s_lut_n == gl->phases) return;
    const int n = gl->phases < TURN_PHASE_CAP ? gl->phases : TURN_PHASE_CAP;
    for (int phase = 0; phase < n; phase++) {
        epd_build_1ppB_lut_1k(s_lut[phase], gl, phase);
        s_lut_ptr[phase] = s_lut[phase];
    }
    s_lut_src = gl;
    s_lut_n = gl->phases;
}

static void copy_front_to_back(EpdiyHighlevelState* hl, EpdRect phys) {
    const int w = epd_width();
    const int h = epd_height();
    clip_rect(&phys, w, h);
    if (phys.width <= 0 || phys.height <= 0) return;

    for (int y = phys.y; y < phys.y + phys.height; y++) {
        uint8_t* src = hl->front_fb + y * w / 2;
        uint8_t* dst = hl->back_fb + y * w / 2;
        int x = phys.x;
        int x_last = phys.x + phys.width - 1;
        if (x & 1) {
            dst[x / 2] = (uint8_t)((src[x / 2] & 0xF0) | (dst[x / 2] & 0x0F));
            x++;
        }
        if ((x_last & 1) == 0) {
            dst[x_last / 2] = (uint8_t)((src[x_last / 2] & 0x0F) | (dst[x_last / 2] & 0xF0));
            x_last--;
        }
        if (x_last >= x) memcpy(dst + x / 2, src + x / 2, (size_t)((x_last - x + 1) / 2));
    }
}

enum EpdDrawError e0470_page_turn(EpdiyHighlevelState* hl, EpdRect area, e0470_turn_dir_t dir) {
    if (hl == NULL) return EPD_DRAW_NO_PHASES_AVAILABLE;
    if (dir > E0470_TURN_BTT) dir = E0470_TURN_RTL;

    const EpdWaveformPhases* gl = e0470_waveform_phases(&E0470_WAVEFORM, MODE_GL16);
    if (gl == NULL || gl->luts == NULL || gl->phases <= 0 || gl->phases > TURN_PHASE_CAP) {
        return EPD_DRAW_NO_PHASES_AVAILABLE;
    }

    const int bands = TURN_BANDS;
    const int nphase = gl->phases;
    const int ticks = bands + nphase - 1;
    const int fb_w = epd_width();
    const int fb_h = epd_height();
    if (fb_h > TURN_LINE_MAX) return EPD_DRAW_INVALID_CROP;

    EpdRect phys = rotate_to_fb(area);
    clip_rect(&phys, fb_w, fb_h);
    if (phys.width <= 0 || phys.height <= 0) return EPD_DRAW_SUCCESS;

    const int y0 = phys.y;
    const int y1 = phys.y + phys.height;
    const bool lr = dir_is_lr(dir);
    const bool reverse = dir_is_reverse(dir);
    const bool line_ph = uses_line_phase(dir);
    const int origin = lr ? area.x : area.y;
    const int span = lr ? (area.width > 0 ? area.width : 1) : (area.height > 0 ? area.height : 1);

    if (line_ph) {
        assign_bands(
            s_band0, s_band1, bands, y0, y1, 1, false, origin, span, lr, reverse
        );
    } else {
        const int x_lo = (phys.x + 15) & ~15;
        const int x_hi = (phys.x + phys.width) & ~15;
        assign_bands(
            s_band0, s_band1, bands, x_lo, x_hi, 16, true, origin, span, lr, reverse
        );
    }

    build_luts(gl);
    epd_poweron();

    const int64_t t_all = esp_timer_get_time();
    const int64_t t_gen = esp_timer_get_time();
    epd_difference_image_cropped(
        hl->front_fb, hl->back_fb, phys, hl->difference_fb, hl->dirty_lines, hl->dirty_columns
    );
    const int64_t gen_us = esp_timer_get_time() - t_gen;

    int64_t draw_us = 0;
    enum EpdDrawError err = EPD_DRAW_SUCCESS;
    for (int tick = 0; tick < ticks; tick++) {
        const int64_t t_tick = esp_timer_get_time();
        memset(hl->dirty_lines, 0, sizeof(bool) * (size_t)fb_h);
        memset(hl->dirty_columns, 0, (size_t)fb_w / 2);
        memset(s_band_phase, -1, (size_t)bands);
        if (line_ph) memset(s_line_phase, -1, (size_t)fb_h);

        bool any = false;
        for (int band = 0; band < bands; band++) {
            const int phase = tick - band;
            if (phase < 0 || phase >= nphase) continue;
            const int a = s_band0[band];
            const int b = s_band1[band];
            if (a >= b) continue;
            s_band_phase[band] = (int8_t)phase;
            if (line_ph) {
                memset(hl->dirty_lines + a, 1, (size_t)(b - a));
                memset(s_line_phase + a, (int)(int8_t)phase, (size_t)(b - a));
            } else {
                memset(hl->dirty_columns + a / 2, 0xFF, (size_t)((b - a) / 2));
            }
            any = true;
        }
        if (!any) continue;

        if (line_ph) {
            int xs = phys.x & ~1;
            int xe = (phys.x + phys.width + 1) & ~1;
            if (xe > xs) {
                memset(hl->dirty_columns + xs / 2, 0xFF, (size_t)((xe - xs) / 2));
            }
            epd_set_line_phase_luts(s_lut_ptr, s_line_phase);
        } else {
            memset(hl->dirty_lines + y0, 1, (size_t)(y1 - y0));
            epd_set_col_phase_luts(s_lut_ptr, s_band0, s_band1, s_band_phase, bands);
        }

        const int64_t t_draw = esp_timer_get_time();
        err = epd_draw_base(
            epd_full_screen(),
            hl->difference_fb,
            epd_full_screen(),
            MODE_PACKING_1PPB_DIFFERENCE | MODE_DU,
            25,
            hl->dirty_lines,
            hl->dirty_columns,
            &E0470_APPLY_WAVEFORM
        );
        draw_us += esp_timer_get_time() - t_draw;
        if (err != EPD_DRAW_SUCCESS) break;
        const int64_t used = esp_timer_get_time() - t_tick;
        if (s_tick_us > 0 && used < s_tick_us) {
            esp_rom_delay_us((uint32_t)(s_tick_us - used));
        }
    }

    copy_front_to_back(hl, phys);
    ESP_LOGI(
        TAG,
        "dir=%s ticks=%d gen=%d ms draw=%d ms wall=%d ms",
        e0470_turn_dir_name(dir),
        ticks,
        (int)(gen_us / 1000),
        (int)(draw_us / 1000),
        (int)((esp_timer_get_time() - t_all) / 1000)
    );
    return err;
}
