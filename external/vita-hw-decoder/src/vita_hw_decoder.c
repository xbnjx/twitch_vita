#include "vita_hw_decoder.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>

#include "module_log.h"
#include "internal/elementary_audio.h"
#include "internal/software_video.h"

#ifndef VITA_HW_DECODER_HAS_H264_VITA
#define VITA_HW_DECODER_HAS_H264_VITA 0
#endif

#define VITA_HW_DECODER_AVIO_BUFFER_SIZE (64 * 1024)
#define VITA_HW_DECODER_CLOCK_WINDOW_US 32000ULL
/* A demux read that has been in flight this long is stuck, not slow: the
 * app's own segment fetch gives up after 40 s (HLS_SERVE_TIMEOUT_US), so
 * nothing legitimate is still pending at 60 s. Without a bound, a live
 * playlist that stops growing parks both demux threads inside libavformat's
 * reload loop for its default 1000 iterations (tens of minutes) while the
 * session keeps reporting error=0/eof=0 and a frozen picture. */
#define VITA_HW_DECODER_READ_STALL_US (60ULL * 1000000ULL)

typedef struct {
	VitaHwDecoderStreamHandle stream;
	AVIOContext *avio;
	AVFormatContext *format;
	volatile int *cancel;
	/* Wall clock at which the owning demux thread entered av_read_frame(),
	 * 0 while no read is in flight. Written and read on that same thread
	 * (libavformat calls interrupt_cb from inside the blocking read), so
	 * the 64-bit width is safe on ARM32. */
	volatile uint64_t *read_started_us;
	int stall_logged;
} VitaHwDecoderInput;

struct VitaHwDecoderPlayer {
	VitaHwDecoderPlayerConfig config;
	VitaHwDecoderInput video_input;
	VitaHwDecoderInput audio_input;
	SoftwareVideoState video;
	ElementaryAudioState audio;
	volatile int local_cancel;
	volatile int *cancel;
	volatile int start_gate;
	int opened;
	int paused;
	/* Sticky: the last open/reopen failed. Reported through status.error so
	 * a session torn down by a failed seek cannot look like a healthy one
	 * (opened=0 alone is read by nobody). Cleared by the next open. */
	int session_error;
	/* The position the current session was asked to resume at (keyframe
	 * aligned). position_ms never reads below it: the raw audio clock is 0
	 * until the first grain after a reopen, and callers use position_ms as
	 * the origin of the next seek and as the quality-switch resume point. */
	uint32_t position_floor_ms;
	uint64_t duration_ms;
	uint64_t clock_us;
	uint64_t clock_wall_us;
	int clock_started;
	uint64_t pts_offset_ms;
	int pts_offset_detected;
	int auto_paused;
};

const char *vita_hw_decoder_backend_name(void) {
	return "hardware";
}

static int stream_avio_read(void *opaque, uint8_t *buffer, int size) {
	VitaHwDecoderInput *input = (VitaHwDecoderInput *)opaque;
	if (!input || !input->stream.read || (input->cancel && *input->cancel))
		return AVERROR_EXIT;
	int ret = input->stream.read(input->stream.opaque, buffer, (size_t)size);
	if (ret == 0) return AVERROR_EOF;
	return ret < 0 ? AVERROR(EIO) : ret;
}

static int64_t stream_avio_seek(void *opaque, int64_t offset, int whence) {
	VitaHwDecoderInput *input = (VitaHwDecoderInput *)opaque;
	if (!input || (input->cancel && *input->cancel)) return AVERROR_EXIT;
	if (whence == AVSEEK_SIZE) return input->stream.size;
	if (!input->stream.seek) return AVERROR(ENOSYS);
	return input->stream.seek(input->stream.opaque, offset,
	                          whence & ~AVSEEK_FORCE);
}

static int interrupt_cb(void *opaque) {
	VitaHwDecoderInput *input = (VitaHwDecoderInput *)opaque;
	if (!input) return 0;
	if (input->cancel && *input->cancel) return 1;
	/* Armed only around an in-flight av_read_frame (see read_started_us),
	 * so a paused session or a slow-but-progressing fetch never trips it. */
	if (input->read_started_us && *input->read_started_us) {
		uint64_t started = *input->read_started_us;
		uint64_t now = sceKernelGetProcessTimeWide();
		if (now > started && now - started > VITA_HW_DECODER_READ_STALL_US) {
			if (!input->stall_logged) {
				input->stall_logged = 1;
				log_printf("demux read stalled for %llu s, aborting it",
				           (unsigned long long)((now - started) / 1000000ULL));
			}
			return 1;
		}
	}
	return 0;
}

