/* Twitch Vita — REST & GraphQL API client implementation */
#include "twitch/twitch_api.h"
#include "twitch/twitch_auth.h"
#include "store.h"
#include "net/http.h"
#include "log.h"
#include "app.h"
#include "ui/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <jansson.h>
#include <psp2/kernel/threadmgr.h>

#define GQL_ENDPOINT "https://gql.twitch.tv/gql"

static int gql_request(const char *body, const char *client_id, volatile int *cancel, http_buf_t *out) {
    if (!body || !out) return -1;

    char auth_header[600] = {0};
    const char *headers[6];
    int h_idx = 0;

    char cid_header[128];
    snprintf(cid_header, sizeof cid_header, "Client-Id: %s", client_id ? client_id : TWITCH_TV_CLIENT_ID);
    headers[h_idx++] = cid_header;
    headers[h_idx++] = "Content-Type: application/json";
    headers[h_idx++] = "Accept: application/json";

    if (twitch_auth_is_logged_in()) {
        const char *token = twitch_auth_get_token();
        if (token && *token) {
            snprintf(auth_header, sizeof auth_header, "Authorization: OAuth %s", token);
            headers[h_idx++] = auth_header;
        }
    }
    headers[h_idx] = NULL;

    long status = 0;
    int ret = http_post(GQL_ENDPOINT, "application/json", body, strlen(body), headers, cancel, out, &status);
    if (ret < 0 || status != 200 || !out->data) {
        vt_log("twitch_api: gql_request failed ret=%d status=%ld", ret, status);
        return -1;
    }
    return 0;
}

