/* Kick Vita — Persistent local storage implementation */
#include "store.h"
#include "app.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <psp2/io/stat.h>

static KickStream g_history[KICK_HISTORY_MAX];
static int g_history_count = 0;

static char g_searches[KICK_SEARCHES_MAX][128];
static int g_searches_count = 0;

static int g_quality = KICK_QUALITY_360P;
static char g_lang[8] = "es";
static char g_app_lang[8] = "es";
static unsigned g_rev = 1;

static void ensure_dir(void) {
    sceIoMkdir(KICK_DATA_DIR, 0777);
}

static void save_state(void) {
    ensure_dir();
    json_t *root = json_object();
    json_object_set_new(root, "quality", json_integer(g_quality));
    json_object_set_new(root, "lang", json_string(g_lang));
    json_object_set_new(root, "stream_lang", json_string(g_lang));
    json_object_set_new(root, "app_lang", json_string(g_app_lang));

    json_t *hist_arr = json_array();
    for (int i = 0; i < g_history_count; i++) {
        json_t *item = json_object();
        json_object_set_new(item, "id", json_integer((json_int_t)g_history[i].id));
        json_object_set_new(item, "title", json_string(g_history[i].session_title));
        json_object_set_new(item, "slug", json_string(g_history[i].channel_slug));
        json_object_set_new(item, "username", json_string(g_history[i].username));
        json_object_set_new(item, "category", json_string(g_history[i].category_name));
        json_object_set_new(item, "thumb", json_string(g_history[i].thumbnail_url));
        json_object_set_new(item, "pic", json_string(g_history[i].profile_pic));
        json_object_set_new(item, "pb", json_string(g_history[i].playback_url));
        json_array_append_new(hist_arr, item);
    }
    json_object_set_new(root, "history", hist_arr);

    json_t *search_arr = json_array();
    for (int i = 0; i < g_searches_count; i++) {
        json_array_append_new(search_arr, json_string(g_searches[i]));
    }
    json_object_set_new(root, "searches", search_arr);

    char *dump = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    if (!dump) return;

    vt_io_lock();
    FILE *f_settings = fopen(KICK_SETTINGS_PATH, "wb");
    if (f_settings) {
        fputs(dump, f_settings);
        fclose(f_settings);
    }
    FILE *f_state = fopen(KICK_STATE_PATH, "wb");
    if (f_state) {
        fputs(dump, f_state);
        fclose(f_state);
    }
    vt_io_unlock();
    free(dump);
    g_rev++;
}

void kick_store_load(void) {
    ensure_dir();
    vt_io_lock();
    FILE *f = fopen(KICK_SETTINGS_PATH, "rb");
    if (!f) f = fopen(KICK_STATE_PATH, "rb");
    if (!f) {
        vt_io_unlock();
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 131072) {
        fclose(f);
        vt_io_unlock();
        return;
    }
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        vt_io_unlock();
        return;
    }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    vt_io_unlock();

    json_error_t jerr;
    json_t *root = json_loads(buf, 0, &jerr);
    free(buf);
    if (!root) return;

    json_t *jq = json_object_get(root, "quality");
    if (json_is_integer(jq)) {
        int q = (int)json_integer_value(jq);
        if (q == KICK_QUALITY_HD || q == KICK_QUALITY_360P || q == KICK_QUALITY_240P || q == KICK_QUALITY_144P)
            g_quality = q;
        else if (q == 160)
            g_quality = KICK_QUALITY_144P;
        else
            g_quality = KICK_QUALITY_360P;
    }

    json_t *jl = json_object_get(root, "stream_lang");
    if (!jl) jl = json_object_get(root, "lang");
    if (json_is_string(jl)) snprintf(g_lang, sizeof g_lang, "%s", json_string_value(jl));

    json_t *j_app = json_object_get(root, "app_lang");
    if (json_is_string(j_app)) snprintf(g_app_lang, sizeof g_app_lang, "%s", json_string_value(j_app));

    json_t *jh = json_object_get(root, "history");
    if (json_is_array(jh)) {
        size_t cnt = json_array_size(jh);
        g_history_count = 0;
        for (size_t i = 0; i < cnt && g_history_count < KICK_HISTORY_MAX; i++) {
            json_t *item = json_array_get(jh, i);
            if (!json_is_object(item)) continue;
            KickStream *st = &g_history[g_history_count];
            memset(st, 0, sizeof *st);
            json_t *j_id = json_object_get(item, "id");
            if (json_is_integer(j_id)) st->id = (uint64_t)json_integer_value(j_id);
            json_t *j_t = json_object_get(item, "title");
            if (json_is_string(j_t)) snprintf(st->session_title, sizeof st->session_title, "%s", json_string_value(j_t));
            json_t *j_s = json_object_get(item, "slug");
            if (json_is_string(j_s)) snprintf(st->channel_slug, sizeof st->channel_slug, "%s", json_string_value(j_s));
            json_t *j_u = json_object_get(item, "username");
            if (json_is_string(j_u)) snprintf(st->username, sizeof st->username, "%s", json_string_value(j_u));
            json_t *j_c = json_object_get(item, "category");
            if (json_is_string(j_c)) snprintf(st->category_name, sizeof st->category_name, "%s", json_string_value(j_c));
            json_t *j_th = json_object_get(item, "thumb");
            if (json_is_string(j_th)) snprintf(st->thumbnail_url, sizeof st->thumbnail_url, "%s", json_string_value(j_th));
            json_t *j_p = json_object_get(item, "pic");
            if (json_is_string(j_p)) snprintf(st->profile_pic, sizeof st->profile_pic, "%s", json_string_value(j_p));
            json_t *j_pb = json_object_get(item, "pb");
            if (json_is_string(j_pb)) snprintf(st->playback_url, sizeof st->playback_url, "%s", json_string_value(j_pb));
            g_history_count++;
        }
    }

    json_t *js = json_object_get(root, "searches");
    if (json_is_array(js)) {
        size_t scnt = json_array_size(js);
        g_searches_count = 0;
        for (size_t i = 0; i < scnt && g_searches_count < KICK_SEARCHES_MAX; i++) {
            json_t *s = json_array_get(js, i);
            if (json_is_string(s)) {
                snprintf(g_searches[g_searches_count], sizeof g_searches[0], "%s", json_string_value(s));
                g_searches_count++;
            }
        }
    }

    json_decref(root);
}

