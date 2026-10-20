#ifndef SNES_CHOICE_MENU_H
#define SNES_CHOICE_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A modal "pick one" list a title's trusted code (a mod plugin, say) can put
 * up between guest frames: a stage select, a difficulty prompt.
 *
 * Opening it only arms it; the desktop host notices at the next frame
 * boundary, freezes the guest exactly like the save-state browser (no guest
 * code runs, audio pauses), draws the list over the frozen picture, and
 * resumes once a choice is made. `done` runs on the host thread with the
 * guest at that frame boundary, so it may change guest RAM.
 *
 * Controls are the player's own (keyboard bindings and pad alike): Up/Down
 * move; A, B or Start chooses, Y cancels (when `cancel_allowed`, reporting
 * -1). Enter or Space also chooses and Escape cancels. B chooses because in
 * many games it is the confirm button, and a list the player reaches by
 * confirming something should not need a different one.
 *
 * Items are copied; up to SNES_CHOICE_MENU_MAX_ITEMS of up to
 * SNES_CHOICE_MENU_MAX_TEXT-1 characters. Returns 0 when one is already
 * open or the arguments are unusable. */
#define SNES_CHOICE_MENU_MAX_ITEMS 16
#define SNES_CHOICE_MENU_MAX_TEXT  40

typedef void (*SnesChoiceMenuDone)(int index, void *user);

int snes_choice_menu_open(const char *title, const char *const *items, int count,
                          int initial, int cancel_allowed,
                          SnesChoiceMenuDone done, void *user);
int snes_choice_menu_is_open(void);

/* Host side. */
void snes_choice_menu_handle_key(int sdl_key, int repeat);
void snes_choice_menu_poll_nav(uint32_t pad, uint32_t now_ms);   /* SNES_PAD_* bits */
/* Finish: index >= 0 chooses it, -1 cancels. Calls `done` once. */
void snes_choice_menu_finish(int index);
/* The panel, ARGB8888 straight alpha, transparent outside its box. */
int snes_choice_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* SNES_CHOICE_MENU_H */
