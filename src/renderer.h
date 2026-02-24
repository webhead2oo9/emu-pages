/*
 * renderer.h — EmuManual framebuffer rendering
 *
 * C64-inspired blue theme, glyph rendering, screen layouts.
 */
#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>
#include "wiki_data.h"

/* ── Screen geometry ─────────────────────────────────────────── */

#define SCREEN_W       640
#define SCREEN_H       480
#define GLYPH_W        8
#define GLYPH_H        16       /* 8x8 font doubled vertically */
#define TERM_COLS      80       /* 640 / 8 */
#define TERM_ROWS      30       /* 480 / 16 */

#define BORDER_COLS    2        /* glyph columns on each side */
#define BORDER_ROWS    1        /* glyph rows top and bottom */
#define TEXT_COLS      76       /* 80 - 2*2 */
#define TEXT_ROWS      28       /* 30 - 2*1 */

#define HEADER_ROW     0        /* page title bar (in text area) */
#define CONTENT_START  2        /* first content row */
#define FOOTER_ROW     27       /* control hints */
#define CONTENT_ROWS   25       /* rows 2..26 for scrollable content */

/* ── C64-inspired color palette ──────────────────────────────── */

#define COL_BORDER     0xFF6C5EB5u  /* Medium blue - outer border */
#define COL_BG         0xFF4039A4u  /* C64 blue - main background */
#define COL_FG         0xFFA0A0E0u  /* Light blue/lavender - body text */
#define COL_TITLE      0xFFFFFFFFu  /* White - page titles */
#define COL_HIGHLIGHT  0xFF70E070u  /* Green - selected item */
#define COL_H2         0xFFE0E050u  /* Yellow - H2 headings */
#define COL_H3         0xFFC8C8E0u  /* Bright lavender - H3 headings */
#define COL_DIM        0xFF7070C0u  /* Dimmed blue - footer hints */
#define COL_CURSOR_FG  0xFF2020A0u  /* Dark blue text on green bg */

/* ── Public API ──────────────────────────────────────────────── */

/* Initialise the renderer with a framebuffer pointer. */
void render_init(uint32_t *fb);

/* Clear to border + inner background. */
void render_clear(void);

/* Draw text at text-area coordinates (col, row are 0-based in inner area). */
void render_text(int col, int row, const char *str, uint32_t color);

/* Draw text with inverted colors (for cursor highlight). */
void render_text_inv(int col, int row, const char *str,
                     uint32_t fg, uint32_t bg);

/* Draw a horizontal line of characters across the text area. */
void render_hline(int row, char ch, uint32_t color);

/* ── High-level screen renderers ─────────────────────────────── */

void render_boot(int frame);
void render_toc(int cursor, int scroll);
void render_page(int page_idx, int scroll);

#endif /* RENDERER_H */
