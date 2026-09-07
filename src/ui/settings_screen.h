/* ViTube — the "Mehr" tab: everything that is a setting rather than a video.
 *
 * Draws into the shell's list area and owns its cursor, but does not own the
 * frame or the input loop: list_screen.c routes to it and reacts to what
 * comes back, because some of these settings invalidate the lists it holds. */
#ifndef VITUBE_UI_SETTINGS_SCREEN_H
#define VITUBE_UI_SETTINGS_SCREEN_H

typedef enum {
    SETTINGS_NOTHING = 0,
    SETTINGS_SIGNED_IN,
    SETTINGS_SIGNED_OUT,
    /* The region changed, or the cache was emptied by hand: everything
     * already fetched is stale or in the wrong language, search included. */
    SETTINGS_RELOAD_ALL
} SettingsResult;

/* `pressed` is the frame's fresh button presses, `direction` the shell's
 * repeat-filtered d-pad bits. Anything that opened a modal is reported back
 * so the caller can resync its own controller state. */
SettingsResult settings_screen_input(unsigned pressed, unsigned direction);

void settings_screen_draw(int list_top_y);
const char *settings_screen_hint(void);

/* True while a picker inside the tab has the input. The shell must hold off
 * its own global bindings then — L1/R1, Triangle and SELECT would otherwise
 * fire straight through an open menu. */
int settings_screen_modal(void);

#endif
