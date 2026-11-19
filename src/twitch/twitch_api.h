/* Twitch Vita — REST & GraphQL API client */
#ifndef TWITCH_API_H
#define TWITCH_API_H

#include "twitch/twitch_model.h"

/* Get featured live streams. If lang is NULL or empty, defaults to "es" or "en". */
int twitch_api_get_featured(const char *lang, int page, volatile int *cancel,
                            TwitchStreamList *out);

/* Get top categories / games. */
int twitch_api_get_categories(volatile int *cancel, TwitchCategoryList *out);

/* Get livestreams for a specific category / game. */
int twitch_api_get_subcategory_streams(const char *slug, int page, volatile int *cancel,
                                       TwitchStreamList *out);

/* Get channel info and its live playback_url (.m3u8). */
int twitch_api_get_channel(const char *channel_slug, volatile int *cancel,
                           TwitchChannel *out);

/* Get recent VOD videos of a channel. */
int twitch_api_get_channel_videos(const char *channel_slug, volatile int *cancel,
                                  TwitchStreamList *out);

/* Get clips of a channel. */
int twitch_api_get_channel_clips(const char *channel_slug, volatile int *cancel,
                                 TwitchStreamList *out);

/* Get live stream HLS URL for channel login */
int twitch_api_get_stream_url(const char *login, char *out_url, size_t cap);

/* Get VOD HLS URL for video ID */
int twitch_api_get_vod_url(uint64_t vod_id, char *out_url, size_t cap);

/* Get Clip progressive playback MP4 URL for a specific quality height. */
int twitch_api_get_clip_url(const char *slug_or_id, int target_height, char *out_url, size_t cap);

/* Follow/Unfollow channel */
int twitch_api_follow_channel(const char *channel_slug);
int twitch_api_follow_channel_async(const char *channel_slug);
int twitch_api_unfollow_channel(const char *channel_slug);
int twitch_api_unfollow_channel_async(const char *channel_slug);
int twitch_api_is_channel_followed(const char *channel_slug);
int twitch_api_sync_followed(volatile int *cancel);
void twitch_api_sync_followed_async(void);
void twitch_api_clear_followed_cache(void);
uint32_t twitch_api_followed_generation(void);

/* Get followed live streams for the authenticated user. */
int twitch_api_get_followed(volatile int *cancel, TwitchStreamList *out);

/* Search for a channel / streamer. */
int twitch_api_search(const char *query, volatile int *cancel,
                      TwitchStreamList *out);

/* Compatibility macros */
#define kick_api_get_featured twitch_api_get_featured
#define kick_api_get_categories twitch_api_get_categories
#define kick_api_get_subcategory_streams twitch_api_get_subcategory_streams
#define kick_api_get_channel twitch_api_get_channel
#define kick_api_get_channel_videos twitch_api_get_channel_videos
#define kick_api_get_channel_clips twitch_api_get_channel_clips
#define kick_api_follow_channel twitch_api_follow_channel
#define kick_api_follow_channel_async twitch_api_follow_channel_async
#define kick_api_unfollow_channel twitch_api_unfollow_channel
#define kick_api_unfollow_channel_async twitch_api_unfollow_channel_async
#define kick_api_is_channel_followed twitch_api_is_channel_followed
#define kick_api_sync_followed twitch_api_sync_followed
#define kick_api_sync_followed_async twitch_api_sync_followed_async
#define kick_api_clear_followed_cache twitch_api_clear_followed_cache
#define kick_api_followed_generation twitch_api_followed_generation
#define kick_api_get_followed twitch_api_get_followed
#define kick_api_search twitch_api_search

#endif
