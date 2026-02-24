/*
 * renderer.c — EmuManual framebuffer rendering
 *
 * C64-inspired blue theme. Uses font8x8_basic (8x8 bitmaps doubled
 * vertically to 16px) for the classic chunky retro look.
 */
#include <string.h>
#include <stdio.h>
#include "renderer.h"
#include "font8x8_basic.h"
#include "mascot_data.h"

static uint32_t *fb;

/* ── Primitives ──────────────────────────────────────────────── */

void render_init(uint32_t *framebuffer)
{
    fb = framebuffer;
}

static void render_glyph(int px, int py, unsigned char ch, uint32_t color)
{
    if (ch >= 128) ch = '?';
    for (int row = 0; row < 8; row++) {
        unsigned char bits = (unsigned char)font8x8_basic[ch][row];
        int sy0 = py + row * 2;
        int sy1 = sy0 + 1;
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                int sx = px + col;
                if (sx >= 0 && sx < SCREEN_W) {
                    if (sy0 >= 0 && sy0 < SCREEN_H)
                        fb[sy0 * SCREEN_W + sx] = color;
                    if (sy1 >= 0 && sy1 < SCREEN_H)
                        fb[sy1 * SCREEN_W + sx] = color;
                }
            }
        }
    }
}

static void fill_rect(int x0, int y0, int x1, int y1, uint32_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            fb[y * SCREEN_W + x] = color;
}

void render_clear(void)
{
    /* Fill entire screen with border color */
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        fb[i] = COL_BORDER;

    /* Fill inner text area with background */
    int x0 = BORDER_COLS * GLYPH_W;
    int y0 = BORDER_ROWS * GLYPH_H;
    int x1 = (TERM_COLS - BORDER_COLS) * GLYPH_W;
    int y1 = (TERM_ROWS - BORDER_ROWS) * GLYPH_H;
    fill_rect(x0, y0, x1, y1, COL_BG);
}

/* ── Text drawing ────────────────────────────────────────────── */

void render_text(int col, int row, const char *str, uint32_t color)
{
    int scol = col + BORDER_COLS;
    int srow = row + BORDER_ROWS;
    for (int i = 0; str[i] && (col + i) < TEXT_COLS; i++) {
        int px = (scol + i) * GLYPH_W;
        int py = srow * GLYPH_H;
        render_glyph(px, py, (unsigned char)str[i], color);
    }
}

void render_text_inv(int col, int row, const char *str,
                     uint32_t fg, uint32_t bg)
{
    int scol = col + BORDER_COLS;
    int srow = row + BORDER_ROWS;

    /* Fill background for the full row width */
    int px0 = scol * GLYPH_W;
    int py0 = srow * GLYPH_H;
    fill_rect(px0, py0, px0 + TEXT_COLS * GLYPH_W, py0 + GLYPH_H, bg);

    /* Draw text */
    for (int i = 0; str[i] && (col + i) < TEXT_COLS; i++) {
        int px = (scol + i) * GLYPH_W;
        render_glyph(px, py0, (unsigned char)str[i], fg);
    }
}

void render_hline(int row, char ch, uint32_t color)
{
    int scol = BORDER_COLS;
    int srow = row + BORDER_ROWS;
    int py = srow * GLYPH_H;
    for (int i = 0; i < TEXT_COLS; i++) {
        int px = (scol + i) * GLYPH_W;
        render_glyph(px, py, (unsigned char)ch, color);
    }
}

/* ── Boot screen ─────────────────────────────────────────────── */

/* Helper: type text progressively, returns total length of str */
static int type_line(const char *str, int col, int row, int avail,
                     uint32_t color)
{
    int len = (int)strlen(str);
    if (avail > 0) {
        char buf[TEXT_COLS + 1];
        int n = avail < len ? avail : len;
        memcpy(buf, str, n);
        buf[n] = '\0';
        render_text(col, row, buf, color);
    }
    return len;
}

