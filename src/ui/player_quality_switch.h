/* Kick Vita — Opening a source and switching quality mid-playback.
 * Supports HLS streaming with qualities: 144p, 240p, max 360p. */
#ifndef KICK_UI_PLAYER_QUALITY_SWITCH_H
#define KICK_UI_PLAYER_QUALITY_SWITCH_H

#include <stdint.h>
#include <stddef.h>

#include "media/hls.h"
#include "media/remote_stream.h"
#include "media/vita_decoder.h"
#include "twitch/twitch_model.h"

typedef struct {
    int is_hls;
    int hls_muxed;
    int height;
    VtRemoteStreamFactory remote;
    VtHlsStreamFactory hls_video;
    VtHlsStreamFactory hls_audio;
} PlayerSource;

/* Initialize player source for Kick HLS stream with a target quality height.
 * Supported max_height: 360 (default & max), 240, 144/160. */
int player_source_init(PlayerSource *source, const char *url, int max_height,
                       int max_fps, volatile int *cancel);

void player_source_term(PlayerSource *source);

void player_source_apply(const PlayerSource *source,
                         VtDecoderPlayerConfig *config);

void player_source_repoint_cancel(PlayerSource *source, volatile int *cancel);

/* Blocking open of a Kick stream. Returns 0 on success, <0 on failure. */
int open_source_blocking(const KickStream *stream, int target_height,
                         int volume, int start_paused, uint64_t start_position_ms,
                         volatile int *cancel,
                         PlayerSource *source_out,
                         VtDecoderPlayer **player_out);

/* Seamlessly switch quality in a VOD, maintaining exact playback position without resetting to 0:00.
 * Returns 0 on success, <0 on failure. */
int switch_vod_quality(VtDecoderPlayer **player_ptr, PlayerSource *source_ptr,
                       const KickStream *stream, int target_height, int volume,
                       int paused, uint64_t current_pos_ms,
                       volatile int *cancel);

/* Async prepare job for seamless quality switching */
typedef struct {
    const char *url;
    int target_height;
    int max_fps;
    PlayerSource *source_out;
    volatile int cancel;
    volatile int done;
    int result;
    VtDecoderPlayer *player;
} PrepareJob;

void *prepare_worker(void *arg);

int commit_prepared_switch(PrepareJob *job, VtDecoderPlayer **current_player,
                           PlayerSource *sources, int *current_source_idx,
                           volatile int *master_cancel);

#endif
