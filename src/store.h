/* Kick Vita — Persistent local storage */
#ifndef KICK_STORE_H
#define KICK_STORE_H

#include "twitch/twitch_model.h"

#define twitch_store_load kick_store_load
#define twitch_store_revision kick_store_revision
#define twitch_store_history_count kick_store_history_count
#define twitch_store_history_at kick_store_history_at
#define twitch_store_history_add kick_store_history_add
#define twitch_store_search_count kick_store_search_count
#define twitch_store_search_at kick_store_search_at
#define twitch_store_search_add kick_store_search_add
#define twitch_store_clear_personal kick_store_clear_personal
#define twitch_store_get_quality kick_store_get_quality
#define twitch_store_set_quality kick_store_set_quality
#define twitch_store_get_lang kick_store_get_lang
#define twitch_store_set_lang kick_store_set_lang
#define twitch_store_get_stream_lang kick_store_get_stream_lang
#define twitch_store_set_stream_lang kick_store_set_stream_lang
#define twitch_store_get_app_lang kick_store_get_app_lang
#define twitch_store_set_app_lang kick_store_set_app_lang

#define KICK_HISTORY_MAX 30
#define KICK_SEARCHES_MAX 10

void kick_store_load(void);
unsigned kick_store_revision(void);

int kick_store_history_count(void);
const KickStream *kick_store_history_at(int index);
void kick_store_history_add(const KickStream *stream);

int kick_store_search_count(void);
const char *kick_store_search_at(int index);
void kick_store_search_add(const char *query);

void kick_store_clear_personal(void);

int kick_store_get_quality(void);
void kick_store_set_quality(int quality);

const char *kick_store_get_lang(void);
void kick_store_set_lang(const char *lang);

const char *kick_store_get_stream_lang(void);
void kick_store_set_stream_lang(const char *lang);

const char *kick_store_get_app_lang(void);
void kick_store_set_app_lang(const char *lang);

#endif
