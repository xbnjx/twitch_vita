/* Twitch Vita — Native Authentication implementation */
#include "twitch/twitch_auth.h"
#include "twitch/twitch_api.h"
#include "app.h"
#include "log.h"
#include "net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#define TWITCH_TOKEN_FILE TWITCH_DATA_DIR "/token.txt"

static TwitchAuthUser g_auth = {0};

static void ensure_data_dir(void) {
    sceIoMkdir(TWITCH_DATA_DIR, 0777);
}

static int fetch_user_profile(const char *token, volatile int *cancel_flag) {
    if (!token || !*token) return -1;

    char auth_header[600];
    snprintf(auth_header, sizeof auth_header, "Authorization: OAuth %s", token);
    const char *headers[] = {
        "Client-Id: " TWITCH_TV_CLIENT_ID,
        auth_header,
        "Accept: application/json",
        NULL
    };

    const char *body = "{\"query\":\"query CurrentUser { currentUser { id login displayName profileImageURL(width: 300) email } }\"}";

    http_buf_t res_buf = {0};
    long status = 0;
    int ret = http_post("https://gql.twitch.tv/gql", "application/json", body, strlen(body), headers, cancel_flag, &res_buf, &status);
    if (ret < 0 || status != 200 || !res_buf.data) {
        http_buf_free(&res_buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(res_buf.data, 0, &jerr);
    http_buf_free(&res_buf);
    if (!root) return -1;

    json_t *j_data = json_object_get(root, "data");
    json_t *j_user = j_data ? json_object_get(j_data, "currentUser") : NULL;
    if (j_user && json_is_object(j_user)) {
        json_t *j_id = json_object_get(j_user, "id");
        if (json_is_string(j_id)) {
            g_auth.user_id = strtoull(json_string_value(j_id), NULL, 10);
        }
        json_t *j_login = json_object_get(j_user, "login");
        if (json_is_string(j_login)) {
            snprintf(g_auth.username, sizeof g_auth.username, "%s", json_string_value(j_login));
        }
        json_t *j_disp = json_object_get(j_user, "displayName");
        if (json_is_string(j_disp)) {
            snprintf(g_auth.display_name, sizeof g_auth.display_name, "%s", json_string_value(j_disp));
        } else {
            snprintf(g_auth.display_name, sizeof g_auth.display_name, "%s", g_auth.username);
        }
        json_t *j_email = json_object_get(j_user, "email");
        if (json_is_string(j_email)) {
            snprintf(g_auth.email, sizeof g_auth.email, "%s", json_string_value(j_email));
        }
        json_t *j_pic = json_object_get(j_user, "profileImageURL");
        if (json_is_string(j_pic)) {
            snprintf(g_auth.profile_pic, sizeof g_auth.profile_pic, "%s", json_string_value(j_pic));
        }
    }
    json_decref(root);
    return 0;
}

static void twitch_auth_save(void) {
    ensure_data_dir();
    json_t *root = json_object();
    json_object_set_new(root, "authenticated", json_integer(g_auth.authenticated));
    json_object_set_new(root, "user_id", json_integer(g_auth.user_id));
    json_object_set_new(root, "username", json_string(g_auth.username));
    json_object_set_new(root, "display_name", json_string(g_auth.display_name));
    json_object_set_new(root, "email", json_string(g_auth.email));
    json_object_set_new(root, "profile_pic", json_string(g_auth.profile_pic));
    json_object_set_new(root, "token", json_string(g_auth.token));
    json_object_set_new(root, "refresh_token", json_string(g_auth.refresh_token));

    char *dump = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    if (!dump) return;

    vt_io_lock();
    FILE *f = fopen(TWITCH_AUTH_PATH, "wb");
    if (f) {
        fwrite(dump, 1, strlen(dump), f);
        fclose(f);
    }
    vt_io_unlock();
    free(dump);
}

void twitch_auth_init(void) {
    memset(&g_auth, 0, sizeof g_auth);
    ensure_data_dir();

    vt_io_lock();
    FILE *f = fopen(TWITCH_AUTH_PATH, "rb");
    if (!f) {
        vt_io_unlock();
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) {
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

    json_t *j_auth = json_object_get(root, "authenticated");
    if (json_is_integer(j_auth)) g_auth.authenticated = json_integer_value(j_auth);

    json_t *j_uid = json_object_get(root, "user_id");
    if (json_is_integer(j_uid)) g_auth.user_id = (uint64_t)json_integer_value(j_uid);

    json_t *j_user = json_object_get(root, "username");
    if (json_is_string(j_user)) snprintf(g_auth.username, sizeof g_auth.username, "%s", json_string_value(j_user));

    json_t *j_disp = json_object_get(root, "display_name");
    if (json_is_string(j_disp)) snprintf(g_auth.display_name, sizeof g_auth.display_name, "%s", json_string_value(j_disp));

    json_t *j_email = json_object_get(root, "email");
    if (json_is_string(j_email)) snprintf(g_auth.email, sizeof g_auth.email, "%s", json_string_value(j_email));

    json_t *j_pic = json_object_get(root, "profile_pic");
    if (json_is_string(j_pic)) snprintf(g_auth.profile_pic, sizeof g_auth.profile_pic, "%s", json_string_value(j_pic));

    json_t *j_tok = json_object_get(root, "token");
    if (json_is_string(j_tok)) snprintf(g_auth.token, sizeof g_auth.token, "%s", json_string_value(j_tok));

    json_t *j_ref = json_object_get(root, "refresh_token");
    if (json_is_string(j_ref)) snprintf(g_auth.refresh_token, sizeof g_auth.refresh_token, "%s", json_string_value(j_ref));

    json_decref(root);

    if (g_auth.authenticated && g_auth.token[0] && g_auth.username[0] == '\0') {
        volatile int cancel_f = 0;
        if (fetch_user_profile(g_auth.token, &cancel_f) == 0 && g_auth.username[0] != '\0') {
            twitch_auth_save();
        }
    }
}

int twitch_auth_refresh_profile(void) {
    if (!g_auth.authenticated || !g_auth.token[0]) return -1;
    volatile int cancel_f = 0;
    int r = fetch_user_profile(g_auth.token, &cancel_f);
    if (r == 0 && g_auth.username[0] != '\0') {
        twitch_auth_save();
        return 0;
    }
    return -1;
}

int twitch_auth_is_logged_in(void) {
    return g_auth.authenticated && g_auth.token[0] != '\0';
}

const TwitchAuthUser *twitch_auth_get_user(void) {
    return &g_auth;
}

const char *twitch_auth_get_token(void) {
    return twitch_auth_is_logged_in() ? g_auth.token : NULL;
}

void twitch_auth_logout(void) {
    memset(&g_auth, 0, sizeof g_auth);
    twitch_api_clear_followed_cache();
    vt_io_lock();
    remove(TWITCH_AUTH_PATH);
    remove(TWITCH_TOKEN_FILE);
    vt_io_unlock();
}

int twitch_auth_tv_start(char *out_device_code, size_t dev_cap,
                         char *out_user_code, size_t code_cap,
                         char *out_url, size_t url_cap) {
    if (!out_device_code || dev_cap < 32 || !out_user_code || code_cap < 8 || !out_url || url_cap < 64) {
        return -1;
    }

    const char *endpoint = "https://id.twitch.tv/oauth2/device";
    const char *post_data = "client_id=" TWITCH_TV_CLIENT_ID
        "&scopes=channel_read+chat:read+user_blocks_edit+user_blocks_read+user_follows_edit+user_read";

    const char *headers[] = {
        "Accept: application/json",
        NULL
    };

    http_buf_t res_buf = {0};
    long status = 0;
    volatile int cancel = 0;
    int ret = http_post(endpoint, "application/x-www-form-urlencoded", post_data, strlen(post_data), headers, &cancel, &res_buf, &status);
    if (ret < 0 || status != 200 || !res_buf.data) {
        vt_log("twitch_auth: failed to get device code, ret=%d status=%ld", ret, status);
        http_buf_free(&res_buf);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(res_buf.data, 0, &jerr);
    http_buf_free(&res_buf);
    if (!root) return -1;

    json_t *j_dev = json_object_get(root, "device_code");
    json_t *j_user = json_object_get(root, "user_code");
    json_t *j_uri = json_object_get(root, "verification_uri");

    if (!json_is_string(j_dev) || !json_is_string(j_user)) {
        json_decref(root);
        return -1;
    }

    snprintf(out_device_code, dev_cap, "%s", json_string_value(j_dev));
    snprintf(out_user_code, code_cap, "%s", json_string_value(j_user));

    const char *ver_uri = json_is_string(j_uri) ? json_string_value(j_uri) : "https://www.twitch.tv/activate";
    /* Format QR Code URL matching official Twitch Android TV app:
     * https://www.twitch.tv/activate?device-code=<user_code>&device-metadata=qr_code */
    snprintf(out_url, url_cap, "%s?device-code=%s&device-metadata=qr_code", ver_uri, out_user_code);

    json_decref(root);
    vt_log("twitch_auth: generated TV Link URL: %s (code: %s)", out_url, out_user_code);
    return 0;
}

int twitch_auth_tv_poll_once(const char *device_code,
                             volatile int *cancel_flag,
                             char *err_msg, size_t err_msg_cap) {
    if (err_msg && err_msg_cap) err_msg[0] = '\0';
    if (!device_code || !*device_code) {
        if (err_msg) snprintf(err_msg, err_msg_cap, "Parámetros inválidos");
        return -1;
    }

    const char *endpoint = "https://id.twitch.tv/oauth2/token";
    char body[512];
    snprintf(body, sizeof body,
             "client_id=" TWITCH_TV_CLIENT_ID
             "&device_code=%s"
             "&grant_type=urn:ietf:params:oauth:grant-type:device_code",
             device_code);

    const char *headers[] = {
        "Accept: application/json",
        NULL
    };

    http_buf_t res_buf = {0};
    long status = 0;
    int ret = http_post(endpoint, "application/x-www-form-urlencoded", body, strlen(body), headers, cancel_flag, &res_buf, &status);
    if (ret < 0) {
        http_buf_free(&res_buf);
        return 0; /* Keep polling */
    }

    /* Twitch returns 200 on approval */
    if (status == 200 && res_buf.data) {
        json_error_t jerr;
        json_t *root = json_loads(res_buf.data, 0, &jerr);
        if (root && json_is_object(root)) {
            json_t *j_tok = json_object_get(root, "access_token");
            if (json_is_string(j_tok)) {
                snprintf(g_auth.token, sizeof g_auth.token, "%s", json_string_value(j_tok));
                json_t *j_ref = json_object_get(root, "refresh_token");
                if (json_is_string(j_ref)) {
                    snprintf(g_auth.refresh_token, sizeof g_auth.refresh_token, "%s", json_string_value(j_ref));
                }
                g_auth.authenticated = 1;
                json_decref(root);
                http_buf_free(&res_buf);

                /* Fetch user profile to get username, email, avatar */
                fetch_user_profile(g_auth.token, cancel_flag);
                twitch_auth_save();
                vt_log("twitch_auth: TV link authorized! User: @%s (%s)", g_auth.username, g_auth.display_name);
                return 1;
            }
            json_decref(root);
        }
    } else if (status == 400 && res_buf.data) {
        /* Check if still pending or fatal error */
        json_error_t jerr;
        json_t *root = json_loads(res_buf.data, 0, &jerr);
        if (root && json_is_object(root)) {
            json_t *j_msg = json_object_get(root, "message");
            if (json_is_string(j_msg)) {
                const char *msg = json_string_value(j_msg);
                if (strstr(msg, "authorization_pending")) {
                    json_decref(root);
                    http_buf_free(&res_buf);
                    return 0; /* Still waiting for user */
                } else if (strstr(msg, "expired")) {
                    if (err_msg) snprintf(err_msg, err_msg_cap, "El código ha expirado");
                    json_decref(root);
                    http_buf_free(&res_buf);
                    return -1;
                }
            }
            json_decref(root);
        }
    }

    http_buf_free(&res_buf);
    return 0;
}
