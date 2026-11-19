/* Kick Vita — raw sceIme text input (adapted from VitaWave, GPL-3.0-only). */
#ifndef KICK_UI_TEXT_INPUT_H
#define KICK_UI_TEXT_INPUT_H

#include <stddef.h>

typedef void (*UiTextInputBgFn)(void *arg);

/* Blocking on-screen keyboard. Returns 1 = confirmed with text,
 * 0 = cancelled/empty, -1 = error. `initial` may equal `out`. */
int ui_text_input(const char *title, const char *initial,
                  char *out, size_t out_max);

/* Extended keyboard with background presenter callback to keep video playing */
int ui_text_input_ex(const char *title, const char *initial,
                     char *out, size_t out_max,
                     UiTextInputBgFn bg_fn, void *bg_arg);

int ui_text_input_ex2(const char *title, const char *initial,
                      char *out, size_t out_max,
                      UiTextInputBgFn bg_fn,
                      UiTextInputBgFn post_fn,
                      void *arg);

#endif