/* Helper: draw blinking cursor block */
static void blink_cursor(int col, int row, int frame)
{
    if ((frame / 30) % 2 == 0) {
        /* Draw a filled block at cursor position */
        int scol = col + BORDER_COLS;
        int srow = row + BORDER_ROWS;
        int px = scol * GLYPH_W;
        int py = srow * GLYPH_H;
        fill_rect(px, py, px + GLYPH_W, py + GLYPH_H, COL_FG);
    }
}

/* Helper: blit mascot sprite with fade (0=invisible, 255=full) */
static void render_mascot(int cx, int cy, int alpha)
{
    int x0 = cx - MASCOT_W / 2;
    int y0 = cy - MASCOT_H / 2;

    for (int y = 0; y < MASCOT_H; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= SCREEN_H) continue;
        for (int x = 0; x < MASCOT_W; x++) {
            int sx = x0 + x;
            if (sx < 0 || sx >= SCREEN_W) continue;

            uint32_t px = mascot_pixels[y * MASCOT_W + x];
            if ((px >> 24) == 0) continue; /* transparent */

            if (alpha >= 255) {
                fb[sy * SCREEN_W + sx] = px | 0xFF000000u;
            } else {
                /* Blend toward COL_BG */
                int sr = (px >> 16) & 0xFF;
                int sg = (px >> 8) & 0xFF;
                int sb = px & 0xFF;
                int br = (COL_BG >> 16) & 0xFF;
                int bgc = (COL_BG >> 8) & 0xFF;
                int bb = COL_BG & 0xFF;
                int r = br + ((sr - br) * alpha) / 255;
                int g = bgc + ((sg - bgc) * alpha) / 255;
                int b = bb + ((sb - bb) * alpha) / 255;
                fb[sy * SCREEN_W + sx] = 0xFF000000u
                    | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
            }
        }
    }
}

/* C64 tape loading color */
#define BAR_COLOR 0xFF924A40u  /* red */

/* Helper: draw loading bar (progress 0..256) */
static void render_loading_bar(int cy, int progress)
{
    int bar_w = 320;
    int bar_h = 12;
    int x0 = (SCREEN_W - bar_w) / 2;
    int y0 = cy - bar_h / 2;

    /* Border */
    fill_rect(x0 - 2, y0 - 2, x0 + bar_w + 2, y0 + bar_h + 2, COL_DIM);
    fill_rect(x0, y0, x0 + bar_w, y0 + bar_h, COL_BG);

    /* Filled segments */
    int filled = (bar_w * progress) / 256;
    int seg_w = bar_w / 20;
    for (int i = 0; i < 20 && (i * seg_w) < filled; i++) {
        int sx = x0 + i * seg_w;
        int sw = seg_w - 1; /* 1px gap between segments */
        if (sx + sw > x0 + filled) sw = x0 + filled - sx;
        if (sw > 0)
            fill_rect(sx, y0 + 1, sx + sw, y0 + bar_h - 1, BAR_COLOR);
    }
}

/*
 * Boot sequence timeline (frame-based):
 *
 *   Phase 1 — C64 BASIC (frames 0-359)
 *     0:   Banner + RAM info appear instantly
 *     10:  READY. appears instantly
 *     20-90:  LOAD "EMUVR",8,1 types at human speed (~4 frames/char)
 *     100: Pause (enter pressed)
 *     110: SEARCHING FOR EMUVR appears
 *     230: LOADING appears (~2 sec search wait)
 *     340: READY. appears (~1.8 sec load wait)
 *     345-360: RUN types
 *
 *   Phase 2 — Mascot + loading bar (frames 360-559)
 *   Phase 3 — Hold complete (frames 560-599)
 */