unsigned kick_store_revision(void) {
    return g_rev;
}

int kick_store_history_count(void) {
    return g_history_count;
}

const KickStream *kick_store_history_at(int index) {
    if (index < 0 || index >= g_history_count) return NULL;
    return &g_history[index];
}

void kick_store_history_add(const KickStream *stream) {
    if (!stream || !stream->channel_slug[0]) return;
    int found = -1;
    for (int i = 0; i < g_history_count; i++) {
        if (strcmp(g_history[i].channel_slug, stream->channel_slug) == 0) {
            found = i;
            break;
        }
    }
    if (found == 0) return; /* Already most recent */

    if (found > 0) {
        static KickStream s_tmp;
        s_tmp = g_history[found];
        memmove(&g_history[1], &g_history[0], found * sizeof(KickStream));
        g_history[0] = s_tmp;
    } else {
        int move_cnt = g_history_count < KICK_HISTORY_MAX ? g_history_count : KICK_HISTORY_MAX - 1;
        memmove(&g_history[1], &g_history[0], move_cnt * sizeof(KickStream));
        g_history[0] = *stream;
        if (g_history_count < KICK_HISTORY_MAX) g_history_count++;
    }
    save_state();
}

int kick_store_search_count(void) {
    return g_searches_count;
}

const char *kick_store_search_at(int index) {
    if (index < 0 || index >= g_searches_count) return NULL;
    return g_searches[index];
}

void kick_store_search_add(const char *query) {
    if (!query || !*query) return;
    int found = -1;
    for (int i = 0; i < g_searches_count; i++) {
        if (strcasecmp(g_searches[i], query) == 0) {
            found = i;
            break;
        }
    }
    if (found == 0) return;

    if (found > 0) {
        char tmp[128];
        snprintf(tmp, sizeof tmp, "%s", g_searches[found]);
        memmove(&g_searches[1], &g_searches[0], found * sizeof(g_searches[0]));
        snprintf(g_searches[0], sizeof g_searches[0], "%s", tmp);
    } else {
        int move_cnt = g_searches_count < KICK_SEARCHES_MAX ? g_searches_count : KICK_SEARCHES_MAX - 1;
        memmove(&g_searches[1], &g_searches[0], move_cnt * sizeof(g_searches[0]));
        snprintf(g_searches[0], sizeof g_searches[0], "%s", query);
        if (g_searches_count < KICK_SEARCHES_MAX) g_searches_count++;
    }
    save_state();
}

void kick_store_clear_personal(void) {
    g_history_count = 0;
    g_searches_count = 0;
    save_state();
}

int kick_store_get_quality(void) {
    if (g_quality <= 0 || g_quality == 540) g_quality = KICK_QUALITY_360P;
    return g_quality;
}

void kick_store_set_quality(int quality) {
    if (quality != KICK_QUALITY_HD &&
        quality != KICK_QUALITY_360P && quality != KICK_QUALITY_240P &&
        quality != KICK_QUALITY_144P)
        quality = KICK_QUALITY_360P;
    if (g_quality != quality) {
        g_quality = quality;
        save_state();
    }
}

const char *kick_store_get_lang(void) {
    return g_lang;
}

void kick_store_set_lang(const char *lang) {
    snprintf(g_lang, sizeof g_lang, "%s", lang ? lang : "");
    save_state();
}

const char *kick_store_get_stream_lang(void) {
    return g_lang;
}

void kick_store_set_stream_lang(const char *lang) {
    snprintf(g_lang, sizeof g_lang, "%s", lang ? lang : "");
    save_state();
}

const char *kick_store_get_app_lang(void) {
    return g_app_lang;
}

void kick_store_set_app_lang(const char *lang) {
    if (lang && *lang) {
        snprintf(g_app_lang, sizeof g_app_lang, "%s", lang);
        save_state();
    }
}
