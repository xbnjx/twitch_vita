/* Kick Vita — Fullscreen video player implementation */
#include "ui/player_screen.h"
#include "app.h"
#include "log.h"
#include "store.h"
#include "media/vita_decoder.h"
#include "ui/player_quality_switch.h"
#include "ui/touch.h"
#include "ui/ui.h"
#include "ui/strings.h"
#include "twitch/twitch_api.h"
#include "twitch/twitch_auth.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/kernel/processmgr.h>

#define HUD_TIMEOUT_US (4000ULL * 1000ULL)
#define MAX_VOD_DURATION_MS 172799000ULL /* 47h 59m 59s */

typedef struct {
    const KickStream *stream;
    int quality;
    int volume;
    int start_paused;
    uint64_t start_position_ms;
    volatile int *cancel;
    PlayerSource *source;
    VtDecoderPlayer **player;
    int result;
} OpenTask;

typedef struct {
    VtDecoderPlayer *player;
    uint64_t target_ms;
    int result;
} SeekTask;

static int open_task_fn(void *arg) {
    OpenTask *t = arg;
    t->result = open_source_blocking(t->stream, t->quality, t->volume,
                                     t->start_paused, t->start_position_ms,
                                     t->cancel, t->source, t->player);
    return t->result;
}

static int seek_task_fn(void *arg) {
    SeekTask *t = arg;
    t->result = vt_decoder_seek(t->player, t->target_ms);
    return t->result;
}

static void format_time_ms(uint64_t ms, char *out, size_t cap) {
    unsigned total_s = (unsigned)(ms / 1000);
    unsigned h = total_s / 3600;
    unsigned m = (total_s % 3600) / 60;
    unsigned s = total_s % 60;
    if (h > 0) {
        snprintf(out, cap, "%02u:%02u:%02u", h, m, s);
    } else {
        snprintf(out, cap, "%02u:%02u", m, s);
    }
}