void render_boot(int frame)
{
    render_clear();

    if (frame < 360) {
        /* ── Phase 1: C64 BASIC prompt ── */

        const char *banner = "**** COMMODORE 64 BASIC V2 ****";
        const char *ram = "64K RAM SYSTEM  38911 BASIC BYTES FREE";
        const char *load_cmd = "LOAD \"EMUVR\",8,1";
        int load_len = (int)strlen(load_cmd);

        /* Banner + RAM: instant */
        if (frame >= 0) {
            int ctr = (TEXT_COLS - (int)strlen(banner)) / 2;
            render_text(ctr, 1, banner, COL_TITLE);
        }
        if (frame >= 2) {
            int ctr2 = (TEXT_COLS - (int)strlen(ram)) / 2;
            render_text(ctr2, 3, ram, COL_FG);
        }

        /* READY. instant */
        if (frame >= 10)
            render_text(0, 5, "READY.", COL_FG);

        /* LOAD "EMUVR",8,1 — human typing speed, ~4 frames/char */
        if (frame >= 20) {
            int chars = (frame - 20) / 4;
            if (chars > load_len) chars = load_len;
            type_line(load_cmd, 0, 6, chars, COL_FG);

            /* Blinking cursor at typing position */
            if (chars < load_len)
                blink_cursor(chars, 6, frame);
            else if (frame < 100)
                blink_cursor(load_len, 6, frame);
        }

        /* SEARCHING FOR EMUVR — appears after a pause */
        if (frame >= 110)
            render_text(0, 8, "SEARCHING FOR EMUVR", COL_FG);

        /* LOADING — appears ~2 sec after searching */
        if (frame >= 230)
            render_text(0, 9, "LOADING", COL_FG);

        /* READY. — appears ~1.8 sec after loading */
        if (frame >= 340)
            render_text(0, 11, "READY.", COL_FG);

        /* RUN — human typing speed */
        if (frame >= 345) {
            const char *run = "RUN";
            int run_len = 3;
            int chars = (frame - 345) / 4;
            if (chars > run_len) chars = run_len;
            type_line(run, 0, 12, chars, COL_FG);
            blink_cursor(chars < run_len ? chars : run_len, 12, frame);
        }

    } else if (frame < 560) {
        /* ── Phase 2: Mascot + loading bar ── */
        int pf = frame - 360; /* 0..199 */

        /* Fade in mascot over first 60 frames */
        int fade = pf * 255 / 60;
        if (fade > 255) fade = 255;

        /* Center mascot in upper portion */
        int mascot_cy = SCREEN_H / 2 - 40;
        render_mascot(SCREEN_W / 2, mascot_cy, fade);

        /* Loading bar below mascot */
        int bar_cy = mascot_cy + MASCOT_H / 2 + 55;
        int progress = (pf - 30) * 256 / 150;
        if (progress < 0) progress = 0;
        if (progress > 256) progress = 256;
        render_loading_bar(bar_cy, progress);

        /* Text above bar */
        if (pf > 20) {
            const char *msg = "LOADING EMUVR WIKI...";
            int mx = (TEXT_COLS - (int)strlen(msg)) / 2;
            int msg_row = (bar_cy - 20) / GLYPH_H - BORDER_ROWS;
            if (msg_row >= 0 && msg_row < TEXT_ROWS)
                render_text(mx, msg_row, msg, COL_DIM);
        }
    } else {
        /* ── Phase 3: Hold complete ── */
        int mascot_cy = SCREEN_H / 2 - 40;
        render_mascot(SCREEN_W / 2, mascot_cy, 255);

        int bar_cy = mascot_cy + MASCOT_H / 2 + 55;
        render_loading_bar(bar_cy, 256);

        const char *msg = "LOADING EMUVR WIKI...";
        int mx = (TEXT_COLS - (int)strlen(msg)) / 2;
        int msg_row = (bar_cy - 20) / GLYPH_H - BORDER_ROWS;
        if (msg_row >= 0 && msg_row < TEXT_ROWS)
            render_text(mx, msg_row, msg, COL_DIM);
    }
}

/* ── Table of Contents ───────────────────────────────────────── */