/* --- HLS sub-resource I/O -------------------------------------------------
 * This build has no network layer and no protocol handler but "file", so
 * FFmpeg cannot fetch the playlists and segments an HLS playlist points at.
 * It does not need to: every sub-resource open goes through
 * AVFormatContext.io_open, and libavformat's hls demuxer routes even its
 * nested demuxers through the parent context's callback. So the embedding
 * app's own HTTPS stack fetches, and hands back a plain memory buffer.
 * The app rewrites the URIs to "file:" beforehand; see the stream handle's
 * fetch() documentation for why that is required. */
typedef struct {
	unsigned char *data;
	size_t size;
	size_t pos;
} MemorySource;

static int memory_read(void *opaque, uint8_t *buffer, int size) {
	MemorySource *src = (MemorySource *)opaque;
	if (!src || size < 0) return AVERROR(EINVAL);
	if (src->pos >= src->size) return AVERROR_EOF;
	size_t left = src->size - src->pos;
	size_t take = (size_t)size < left ? (size_t)size : left;
	memcpy(buffer, src->data + src->pos, take);
	src->pos += take;
	return (int)take;
}

static int64_t memory_seek(void *opaque, int64_t offset, int whence) {
	MemorySource *src = (MemorySource *)opaque;
	if (!src) return AVERROR(EINVAL);
	whence &= ~AVSEEK_FORCE;
	if (whence == AVSEEK_SIZE) return (int64_t)src->size;
	int64_t target = offset;
	if (whence == SEEK_CUR) target = (int64_t)src->pos + offset;
	else if (whence == SEEK_END) target = (int64_t)src->size + offset;
	else if (whence != SEEK_SET) return AVERROR(EINVAL);
	if (target < 0 || target > (int64_t)src->size) return AVERROR(EINVAL);
	src->pos = (size_t)target;
	return target;
}

static void memory_source_free(MemorySource *src) {
	if (!src) return;
	free(src->data);
	free(src);
}

static int hls_io_open(AVFormatContext *format, AVIOContext **pb,
	                   const char *url, int flags, AVDictionary **options) {
	(void)flags;
	(void)options;
	VitaHwDecoderInput *input =
	    format ? (VitaHwDecoderInput *)format->opaque : NULL;
	if (!input || !input->stream.fetch || !pb || !url) return AVERROR(EINVAL);
	if (input->cancel && *input->cancel) return AVERROR_EXIT;

	unsigned char *data = NULL;
	size_t size = 0;
	int ret = input->stream.fetch(input->stream.opaque, url, &data, &size);
	if (ret < 0) {
		free(data);
		return ret;
	}
	if (!data || !size) {
		free(data);
		return AVERROR_INVALIDDATA;
	}

	MemorySource *src = calloc(1, sizeof(*src));
	unsigned char *buffer = src ? av_malloc(VITA_HW_DECODER_AVIO_BUFFER_SIZE)
	                            : NULL;
	if (!buffer) {
		free(src);
		free(data);
		return AVERROR(ENOMEM);
	}
	src->data = data;
	src->size = size;
	AVIOContext *avio = avio_alloc_context(buffer, VITA_HW_DECODER_AVIO_BUFFER_SIZE,
	                                       0, src, memory_read, NULL,
	                                       memory_seek);
	if (!avio) {
		av_free(buffer);
		memory_source_free(src);
		return AVERROR(ENOMEM);
	}
	avio->seekable = AVIO_SEEKABLE_NORMAL;
	*pb = avio;
	return 0;
}

static int hls_io_close(AVFormatContext *format, AVIOContext *pb) {
	(void)format;
	if (!pb) return 0;
	MemorySource *src = (MemorySource *)pb->opaque;
	av_freep(&pb->buffer);
	avio_context_free(&pb);
	memory_source_free(src);
	return 0;
}

static void input_close(VitaHwDecoderInput *input) {
	if (!input) return;
	if (input->format) avformat_close_input(&input->format);
	if (input->avio) {
		av_freep(&input->avio->buffer);
		avio_context_free(&input->avio);
	}
	if (input->stream.close && input->stream.opaque)
		input->stream.close(input->stream.opaque);
	memset(input, 0, sizeof(*input));
}

