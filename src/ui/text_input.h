/* ViTube — raw sceIme text input (adapted from VitaWave, GPL-3.0-only). */
#ifndef VITUBE_UI_TEXT_INPUT_H
#define VITUBE_UI_TEXT_INPUT_H

#include <stddef.h>

/* Blocking on-screen keyboard. Returns 1 = confirmed with text,
 * 0 = cancelled/empty, -1 = error. `initial` may equal `out`. */
int ui_text_input(const char *title, const char *initial,
                  char *out, size_t out_max);

#endif
