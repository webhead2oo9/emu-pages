/*
 * emu_pages.c — The Emu Pages libretro core
 *
 * A Commodore 64-styled wiki viewer for EmuVR.
 * All wiki content is baked in at compile time via wiki_data.h.
 *
 * Joypad-only input (matches EmuVR's VR controller mapping).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include "libretro.h"
#include "renderer.h"
#include "wiki_data.h"

/* ── Callbacks ───────────────────────────────────────────────── */

static retro_video_refresh_t     video_cb;
static retro_audio_sample_t      audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t       environ_cb;
static retro_input_poll_t        input_poll_cb;
static retro_input_state_t       input_state_cb;
static retro_log_printf_t        log_cb;

/* ── State ───────────────────────────────────────────────────── */

typedef enum {
    STATE_BOOT,
    STATE_TOC,
    STATE_PAGE
} core_state_t;

static uint32_t framebuffer[SCREEN_W * SCREEN_H];
static core_state_t state = STATE_BOOT;
static bool game_loaded = false;

/* Boot */
static int boot_timer = 0;
#define BOOT_FRAMES 600  /* 10 seconds at 60fps (skippable) */

/* TOC */
static int toc_cursor = 0;
static int toc_scroll = 0;

/* Page viewer */
static int current_page = 0;
static int page_scroll = 0;

/* Audio silence buffer (735 samples at 44100Hz / 60fps, stereo) */
#define AUDIO_RATE   44100
#define AUDIO_FRAMES 735
static int16_t audio_silence[AUDIO_FRAMES * 2];

/* ── Input handling with auto-repeat ─────────────────────────── */

#define REPEAT_DELAY  24   /* ~400ms before repeat starts */
#define REPEAT_RATE   4    /* ~67ms between repeats */
#define BTN_COUNT     16

static int held_frames[BTN_COUNT];

static bool btn_pressed(int btn)
{
    int16_t s = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, btn);
    if (s) {
        held_frames[btn]++;
        if (held_frames[btn] == 1)
            return true;  /* initial press */
        if (held_frames[btn] >= REPEAT_DELAY &&
            (held_frames[btn] - REPEAT_DELAY) % REPEAT_RATE == 0)
            return true;  /* auto-repeat */
        return false;
    }
    held_frames[btn] = 0;
    return false;
}

static bool any_btn_pressed(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i))
            return true;
    }
    return false;
}

/* ── Input handlers per state ────────────────────────────────── */

static void handle_toc_input(void)
{
    int list_rows = CONTENT_ROWS - 3;

    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_UP)) {
        if (toc_cursor > 0) toc_cursor--;
        if (toc_cursor < toc_scroll)
            toc_scroll = toc_cursor;
    }
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_DOWN)) {
        if (toc_cursor < WIKI_PAGE_COUNT - 1) toc_cursor++;
        if (toc_cursor >= toc_scroll + list_rows)
            toc_scroll = toc_cursor - list_rows + 1;
    }
    /* Page up/down in the list */
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_L)) {
        toc_cursor -= list_rows;
        if (toc_cursor < 0) toc_cursor = 0;
        toc_scroll = toc_cursor;
    }
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_R)) {
        toc_cursor += list_rows;
        if (toc_cursor >= WIKI_PAGE_COUNT) toc_cursor = WIKI_PAGE_COUNT - 1;
        if (toc_cursor >= toc_scroll + list_rows)
            toc_scroll = toc_cursor - list_rows + 1;
    }
    /* Open page */
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_A) ||
        btn_pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT)) {
        current_page = toc_cursor;
        page_scroll = 0;
        state = STATE_PAGE;
    }
    /* Quick prev/next page navigation */
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_LEFT)) {
        /* Navigate to previous page from cursor */
        if (toc_cursor > 0) toc_cursor--;
        if (toc_cursor < toc_scroll)
            toc_scroll = toc_cursor;
        current_page = toc_cursor;
        page_scroll = 0;
        state = STATE_PAGE;
    }
}