static int input_open(VitaHwDecoderInput *input,
	                  const VitaHwDecoderStreamFactory *factory,
	                  volatile int *cancel, int is_live) {
	if (!input || !factory || !factory->open) return AVERROR(EINVAL);
	memset(input, 0, sizeof(*input));
	input->cancel = cancel;
	int ret = factory->open(factory->opaque, &input->stream);
	if (ret < 0 || !input->stream.read || !input->stream.seek) {
		input_close(input);
		return ret < 0 ? ret : AVERROR(EINVAL);
	}
	/* fetch and url belong together: a playlist source without its own URL
	 * would reach avformat_open_input as a bare pointer and be strlen'd
	 * there. Refusing it turns a factory that forgot one of the two into a
	 * failed open instead of a data abort. */
	if ((input->stream.fetch != NULL) != (input->stream.url != NULL)) {
		input_close(input);
		return AVERROR(EINVAL);
	}
	unsigned char *buffer = av_malloc(VITA_HW_DECODER_AVIO_BUFFER_SIZE);
	if (!buffer) {
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->avio = avio_alloc_context(buffer, VITA_HW_DECODER_AVIO_BUFFER_SIZE, 0,
	                                 input, stream_avio_read, NULL,
	                                 stream_avio_seek);
	if (!input->avio) {
		av_free(buffer);
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->avio->seekable = AVIO_SEEKABLE_NORMAL;
	input->format = avformat_alloc_context();
	if (!input->format) {
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->format->pb = input->avio;
	input->format->flags |= AVFMT_FLAG_CUSTOM_IO;
	input->format->interrupt_callback.callback = interrupt_cb;
	input->format->interrupt_callback.opaque = (void *)input;
	AVDictionary *options = NULL;
	if (input->stream.fetch) {
		/* opaque is the user's field and untouched by libavformat; the hls
		 * demuxer hands the parent context back to io_open, including for
		 * its nested demuxers, so this is how the callback finds us. */
		input->format->opaque = input;
		input->format->io_open = hls_io_open;
		input->format->io_close2 = hls_io_close;
		/* hls.c refuses a "file" URI whose extension is not a known media
		 * one, and YouTube's segment URLs have no extension at all. */
		av_dict_set(&options, "allowed_extensions", "ALL", 0);
		/* Join a live stream further back than the default three segments.
		 * Three is a latency choice made for players with bandwidth to
		 * spare: it leaves only ~6 s of published stream ahead of the play
		 * position, and nothing can be read ahead into what has not been
		 * published yet. On this console the fetch is the bottleneck, so
		 * that ceiling became the buffer ceiling — the frame ring sat empty
		 * (measured on hardware 2026-09-01: ready=0/16, media/wall 0.4x,
		 * while decode itself needed 7 ms a frame). Five segments back is
		 * ~10-20 s of runway for the read-ahead to work with, at the price
		 * of being that much behind the live edge, which nobody watching a
		 * stream on a Vita will mind. Ignored for a finished playlist. */
		av_dict_set_int(&options, "live_start_index", is_live ? -5 : 0, 0);
		/* Backstop for the read watchdog (interrupt_cb): both default to
		 * 1000 reload rounds, i.e. tens of minutes of frozen picture before
		 * a dead live source finally reports EOF. Generous on purpose —
		 * "playlist refreshed, nothing new" is a normal observation here,
		 * since the app refreshes every target_duration while FFmpeg polls
		 * at half that — so these only end a source the watchdog somehow
		 * did not (~1.5-2.5 min). */
		av_dict_set_int(&options, "max_reload", 40, 0);
		av_dict_set_int(&options, "m3u8_hold_counters", 40, 0);
		/* Nothing may reach a real protocol handler; io_open serves every
		 * sub-resource. Stating the whitelist keeps that explicit rather
		 * than relying on the callback being the only path. */
		input->format->protocol_whitelist = av_strdup("file");
	}
	/* This app only ever opens MP4/MOV (our own HD server's faststart mux,
	 * or YouTube's own direct progressive formats) — never TS or anything
	 * requiring real packet analysis to discover codec parameters. For
	 * avc1/mp4a, the moov atom's avcC/esds boxes already carry SPS/PPS and
	 * channel/sample-rate directly, so avformat_find_stream_info() below
	 * needs no additional packets at all once avformat_open_input() has
	 * parsed moov — the generous 1MB/2s budget below was pure headroom for
	 * file types this app never encounters, not fetched-and-used capacity.
	 * Cutting it tight removes that dead time from every open (observed:
	 * ~5s from api-player to first decoded frame, most of it here and in
	 * the sibling cursor's identical probe, not in the actual data
	 * fetches). Kept non-zero as a safety margin, not zero, in case some
	 * upstream re-mux ever needs a peek at an early packet. */
	input->format->probesize = 256 * 1024;
	input->format->max_analyze_duration = AV_TIME_BASE / 4;
	if (input->stream.fetch) {
		/* The reasoning above holds for MP4 only. An HLS playlist points at
		 * MPEG-TS segments, where codec parameters live in-band (SPS/PPS in
		 * the first keyframe, ADTS headers for audio) and really do have to
		 * be discovered by analysing packets. One segment is a few hundred
		 * KB and already carries everything, so this stays bounded. */
		input->format->probesize = 1024 * 1024;
		input->format->max_analyze_duration = AV_TIME_BASE;
	}
	/* Naming the demuxer instead of letting avformat probe: an HLS playlist
	 * handed over as an anonymous memory buffer has neither the .m3u8 file
	 * extension nor a MIME type, and hls_probe rejects exactly that case in
	 * newer FFmpeg ("Not detecting m3u8/hls with non standard extension and
	 * non standard mime type"). The pinned build here still probes it
	 * happily, but there is no reason to depend on that. */
	const AVInputFormat *forced = input->stream.fetch
	                            ? av_find_input_format("hls") : NULL;
	/* The URL is only ever a base for resolving playlist entries — nothing
	 * opens it, since pb is already set (AVFMT_FLAG_CUSTOM_IO). NULL for a
	 * plain media file, exactly as before. */
	ret = avformat_open_input(&input->format, input->stream.url, forced,
	                          &options);
	av_dict_free(&options);
	if (ret < 0) {
		input_close(input);
		return ret;
	}
	ret = avformat_find_stream_info(input->format, NULL);
	if (ret < 0) {
		input_close(input);
		return ret;
	}
	return 0;
}

/* Opening the two cursors CONCURRENTLY (one pthread each) was tried
 * (2026-08-27) to shave the redundant handshake+moov-fetch+probe cost off
 * a fresh open, and reverted the same day: it introduced an intermittent
 * freeze on real hardware (no crash dump — a genuine hang, not a fault),
 * with the log always cutting off partway through the two cursors' first
 * few reads. The suspected cause is libcurl's synchronous DNS resolver:
 * without a threaded resolver or c-ares, concurrent getaddrinfo() calls
 * from two threads are not guaranteed safe, and the Vita's homebrew
 * network stack is exactly the kind of platform where that guarantee is
 * unlikely to hold. Opening sequentially avoids the risk; the
 * probesize/analyze-duration reduction above already removes the bigger
 * share of the cold-open latency without touching concurrency at all. */
static int find_stream(const AVFormatContext *format, enum AVMediaType type,
	                   enum AVCodecID codec) {
	for (unsigned int i = 0; format && i < format->nb_streams; i++) {
		const AVCodecParameters *params = format->streams[i]->codecpar;
		if (params->codec_type == type && params->codec_id == codec)
			return (int)i;
	}
	return -1;
}

/* Marks every stream but `keep` as one the demuxer may drop outright. */
static void discard_foreign_streams(AVFormatContext *format, int keep) {
	if (!format) return;
	for (unsigned int i = 0; i < format->nb_streams; i++)
		format->streams[i]->discard =
		    (int)i == keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

static uint64_t input_duration_ms(const AVFormatContext *format) {
	if (!format || format->duration <= 0) return 0;
	return (uint64_t)format->duration * 1000ULL / AV_TIME_BASE;
}

static void close_session(VitaHwDecoderPlayer *player) {
	if (!player) return;
	/* The cancel flag doubles as the stop signal for the session's threads,
	 * but it is the CALLER's flag and the caller reads it afterwards as "the
	 * user cancelled". Raise it for the joins only and put it back unless
	 * the caller had raised it itself — otherwise every failed open returns
	 * with the flag up and the app mistakes the failure for a Circle press
	 * (no fallback, no toast). */
	int caller_cancelled = player->cancel ? *player->cancel : 0;
	if (player->cancel) *player->cancel = 1;
	__sync_synchronize();
	if (player->video.started) vita_hw_software_video_join(&player->video);
	if (player->audio.started || player->audio.decoder_ready || player->audio.port_open)
		vita_hw_elementary_audio_join(&player->audio);
	input_close(&player->audio_input);
	input_close(&player->video_input);
	if (player->cancel && !caller_cancelled) *player->cancel = 0;
	/* Nothing of the old session may show through get_status: a dead
	 * session that still reported the previous duration or eof looked like
	 * a live one. Preserve duration_ms and stream_base_ms across seeks. */
	memset(&player->video, 0, sizeof(player->video));
	memset(&player->audio, 0, sizeof(player->audio));
	player->position_floor_ms = 0;
	player->opened = 0;
	player->start_gate = 0;
	player->auto_paused = 0;
}

static void stop_decoders(VitaHwDecoderPlayer *player) {
	if (!player) return;
	int caller_cancelled = player->cancel ? *player->cancel : 0;
	if (player->cancel) *player->cancel = 1;
	__sync_synchronize();
	if (player->video.started) vita_hw_software_video_join(&player->video);
	if (player->audio.started || player->audio.decoder_ready || player->audio.port_open)
		vita_hw_elementary_audio_join(&player->audio);
	if (player->cancel && !caller_cancelled) *player->cancel = 0;
	memset(&player->video, 0, sizeof(player->video));
	memset(&player->audio, 0, sizeof(player->audio));
	player->position_floor_ms = 0;
	player->clock_us = 0;
	player->clock_wall_us = 0;
	player->clock_started = 0;
	player->opened = 0;
	player->start_gate = 0;
	player->auto_paused = 0;
}

static int start_decoders_at(VitaHwDecoderPlayer *player, int video_index,
                             int audio_index, uint64_t start_position_ms,
                             int force_seek) {
	int ret = 0;
	player->cancel = player->config.cancel_flag
	               ? player->config.cancel_flag : &player->local_cancel;
	*player->cancel = 0;
	player->start_gate = 0;
	player->clock_us = 0;
	player->clock_wall_us = 0;
	player->clock_started = 0;
	player->session_error = 0;
	player->position_floor_ms = 0;

	uint64_t offset_ms = player->pts_offset_detected ? player->pts_offset_ms : 0;
	uint64_t video_container_ms = start_position_ms + offset_ms;
	uint64_t audio_playlist_ms = start_position_ms;
	if (force_seek || start_position_ms > 0) {
		if (player->video_input.format->pb)
			player->video_input.format->pb->eof_reached = 0;
		AVStream *video_stream = player->video_input.format->streams[video_index];
		int64_t video_target = av_rescale_q((int64_t)video_container_ms,
		                                    (AVRational){ 1, 1000 },
		                                    video_stream->time_base);
		int video_seek = av_seek_frame(player->video_input.format, video_index,
		                               video_target, AVSEEK_FLAG_BACKWARD);
		if (video_seek < 0) {
			log_printf("seek to %llu ms: video -> %d",
			           (unsigned long long)video_container_ms, video_seek);
			if (force_seek) return video_seek;
		}
	}
	player->position_floor_ms = audio_playlist_ms > UINT32_MAX
	                          ? UINT32_MAX : (uint32_t)audio_playlist_ms;

	ret = vita_hw_software_video_start(&player->video, player->video_input.format,
	                           video_index, player->config.expected_width,
	                           player->config.expected_height,
	                           player->config.expected_fps,
	                           start_position_ms > 0 ? video_container_ms : 0,
	                           player->config.approximate_seek,
	                           player->cancel, &player->start_gate);
	if (ret < 0) return ret;

	/* Video is allowed to decode while audio waits on the start gate. A small
	 * presentation cushion prevents the audio clock from outrunning the first
	 * remote frames without adding a fixed startup delay. */
	int target_ready = player->video.source_fps >= 50 ? 8 : 4;
	if (force_seek) {
		target_ready = 1;
	} else if (player->config.approximate_seek && start_position_ms > 0) {
		target_ready = 2;
	}
	uint64_t deadline = sceKernelGetProcessTimeWide() + 3000000ULL;
	while (!*player->cancel && !player->video.had_error && !player->video.eof &&
	       sceKernelGetProcessTimeWide() < deadline) {
		int ready = 0;
		vita_hw_software_video_buffer_status(&player->video, &ready, NULL, NULL);
		if (ready >= target_ready) break;
		sceKernelDelayThread(1000);
	}
	if (*player->cancel) return AVERROR_EXIT;
	if (player->video.had_error) return AVERROR_EXTERNAL;

	/* Align audio to the exact keyframe found by the video decoder */
	if (start_position_ms > 0 && player->video.initial_keyframe_found && player->video.resume_keyframe_ms >= 0) {
		uint64_t kf_ms = (uint64_t)player->video.resume_keyframe_ms;
		uint64_t kf_playlist_ms = (kf_ms >= offset_ms) ? (kf_ms - offset_ms) : kf_ms;
		if (player->duration_ms > 0 && kf_playlist_ms > player->duration_ms) {
			kf_playlist_ms = player->duration_ms;
		}
		if (kf_playlist_ms <= start_position_ms + 5000ULL) {
			audio_playlist_ms = kf_playlist_ms;
		}
	}
	if (player->duration_ms > 0 && audio_playlist_ms > player->duration_ms) {
		audio_playlist_ms = player->duration_ms;
	}
	player->position_floor_ms = audio_playlist_ms > UINT32_MAX
	                          ? UINT32_MAX : (uint32_t)audio_playlist_ms;

	uint64_t audio_container_ms = audio_playlist_ms + offset_ms;
	if (force_seek || start_position_ms > 0) {
		if (player->audio_input.format->pb)
			player->audio_input.format->pb->eof_reached = 0;
		AVStream *audio_stream = player->audio_input.format->streams[audio_index];
		int64_t audio_target = av_rescale_q((int64_t)audio_container_ms,
		                                    (AVRational){ 1, 1000 },
		                                    audio_stream->time_base);
		int audio_seek = av_seek_frame(player->audio_input.format, audio_index,
		                               audio_target, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
		if (audio_seek < 0) {
			log_printf("seek audio to %llu ms -> %d",
			           (unsigned long long)audio_container_ms, audio_seek);
			if (force_seek) return audio_seek;
		}
	}

	ret = vita_hw_elementary_audio_start(&player->audio, player->audio_input.format,
	                            audio_index,
	                            start_position_ms > 0 ? audio_container_ms : 0,
	                            player->cancel, &player->start_gate);
	if (ret < 0) return ret;
	vita_hw_elementary_audio_set_volume(&player->audio, player->config.volume_percent);
	player->audio.paused = player->paused;
	if (player->video.had_error || player->audio.had_error) {
		return AVERROR_EXTERNAL;
	}
	if (player->video.eof && start_position_ms == 0 &&
	    player->video.frames_decoded == 0) {
		return AVERROR_INVALIDDATA;
	}
	__sync_synchronize();
	player->start_gate = 1;
	player->opened = 1;
	return 0;
}

static int open_session(VitaHwDecoderPlayer *player, uint64_t start_position_ms) {
	player->cancel = player->config.cancel_flag
	               ? player->config.cancel_flag : &player->local_cancel;
	*player->cancel = 0;
	player->start_gate = 0;
	player->clock_us = 0;
	player->clock_wall_us = 0;
	player->clock_started = 0;
	player->session_error = 0;
	player->position_floor_ms = 0;
	memset(&player->video, 0, sizeof(player->video));
	memset(&player->audio, 0, sizeof(player->audio));

	int ret = input_open(&player->video_input, &player->config.stream,
	                     player->cancel, player->config.is_live);
	if (ret < 0) goto fail;
	const VitaHwDecoderStreamFactory *audio_source =
	    player->config.stream_audio.open ? &player->config.stream_audio
	                                     : &player->config.stream;
	ret = input_open(&player->audio_input, audio_source, player->cancel,
	                 player->config.is_live);
	if (ret < 0) goto fail;
	/* Read watchdog (interrupt_cb): each cursor watches its own thread's
	 * in-flight read. input_open() reset the inputs, so this comes after. */
	player->video_input.read_started_us = &player->video.read_started_us;
	player->audio_input.read_started_us = &player->audio.read_started_us;

	int video_index = find_stream(player->video_input.format,
	                              AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264);
	int audio_index = find_stream(player->audio_input.format,
	                              AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC);
	if (video_index < 0 || audio_index < 0) {
		ret = AVERROR_DECODER_NOT_FOUND;
		goto fail;
	}
	discard_foreign_streams(player->video_input.format, video_index);
	discard_foreign_streams(player->audio_input.format, audio_index);

	if (!player->duration_ms) {
		player->duration_ms = input_duration_ms(player->video_input.format);
		if (!player->duration_ms)
			player->duration_ms = input_duration_ms(player->audio_input.format);
	}

	/* Detect non-zero container start_time (e.g. Twitch VOD MPEG-TS starting at 00:01:02) */
	if (!player->config.is_live && !player->pts_offset_detected) {
		int64_t st_us = player->video_input.format->start_time;
		if (st_us == AV_NOPTS_VALUE) st_us = player->audio_input.format->start_time;
		if (st_us != AV_NOPTS_VALUE && st_us > 0) {
			uint64_t ms = (uint64_t)st_us * 1000ULL / AV_TIME_BASE;
			if (ms < 90000000ULL) {
				player->pts_offset_ms = ms;
				player->pts_offset_detected = 1;
			}
		}
	}

	ret = start_decoders_at(player, video_index, audio_index,
	                        start_position_ms, 0);
	if (ret < 0) goto fail;

	if (!player->config.is_live && start_position_ms == 0 && !player->pts_offset_detected) {
		uint64_t first_ms = player->video.last_decoded_pts_us / 1000ULL;
		if (first_ms > 500ULL && first_ms < 90000000ULL) {
			player->pts_offset_ms = first_ms;
			player->pts_offset_detected = 1;
		}
	}
	return 0;

fail:
	/* A cancelled open is not an error of the session. */
	if (ret != AVERROR_EXIT) player->session_error = 1;
	close_session(player);
	return ret;
}

VitaHwDecoderPlayer *vita_hw_decoder_create(void) {
	return calloc(1, sizeof(VitaHwDecoderPlayer));
}

int vita_hw_decoder_open(VitaHwDecoderPlayer *player,
	                    const VitaHwDecoderPlayerConfig *config) {
	if (!player || !config || !config->stream.open) return AVERROR(EINVAL);
	close_session(player);
	player->duration_ms = 0;
	player->pts_offset_ms = 0;
	player->pts_offset_detected = 0;
	player->auto_paused = 0;
	player->config = *config;
	if (player->config.volume_percent < 0) player->config.volume_percent = 0;
	if (player->config.volume_percent > 300) player->config.volume_percent = 300;
	/* Callers that keep a pause across a reopen (quality switch, error
	 * recovery) used to open-then-pause, and the audio thread is live the
	 * moment open() returns — an audible burst from the new position on a
	 * player the viewer had paused. open_session() hands player->paused to
	 * the audio state before the start gate opens. */
	player->paused = config->start_paused != 0;
	return open_session(player, config->start_position_ms);
}

void vita_hw_decoder_close(VitaHwDecoderPlayer *player) {
	close_session(player);
}

void vita_hw_decoder_destroy(VitaHwDecoderPlayer *player) {
	if (!player) return;
	close_session(player);
	free(player);
}

void vita_hw_decoder_set_paused(VitaHwDecoderPlayer *player, int paused) {
	if (!player) return;
	player->paused = paused != 0;
	if (!player->auto_paused) {
		player->audio.paused = player->paused;
	}
	player->clock_wall_us = sceKernelGetProcessTimeWide();
}

void vita_hw_decoder_set_volume(VitaHwDecoderPlayer *player, int percent) {
	if (!player) return;
	if (percent < 0) percent = 0;
	if (percent > 300) percent = 300;
	player->config.volume_percent = percent;
	vita_hw_elementary_audio_set_volume(&player->audio, percent);
}

void vita_hw_decoder_request_stop(VitaHwDecoderPlayer *player) {
	if (player && player->cancel) *player->cancel = 1;
}

int vita_hw_decoder_seek(VitaHwDecoderPlayer *player, uint64_t position_ms) {
	if (!player || !player->opened) return AVERROR(EINVAL);
	if (player->duration_ms && position_ms > player->duration_ms)
		position_ms = player->duration_ms;
	int paused = player->paused;
	player->config.approximate_seek = 1;

	/* Fast warm in-place seek: keep video_input & audio_input open (reusing
	 * TLS connections, MP4 moov atoms, and parsed HLS playlists). */
	if (player->video_input.format && player->audio_input.format) {
		int video_index = find_stream(player->video_input.format,
		                              AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264);
		int audio_index = find_stream(player->audio_input.format,
		                              AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC);
		if (video_index >= 0 && audio_index >= 0) {
			stop_decoders(player);
			player->paused = paused;
			int ret = start_decoders_at(player, video_index, audio_index,
			                            position_ms, 1);
			if (ret >= 0 || ret == AVERROR_EXIT) {
				return ret;
			}
			log_printf("warm seek to %llu ms failed (%d), falling back to full reopen",
			           (unsigned long long)position_ms, ret);
		}
	}

	/* Fallback: full cold reopen if warm seek failed */
	close_session(player);
	player->paused = paused;
	return open_session(player, position_ms);
}

static void update_auto_pause(VitaHwDecoderPlayer *player) {
	if (!player || !player->opened) return;
	int ready_frames = 0;
	vita_hw_software_video_buffer_status(&player->video, &ready_frames, NULL, NULL);

	/* Buffer starvation protection:
	 * If video runs out of frames while playback is active, auto-pause audio so
	 * audio doesn't run ahead for seconds while video is stalled.
	 * As soon as video decodes ready frames, resume audio. */
	if (!player->paused && player->video.started && !player->video.eof) {
		if (ready_frames == 0) {
			if (!player->auto_paused) {
				player->auto_paused = 1;
				player->audio.paused = 1;
			}
		} else if (player->auto_paused && ready_frames > 0) {
			player->auto_paused = 0;
			player->audio.paused = player->paused;
			player->clock_wall_us = sceKernelGetProcessTimeWide();
		}
	} else if (player->auto_paused) {
		player->auto_paused = 0;
		player->audio.paused = player->paused;
	}
}

static uint64_t presentation_clock(VitaHwDecoderPlayer *player) {
	uint64_t now = sceKernelGetProcessTimeWide();
	uint64_t raw = vita_hw_elementary_audio_clock_us(&player->audio);
	/* Once the audio thread has left (EOF, or died), its clock is frozen
	 * for good. Clamping to it would freeze the picture with it and park
	 * the decode thread in reserve_slot on a full ring: video.eof could
	 * then never be reached and status.eof never become 1. Let the clock
	 * free-run on wall time instead so the video tail drains; a session
	 * whose audio ended before its first grain is seeded from the resume
	 * position, where the first decoded frames sit. */
	int audio_done = player->audio.eof || player->audio.had_error;
	if (!raw) {
		if (!audio_done) return 0;
		uint64_t offset_ms = player->pts_offset_detected ? player->pts_offset_ms : 0;
		raw = ((uint64_t)player->position_floor_ms + offset_ms) * 1000ULL;
	}
	if (!player->clock_started) {
		player->clock_started = 1;
		player->clock_us = raw;
		player->clock_wall_us = now;
		return raw;
	}
	uint64_t delta = now - player->clock_wall_us;
	player->clock_wall_us = now;
	if (!player->paused && !player->auto_paused) player->clock_us += delta;
	uint64_t minimum = raw > VITA_HW_DECODER_CLOCK_WINDOW_US
	                 ? raw - VITA_HW_DECODER_CLOCK_WINDOW_US : 0;
	uint64_t maximum = raw + VITA_HW_DECODER_CLOCK_WINDOW_US;
	if (player->clock_us < minimum) player->clock_us = minimum;
	if (!audio_done && player->clock_us > maximum) player->clock_us = maximum;
	return player->clock_us;
}

int vita_hw_decoder_present(VitaHwDecoderPlayer *player, int fill_screen) {
	if (!player || !player->opened) return 0;
	update_auto_pause(player);
	uint64_t raw = vita_hw_elementary_audio_clock_us(&player->audio);
	return vita_hw_software_video_present(&player->video, presentation_clock(player), raw,
	                              fill_screen, !player->paused);
}

int vita_hw_decoder_present_rect(VitaHwDecoderPlayer *player,
	                            float x, float y, float width, float height,
	                            int fill_rect) {
	if (!player || !player->opened) return 0;
	update_auto_pause(player);
	uint64_t raw = vita_hw_elementary_audio_clock_us(&player->audio);
	return vita_hw_software_video_present_rect(&player->video,
	                                   presentation_clock(player), raw,
	                                   x, y, width, height, fill_rect,
	                                   !player->paused);
}

void vita_hw_decoder_render_complete(VitaHwDecoderPlayer *player) {
	if (player && player->opened) vita_hw_software_video_render_complete(&player->video);
}

void vita_hw_decoder_get_status(VitaHwDecoderPlayer *player,
	                           VitaHwDecoderPlayerStatus *status) {
	if (!status) return;
	memset(status, 0, sizeof(*status));
	if (!player) return;
	status->opened = player->opened;
	status->paused = player->paused;
	status->eof = player->video.eof && player->audio.eof;
	status->error = player->video.had_error || player->audio.had_error ||
	                player->session_error;
	status->width = player->video.content_width;
	status->height = player->video.content_height;
	status->duration_ms = player->duration_ms;
	uint64_t raw_ms = vita_hw_elementary_audio_clock_us(&player->audio) / 1000ULL;
	if (player->pts_offset_detected && player->pts_offset_ms > 0) {
		status->position_ms = raw_ms >= player->pts_offset_ms
		                    ? (raw_ms - player->pts_offset_ms) : 0;
	} else {
		status->position_ms = raw_ms;
	}
	if (player->duration_ms > 0 && status->position_ms > player->duration_ms)
		status->position_ms = player->duration_ms;
	if (status->position_ms < player->position_floor_ms &&
	    (player->duration_ms == 0 || player->position_floor_ms <= player->duration_ms))
		status->position_ms = player->position_floor_ms;
	SoftwareVideoDebugSnapshot debug;
	vita_hw_software_video_debug_snapshot(&player->video, &debug);
	status->hardware_accelerated = debug.hardware_accelerated;
	status->direct_rendering = debug.direct_rendering;
	status->ready_frames = debug.ready_frames;
	status->frame_capacity = debug.queue_capacity;
	status->fps = debug.source_fps;
	status->frames_decoded = debug.frames_decoded;
	status->frames_shown = debug.frames_shown;
	status->frames_dropped = debug.frames_dropped;
	status->audio_underruns = player->audio.underruns;
	status->audio_worst_prep_ms = player->audio.worst_prep_ms;
	status->audio_pts_jumps = player->audio.pts_jumps;
}
