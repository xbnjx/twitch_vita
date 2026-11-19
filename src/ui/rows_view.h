/* Kick Vita — Row rendering for streams and categories */
#ifndef KICK_UI_ROWS_VIEW_H
#define KICK_UI_ROWS_VIEW_H

#include "twitch/twitch_model.h"

void draw_stream_row(const KickStream *stream, int y, int selected);
void draw_category_row(const KickCategory *cat, int y, int selected);
void draw_empty_home(int list_top_y, int logged_in);

#endif