int twitch_api_get_stream_url(const char *login, char *out_url, size_t cap) {
    if (!login || !*login || !out_url || cap < 64) return -1;
    out_url[0] = '\0';

    json_t *req = json_object();
    json_object_set_new(req, "operationName", json_string("PlaybackAccessToken_Live"));
    json_object_set_new(req, "query", json_string(
        "query PlaybackAccessToken_Live($login: String!, $playerType: String!) { "
        "streamPlaybackAccessToken(channelName: $login, params: {platform: \"web\", playerBackend: \"mediaplayer\", playerType: $playerType}) { "
        "value signature } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "login", json_string(login));
    json_object_set_new(vars, "playerType", json_string("embed"));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    volatile int cancel = 0;
    /* Use web client ID for playback token */
    int ret = gql_request(body, TWITCH_WEB_CLIENT_ID, &cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_tok_obj = j_data ? json_object_get(j_data, "streamPlaybackAccessToken") : NULL;
    if (!j_tok_obj || !json_is_object(j_tok_obj)) {
        json_decref(root);
        return -1;
    }

    json_t *j_val = json_object_get(j_tok_obj, "value");
    json_t *j_sig = json_object_get(j_tok_obj, "signature");
    if (!json_is_string(j_val) || !json_is_string(j_sig)) {
        json_decref(root);
        return -1;
    }

    const char *raw_token = json_string_value(j_val);
    const char *sig = json_string_value(j_sig);
    char *escaped_token = http_escape(raw_token);
    if (!escaped_token) {
        json_decref(root);
        return -1;
    }

    snprintf(out_url, cap,
             "https://usher.ttvnw.net/api/channel/hls/%s.m3u8?token=%s&sig=%s&player=twitchweb&allow_source=true&allow_audio_only=true&fast_bread=true",
             login, escaped_token, sig);

    free(escaped_token);
    json_decref(root);
    return 0;
}



int twitch_api_get_channel_clips(const char *channel_slug, volatile int *cancel, TwitchStreamList *out) {
    if (!channel_slug || !*channel_slug || !out) return -1;
    memset(out, 0, sizeof *out);

    json_t *req = json_object();
    json_object_set_new(req, "query", json_string(
        "query ChannelClips($login: String!, $limit: Int!) { "
        "user(login: $login) { clips(first: $limit) { edges { node { "
        "id slug title durationSeconds viewCount thumbnailURL "
        "broadcaster { login displayName } } } } } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "login", json_string(channel_slug));
    json_object_set_new(vars, "limit", json_integer(30));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_user = j_data ? json_object_get(j_data, "user") : NULL;
    json_t *j_clips = j_user ? json_object_get(j_user, "clips") : NULL;
    json_t *j_edges = j_clips ? json_object_get(j_clips, "edges") : NULL;

    if (json_is_array(j_edges)) {
        size_t arr_sz = json_array_size(j_edges);
        for (size_t i = 0; i < arr_sz && out->count < TWITCH_PAGE_SIZE_MAX; i++) {
            json_t *edge = json_array_get(j_edges, i);
            json_t *node = edge ? json_object_get(edge, "node") : NULL;
            if (!node || !json_is_object(node)) continue;

            TwitchStream *s = &out->items[out->count];
            json_t *j_id = json_object_get(node, "id");
            if (json_is_string(j_id)) s->id = strtoull(json_string_value(j_id), NULL, 10);

            json_t *j_slug = json_object_get(node, "slug");
            if (json_is_string(j_slug)) snprintf(s->clip_slug, sizeof s->clip_slug, "%s", json_string_value(j_slug));

            json_t *j_title = json_object_get(node, "title");
            if (json_is_string(j_title)) snprintf(s->session_title, sizeof s->session_title, "%s", json_string_value(j_title));

            json_t *j_views = json_object_get(node, "viewCount");
            if (json_is_integer(j_views)) s->viewer_count = (uint32_t)json_integer_value(j_views);

            json_t *j_dur = json_object_get(node, "durationSeconds");
            if (json_is_number(j_dur)) {
                s->duration_ms = (uint64_t)(json_number_value(j_dur) * 1000.0);
            } else if (json_is_string(j_dur)) {
                s->duration_ms = (uint64_t)(atof(json_string_value(j_dur)) * 1000.0);
            }

            json_t *j_thumb = json_object_get(node, "thumbnailURL");
            if (json_is_string(j_thumb)) snprintf(s->thumbnail_url, sizeof s->thumbnail_url, "%s", json_string_value(j_thumb));

            snprintf(s->channel_slug, sizeof s->channel_slug, "%s", channel_slug);
            snprintf(s->username, sizeof s->username, "%s", channel_slug);
            s->is_live = 0;
            s->is_clip = 1;
            out->count++;
        }
    }

    json_decref(root);
    return 0;
}

int twitch_api_get_vod_url(uint64_t vod_id, char *out_url, size_t cap) {
    if (!vod_id || !out_url || cap < 64) return -1;
    out_url[0] = '\0';

    char id_str[32];
    snprintf(id_str, sizeof id_str, "%llu", (unsigned long long)vod_id);

    json_t *req = json_object();
    json_object_set_new(req, "operationName", json_string("PlaybackAccessToken_Vod"));
    json_object_set_new(req, "query", json_string(
        "query PlaybackAccessToken_Vod($id: ID!, $playerType: String!) { "
        "videoPlaybackAccessToken(id: $id, params: {platform: \"web\", playerBackend: \"mediaplayer\", playerType: $playerType}) { "
        "value signature } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "id", json_string(id_str));
    json_object_set_new(vars, "playerType", json_string("embed"));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    volatile int cancel = 0;
    int ret = gql_request(body, TWITCH_WEB_CLIENT_ID, &cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_tok_obj = j_data ? json_object_get(j_data, "videoPlaybackAccessToken") : NULL;
    if (!j_tok_obj || !json_is_object(j_tok_obj)) {
        json_decref(root);
        return -1;
    }

    json_t *j_val = json_object_get(j_tok_obj, "value");
    json_t *j_sig = json_object_get(j_tok_obj, "signature");
    if (!json_is_string(j_val) || !json_is_string(j_sig)) {
        json_decref(root);
        return -1;
    }

    const char *raw_token = json_string_value(j_val);
    const char *sig = json_string_value(j_sig);
    char *escaped_token = http_escape(raw_token);
    if (!escaped_token) {
        json_decref(root);
        return -1;
    }

    snprintf(out_url, cap,
             "https://usher.ttvnw.net/vod/%s.m3u8?token=%s&sig=%s&player=twitchweb&allow_source=true&allow_audio_only=true",
             id_str, escaped_token, sig);

    free(escaped_token);
    json_decref(root);
    return 0;
}



int twitch_api_get_clip_url(const char *slug_or_id, int target_height, char *out_url, size_t cap) {
    if (!slug_or_id || !*slug_or_id || !out_url || cap < 64) return -1;
    out_url[0] = '\0';

    if (target_height <= 0) target_height = 360;

    json_t *req = json_object();
    json_object_set_new(req, "operationName", json_string("ClipPlayback"));
    json_object_set_new(req, "query", json_string(
        "query ClipPlayback($slug: ID!) { "
        "clip(slug: $slug) { id "
        "playbackAccessToken(params: {platform: \"web\", playerBackend: \"mediaplayer\", playerType: \"embed\"}) { "
        "value signature } "
        "videoQualities { quality sourceURL } } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "slug", json_string(slug_or_id));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    volatile int cancel = 0;
    int ret = gql_request(body, TWITCH_WEB_CLIENT_ID, &cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_clip = j_data ? json_object_get(j_data, "clip") : NULL;
    if (!j_clip || !json_is_object(j_clip)) {
        json_decref(root);
        return -1;
    }

    json_t *j_tok_obj = json_object_get(j_clip, "playbackAccessToken");
    if (!j_tok_obj || !json_is_object(j_tok_obj)) {
        json_decref(root);
        return -1;
    }

    json_t *j_val = json_object_get(j_tok_obj, "value");
    json_t *j_sig = json_object_get(j_tok_obj, "signature");
    if (!json_is_string(j_val) || !json_is_string(j_sig)) {
        json_decref(root);
        return -1;
    }

    const char *raw_token = json_string_value(j_val);
    const char *sig = json_string_value(j_sig);
    char *escaped_token = http_escape(raw_token);
    if (!escaped_token) {
        json_decref(root);
        return -1;
    }

    json_t *j_quals = json_object_get(j_clip, "videoQualities");
    const char *chosen_source = NULL;
    if (json_is_array(j_quals)) {
        size_t qcount = json_array_size(j_quals);
        int best_diff = 999999;
        /* Find quality that matches target_height or closest lower */
        for (size_t i = 0; i < qcount; i++) {
            json_t *q_entry = json_array_get(j_quals, i);
            if (!q_entry || !json_is_object(q_entry)) continue;
            json_t *q_str = json_object_get(q_entry, "quality");
            json_t *s_url = json_object_get(q_entry, "sourceURL");
            if (!json_is_string(q_str) || !json_is_string(s_url)) continue;
            int qh = atoi(json_string_value(q_str));
            if (qh <= 0) continue;
            if (qh <= target_height) {
                int diff = target_height - qh;
                if (diff < best_diff) {
                    best_diff = diff;
                    chosen_source = json_string_value(s_url);
                }
            }
        }
        /* Fallback: if no quality was <= target_height, choose lowest available */
        if (!chosen_source && qcount > 0) {
            json_t *last_entry = json_array_get(j_quals, qcount - 1);
            if (last_entry && json_is_object(last_entry)) {
                json_t *s_url = json_object_get(last_entry, "sourceURL");
                if (json_is_string(s_url)) chosen_source = json_string_value(s_url);
            }
        }
    }

    if (!chosen_source) {
        free(escaped_token);
        json_decref(root);
        return -1;
    }

    snprintf(out_url, cap, "%s?sig=%s&token=%s", chosen_source, sig, escaped_token);
    free(escaped_token);
    json_decref(root);
    return 0;
}






int twitch_api_get_featured(const char *lang, int page, volatile int *cancel, TwitchStreamList *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);

    const char *body =
        "{\"operationName\":\"BrowsePage_Popular\","
        "\"variables\":{\"limit\":30},"
        "\"query\":\"query BrowsePage_Popular($limit: Int!) { "
        "streams(first: $limit) { edges { node { id title viewersCount previewImageURL(width: 440, height: 248) "
        "broadcaster { id login displayName profileImageURL(width: 70) } "
        "game { id name } } } } }\"}";

    http_buf_t buf = {0};
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, cancel, &buf);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_streams = j_data ? json_object_get(j_data, "streams") : NULL;
    json_t *j_edges = j_streams ? json_object_get(j_streams, "edges") : NULL;

    if (json_is_array(j_edges)) {
        size_t arr_sz = json_array_size(j_edges);
        for (size_t i = 0; i < arr_sz && out->count < TWITCH_PAGE_SIZE_MAX; i++) {
            json_t *edge = json_array_get(j_edges, i);
            json_t *node = edge ? json_object_get(edge, "node") : NULL;
            if (!node || !json_is_object(node)) continue;

            TwitchStream *s = &out->items[out->count];
            json_t *j_id = json_object_get(node, "id");
            if (json_is_string(j_id)) s->id = strtoull(json_string_value(j_id), NULL, 10);

            json_t *j_title = json_object_get(node, "title");
            if (json_is_string(j_title)) snprintf(s->session_title, sizeof s->session_title, "%s", json_string_value(j_title));

            json_t *j_viewers = json_object_get(node, "viewersCount");
            if (json_is_integer(j_viewers)) s->viewer_count = (uint32_t)json_integer_value(j_viewers);

            json_t *j_thumb = json_object_get(node, "previewImageURL");
            if (json_is_string(j_thumb)) snprintf(s->thumbnail_url, sizeof s->thumbnail_url, "%s", json_string_value(j_thumb));

            json_t *j_bc = json_object_get(node, "broadcaster");
            if (j_bc && json_is_object(j_bc)) {
                json_t *j_login = json_object_get(j_bc, "login");
                if (json_is_string(j_login)) snprintf(s->channel_slug, sizeof s->channel_slug, "%s", json_string_value(j_login));

                json_t *j_name = json_object_get(j_bc, "displayName");
                if (json_is_string(j_name)) snprintf(s->username, sizeof s->username, "%s", json_string_value(j_name));

                json_t *j_avatar = json_object_get(j_bc, "profileImageURL");
                if (json_is_string(j_avatar)) snprintf(s->profile_pic, sizeof s->profile_pic, "%s", json_string_value(j_avatar));
            }

            json_t *j_game = json_object_get(node, "game");
            if (j_game && json_is_object(j_game)) {
                json_t *j_gname = json_object_get(j_game, "name");
                if (json_is_string(j_gname)) {
                    snprintf(s->category_name, sizeof s->category_name, "%s", json_string_value(j_gname));
                    snprintf(s->category_slug, sizeof s->category_slug, "%s", json_string_value(j_gname));
                }
            }

            s->is_live = 1;
            out->count++;
        }
    }

    out->current_page = page;
    out->has_next_page = (out->count >= 30);
    json_decref(root);
    return 0;
}




int twitch_api_get_categories(volatile int *cancel, TwitchCategoryList *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);

    const char *body =
        "{\"operationName\":\"TopGames\","
        "\"variables\":{\"limit\":30},"
        "\"query\":\"query TopGames($limit: Int!) { "
        "games(first: $limit) { edges { node { id name boxArtURL(width: 285, height: 380) viewersCount } } } }\"}";

    http_buf_t buf = {0};
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, cancel, &buf);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_games = j_data ? json_object_get(j_data, "games") : NULL;
    json_t *j_edges = j_games ? json_object_get(j_games, "edges") : NULL;

    if (json_is_array(j_edges)) {
        size_t arr_sz = json_array_size(j_edges);
        for (size_t i = 0; i < arr_sz && out->count < TWITCH_PAGE_SIZE_MAX; i++) {
            json_t *edge = json_array_get(j_edges, i);
            json_t *node = edge ? json_object_get(edge, "node") : NULL;
            if (!node || !json_is_object(node)) continue;

            TwitchCategory *c = &out->items[out->count];
            json_t *j_id = json_object_get(node, "id");
            if (json_is_string(j_id)) c->id = strtoull(json_string_value(j_id), NULL, 10);

            json_t *j_name = json_object_get(node, "name");
            if (json_is_string(j_name)) {
                snprintf(c->name, sizeof c->name, "%s", json_string_value(j_name));
                snprintf(c->slug, sizeof c->slug, "%s", json_string_value(j_name));
            }

            json_t *j_box = json_object_get(node, "boxArtURL");
            if (json_is_string(j_box)) snprintf(c->banner_url, sizeof c->banner_url, "%s", json_string_value(j_box));

            json_t *j_v = json_object_get(node, "viewersCount");
            if (json_is_integer(j_v)) c->viewers = (uint32_t)json_integer_value(j_v);

            out->count++;
        }
    }

    json_decref(root);
    return 0;
}




int twitch_api_get_subcategory_streams(const char *slug, int page, volatile int *cancel, TwitchStreamList *out) {
    if (!slug || !*slug || !out) return -1;
    memset(out, 0, sizeof *out);

    json_t *req = json_object();
    json_object_set_new(req, "query", json_string(
        "query GameStreams($game: String!, $limit: Int!) { "
        "game(name: $game) { streams(first: $limit) { edges { node { id title viewersCount previewImageURL(width: 440, height: 248) "
        "broadcaster { id login displayName profileImageURL(width: 70) } "
        "game { id name } } } } } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "game", json_string(slug));
    json_object_set_new(vars, "limit", json_integer(30));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_game = j_data ? json_object_get(j_data, "game") : NULL;
    json_t *j_streams = j_game ? json_object_get(j_game, "streams") : NULL;
    json_t *j_edges = j_streams ? json_object_get(j_streams, "edges") : NULL;

    if (json_is_array(j_edges)) {
        size_t arr_sz = json_array_size(j_edges);
        for (size_t i = 0; i < arr_sz && out->count < TWITCH_PAGE_SIZE_MAX; i++) {
            json_t *edge = json_array_get(j_edges, i);
            json_t *node = edge ? json_object_get(edge, "node") : NULL;
            if (!node || !json_is_object(node)) continue;

            TwitchStream *s = &out->items[out->count];
            json_t *j_id = json_object_get(node, "id");
            if (json_is_string(j_id)) s->id = strtoull(json_string_value(j_id), NULL, 10);

            json_t *j_title = json_object_get(node, "title");
            if (json_is_string(j_title)) snprintf(s->session_title, sizeof s->session_title, "%s", json_string_value(j_title));

            json_t *j_viewers = json_object_get(node, "viewersCount");
            if (json_is_integer(j_viewers)) s->viewer_count = (uint32_t)json_integer_value(j_viewers);

            json_t *j_thumb = json_object_get(node, "previewImageURL");
            if (json_is_string(j_thumb)) snprintf(s->thumbnail_url, sizeof s->thumbnail_url, "%s", json_string_value(j_thumb));

            json_t *j_bc = json_object_get(node, "broadcaster");
            if (j_bc && json_is_object(j_bc)) {
                json_t *j_login = json_object_get(j_bc, "login");
                if (json_is_string(j_login)) snprintf(s->channel_slug, sizeof s->channel_slug, "%s", json_string_value(j_login));

                json_t *j_name = json_object_get(j_bc, "displayName");
                if (json_is_string(j_name)) snprintf(s->username, sizeof s->username, "%s", json_string_value(j_name));

                json_t *j_avatar = json_object_get(j_bc, "profileImageURL");
                if (json_is_string(j_avatar)) snprintf(s->profile_pic, sizeof s->profile_pic, "%s", json_string_value(j_avatar));
            }

            snprintf(s->category_name, sizeof s->category_name, "%s", slug);
            snprintf(s->category_slug, sizeof s->category_slug, "%s", slug);
            s->is_live = 1;
            out->count++;
        }
    }

    out->current_page = page;
    out->has_next_page = (out->count >= 30);
    json_decref(root);
    return 0;
}




static void followed_cache_add(const char *slug);
static void followed_cache_remove(const char *slug);

int twitch_api_get_channel(const char *channel_slug, volatile int *cancel, TwitchChannel *out) {
    if (!channel_slug || !*channel_slug || !out) return -1;
    memset(out, 0, sizeof *out);

    json_t *req = json_object();
    json_object_set_new(req, "query", json_string(
        "query ChannelInfo($login: String!) { "
        "user(login: $login) { id login displayName profileImageURL(width: 300) description "
        "self { follower { followedAt } } "
        "stream { id title viewersCount previewImageURL(width: 440, height: 248) "
        "game { id name } } } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "login", json_string(channel_slug));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_user = j_data ? json_object_get(j_data, "user") : NULL;
    if (!j_user || !json_is_object(j_user)) {
        json_decref(root);
        return -1;
    }

    json_t *j_id = json_object_get(j_user, "id");
    if (json_is_string(j_id)) out->id = strtoull(json_string_value(j_id), NULL, 10);

    json_t *j_login = json_object_get(j_user, "login");
    if (json_is_string(j_login)) snprintf(out->slug, sizeof out->slug, "%s", json_string_value(j_login));

    json_t *j_name = json_object_get(j_user, "displayName");
    if (json_is_string(j_name)) snprintf(out->username, sizeof out->username, "%s", json_string_value(j_name));

    json_t *j_pic = json_object_get(j_user, "profileImageURL");
    if (json_is_string(j_pic)) snprintf(out->profile_pic, sizeof out->profile_pic, "%s", json_string_value(j_pic));

    json_t *j_bio = json_object_get(j_user, "description");
    if (json_is_string(j_bio)) snprintf(out->bio, sizeof out->bio, "%s", json_string_value(j_bio));

    /* Sync authoritative follow status from Twitch server */
    if (twitch_auth_is_logged_in() && out->slug[0]) {
        json_t *j_self = json_object_get(j_user, "self");
        if (j_self && json_is_object(j_self)) {
            json_t *j_fol = json_object_get(j_self, "follower");
            if (j_fol && json_is_object(j_fol)) {
                followed_cache_add(out->slug);
            } else if (json_is_null(j_fol)) {
                followed_cache_remove(out->slug);
            }
        }
    }

    json_t *j_stream = json_object_get(j_user, "stream");
    if (j_stream && json_is_object(j_stream)) {
        out->is_live = 1;
        out->livestream.is_live = 1;

        json_t *js_id = json_object_get(j_stream, "id");
        if (json_is_string(js_id)) out->livestream.id = strtoull(json_string_value(js_id), NULL, 10);

        json_t *js_title = json_object_get(j_stream, "title");
        if (json_is_string(js_title)) snprintf(out->livestream.session_title, sizeof out->livestream.session_title, "%s", json_string_value(js_title));

        json_t *js_v = json_object_get(j_stream, "viewersCount");
        if (json_is_integer(js_v)) out->livestream.viewer_count = (uint32_t)json_integer_value(js_v);

        json_t *js_thumb = json_object_get(j_stream, "previewImageURL");
        if (json_is_string(js_thumb)) snprintf(out->livestream.thumbnail_url, sizeof out->livestream.thumbnail_url, "%s", json_string_value(js_thumb));

        json_t *js_game = json_object_get(j_stream, "game");
        if (js_game && json_is_object(js_game)) {
            json_t *jg_name = json_object_get(js_game, "name");
            if (json_is_string(jg_name)) {
                snprintf(out->livestream.category_name, sizeof out->livestream.category_name, "%s", json_string_value(jg_name));
                snprintf(out->livestream.category_slug, sizeof out->livestream.category_slug, "%s", json_string_value(jg_name));
            }
        }

        snprintf(out->livestream.channel_slug, sizeof out->livestream.channel_slug, "%s", out->slug);
        snprintf(out->livestream.username, sizeof out->livestream.username, "%s", out->username);
        snprintf(out->livestream.profile_pic, sizeof out->livestream.profile_pic, "%s", out->profile_pic);

        /* Obtain playback URL from Usher */
        twitch_api_get_stream_url(out->slug, out->livestream.playback_url, sizeof out->livestream.playback_url);
    }

    json_decref(root);
    return 0;
}




int twitch_api_get_channel_videos(const char *channel_slug, volatile int *cancel, TwitchStreamList *out) {
    if (!channel_slug || !*channel_slug || !out) return -1;
    memset(out, 0, sizeof *out);

    json_t *req = json_object();
    json_object_set_new(req, "query", json_string(
        "query ChannelVideos($login: String!, $limit: Int!) { "
        "user(login: $login) { videos(first: $limit, types: ARCHIVE) { edges { node { id title lengthSeconds previewThumbnailURL(width: 440, height: 248) viewCount } } } } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "login", json_string(channel_slug));
    json_object_set_new(vars, "limit", json_integer(30));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_user = j_data ? json_object_get(j_data, "user") : NULL;
    json_t *j_videos = j_user ? json_object_get(j_user, "videos") : NULL;
    json_t *j_edges = j_videos ? json_object_get(j_videos, "edges") : NULL;

    if (json_is_array(j_edges)) {
        size_t arr_sz = json_array_size(j_edges);
        for (size_t i = 0; i < arr_sz && out->count < TWITCH_PAGE_SIZE_MAX; i++) {
            json_t *edge = json_array_get(j_edges, i);
            json_t *node = edge ? json_object_get(edge, "node") : NULL;
            if (!node || !json_is_object(node)) continue;

            TwitchStream *s = &out->items[out->count];
            json_t *j_id = json_object_get(node, "id");
            if (json_is_string(j_id)) s->id = strtoull(json_string_value(j_id), NULL, 10);

            json_t *j_title = json_object_get(node, "title");
            if (json_is_string(j_title)) snprintf(s->session_title, sizeof s->session_title, "%s", json_string_value(j_title));

            json_t *j_views = json_object_get(node, "viewCount");
            if (json_is_integer(j_views)) s->viewer_count = (uint32_t)json_integer_value(j_views);

            json_t *j_len = json_object_get(node, "lengthSeconds");
            if (json_is_number(j_len)) {
                s->duration_ms = (uint64_t)(json_number_value(j_len) * 1000.0);
            } else if (json_is_string(j_len)) {
                s->duration_ms = (uint64_t)(atof(json_string_value(j_len)) * 1000.0);
            }

            json_t *j_thumb = json_object_get(node, "previewThumbnailURL");
            if (json_is_string(j_thumb)) snprintf(s->thumbnail_url, sizeof s->thumbnail_url, "%s", json_string_value(j_thumb));

            snprintf(s->channel_slug, sizeof s->channel_slug, "%s", channel_slug);
            snprintf(s->username, sizeof s->username, "%s", channel_slug);
            s->is_live = 0;
            out->count++;
        }
    }

    json_decref(root);
    return 0;
}





int twitch_api_get_followed(volatile int *cancel, TwitchStreamList *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!twitch_auth_is_logged_in()) return 0;

    const char *body =
        "{\"query\":\"query FollowedStreams { currentUser { "
        "follows(first: 100) { edges { node { id login displayName profileImageURL(width: 70) "
        "stream { id title viewersCount previewImageURL(width: 440, height: 248) "
        "game { name } } } } } } }\"}";

    http_buf_t buf = {0};
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, cancel, &buf);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_curr = j_data ? json_object_get(j_data, "currentUser") : NULL;
    json_t *j_follows = j_curr ? json_object_get(j_curr, "follows") : NULL;
    json_t *j_edges = j_follows ? json_object_get(j_follows, "edges") : NULL;

    if (json_is_array(j_edges)) {
        size_t arr_sz = json_array_size(j_edges);
        /* Refresh authoritative followed cache with ALL returned followed logins */
        twitch_api_clear_followed_cache();
        for (size_t i = 0; i < arr_sz; i++) {
            json_t *edge = json_array_get(j_edges, i);
            json_t *node = edge ? json_object_get(edge, "node") : NULL;
            if (!node || !json_is_object(node)) continue;

            json_t *j_login = json_object_get(node, "login");
            if (json_is_string(j_login)) {
                followed_cache_add(json_string_value(j_login));
            }
            if (out->count >= TWITCH_PAGE_SIZE_MAX) continue;

            TwitchStream *s = &out->items[out->count];

            if (json_is_string(j_login)) snprintf(s->channel_slug, sizeof s->channel_slug, "%s", json_string_value(j_login));

            json_t *j_name = json_object_get(node, "displayName");
            if (json_is_string(j_name)) snprintf(s->username, sizeof s->username, "%s", json_string_value(j_name));

            json_t *j_pic = json_object_get(node, "profileImageURL");
            if (json_is_string(j_pic)) snprintf(s->profile_pic, sizeof s->profile_pic, "%s", json_string_value(j_pic));

            json_t *j_node_id = json_object_get(node, "id");
            if (json_is_string(j_node_id)) s->id = strtoull(json_string_value(j_node_id), NULL, 10);

            json_t *j_stream = json_object_get(node, "stream");
            if (j_stream && json_is_object(j_stream)) {
                s->is_live = 1;

                json_t *js_id = json_object_get(j_stream, "id");
                if (json_is_string(js_id)) s->id = strtoull(json_string_value(js_id), NULL, 10);

                json_t *js_title = json_object_get(j_stream, "title");
                if (json_is_string(js_title)) snprintf(s->session_title, sizeof s->session_title, "%s", json_string_value(js_title));

                json_t *js_v = json_object_get(j_stream, "viewersCount");
                if (json_is_integer(js_v)) s->viewer_count = (uint32_t)json_integer_value(js_v);

                json_t *js_thumb = json_object_get(j_stream, "previewImageURL");
                if (json_is_string(js_thumb)) snprintf(s->thumbnail_url, sizeof s->thumbnail_url, "%s", json_string_value(js_thumb));

                json_t *js_game = json_object_get(j_stream, "game");
                if (js_game && json_is_object(js_game)) {
                    json_t *jg_name = json_object_get(js_game, "name");
                    if (json_is_string(jg_name)) {
                        snprintf(s->category_name, sizeof s->category_name, "%s", json_string_value(jg_name));
                        snprintf(s->category_slug, sizeof s->category_slug, "%s", json_string_value(jg_name));
                    }
                }
            } else {
                s->is_live = 0;
                s->viewer_count = 0;
                s->session_title[0] = '\0';
                s->thumbnail_url[0] = '\0';
                s->category_name[0] = '\0';
                s->category_slug[0] = '\0';
            }

            out->count++;
        }

        /* Sort: live channels first (by viewers descending), then offline channels alphabetically */
        for (int a = 0; a < out->count - 1; a++) {
            for (int b = a + 1; b < out->count; b++) {
                int swap = 0;
                if (!out->items[a].is_live && out->items[b].is_live) {
                    swap = 1;
                } else if (out->items[a].is_live && out->items[b].is_live) {
                    if (out->items[a].viewer_count < out->items[b].viewer_count) swap = 1;
                } else if (!out->items[a].is_live && !out->items[b].is_live) {
                    if (strcasecmp(out->items[a].username, out->items[b].username) > 0) swap = 1;
                }
                if (swap) {
                    TwitchStream tmp = out->items[a];
                    out->items[a] = out->items[b];
                    out->items[b] = tmp;
                }
            }
        }
    }

    json_decref(root);
    return 0;
}




int twitch_api_search(const char *query, volatile int *cancel, TwitchStreamList *out) {
    if (!query || !*query || !out) return -1;
    memset(out, 0, sizeof *out);

    json_t *req = json_object();
    json_object_set_new(req, "query", json_string(
        "query Search($userQuery: String!) { "
        "searchFor(userQuery: $userQuery, platform: \"web\") { "
        "channels { items { id login displayName profileImageURL(width: 70) "
        "stream { id viewersCount game { name } previewImageURL(width: 440, height: 248) } } } } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "userQuery", json_string(query));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_search = j_data ? json_object_get(j_data, "searchFor") : NULL;
    json_t *j_channels = j_search ? json_object_get(j_search, "channels") : NULL;
    json_t *j_items = j_channels ? json_object_get(j_channels, "items") : NULL;

    if (json_is_array(j_items)) {
        size_t arr_sz = json_array_size(j_items);
        for (size_t i = 0; i < arr_sz && out->count < TWITCH_PAGE_SIZE_MAX; i++) {
            json_t *item = json_array_get(j_items, i);
            if (!item || !json_is_object(item)) continue;

            TwitchStream *s = &out->items[out->count];

            json_t *j_login = json_object_get(item, "login");
            if (json_is_string(j_login)) snprintf(s->channel_slug, sizeof s->channel_slug, "%s", json_string_value(j_login));

            json_t *j_name = json_object_get(item, "displayName");
            if (json_is_string(j_name)) snprintf(s->username, sizeof s->username, "%s", json_string_value(j_name));

            json_t *j_pic = json_object_get(item, "profileImageURL");
            if (json_is_string(j_pic)) snprintf(s->profile_pic, sizeof s->profile_pic, "%s", json_string_value(j_pic));

            json_t *j_stream = json_object_get(item, "stream");
            if (j_stream && json_is_object(j_stream)) {
                s->is_live = 1;
                json_t *js_id = json_object_get(j_stream, "id");
                if (json_is_string(js_id)) s->id = strtoull(json_string_value(js_id), NULL, 10);

                json_t *js_v = json_object_get(j_stream, "viewersCount");
                if (json_is_integer(js_v)) s->viewer_count = (uint32_t)json_integer_value(js_v);

                json_t *js_thumb = json_object_get(j_stream, "previewImageURL");
                if (json_is_string(js_thumb)) snprintf(s->thumbnail_url, sizeof s->thumbnail_url, "%s", json_string_value(js_thumb));

                json_t *js_game = json_object_get(j_stream, "game");
                if (js_game && json_is_object(js_game)) {
                    json_t *jg_name = json_object_get(js_game, "name");
                    if (json_is_string(jg_name)) {
                        snprintf(s->category_name, sizeof s->category_name, "%s", json_string_value(jg_name));
                        snprintf(s->category_slug, sizeof s->category_slug, "%s", json_string_value(jg_name));
                    }
                }
            } else {
                s->is_live = 0;
            }

            out->count++;
        }
    }

    json_decref(root);
    return 0;
}




/* Follow management */
#define MAX_FOLLOWED_CACHE 512
static char s_followed_cache[MAX_FOLLOWED_CACHE][TWITCH_SLUG_MAX];
static int s_followed_count = 0;
static volatile uint32_t s_follow_generation = 0;
static pthread_mutex_t s_followed_lock = PTHREAD_MUTEX_INITIALIZER;

static void followed_cache_add(const char *slug) {
    if (!slug || !*slug) return;
    pthread_mutex_lock(&s_followed_lock);
    for (int i = 0; i < s_followed_count; i++) {
        if (strcasecmp(s_followed_cache[i], slug) == 0) {
            pthread_mutex_unlock(&s_followed_lock);
            return;
        }
    }
    if (s_followed_count < MAX_FOLLOWED_CACHE) {
        snprintf(s_followed_cache[s_followed_count++], TWITCH_SLUG_MAX, "%s", slug);
    }
    pthread_mutex_unlock(&s_followed_lock);
}

static void followed_cache_remove(const char *slug) {
    if (!slug || !*slug) return;
    pthread_mutex_lock(&s_followed_lock);
    for (int i = 0; i < s_followed_count; i++) {
        if (strcasecmp(s_followed_cache[i], slug) == 0) {
            for (int j = i; j < s_followed_count - 1; j++) {
                memcpy(s_followed_cache[j], s_followed_cache[j + 1], TWITCH_SLUG_MAX);
            }
            s_followed_count--;
            break;
        }
    }
    pthread_mutex_unlock(&s_followed_lock);
}

void twitch_api_clear_followed_cache(void) {
    pthread_mutex_lock(&s_followed_lock);
    s_followed_count = 0;
    pthread_mutex_unlock(&s_followed_lock);
}

uint32_t twitch_api_followed_generation(void) {
    return s_follow_generation;
}

int twitch_api_is_channel_followed(const char *channel_slug) {
    if (!channel_slug || !*channel_slug) return 0;
    pthread_mutex_lock(&s_followed_lock);
    int found = 0;
    for (int i = 0; i < s_followed_count; i++) {
        if (strcasecmp(s_followed_cache[i], channel_slug) == 0) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_followed_lock);
    return found;
}

static int resolve_user_id(const char *login, char *out_id, size_t cap) {
    if (!login || !*login || !out_id || cap < 4) return -1;
    out_id[0] = '\0';

    json_t *req = json_object();
    json_object_set_new(req, "query", json_string("query ResolveUser($login: String!) { user(login: $login) { id } }"));
    json_t *vars = json_object();
    json_object_set_new(vars, "login", json_string(login));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    volatile int cancel = 0;
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, &cancel, &buf);
    free(body);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf.data, 0, &jerr);
    http_buf_free(&buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_user = j_data ? json_object_get(j_data, "user") : NULL;
    json_t *j_id = j_user ? json_object_get(j_user, "id") : NULL;
    if (json_is_string(j_id)) {
        snprintf(out_id, cap, "%s", json_string_value(j_id));
        json_decref(root);
        return 0;
    }

    json_decref(root);
    return -1;
}

int twitch_api_follow_channel(const char *channel_slug) {
    if (!channel_slug || !*channel_slug) return -1;

    /* Add to local cache if not present */
    followed_cache_add(channel_slug);
    s_follow_generation++;

    if (!twitch_auth_is_logged_in()) return 0;

    char target_id[64] = {0};
    if (resolve_user_id(channel_slug, target_id, sizeof target_id) < 0 || !target_id[0]) {
        vt_log("twitch_api: could not resolve user ID for follow: %s", channel_slug);
        return -1;
    }

    json_t *req = json_object();
    json_object_set_new(req, "query", json_string(
        "mutation FollowUser($targetID: ID!) { "
        "followUser(input: { targetID: $targetID, disableNotifications: false }) { "
        "follow { user { id } } } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "targetID", json_string(target_id));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    volatile int cancel = 0;
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, &cancel, &buf);
    free(body);
    http_buf_free(&buf);
    return ret;
}

int twitch_api_unfollow_channel(const char *channel_slug) {
    if (!channel_slug || !*channel_slug) return -1;

    /* Remove from local cache */
    followed_cache_remove(channel_slug);
    s_follow_generation++;

    if (!twitch_auth_is_logged_in()) return 0;

    char target_id[64] = {0};
    if (resolve_user_id(channel_slug, target_id, sizeof target_id) < 0 || !target_id[0]) {
        vt_log("twitch_api: could not resolve user ID for unfollow: %s", channel_slug);
        return -1;
    }

    json_t *req = json_object();
    json_object_set_new(req, "query", json_string(
        "mutation UnfollowUser($targetID: ID!) { "
        "unfollowUser(input: { targetID: $targetID }) { "
        "follow { user { id } } } }"));

    json_t *vars = json_object();
    json_object_set_new(vars, "targetID", json_string(target_id));
    json_object_set_new(req, "variables", vars);

    char *body = json_dumps(req, 0);
    json_decref(req);
    if (!body) return -1;

    http_buf_t buf = {0};
    volatile int cancel = 0;
    int ret = gql_request(body, TWITCH_TV_CLIENT_ID, &cancel, &buf);
    free(body);
    http_buf_free(&buf);
    return ret;
}

typedef struct {
    char slug[TWITCH_SLUG_MAX];
    int follow;
} FollowTaskArg;

static void *follow_worker_th(void *arg) {
    FollowTaskArg *t = (FollowTaskArg *)arg;
    if (t) {
        if (t->follow) {
            twitch_api_follow_channel(t->slug);
        } else {
            twitch_api_unfollow_channel(t->slug);
        }
        free(t);
    }
    return NULL;
}

int twitch_api_follow_channel_async(const char *channel_slug) {
    if (!channel_slug || !*channel_slug) return -1;
    followed_cache_add(channel_slug);
    s_follow_generation++;
    FollowTaskArg *arg = malloc(sizeof(FollowTaskArg));
    if (!arg) return -1;
    snprintf(arg->slug, sizeof arg->slug, "%s", channel_slug);
    arg->follow = 1;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_create(&th, &attr, follow_worker_th, arg);
    pthread_attr_destroy(&attr);
    return 0;
}

int twitch_api_unfollow_channel_async(const char *channel_slug) {
    if (!channel_slug || !*channel_slug) return -1;
    followed_cache_remove(channel_slug);
    s_follow_generation++;
    FollowTaskArg *arg = malloc(sizeof(FollowTaskArg));
    if (!arg) return -1;
    snprintf(arg->slug, sizeof arg->slug, "%s", channel_slug);
    arg->follow = 0;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_create(&th, &attr, follow_worker_th, arg);
    pthread_attr_destroy(&attr);
    return 0;
}

int twitch_api_sync_followed(volatile int *cancel) {
    TwitchStreamList list;
    return twitch_api_get_followed(cancel, &list);
}

static void *sync_followed_worker_th(void *arg) {
    (void)arg;
    volatile int cancel = 0;
    twitch_api_sync_followed(&cancel);
    return NULL;
}

void twitch_api_sync_followed_async(void) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_create(&th, &attr, sync_followed_worker_th, NULL);
    pthread_attr_destroy(&attr);
}