void player_screen_run(const KickStream *stream) {
    if (!stream) return;

    /* Enforce 47h:59m:59s VOD limit */
    if (!stream->is_live && !stream->is_clip && stream->duration_ms > MAX_VOD_DURATION_MS) {
        vt_ui_toast(kick_tr("No se puede reproducir este VOD porque supera el limite del reproductor",
                            "Cannot play this VOD because it exceeds the player limit",
                            "Nao e possivel reproduzir este VOD pois excede o limite do player"),
                    stream->username, 120);
        return;
    }

    /* Add to history */
    kick_store_history_add(stream);

    volatile int cancel = 0;
    PlayerSource sources[2] = {0};
    int current_source_idx = 0;
    VtDecoderPlayer *player = NULL;

    int current_quality = kick_store_get_quality();
    if (current_quality <= 0 || (current_quality != 720 && current_quality != 360 && current_quality != 240 && current_quality != 144)) {
        current_quality = 360;
    }

    int volume = 100;

    OpenTask task = {
        .stream = stream,
        .quality = current_quality,
        .volume = volume,
        .start_paused = 0,
        .start_position_ms = 0,
        .cancel = &cancel,
        .source = &sources[current_source_idx],
        .player = &player,
        .result = -1
    };

    vt_ui_run_task(kick_tr("Cargando...", "Loading...", "Carregando..."), open_task_fn, &task, &cancel);
    if (cancel || !player || task.result < 0) {
        if (!cancel) {
            vt_ui_toast("No se pudo reproducir el contenido", stream->username, 90);
        }
        player_source_term(&sources[current_source_idx]);
        return;
    }

    /* Re-verify VOD duration from playlist/container in case API duration was 0 */
    if (!stream->is_live && !stream->is_clip) {
        VtDecoderPlayerStatus init_st = {0};
        vt_decoder_get_status(player, &init_st);
        uint64_t check_dur = stream->duration_ms > 0 ? stream->duration_ms : init_st.duration_ms;
        if (check_dur > MAX_VOD_DURATION_MS) {
            vt_decoder_destroy(player);
            player = NULL;
            player_source_term(&sources[current_source_idx]);
            vt_ui_toast(kick_tr("No se puede reproducir este VOD porque supera el limite del reproductor",
                                "Cannot play this VOD because it exceeds the player limit",
                                "Nao e possivel reproduzir este VOD pois excede o limite do player"),
                        stream->username, 120);
            return;
        }
    }

    int hud_visible = 1;
    uint64_t last_hud_active_us = sceKernelGetProcessTimeWide();
    SceCtrlData old_pad = {0};
    int paused = 0;
    const char *follow_slug = stream->channel_slug[0] ? stream->channel_slug : stream->username;
    int is_following = kick_auth_is_logged_in() ? kick_api_is_channel_followed(follow_slug) : 0;

    /* Check if viewing own channel/stream */
    const KickAuthUser *me = kick_auth_get_user();
    int is_me = 0;
    if (me && me->username[0]) {
        if (strcasecmp(me->username, stream->channel_slug) == 0 ||
            strcasecmp(me->username, stream->username) == 0) {
            is_me = 1;
        }
    }

    /* Scrubber state for 1:1 finger tracking (commits seek only on true finger release) */
    int is_scrubbing = 0;
    int scrub_releasing = 0;
    uint64_t scrub_release_us = 0;
    uint64_t scrub_preview_ms = 0;

    int quality_menu_open = 0;
    static const int q_values[] = { 720, 360, 240, 144 };
    #define QUALITY_OPTION_COUNT 4
    int quality_sel = 1;
    for (int i = 0; i < QUALITY_OPTION_COUNT; i++) {
        if (q_values[i] == current_quality) { quality_sel = i; break; }
    }

    while (1) {
        /* Prevent sleep during playback */
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        UiTouchEvent ev;
        unsigned int ev_flags = ui_touch_poll(&ev);

        unsigned int pressed = pad.buttons & ~old_pad.buttons;
        old_pad = pad;

        uint64_t now_us = sceKernelGetProcessTimeWide();

        VtDecoderPlayerStatus status = {0};
        if (player) {
            vt_decoder_get_status(player, &status);
        }
        uint64_t dur_ms = stream->duration_ms > 0 ? stream->duration_ms : status.duration_ms;

        /* 1. Touch Events Handling */
        if (quality_menu_open) {
            if (ev_flags & UI_TOUCH_EVENT_TAP) {
                last_hud_active_us = now_us;
                int mw = 440, mh = 248;
                int mx = (VT_SCREEN_W - mw) / 2;
                int my = (VT_SCREEN_H - mh) / 2;
                if (ev.x >= mx && ev.x <= mx + mw && ev.y >= my + 55 && ev.y <= my + 55 + QUALITY_OPTION_COUNT * 44) {
                    int clicked_sel = (ev.y - (my + 55)) / 44;
                    if (clicked_sel >= 0 && clicked_sel < QUALITY_OPTION_COUNT) {
                        quality_sel = clicked_sel;
                        pressed |= SCE_CTRL_CROSS;
                    }
                } else if (ev.x < mx || ev.x > mx + mw || ev.y < my || ev.y > my + mh) {
                    quality_menu_open = 0;
                }
            }
        } else {
            if (ev_flags & UI_TOUCH_EVENT_DOWN) {
                /* Resume an interrupted drag if finger had a brief capacitive dropout */
                if (is_scrubbing && scrub_releasing && dur_ms > 0) {
                    scrub_releasing = 0;
                    int rel_x = ev.x - 250;
                    if (rel_x < 0) rel_x = 0;
                    if (rel_x > 685) rel_x = 685;
                    scrub_preview_ms = (uint64_t)rel_x * dur_ms / 685;
                    hud_visible = 1;
                    last_hud_active_us = now_us;
                }
                /* Seekbar drag start (for VOD / Clips, only when HUD is visible) */
                else if (hud_visible && !stream->is_live && dur_ms > 0 &&
                    ev.y >= VT_SCREEN_H - 56 && ev.y <= VT_SCREEN_H &&
                    ev.x >= 235 && ev.x <= 950) {
                    is_scrubbing = 1;
                    scrub_releasing = 0;
                    int rel_x = ev.x - 250;
                    if (rel_x < 0) rel_x = 0;
                    if (rel_x > 685) rel_x = 685;
                    scrub_preview_ms = (uint64_t)rel_x * dur_ms / 685;
                    hud_visible = 1;
                    last_hud_active_us = now_us;
                }
            }
            else if (ev_flags & (UI_TOUCH_EVENT_MOVE | UI_TOUCH_EVENT_HOLD)) {
                if (is_scrubbing && dur_ms > 0) {
                    scrub_releasing = 0;
                    int rel_x = ev.x - 250;
                    if (rel_x < 0) rel_x = 0;
                    if (rel_x > 685) rel_x = 685;
                    scrub_preview_ms = (uint64_t)rel_x * dur_ms / 685;
                    hud_visible = 1;
                    last_hud_active_us = now_us;
                }
            }
            else if (ev_flags & UI_TOUCH_EVENT_UP) {
                if (is_scrubbing && dur_ms > 0) {
                    /* Debounce release (~85ms) so capacitive micro-dropouts while
                     * sliding never trigger a seek until the finger is truly lifted. */
                    scrub_releasing = 1;
                    scrub_release_us = now_us;
                    hud_visible = 1;
                    last_hud_active_us = now_us;
                }
            }

            /* Commit seek only after the finger has genuinely been lifted */
            if (is_scrubbing && scrub_releasing && dur_ms > 0 &&
                (now_us - scrub_release_us >= 85000ULL)) {
                uint64_t seek_target = scrub_preview_ms;
                if (!stream->is_clip && sources[current_source_idx].is_hls) {
                    seek_target = vt_hls_segment_start_ms(&sources[current_source_idx].hls_video, seek_target);
                }
                if (dur_ms > 0 && seek_target > dur_ms) seek_target = dur_ms;
                is_scrubbing = 0;
                scrub_releasing = 0;
                SeekTask stask = { .player = player, .target_ms = seek_target, .result = -1 };
                vt_ui_run_task(kick_tr("Cargando...", "Loading...", "Carregando..."), seek_task_fn, &stask, &cancel);
                ui_touch_reset();
                if (cancel) break;
                last_hud_active_us = sceKernelGetProcessTimeWide();
            }

            if ((ev_flags & UI_TOUCH_EVENT_TAP) && !is_scrubbing) {
                last_hud_active_us = now_us;

                if (hud_visible) {
                    /* Exit button (top right: x 840..960, y 0..55) */
                    if (ev.x >= 840 && ev.x <= 960 && ev.y >= 0 && ev.y <= 55) {
                        break;
                    }
                    /* Follow / Unfollow button (top: x 505..665, y 12..52) - only if not own channel */
                    else if (!is_me && ev.x >= 505 && ev.x <= 665 && ev.y >= 12 && ev.y <= 52) {
                        if (!kick_auth_is_logged_in()) {
                            vt_ui_toast(kick_tr("Inicia sesión para seguir", "Sign in to follow", "Inicie sessão para seguir"), stream->username, 60);
                        } else if (!is_following) {
                            kick_api_follow_channel_async(follow_slug);
                            is_following = 1;
                            vt_ui_toast(kick_tr("Siguiendo a", "Following", "Seguindo"), stream->username, 45);

                            /* Reconnect stream to refresh session state (only for live streams) */
                            if (stream->is_live) {
                                if (player) {
                                    vt_decoder_destroy(player);
                                    player = NULL;
                                }
                                player_source_term(&sources[current_source_idx]);
                                task.quality = current_quality;
                                task.start_position_ms = 0;
                                task.start_paused = 0;
                                task.result = -1;
                                vt_ui_run_task(kick_tr("Cargando...", "Loading...", "Carregando..."), open_task_fn, &task, &cancel);
                                if (cancel || !player || task.result < 0) {
                                    player_source_term(&sources[current_source_idx]);
                                    return;
                                }
                                last_hud_active_us = sceKernelGetProcessTimeWide();
                            }
                        } else {
                            kick_api_unfollow_channel_async(follow_slug);
                            is_following = 0;
                            vt_ui_toast(kick_tr("Dejaste de seguir a", "Unfollowed", "Deixou de seguir"), stream->username, 45);

                            /* Reconnect stream to refresh session state (only for live streams) */
                            if (stream->is_live) {
                                if (player) {
                                    vt_decoder_destroy(player);
                                    player = NULL;
                                }
                                player_source_term(&sources[current_source_idx]);
                                task.quality = current_quality;
                                task.start_position_ms = 0;
                                task.start_paused = 0;
                                task.result = -1;
                                vt_ui_run_task(kick_tr("Cargando...", "Loading...", "Carregando..."), open_task_fn, &task, &cancel);
                                if (cancel || !player || task.result < 0) {
                                    player_source_term(&sources[current_source_idx]);
                                    return;
                                }
                                last_hud_active_us = sceKernelGetProcessTimeWide();
                            }
                        }
                    }
                    /* Quality button (top: x 670..775, y 12..52) */
                    else if (ev.x >= 670 && ev.x <= 775 && ev.y >= 12 && ev.y <= 52) {
                        quality_menu_open = 1;
                        for (int i = 0; i < QUALITY_OPTION_COUNT; i++) {
                            if (q_values[i] == current_quality) { quality_sel = i; break; }
                        }
                        hud_visible = 1;
                    }
                    /* Live stream touch controls: Play/Pause and Reload buttons */
                    else if (stream->is_live && ev.x >= 365 && ev.x <= 475 && ev.y >= 217 && ev.y <= 327) {
                        paused = !paused;
                        vt_decoder_set_paused(player, paused);
                    }
                    else if (stream->is_live && ev.x >= 485 && ev.x <= 595 && ev.y >= 217 && ev.y <= 327) {
                        if (player) {
                            vt_decoder_destroy(player);
                            player = NULL;
                        }
                        player_source_term(&sources[current_source_idx]);
                        task.quality = current_quality;
                        task.start_position_ms = 0;
                        task.start_paused = 0;
                        task.result = -1;
                        vt_ui_run_task(kick_tr("Recargando señal...", "Reloading stream...", "Recarregando sinal..."), open_task_fn, &task, &cancel);
                        if (cancel || !player || task.result < 0) {
                            if (!cancel) vt_ui_toast(kick_tr("Error al recargar stream", "Error reloading stream", "Erro ao recarregar stream"), stream->username, 60);
                            player_source_term(&sources[current_source_idx]);
                            return;
                        }
                        last_hud_active_us = sceKernelGetProcessTimeWide();
                        vt_ui_toast(kick_tr("Señal recargada", "Stream reloaded", "Sinal recarregado"), stream->username, 40);
                    }
                    /* VOD Center play/pause (x 420..540, y 210..330) */
                    else if (!stream->is_live && ev.x >= 420 && ev.x <= 540 && ev.y >= 210 && ev.y <= 330) {
                        paused = !paused;
                        vt_decoder_set_paused(player, paused);
                    }
                    /* VOD Bottom bar play/pause tap (x: 10..90, y: 480..544) */
                    else if (!stream->is_live && ev.y >= VT_SCREEN_H - 56 && ev.y <= VT_SCREEN_H && ev.x >= 10 && ev.x <= 90) {
                        paused = !paused;
                        vt_decoder_set_paused(player, paused);
                    }
                    /* Seekbar tap: handled by UI_TOUCH_EVENT_UP above, keep HUD active */
                    else if (!stream->is_live && ev.y >= VT_SCREEN_H - 56 && ev.y <= VT_SCREEN_H && ev.x >= 235 && ev.x <= 950 && dur_ms > 0) {
                        hud_visible = 1;
                        last_hud_active_us = now_us;
                    }
                    else {
                        hud_visible = !hud_visible;
                    }
                } else {
                    hud_visible = 1;
                }
            }
        }

        /* 2. Physical Buttons Handling */
        if (quality_menu_open) {
            if (pressed & SCE_CTRL_UP) {
                quality_sel = (quality_sel > 0) ? quality_sel - 1 : QUALITY_OPTION_COUNT - 1;
                last_hud_active_us = now_us;
            } else if (pressed & SCE_CTRL_DOWN) {
                quality_sel = (quality_sel < QUALITY_OPTION_COUNT - 1) ? quality_sel + 1 : 0;
                last_hud_active_us = now_us;
            } else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) {
                quality_menu_open = 0;
                last_hud_active_us = now_us;
            } else if (pressed & SCE_CTRL_CROSS) {
                int target_q = (quality_sel >= 0 && quality_sel < QUALITY_OPTION_COUNT) ? q_values[quality_sel] : 360;
                quality_menu_open = 0;
                if (target_q != current_quality) {
                    current_quality = target_q;
                    kick_store_set_quality(current_quality);

                    uint64_t current_pos_ms = 0;
                    if (player) {
                        VtDecoderPlayerStatus cur_st = {0};
                        vt_decoder_get_status(player, &cur_st);
                        current_pos_ms = cur_st.position_ms;
                        if (dur_ms > 0 && current_pos_ms > dur_ms) current_pos_ms = dur_ms;
                        vt_decoder_destroy(player);
                        player = NULL;
                    }
                    player_source_term(&sources[current_source_idx]);

                    task.quality = current_quality;
                    task.volume = volume;
                    task.start_paused = paused;
                    task.start_position_ms = stream->is_live ? 0 : current_pos_ms;
                    task.result = -1;

                    char qmsg[64];
                    if (stream->is_live) {
                        if (current_quality == 720) snprintf(qmsg, sizeof qmsg, "Reconectando en HD...");
                        else snprintf(qmsg, sizeof qmsg, "Reconectando en %dp...", current_quality);
                    } else {
                        if (current_quality == 720) snprintf(qmsg, sizeof qmsg, kick_tr("Cambiando a HD...", "Switching to HD...", "Mudando para HD..."));
                        else snprintf(qmsg, sizeof qmsg, kick_tr("Cambiando a %dp...", "Switching to %dp...", "Mudando para %dp..."), current_quality);
                    }

                    vt_ui_run_task(qmsg, open_task_fn, &task, &cancel);
                    if (cancel || !player || task.result < 0) {
                        if (!cancel) vt_ui_toast(kick_tr("Error al cambiar resolución", "Error changing resolution", "Erro ao mudar resolução"), stream->username, 60);
                        player_source_term(&sources[current_source_idx]);
                        return;
                    }
                    last_hud_active_us = sceKernelGetProcessTimeWide();
                }
            }
        } else {
            if (pressed & SCE_CTRL_CIRCLE) {
                break;
            }

            if (!is_me && (pressed & SCE_CTRL_SQUARE)) {
                hud_visible = 1;
                last_hud_active_us = now_us;
                if (!kick_auth_is_logged_in()) {
                    vt_ui_toast(kick_tr("Inicia sesión para seguir", "Sign in to follow", "Inicie sessão para seguir"), stream->username, 60);
                } else if (!is_following) {
                    kick_api_follow_channel_async(follow_slug);
                    is_following = 1;
                    vt_ui_toast(kick_tr("Siguiendo a", "Following", "Seguindo"), stream->username, 45);

                    /* Reconnect stream to refresh session state (only for live streams) */
                    if (stream->is_live) {
                        if (player) {
                            vt_decoder_destroy(player);
                            player = NULL;
                        }
                        player_source_term(&sources[current_source_idx]);
                        task.quality = current_quality;
                        task.start_position_ms = 0;
                        task.start_paused = 0;
                        task.result = -1;
                        vt_ui_run_task(kick_tr("Cargando...", "Loading...", "Carregando..."), open_task_fn, &task, &cancel);
                        if (cancel || !player || task.result < 0) {
                            player_source_term(&sources[current_source_idx]);
                            return;
                        }
                        last_hud_active_us = sceKernelGetProcessTimeWide();
                    }
                } else {
                    kick_api_unfollow_channel_async(follow_slug);
                    is_following = 0;
                    vt_ui_toast(kick_tr("Dejaste de seguir a", "Unfollowed", "Deixou de seguir"), stream->username, 45);

                    /* Reconnect stream to refresh session state (only for live streams) */
                    if (stream->is_live) {
                        if (player) {
                            vt_decoder_destroy(player);
                            player = NULL;
                        }
                        player_source_term(&sources[current_source_idx]);
                        task.quality = current_quality;
                        task.start_position_ms = 0;
                        task.start_paused = 0;
                        task.result = -1;
                        vt_ui_run_task(kick_tr("Cargando...", "Loading...", "Carregando..."), open_task_fn, &task, &cancel);
                        if (cancel || !player || task.result < 0) {
                            player_source_term(&sources[current_source_idx]);
                            return;
                        }
                        last_hud_active_us = sceKernelGetProcessTimeWide();
                    }
                }
            }

            if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
                if (!stream->is_live && status.eof) {
                    paused = 0;
                    vt_decoder_set_paused(player, 0);
                    SeekTask stask = { .player = player, .target_ms = 0, .result = -1 };
                    vt_ui_run_task(kick_tr("Cargando...", "Loading...", "Carregando..."), seek_task_fn, &stask, &cancel);
                    ui_touch_reset();
                    if (cancel) break;
                    last_hud_active_us = sceKernelGetProcessTimeWide();
                } else {
                    paused = !paused;
                    vt_decoder_set_paused(player, paused);
                    last_hud_active_us = now_us;
                }
                hud_visible = 1;
            }

            if (!stream->is_live && (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
                uint64_t cur = status.position_ms;
                if (dur_ms > 0 && cur > dur_ms) cur = dur_ms;
                int64_t new_pos = (int64_t)cur;
                int do_seek = 0;
                if (pressed & SCE_CTRL_LEFT) { new_pos -= (stream->is_clip ? 5000 : 10000); do_seek = 1; }
                else if (pressed & SCE_CTRL_RIGHT) { new_pos += (stream->is_clip ? 5000 : 10000); do_seek = 1; }
                else if (!stream->is_clip && (pressed & SCE_CTRL_LTRIGGER)) { new_pos -= 60000; do_seek = 1; }
                else if (!stream->is_clip && (pressed & SCE_CTRL_RTRIGGER)) { new_pos += 60000; do_seek = 1; }
                if (do_seek) {
                    if (new_pos < 0) new_pos = 0;
                    if (dur_ms > 0 && (uint64_t)new_pos > dur_ms) new_pos = dur_ms;
                    uint64_t seek_target = (uint64_t)new_pos;
                    if (!stream->is_clip && sources[current_source_idx].is_hls) {
                        seek_target = vt_hls_segment_start_ms(&sources[current_source_idx].hls_video, seek_target);
                    }
                    if (dur_ms > 0 && seek_target > dur_ms) seek_target = dur_ms;
                    SeekTask stask = { .player = player, .target_ms = seek_target, .result = -1 };
                    vt_ui_run_task(kick_tr("Cargando...", "Loading...", "Carregando..."), seek_task_fn, &stask, &cancel);
                    ui_touch_reset();
                    if (cancel) break;
                    hud_visible = 1;
                    last_hud_active_us = sceKernelGetProcessTimeWide();
                }
            }

            if ((pressed & SCE_CTRL_SELECT) && stream->is_live) {
                if (player) {
                    vt_decoder_destroy(player);
                    player = NULL;
                }
                player_source_term(&sources[current_source_idx]);
                task.quality = current_quality;
                task.start_position_ms = 0;
                task.start_paused = 0;
                task.result = -1;
                vt_ui_run_task(kick_tr("Recargando señal...", "Reloading stream...", "Recarregando sinal..."), open_task_fn, &task, &cancel);
                if (cancel || !player || task.result < 0) {
                    if (!cancel) vt_ui_toast(kick_tr("Error al recargar stream", "Error reloading stream", "Erro ao recarregar stream"), stream->username, 60);
                    player_source_term(&sources[current_source_idx]);
                    return;
                }
                last_hud_active_us = sceKernelGetProcessTimeWide();
                vt_ui_toast(kick_tr("Señal recargada", "Stream reloaded", "Sinal recarregado"), stream->username, 40);
                hud_visible = 1;
            }

            if (pressed & SCE_CTRL_TRIANGLE) {
                quality_menu_open = 1;
                for (int i = 0; i < QUALITY_OPTION_COUNT; i++) {
                    if (q_values[i] == current_quality) { quality_sel = i; break; }
                }
                hud_visible = 1;
                last_hud_active_us = now_us;
            }
        }

        /* Auto-hide HUD after inactivity */
        if (!quality_menu_open && hud_visible && !paused && !is_scrubbing && (now_us - last_hud_active_us > HUD_TIMEOUT_US)) {
            hud_visible = 0;
        }

        /* 3. Present Video Frame */
        vita2d_start_drawing();
        vita2d_clear_screen();

        if (player) {
            vt_decoder_present(player, 1);
        }

        /* 4. Render Overlay */
        if (quality_menu_open) {
            int mw = 440, mh = 248;
            int mx = (VT_SCREEN_W - mw) / 2;
            int my = (VT_SCREEN_H - mh) / 2;

            vt_ui_rect(0, 0, VT_SCREEN_W, VT_SCREEN_H, RGBA8(0, 0, 0, 180));
            vt_ui_rect(mx, my, mw, mh, VT_COLOR_SURFACE);
            vt_ui_rect(mx, my, 4, mh, VT_COLOR_ACCENT);
            vt_ui_text(mx + 25, my + 20, VT_COLOR_ACCENT, 1.0f,
                       kick_tr("Seleccionar Calidad", "Select Quality", "Selecionar Qualidade"));

            static const char *labels[] = { "HD", "360p", "240p", "144p" };
            for (int i = 0; i < QUALITY_OPTION_COUNT; i++) {
                int row_y = my + 55 + i * 44;
                if (i == quality_sel) {
                    vt_ui_rect(mx + 15, row_y, mw - 30, 38, VT_COLOR_FOCUS);
                    vt_ui_rect(mx + 15, row_y, 3, 38, VT_COLOR_ACCENT);
                }
                unsigned int col = (i == quality_sel) ? VT_COLOR_TEXT : VT_COLOR_MUTED;
                vt_ui_text(mx + 30, row_y + 9, col, 0.90f, labels[i]);
            }
        } else if (hud_visible) {
            /* Top bar */
            vt_ui_rect(0, 0, VT_SCREEN_W, 60, RGBA8(10, 12, 16, 230));
            vt_ui_rect(0, 59, VT_SCREEN_W, 1, VT_COLOR_BORDER);

            /* LIVE badge, CLIP badge, or VOD badge */
            if (stream->is_live) {
                vt_ui_rect(16, 17, 50, 26, VT_COLOR_DANGER);
                vt_ui_text(24, 20, VT_COLOR_TEXT, 0.80f, "LIVE");
            } else if (stream->is_clip) {
                vt_ui_rect(16, 17, 50, 26, VT_COLOR_SURFACE);
                vt_ui_rect(16, 17, 3, 26, RGBA8(160, 110, 255, 255));
                vt_ui_text(22, 20, RGBA8(160, 110, 255, 255), 0.80f, "CLIP");
            } else {
                vt_ui_rect(16, 17, 50, 26, VT_COLOR_SURFACE);
                vt_ui_rect(16, 17, 3, 26, VT_COLOR_ACCENT);
                vt_ui_text(23, 20, VT_COLOR_ACCENT, 0.80f, "VOD");
            }

            /* Exit Button (Circle icon + "Salir") at top right */
            const char *exit_txt = kick_tr("Salir", "Exit", "Sair");
            int etw = vt_ui_text_width(0.85f, exit_txt);
            float cir_sz = 18.0f;
            float cir_w = vt_ui_btn_icon_width(VT_BTN_CIRCLE, cir_sz);
            int xbw = (int)cir_w + 10 + etw + 20;
            int xbx = VT_SCREEN_W - 14 - xbw;
            int xby = 12, xbh = 36;
            vt_ui_rect(xbx, xby, xbw, xbh, VT_COLOR_SURFACE);
            vt_ui_draw_btn_icon(VT_BTN_CIRCLE, (float)(xbx + 10), (float)(xby + 9), cir_sz);
            vt_ui_text(xbx + 10 + (int)cir_w + 6, xby + 9, VT_COLOR_TEXT, 0.85f, exit_txt);

            /* Battery (to the left of Exit button) */
            int batt = vt_ui_get_battery_percent();
            int chrg = vt_ui_is_battery_charging();
            char bstr[32];
            if (chrg) snprintf(bstr, sizeof bstr, "⚡ %d%%", batt);
            else snprintf(bstr, sizeof bstr, "🔋 %d%%", batt);
            int rw = vt_ui_text_width(0.82f, bstr);
            int batt_x = xbx - 14 - rw;
            vt_ui_text(batt_x, 20, VT_COLOR_MUTED, 0.82f, bstr);

            /* Quality Selector Button (strictly to the left of Battery) */
            int qbw = 92, qbh = 36;
            int qbx = batt_x - 14 - qbw;
            int qby = 12;
            vt_ui_rect(qbx, qby, qbw, qbh, VT_COLOR_SURFACE);
            char qbtn[32];
            if (current_quality == 720) snprintf(qbtn, sizeof qbtn, "HD");
            else snprintf(qbtn, sizeof qbtn, "%dp", current_quality);
            int qtw = vt_ui_text_width(0.82f, qbtn);
            float tri_sz = 18.0f;
            float tri_w = vt_ui_btn_icon_width(VT_BTN_TRIANGLE, tri_sz);
            int tot_q = (int)tri_w + 6 + qtw;
            int sx_q = qbx + (qbw - tot_q) / 2;
            vt_ui_draw_btn_icon(VT_BTN_TRIANGLE, (float)sx_q, (float)(qby + 9), tri_sz);
            vt_ui_text(sx_q + (int)tri_w + 6, qby + 9, VT_COLOR_TEXT, 0.82f, qbtn);

            /* Follow Button (hidden if viewing own stream, strictly to the left of Quality) */
            int fbw = 145, fbh = 36;
            int fbx = qbx - 14 - fbw;
            int fby = 12;
            if (!is_me) {
                float sq_sz = 18.0f;
                float sq_w = vt_ui_btn_icon_width(VT_BTN_SQUARE, sq_sz);
                if (is_following) {
                    vt_ui_rect(fbx, fby, fbw, fbh, VT_COLOR_SURFACE);
                    vt_ui_rect(fbx, fby, 3, fbh, VT_COLOR_ACCENT);
                    const char *fol_txt = kick_tr("Siguiendo", "Following", "Seguindo");
                    int ftw = vt_ui_text_width(0.82f, fol_txt);
                    int tot = (int)sq_w + 6 + ftw;
                    int sx = fbx + (fbw - tot) / 2;
                    vt_ui_draw_btn_icon(VT_BTN_SQUARE, (float)sx, (float)(fby + 9), sq_sz);
                    vt_ui_text(sx + (int)sq_w + 6, fby + 9, VT_COLOR_ACCENT, 0.82f, fol_txt);
                } else {
                    vt_ui_rect(fbx, fby, fbw, fbh, VT_COLOR_ACCENT);
                    const char *fol_txt = kick_tr("Seguir", "Follow", "Seguir");
                    int ftw = vt_ui_text_width(0.85f, fol_txt);
                    int tot = (int)sq_w + 6 + ftw;
                    int sx = fbx + (fbw - tot) / 2;
                    vt_ui_draw_btn_icon(VT_BTN_SQUARE, (float)sx, (float)(fby + 9), sq_sz);
                    vt_ui_text(sx + (int)sq_w + 6, fby + 9, RGBA8(0, 0, 0, 255), 0.85f, fol_txt);
                }
            }

            /* Stream title & channel (clipped cleanly to avoid overlapping buttons) */
            int avail_title_w = (is_me ? qbx : fbx) - 76 - 12;
            if (avail_title_w < 180) avail_title_w = 180;
            const char *title = stream->session_title[0] ? stream->session_title : stream->username;
            vt_ui_text_clipped(76, 10, VT_COLOR_TEXT, 0.95f, avail_title_w, title);

            char subline[128];
            char qname[32];
            if (current_quality == 720) snprintf(qname, sizeof qname, "HD");
            else snprintf(qname, sizeof qname, "%dp", current_quality);
            snprintf(subline, sizeof subline, "%s  •  %s", stream->username, qname);
            vt_ui_text_clipped(76, 34, VT_COLOR_ACCENT, 0.78f, avail_title_w, subline);

            /* Live stream on-screen touch buttons: Play/Pause (START) and Reload (SELECT) */
            if (stream->is_live) {
                int btn_w = 90, btn_h = 84;
                int p_x = 480 - btn_w - 24;
                int r_x = 480 + 24;
                int btn_y = (VT_SCREEN_H - btn_h) / 2;

                /* --- Play/Pause Button (START) --- */
                vt_ui_rect(p_x, btn_y, btn_w, btn_h, RGBA8(10, 14, 20, 220));
                vt_ui_rect(p_x, btn_y, btn_w, 2, RGBA8(60, 70, 85, 255));
                vt_ui_rect(p_x, btn_y + btn_h - 2, btn_w, 2, RGBA8(60, 70, 85, 255));
                vt_ui_rect(p_x, btn_y, 2, btn_h, RGBA8(60, 70, 85, 255));
                vt_ui_rect(p_x + btn_w - 2, btn_y, 2, btn_h, RGBA8(60, 70, 85, 255));
                vt_ui_rect(p_x + 2, btn_y + btn_h - 4, btn_w - 4, 3, paused ? VT_COLOR_DANGER : VT_COLOR_ACCENT);

                if (paused) {
                    int ptw = vt_ui_text_width(1.4f, "▶");
                    vt_ui_text(p_x + (btn_w - ptw) / 2 + 2, btn_y + 12, VT_COLOR_TEXT, 1.4f, "▶");
                } else {
                    int ptw = vt_ui_text_width(1.3f, "⏸");
                    vt_ui_text(p_x + (btn_w - ptw) / 2, btn_y + 12, VT_COLOR_TEXT, 1.3f, "⏸");
                }
                float st_h = 18.0f;
                float st_w = vt_ui_btn_icon_width(VT_BTN_START, st_h);
                vt_ui_draw_btn_icon(VT_BTN_START, (float)(p_x + (btn_w - (int)st_w) / 2), (float)(btn_y + 52), st_h);

                /* --- Reload Button (SELECT) --- */
                vt_ui_rect(r_x, btn_y, btn_w, btn_h, RGBA8(10, 14, 20, 220));
                vt_ui_rect(r_x, btn_y, btn_w, 2, RGBA8(60, 70, 85, 255));
                vt_ui_rect(r_x, btn_y + btn_h - 2, btn_w, 2, RGBA8(60, 70, 85, 255));
                vt_ui_rect(r_x, btn_y, 2, btn_h, RGBA8(60, 70, 85, 255));
                vt_ui_rect(r_x + btn_w - 2, btn_y, 2, btn_h, RGBA8(60, 70, 85, 255));
                vt_ui_rect(r_x + 2, btn_y + btn_h - 4, btn_w - 4, 3, VT_COLOR_ACCENT);

                int rtw = vt_ui_text_width(1.4f, "⟳");
                vt_ui_text(r_x + (btn_w - rtw) / 2, btn_y + 10, VT_COLOR_ACCENT, 1.4f, "⟳");

                float sel_h = 18.0f;
                float sel_w = vt_ui_btn_icon_width(VT_BTN_SELECT, sel_h);
                vt_ui_draw_btn_icon(VT_BTN_SELECT, (float)(r_x + (btn_w - (int)sel_w) / 2), (float)(btn_y + 52), sel_h);
            } else {
                /* VOD Center Big Pause Icon if Paused */
                if (paused) {
                    int pw = 80, ph = 80;
                    int px = (VT_SCREEN_W - pw) / 2;
                    int py = (VT_SCREEN_H - ph) / 2;
                    vt_ui_rect(px, py, pw, ph, RGBA8(0, 0, 0, 160));
                    vt_ui_rect(px + 22, py + 18, 12, 44, VT_COLOR_TEXT);
                    vt_ui_rect(px + 46, py + 18, 12, 44, VT_COLOR_TEXT);
                }
            }

            /* Bottom Bar / Seekbar for VOD & Clips */
            if (!stream->is_live) {
                int bbar_h = 56;
                int bbar_y = VT_SCREEN_H - bbar_h;
                vt_ui_rect(0, bbar_y, VT_SCREEN_W, bbar_h, RGBA8(10, 12, 16, 230));

                /* Play / Pause indicator: START button + gap + pause/play icon */
                float st_h = 18.0f;
                float st_w = vt_ui_btn_icon_width(VT_BTN_START, st_h);
                float st_x = 14.0f;
                vt_ui_draw_btn_icon(VT_BTN_START, st_x, (float)(bbar_y + 19), st_h);
                int pp_x = (int)(st_x + st_w) + 12;
                vt_ui_text(pp_x, bbar_y + 16, VT_COLOR_TEXT, 1.05f, paused ? "▶" : "⏸");

                /* Time display */
                uint64_t cur_pos = is_scrubbing ? scrub_preview_ms : status.position_ms;
                if (dur_ms > 0 && cur_pos > dur_ms) cur_pos = dur_ms;
                char pos_str[32], dur_str[32], tline[128];
                format_time_ms(cur_pos, pos_str, sizeof pos_str);
                if (dur_ms > 0) {
                    format_time_ms(dur_ms, dur_str, sizeof dur_str);
                    snprintf(tline, sizeof tline, "%s / %s", pos_str, dur_str);
                } else {
                    snprintf(tline, sizeof tline, "%s / --:--", pos_str);
                }
                int time_x = pp_x + 24;
                vt_ui_text(time_x, bbar_y + 18, VT_COLOR_TEXT, 0.82f, tline);

                /* Seekbar Track (moved to the right: x: 250..935 = 685px) */
                int track_x = 250;
                int track_w = VT_SCREEN_W - 25 - track_x;
                int track_y = bbar_y + 24;
                int track_h = 8;
                vt_ui_rect(track_x, track_y, track_w, track_h, RGBA8(50, 55, 65, 255));

                /* Played fill */
                int fill_w = 0;
                if (dur_ms > 0) {
                    fill_w = (int)((uint64_t)track_w * cur_pos / dur_ms);
                    if (fill_w > track_w) fill_w = track_w;
                    if (fill_w < 0) fill_w = 0;
                }
                vt_ui_rect(track_x, track_y, fill_w, track_h, VT_COLOR_ACCENT);

                /* Scrubber Knob (Circle or Rounded Box) */
                int knob_sz = 18;
                int knob_center_x = track_x + fill_w;
                int knob_x = knob_center_x - knob_sz / 2;
                int knob_y = track_y + (track_h - knob_sz) / 2;
                vt_ui_rect(knob_x, knob_y, knob_sz, knob_sz, RGBA8(255, 255, 255, 255));
                vt_ui_rect(knob_x + 3, knob_y + 3, knob_sz - 6, knob_sz - 6, VT_COLOR_ACCENT);

                /* Floating time preview tooltip above knob while scrubbing */
                if (is_scrubbing) {
                    int tip_w = vt_ui_text_width(0.80f, pos_str) + 16;
                    if (tip_w < 74) tip_w = 74;
                    int tip_h = 24;
                    int tip_x = knob_center_x - tip_w / 2;
                    if (tip_x < 10) tip_x = 10;
                    if (tip_x > VT_SCREEN_W - tip_w - 10) tip_x = VT_SCREEN_W - tip_w - 10;
                    int tip_y = track_y - 34;
                    vt_ui_rect(tip_x, tip_y, tip_w, tip_h, RGBA8(10, 12, 16, 245));
                    vt_ui_rect(tip_x, tip_y, 2, tip_h, VT_COLOR_ACCENT);
                    vt_ui_text(tip_x + 8, tip_y + 4, VT_COLOR_ACCENT, 0.80f, pos_str);
                }
            }
        }

        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();

        if (player) {
            vt_decoder_render_complete(player);
        }
    }

    /* Cleanup */
    if (player) {
        vt_decoder_destroy(player);
        player = NULL;
    }
    player_source_term(&sources[0]);
    player_source_term(&sources[1]);
}