void render_toc(int cursor, int scroll)
{
    render_clear();

    /* Title */
    const char *title = "**** THE EMU PAGES ****";
    int cx = (TEXT_COLS - (int)strlen(title)) / 2;
    render_text(cx, 0, title, COL_TITLE);

    /* Info line */
    char info[TEXT_COLS + 1];
    snprintf(info, sizeof(info), "%d WIKI PAGES LOADED. READY.", WIKI_PAGE_COUNT);
    render_text(1, 2, info, COL_FG);

    /* Separator */
    render_hline(3, '-', COL_DIM);

    /* Page list */
    int list_rows = CONTENT_ROWS - 3;  /* rows 4 .. FOOTER_ROW-2 */
    for (int i = 0; i < list_rows && (scroll + i) < WIKI_PAGE_COUNT; i++) {
        int page_idx = scroll + i;
        int row = 4 + i;
        int selected = (page_idx == cursor);

        char line[TEXT_COLS + 1];
        if (selected) {
            snprintf(line, sizeof(line), " > %-73s", wiki_pages[page_idx].title);
            render_text_inv(0, row, line, COL_CURSOR_FG, COL_HIGHLIGHT);
        } else {
            snprintf(line, sizeof(line), "   %s", wiki_pages[page_idx].title);
            render_text(0, row, line, COL_FG);
        }
    }

    /* Scroll indicators */
    if (scroll > 0)
        render_text(TEXT_COLS - 3, 3, "[^]", COL_DIM);
    if (scroll + list_rows < WIKI_PAGE_COUNT)
        render_text(TEXT_COLS - 3, FOOTER_ROW - 1, "[v]", COL_DIM);

    /* Footer */
    render_hline(FOOTER_ROW - 1, '-', COL_DIM);
    render_text(1, FOOTER_ROW,
        "[UP/DN] SELECT  [A] OPEN  [LEFT/RIGHT] PREV/NEXT", COL_DIM);
}

/* ── Page Viewer ─────────────────────────────────────────────── */

void render_page(int page_idx, int scroll)
{
    render_clear();

    const wiki_page_t *page = &wiki_pages[page_idx];

    /* Header: title + page index */
    char header[TEXT_COLS + 1];
    snprintf(header, sizeof(header), "<< %-60s [%d/%d]",
             page->title, page_idx + 1, WIKI_PAGE_COUNT);
    render_text(0, HEADER_ROW, header, COL_TITLE);
    render_hline(1, '=', COL_DIM);

    /* Content area */
    int max_scroll = page->line_count - CONTENT_ROWS;
    if (max_scroll < 0) max_scroll = 0;

    for (int i = 0; i < CONTENT_ROWS; i++) {
        int line_idx = scroll + i;
        if (line_idx >= page->line_count) break;

        const wiki_line_t *line = &page->lines[line_idx];
        uint32_t color;

        switch (line->type) {
        case LINE_H2:
            color = COL_H2;
            break;
        case LINE_H3:
            color = COL_H3;
            break;
        default:
            color = COL_FG;
            break;
        }

        if (line->type == LINE_H2) {
            /* H2: yellow with == markers */
            char buf[TEXT_COLS + 1];
            snprintf(buf, sizeof(buf), "== %s ==", line->text);
            render_text(0, CONTENT_START + i, buf, color);
        } else if (line->type == LINE_H3) {
            /* H3: bright with --- markers */
            char buf[TEXT_COLS + 1];
            snprintf(buf, sizeof(buf), "--- %s ---", line->text);
            render_text(0, CONTENT_START + i, buf, color);
        } else {
            render_text(1, CONTENT_START + i, line->text, color);
        }
    }

    /* Scroll indicators */
    if (scroll > 0)
        render_text(TEXT_COLS - 3, 1, "[^]", COL_DIM);
    if (scroll < max_scroll)
        render_text(TEXT_COLS - 3, FOOTER_ROW - 1, "[v]", COL_DIM);

    /* Footer */
    render_hline(FOOTER_ROW - 1, '-', COL_DIM);
    render_text(1, FOOTER_ROW,
        "[UP/DN] SCROLL  [B] BACK  [L/R] PG UP/DN  [<//>] PREV/NEXT", COL_DIM);
}
