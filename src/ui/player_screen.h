/* Kick Vita — Fullscreen video player */
#ifndef KICK_UI_PLAYER_SCREEN_H
#define KICK_UI_PLAYER_SCREEN_H

#include "twitch/twitch_model.h"

/* Opens and plays a Kick live stream in fullscreen.
 * Qualities supported: 144p, 240p, max 360p. */
void player_screen_run(const KickStream *stream);

#endif
