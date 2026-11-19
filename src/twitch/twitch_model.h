/* Twitch Vita — Compact Data models for streams, channels, and categories. */
#ifndef TWITCH_MODEL_H
#define TWITCH_MODEL_H

#include <stdint.h>
#include <stddef.h>

#define TWITCH_TITLE_MAX 128
#define TWITCH_SLUG_MAX 48
#define TWITCH_NAME_MAX 64
#define TWITCH_URL_MAX 2048
#define TWITCH_THUMB_MAX 256
#define TWITCH_PAGE_SIZE_MAX 30

/* Compatibility aliases */
#define KICK_TITLE_MAX TWITCH_TITLE_MAX
#define KICK_SLUG_MAX TWITCH_SLUG_MAX
#define KICK_NAME_MAX TWITCH_NAME_MAX
#define KICK_URL_MAX TWITCH_URL_MAX
#define KICK_THUMB_MAX TWITCH_THUMB_MAX
#define KICK_PAGE_SIZE_MAX TWITCH_PAGE_SIZE_MAX

typedef struct TwitchStream {
    uint64_t id;
    uint64_t chatroom_id;
    char session_title[TWITCH_TITLE_MAX];
    char channel_slug[TWITCH_SLUG_MAX];    /* broadcaster login, e.g. "rubius" */
    char username[TWITCH_NAME_MAX];        /* displayName, e.g. "Rubius" */
    char category_name[TWITCH_NAME_MAX];   /* game name, e.g. "Just Chatting" */
    char category_slug[TWITCH_SLUG_MAX];
    char thumbnail_url[TWITCH_THUMB_MAX];
    char profile_pic[TWITCH_THUMB_MAX];
    char playback_url[TWITCH_URL_MAX];     /* .m3u8 playlist */
    uint32_t viewer_count;
    int is_live;
    uint64_t duration_ms;
    int is_clip;
    char clip_slug[64];
} TwitchStream;

typedef struct TwitchStreamList {
    TwitchStream items[TWITCH_PAGE_SIZE_MAX];
    int count;
    int current_page;
    int has_next_page;
} TwitchStreamList;

typedef struct TwitchCategory {
    uint64_t id;
    char name[TWITCH_NAME_MAX];
    char slug[TWITCH_SLUG_MAX];
    char banner_url[TWITCH_THUMB_MAX];
    uint32_t viewers;
} TwitchCategory;

typedef struct TwitchCategoryList {
    TwitchCategory items[TWITCH_PAGE_SIZE_MAX];
    int count;
} TwitchCategoryList;

typedef struct TwitchChannel {
    uint64_t id;
    uint64_t chatroom_id;
    char slug[TWITCH_SLUG_MAX];
    char username[TWITCH_NAME_MAX];
    char profile_pic[TWITCH_THUMB_MAX];
    char bio[256];
    int is_live;
    TwitchStream livestream;
} TwitchChannel;

/* Compatibility typedefs */
typedef TwitchStream KickStream;
typedef TwitchStreamList KickStreamList;
typedef TwitchCategory KickCategory;
typedef TwitchCategoryList KickCategoryList;
typedef TwitchChannel KickChannel;

#endif
