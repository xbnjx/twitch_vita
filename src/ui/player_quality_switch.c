/* Kick Vita — Quality switching and source opening */
#include "ui/player_quality_switch.h"
#include "app.h"
#include "log.h"
#include "twitch/twitch_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

int player_source_init(PlayerSource *source, const char *url, int max_height,
                       int max_fps, volatile int *cancel) {
    if (!source || !url || !*url) return -1;
    memset(source, 0, sizeof *source);
    int ret = -1;

    /* Allow up to 720p (HD) */
    if (max_height <= 0 || max_height > 720) max_height = 720;

    /* Direct MP4 progressive video (e.g. Twitch Clips) */
    if (strstr(url, ".mp4") != NULL) {
        ret = vt_remote_stream_factory_init(&source->remote, url, cancel);
        if (ret == 0) {
            source->is_hls = 0;
            source->height = max_height;
            return 0;
        }
        return -1;
    }

    char *master = NULL;
    if (vt_hls_fetch_master(url, cancel, &master) < 0) {
        vt_log("kick_hls: master playlist fetch failed: %s", url);
        return -1;
    }

    char *video_url = malloc(4096);
    char *audio_url = malloc(4096);
    int muxed = 1;
    if (video_url && audio_url) {
        if (strstr(master, "#EXT-X-STREAM-INF") != NULL) {
            ret = vt_hls_pick(master, max_height, max_fps, video_url, 4096, audio_url, 4096);
            if (ret == 0 && video_url[0] &&
                strncmp(video_url, "http://", 7) != 0 &&
                strncmp(video_url, "https://", 8) != 0) {
                char base[2048];
                snprintf(base, sizeof base, "%s", url);
                char *q = strchr(base, '?');
                if (q) *q = '\0';
                char *slash = strrchr(base, '/');
                if (slash) *(slash + 1) = '\0';
                char resolved[4096];
                snprintf(resolved, sizeof resolved, "%s%s", base, video_url);
                snprintf(video_url, 4096, "%s", resolved);
            }
            muxed = (!audio_url || !audio_url[0]);
        } else {
            /* Direct media playlist without master variants (e.g. Kick Clips) */
            snprintf(video_url, 4096, "%s", url);
            audio_url[0] = '\0';
            muxed = 1;
            ret = 0;
        }
    }
    free(master);

    if (ret == 0) {
        ret = vt_hls_stream_factory_init(&source->hls_video, video_url, cancel);
        if (ret == 0 && !muxed) {
            ret = vt_hls_stream_factory_init(&source->hls_audio, audio_url, cancel);
            if (ret < 0) vt_hls_stream_factory_term(&source->hls_video);
        }
    }
    free(video_url);
    free(audio_url);

    if (ret < 0) return -1;

    source->is_hls = 1;
    source->hls_muxed = muxed;
    source->height = max_height;
    return 0;
}

void player_source_term(PlayerSource *source) {
    if (!source) return;
    if (source->is_hls) {
        vt_hls_stream_factory_term(&source->hls_video);
        if (!source->hls_muxed) vt_hls_stream_factory_term(&source->hls_audio);
    } else {
        vt_remote_stream_factory_term(&source->remote);
    }
    source->is_hls = 0;
    source->hls_muxed = 0;
}

void player_source_apply(const PlayerSource *source,
                         VtDecoderPlayerConfig *config) {
    if (!source || !config) return;
    if (source->is_hls) {
        config->stream = source->hls_video.factory;
        if (!source->hls_muxed) {
            config->stream_audio = source->hls_audio.factory;
        } else {
            memset(&config->stream_audio, 0, sizeof config->stream_audio);
        }
        config->approximate_seek = 1;
    } else {
        config->stream = source->remote.factory;
        memset(&config->stream_audio, 0, sizeof config->stream_audio);
    }
}

void player_source_repoint_cancel(PlayerSource *source, volatile int *cancel) {
    if (!source) return;
    if (source->is_hls) {
        source->hls_video.cancel_flag = cancel;
        if (!source->hls_muxed)
            source->hls_audio.cancel_flag = cancel;
    } else {
        source->remote.cancel_flag = cancel;
    }
}

static int get_expected_width(int target_height) {
    if (target_height >= 720) return 1280;
    if (target_height >= 480) return 854;
    if (target_height >= 360) return 640;
    if (target_height >= 240) return 426;
    return 256; /* 144p */
}

