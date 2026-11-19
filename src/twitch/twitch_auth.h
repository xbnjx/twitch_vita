/* Twitch Vita — Native Authentication (OAuth 2.0 Device Code Grant & QR Flow) */
#ifndef TWITCH_AUTH_H
#define TWITCH_AUTH_H

#include <stdint.h>
#include <stddef.h>

#define TWITCH_TV_CLIENT_ID "ue6666qo983tsx6so1t0vnawi233wa"
#define TWITCH_WEB_CLIENT_ID "kimne78kx3ncx6brgo4mv6wki5h1ko"

typedef enum {
    TWITCH_AUTH_SUCCESS = 0,
    TWITCH_AUTH_ERR_CREDENTIALS = -1,
    TWITCH_AUTH_NEED_2FA = -2,
    TWITCH_AUTH_ERR_NETWORK = -3,
    TWITCH_AUTH_ERR_UNKNOWN = -4
} TwitchAuthStatus;

typedef struct TwitchAuthUser {
    int authenticated;
    uint64_t user_id;
    char username[64];
    char display_name[64];
    char email[128];
    char profile_pic[512];
    char token[512];         /* OAuth access token */
    char refresh_token[512]; /* OAuth refresh token */
} TwitchAuthUser;

/* Compatibility typedefs */
typedef TwitchAuthStatus KickAuthStatus;
typedef TwitchAuthUser KickAuthUser;
#define KICK_AUTH_SUCCESS TWITCH_AUTH_SUCCESS
#define KICK_AUTH_ERR_CREDENTIALS TWITCH_AUTH_ERR_CREDENTIALS
#define KICK_AUTH_NEED_2FA TWITCH_AUTH_NEED_2FA
#define KICK_AUTH_ERR_NETWORK TWITCH_AUTH_ERR_NETWORK
#define KICK_AUTH_ERR_UNKNOWN TWITCH_AUTH_ERR_UNKNOWN

void twitch_auth_init(void);
int twitch_auth_refresh_profile(void);
int twitch_auth_is_logged_in(void);
const TwitchAuthUser *twitch_auth_get_user(void);
const char *twitch_auth_get_token(void);
void twitch_auth_logout(void);

/* Generate TV login parameters: calls Twitch Device Code endpoint.
 * Returns device_code, user_code (e.g. "WDJBZST3"), and qr_url ("https://www.twitch.tv/activate?device-code=...&device-metadata=qr_code") */
int twitch_auth_tv_start(char *out_device_code, size_t dev_cap,
                         char *out_user_code, size_t code_cap,
                         char *out_url, size_t url_cap);

/* Polls Twitch OAuth token endpoint once.
 * Returns 1 on success, 0 on pending, -1 on fatal error. */
int twitch_auth_tv_poll_once(const char *device_code,
                             volatile int *cancel_flag,
                             char *err_msg, size_t err_msg_cap);

/* Compatibility macros */
#define kick_auth_init twitch_auth_init
#define kick_auth_refresh_profile twitch_auth_refresh_profile
#define kick_auth_is_logged_in twitch_auth_is_logged_in
#define kick_auth_get_user twitch_auth_get_user
#define kick_auth_get_token twitch_auth_get_token
#define kick_auth_logout twitch_auth_logout
#define kick_auth_tv_start(uuid, uuid_cap, key, key_cap, url, url_cap) \
    twitch_auth_tv_start(uuid, uuid_cap, key, key_cap, url, url_cap)
#define kick_auth_tv_poll_once(uuid, key, cancel, err_msg, err_cap) \
    twitch_auth_tv_poll_once(uuid, cancel, err_msg, err_cap)

#endif