static void handle_page_input(void)
{
    int max_scroll = wiki_pages[current_page].line_count - CONTENT_ROWS;
    if (max_scroll < 0) max_scroll = 0;

    /* Scroll */
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_UP)) {
        if (page_scroll > 0) page_scroll--;
    }
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_DOWN)) {
        if (page_scroll < max_scroll) page_scroll++;
    }
    /* Page up/down */
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_L)) {
        page_scroll -= CONTENT_ROWS;
        if (page_scroll < 0) page_scroll = 0;
    }
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_R)) {
        page_scroll += CONTENT_ROWS;
        if (page_scroll > max_scroll) page_scroll = max_scroll;
    }
    /* Previous / next page */
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_LEFT)) {
        current_page--;
        if (current_page < 0) current_page = WIKI_PAGE_COUNT - 1;
        page_scroll = 0;
    }
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT)) {
        current_page++;
        if (current_page >= WIKI_PAGE_COUNT) current_page = 0;
        page_scroll = 0;
    }
    /* Back to TOC */
    if (btn_pressed(RETRO_DEVICE_ID_JOYPAD_B) ||
        btn_pressed(RETRO_DEVICE_ID_JOYPAD_START)) {
        toc_cursor = current_page;
        state = STATE_TOC;
        /* Adjust scroll so cursor is visible */
        int list_rows = CONTENT_ROWS - 3;
        if (toc_cursor < toc_scroll)
            toc_scroll = toc_cursor;
        if (toc_cursor >= toc_scroll + list_rows)
            toc_scroll = toc_cursor - list_rows + 1;
    }
}

/* ── Logging fallback ────────────────────────────────────────── */

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ── Libretro API ────────────────────────────────────────────── */

void retro_init(void)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    memset(held_frames, 0, sizeof(held_frames));
    memset(audio_silence, 0, sizeof(audio_silence));
}

void retro_deinit(void)
{
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "The Emu Pages";
    info->library_version  = "1.0.0";
    info->need_fullpath    = true;
    info->valid_extensions = "emupages";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = (double)AUDIO_RATE;
    info->geometry.base_width   = SCREEN_W;
    info->geometry.base_height  = SCREEN_H;
    info->geometry.max_width    = SCREEN_W;
    info->geometry.max_height   = SCREEN_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    else
        log_cb = fallback_log;

    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port; (void)device;
}

void retro_set_video_refresh(retro_video_refresh_t cb)   { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)     { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
}
void retro_set_input_poll(retro_input_poll_t cb)         { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)       { input_state_cb = cb; }

bool retro_load_game(const struct retro_game_info *info)
{
    if (!info || !info->path) {
        log_cb(RETRO_LOG_ERROR, "Emu Pages: No ROM file provided\n");
        return false;
    }

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log_cb(RETRO_LOG_ERROR, "Emu Pages: XRGB8888 not supported\n");
        return false;
    }

    render_init(framebuffer);
    state = STATE_BOOT;
    boot_timer = 0;
    toc_cursor = 0;
    toc_scroll = 0;
    current_page = 0;
    page_scroll = 0;
    memset(held_frames, 0, sizeof(held_frames));

    game_loaded = true;
    log_cb(RETRO_LOG_INFO,
           "Emu Pages: Loaded %d wiki pages (built %s)\n",
           WIKI_PAGE_COUNT, WIKI_BUILD_DATE);
    return true;
}

void retro_unload_game(void)
{
    game_loaded = false;
}

void retro_run(void)
{
    if (!game_loaded) return;

    input_poll_cb();

    switch (state) {
    case STATE_BOOT:
        boot_timer++;
        if (boot_timer >= BOOT_FRAMES || any_btn_pressed()) {
            state = STATE_TOC;
            /* Reset held state so initial press doesn't carry over */
            memset(held_frames, 0, sizeof(held_frames));
        }
        render_boot(boot_timer);
        break;

    case STATE_TOC:
        handle_toc_input();
        render_toc(toc_cursor, toc_scroll);
        break;

    case STATE_PAGE:
        handle_page_input();
        render_page(current_page, page_scroll);
        break;
    }

    video_cb(framebuffer, SCREEN_W, SCREEN_H, SCREEN_W * sizeof(uint32_t));
    audio_batch_cb(audio_silence, AUDIO_FRAMES);
}

/* ── Stubs for unused libretro callbacks ─────────────────────── */

void retro_reset(void)
{
    state = STATE_BOOT;
    boot_timer = 0;
    toc_cursor = 0;
    toc_scroll = 0;
    current_page = 0;
    page_scroll = 0;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info,
                             size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size)
{
    (void)data; (void)size;
    return false;
}
bool retro_unserialize(const void *data, size_t size)
{
    (void)data; (void)size;
    return false;
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