int open_source_blocking(const KickStream *stream, int target_height,
                         int volume, int start_paused, uint64_t start_position_ms,
                         volatile int *cancel,
                         PlayerSource *source_out,
                         VtDecoderPlayer **player_out) {
    if (!stream || !source_out || !player_out) return -1;
    *player_out = NULL;

    /* For VODs and Clips, map 720p (HD) to 480p in backend for smooth hardware decoding */
    int backend_height = target_height;
    if (!stream->is_live || stream->is_clip) {
        if (backend_height >= 720) {
            backend_height = 480;
        }
    }
    if (backend_height <= 0 || backend_height > 720) backend_height = stream->is_live ? 720 : 480;

    const char *stream_url = stream->playback_url;
    char live_url_buf[4096] = {0};

    /* For clips, always fetch matching quality URL */
    if (stream->is_clip && stream->clip_slug[0]) {
        if (twitch_api_get_clip_url(stream->clip_slug, backend_height, live_url_buf, sizeof live_url_buf) == 0) {
            stream_url = live_url_buf;
        }
    } else if (!stream_url || !*stream_url) {
        if (!stream->is_live && stream->id) {
            if (twitch_api_get_vod_url(stream->id, live_url_buf, sizeof live_url_buf) == 0) {
                stream_url = live_url_buf;
            }
        } else {
            if (twitch_api_get_stream_url(stream->channel_slug, live_url_buf, sizeof live_url_buf) == 0) {
                stream_url = live_url_buf;
            } else {
                KickChannel ch = {0};
                if (kick_api_get_channel(stream->channel_slug, cancel, &ch) == 0 && ch.livestream.playback_url[0]) {
                    snprintf(live_url_buf, sizeof live_url_buf, "%s", ch.livestream.playback_url);
                    stream_url = live_url_buf;
                }
            }
        }
    }

    if (!stream_url || !*stream_url) {
        vt_log("open_source: no playback_url for channel %s (is_live=%d id=%llu)",
               stream->channel_slug, stream->is_live, (unsigned long long)stream->id);
        return -1;
    }

    int ret = player_source_init(source_out, stream_url, backend_height, 30, cancel);
    if (ret < 0 && stream_url != live_url_buf) {
        /* Retry with fresh playback_url in case the token was stale or expired */
        if (!stream->is_live && stream->id) {
            if (twitch_api_get_vod_url(stream->id, live_url_buf, sizeof live_url_buf) == 0) {
                stream_url = live_url_buf;
                ret = player_source_init(source_out, stream_url, backend_height, 30, cancel);
            }
        } else {
            if (twitch_api_get_stream_url(stream->channel_slug, live_url_buf, sizeof live_url_buf) == 0) {
                stream_url = live_url_buf;
                ret = player_source_init(source_out, stream_url, backend_height, 30, cancel);
            }
        }
    }
    if (ret < 0) return -1;

    VtDecoderPlayerConfig config = {0};
    player_source_apply(source_out, &config);
    config.volume_percent = volume;
    config.cancel_flag = cancel;
    config.expected_width = get_expected_width(backend_height);
    config.expected_height = backend_height;
    config.expected_fps = 30;
    config.start_paused = start_paused;
    config.start_position_ms = start_position_ms;
    config.approximate_seek = 1;
    config.is_live = stream->is_live ? 1 : 0;

    VtDecoderPlayer *pl = vt_decoder_create();
    if (!pl) {
        player_source_term(source_out);
        return -1;
    }

    ret = vt_decoder_open(pl, &config);
    if (ret < 0) {
        vt_decoder_destroy(pl);
        player_source_term(source_out);
        return -1;
    }

    *player_out = pl;
    return 0;
}

void *prepare_worker(void *arg) {
    PrepareJob *job = arg;
    if (!job) return NULL;

    PlayerSource *source = job->source_out;
    job->result = player_source_init(source, job->url, job->target_height, job->max_fps, &job->cancel);
    if (job->result < 0 || job->cancel) {
        player_source_term(source);
        job->result = -1;
        job->done = 1;
        return NULL;
    }

    VtDecoderPlayerConfig config = {0};
    player_source_apply(source, &config);
    config.cancel_flag = &job->cancel;
    config.expected_height = job->target_height;
    config.expected_width = get_expected_width(job->target_height);
    config.expected_fps = 30;

    VtDecoderPlayer *pl = vt_decoder_create();
    if (!pl) {
        player_source_term(source);
        job->result = -1;
        job->done = 1;
        return NULL;
    }

    job->player = pl;
    job->result = 0;
    job->done = 1;
    return NULL;
}

int commit_prepared_switch(PrepareJob *job, VtDecoderPlayer **current_player,
                           PlayerSource *sources, int *current_source_idx,
                           volatile int *master_cancel) {
    if (!job || !current_player || !sources || !current_source_idx) return -1;

    int old_vol = 100;
    int was_paused = 0;
    if (*current_player) {
        VtDecoderPlayerStatus st = {0};
        vt_decoder_get_status(*current_player, &st);
        was_paused = st.paused;
        vt_decoder_destroy(*current_player);
        *current_player = NULL;
    }

    int old_idx = *current_source_idx;
    int new_idx = 1 - old_idx;
    player_source_term(&sources[old_idx]);

    PlayerSource *new_source = &sources[new_idx];
    player_source_repoint_cancel(new_source, master_cancel);

    VtDecoderPlayerConfig config = {0};
    player_source_apply(new_source, &config);
    config.volume_percent = old_vol;
    config.cancel_flag = master_cancel;
    config.expected_height = job->target_height;
    config.expected_width = get_expected_width(job->target_height);
    config.expected_fps = 30;
    config.start_paused = was_paused;

    VtDecoderPlayer *new_p = job->player ? job->player : vt_decoder_create();
    if (!new_p) {
        player_source_term(new_source);
        return -1;
    }

    int ret = vt_decoder_open(new_p, &config);
    if (ret < 0) {
        vt_decoder_destroy(new_p);
        player_source_term(new_source);
        return -1;
    }

    *current_player = new_p;
    *current_source_idx = new_idx;
    return 0;
}

int switch_vod_quality(VtDecoderPlayer **player_ptr, PlayerSource *source_ptr,
                       const KickStream *stream, int target_height, int volume,
                       int paused, uint64_t current_pos_ms,
                       volatile int *cancel) {
    if (!player_ptr || !source_ptr || !stream) return -1;

    /* Destroy existing player and free SceAvcdec hardware instance & audio port first */
    if (*player_ptr) {
        vt_decoder_destroy(*player_ptr);
        *player_ptr = NULL;
    }
    player_source_term(source_ptr);

    return open_source_blocking(stream, target_height, volume, paused,
                                current_pos_ms, cancel, source_ptr, player_ptr);
}
