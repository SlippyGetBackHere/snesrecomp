/* snes_choice_menu.c -- see snes_choice_menu.h. */
#include "snes_choice_menu.h"
#include "snes_overlay_draw.h"
#include "desktop/sdl_compat.h"

#include <stdio.h>
#include <string.h>

#define CM_W 512
#define CM_H 448
#define CM_SCALE 2                 /* 16-pixel glyphs on the 512x448 panel */
#define CM_CHAR (8 * CM_SCALE)
#define CM_ROW_H 24
#define CM_PAD 16

#define CM_COL_BOX    0xE0101820u
#define CM_COL_EDGE   0xFF6A7A90u
#define CM_COL_TITLE  0xFFFFD24Du
#define CM_COL_TEXT   0xFFD8D8D8u
#define CM_COL_SEL_BG 0xFF2C4A78u
#define CM_COL_SEL    0xFFFFFFFFu
#define CM_COL_HINT   0xFF9AA4B0u

static int  s_open;
static char s_title[SNES_CHOICE_MENU_MAX_TEXT];
static char s_items[SNES_CHOICE_MENU_MAX_ITEMS][SNES_CHOICE_MENU_MAX_TEXT];
static int  s_count, s_sel, s_cancel_allowed;
static SnesChoiceMenuDone s_done;
static void *s_user;
static int  s_dirty;
static uint32_t s_img[CM_W * CM_H];
/* d-pad edge/repeat state */
static uint32_t s_prev_pad, s_held_dir, s_held_since, s_last_repeat;
static int s_nav_primed;

int snes_choice_menu_open(const char *title, const char *const *items, int count,
                          int initial, int cancel_allowed,
                          SnesChoiceMenuDone done, void *user) {
    if (s_open || !items || count <= 0 || !done) return 0;
    if (count > SNES_CHOICE_MENU_MAX_ITEMS) count = SNES_CHOICE_MENU_MAX_ITEMS;
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    for (int i = 0; i < count; i++)
        snprintf(s_items[i], sizeof(s_items[i]), "%s", items[i] ? items[i] : "");
    s_count = count;
    s_sel = initial >= 0 && initial < count ? initial : 0;
    s_cancel_allowed = cancel_allowed;
    s_done = done;
    s_user = user;
    s_open = 1;
    s_dirty = 1;
    /* The button that confirmed whatever led here is probably still held:
     * the first poll only records it, so it cannot choose straight away. */
    s_nav_primed = 0;
    s_held_dir = 0;
    return 1;
}

int snes_choice_menu_is_open(void) { return s_open; }

void snes_choice_menu_finish(int index) {
    if (!s_open) return;
    if (index < 0 && !s_cancel_allowed) return;
    SnesChoiceMenuDone done = s_done;
    void *user = s_user;
    s_open = 0;
    s_done = NULL;
    if (done) done(index < s_count ? index : -1, user);
}

static void move(int d) {
    s_sel = (s_sel + d + s_count) % s_count;
    s_dirty = 1;
}

void snes_choice_menu_handle_key(int key, int repeat) {
    (void)repeat;
    if (!s_open) return;
    /* Movement comes through the player's own controls (poll_nav), which the
     * host feeds from the keyboard bindings as well as the pad; handling the
     * arrow keys here too would move twice. SDLK_* is unsigned under SDL3
     * and signed under SDL2. */
    if (key == (int)SDLK_RETURN || key == (int)SDLK_KP_ENTER || key == (int)SDLK_SPACE)
        snes_choice_menu_finish(s_sel);
    else if (key == (int)SDLK_ESCAPE)
        snes_choice_menu_finish(-1);
}

void snes_choice_menu_poll_nav(uint32_t pad, uint32_t now) {
    if (!s_open) return;
    if (!s_nav_primed) {
        s_prev_pad = pad;
        s_nav_primed = 1;
        return;
    }
    const uint32_t pressed = pad & ~s_prev_pad;
    s_prev_pad = pad;
    if (pressed & SNES_PAD_UP) move(-1);
    if (pressed & SNES_PAD_DOWN) move(+1);
    const uint32_t dir = pad & (SNES_PAD_UP | SNES_PAD_DOWN);
    if (dir && dir != (SNES_PAD_UP | SNES_PAD_DOWN)) {
        if (dir != s_held_dir) {
            s_held_dir = dir;
            s_held_since = s_last_repeat = now;
        } else if (now - s_held_since >= SNES_OVL_REPEAT_DELAY &&
                   now - s_last_repeat >= SNES_OVL_REPEAT_RATE) {
            move((dir & SNES_PAD_UP) ? -1 : +1);
            s_last_repeat = now;
        }
    } else {
        s_held_dir = 0;
    }
    if (pressed & (SNES_PAD_A | SNES_PAD_B | SNES_PAD_START))
        snes_choice_menu_finish(s_sel);
    else if (pressed & SNES_PAD_Y)
        snes_choice_menu_finish(-1);
}

static void text(int x, int y, const char *s, uint32_t col) {
    snes_ovl_draw_text(s_img, CM_W, CM_H, x, y, s, col, CM_SCALE);
}

static void render(void) {
    memset(s_img, 0, sizeof(s_img));
    int widest = (int)strlen(s_title);
    for (int i = 0; i < s_count; i++) {
        int n = (int)strlen(s_items[i]) + 2;    /* room for the marker */
        if (n > widest) widest = n;
    }
    const char *hint = s_cancel_allowed ? "A/ENTER: CHOOSE   Y/ESC: CANCEL"
                                        : "A/ENTER: CHOOSE";
    int hint_w = (int)strlen(hint) * 8;          /* hint at scale 1 */
    int box_w = widest * CM_CHAR + 2 * CM_PAD;
    if (hint_w + 2 * CM_PAD > box_w) box_w = hint_w + 2 * CM_PAD;
    if (box_w > CM_W - 16) box_w = CM_W - 16;
    const int box_h = CM_PAD + CM_CHAR + 12 + s_count * CM_ROW_H + 12 + 8 + CM_PAD;
    const int bx = (CM_W - box_w) / 2, by = (CM_H - box_h) / 2;
    snes_ovl_fill_rect(s_img, CM_W, CM_H, bx, by, box_w, box_h, CM_COL_BOX);
    snes_ovl_stroke_rect(s_img, CM_W, CM_H, bx, by, box_w, box_h, CM_COL_EDGE);
    int y = by + CM_PAD;
    text(bx + (box_w - (int)strlen(s_title) * CM_CHAR) / 2, y, s_title, CM_COL_TITLE);
    y += CM_CHAR + 12;
    for (int i = 0; i < s_count; i++) {
        const int sel = i == s_sel;
        if (sel)
            snes_ovl_fill_rect(s_img, CM_W, CM_H, bx + 4, y - 4, box_w - 8, CM_ROW_H,
                               CM_COL_SEL_BG);
        char line[SNES_CHOICE_MENU_MAX_TEXT + 4];
        snprintf(line, sizeof(line), "%s%s", sel ? "> " : "  ", s_items[i]);
        text(bx + CM_PAD, y, line, sel ? CM_COL_SEL : CM_COL_TEXT);
        y += CM_ROW_H;
    }
    y += 12;
    snes_ovl_draw_text(s_img, CM_W, CM_H, bx + (box_w - hint_w) / 2, y, hint,
                       CM_COL_HINT, 1);
}

int snes_choice_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
    if (!s_open) return 0;
    if (s_dirty) {
        render();
        s_dirty = 0;
    }
    if (pixels) *pixels = s_img;
    if (w) *w = CM_W;
    if (h) *h = CM_H;
    return 1;
}
