/* Kick Vita — Channel Profile Screen (Live, VODs, Offline Follow) */
#include "ui/channel_screen.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "app.h"
#include "log.h"
#include "store.h"
#include "twitch/twitch_api.h"
#include "twitch/twitch_auth.h"
#include "ui/ui.h"
#include "ui/thumbs.h"
#include "ui/rows_view.h"
#include "ui/player_screen.h"
#include "ui/touch.h"
#include "ui/strings.h"

enum {
    SUBTAB_LIVE = 0,
    SUBTAB_VODS,
    SUBTAB_CLIPS,
    SUBTAB_COUNT
};

static const char *get_subtab_name(int subtab) {
    if (subtab == SUBTAB_LIVE) return kick_tr("🔴 En vivo", "🔴 Live", "🔴 Ao vivo");
    if (subtab == SUBTAB_VODS) return kick_tr("🎬 Videos", "🎬 Videos", "🎬 Vídeos");
    return kick_tr("✂️ Clips", "✂️ Clips", "✂️ Clips");
}

/* Allocated in BSS to prevent stack overflow on worker threads */
static KickChannel s_channel;
static KickStreamList s_vods;
static KickStreamList s_clips;

typedef struct {
    char slug[64];
    volatile int *cancel;
    KickChannel *out_chan;
    KickStreamList *out_streams;
} ChannelTask;

static int fetch_channel_task(void *arg) {
    ChannelTask *t = arg;
    return kick_api_get_channel(t->slug, t->cancel, t->out_chan);
}

static int fetch_vods_task(void *arg) {
    ChannelTask *t = arg;
    return kick_api_get_channel_videos(t->slug, t->cancel, t->out_streams);
}

static int fetch_clips_task(void *arg) {
    ChannelTask *t = arg;
    return kick_api_get_channel_clips(t->slug, t->cancel, t->out_streams);
}

#define VODS_PER_PAGE 3

void channel_screen_run(const char *channel_slug, const char *username_hint) {
    if (!channel_slug || !*channel_slug) return;

    memset(&s_channel, 0, sizeof s_channel);
    memset(&s_vods, 0, sizeof s_vods);
    memset(&s_clips, 0, sizeof s_clips);

    volatile int cancel_init = 0;
    ChannelTask init_task = {
        .cancel = &cancel_init,
        .out_chan = &s_channel
    };
    snprintf(init_task.slug, sizeof init_task.slug, "%s", channel_slug);

    char load_msg[128];
    snprintf(load_msg, sizeof load_msg,
             kick_is_en() ? "Loading channel %s..." : "Cargando canal %s...",
             (username_hint && *username_hint) ? username_hint : channel_slug);
    int res = vt_ui_run_task(load_msg, fetch_channel_task, &init_task, &cancel_init);
    if (cancel_init || res != 0) {
        if (!cancel_init) vt_ui_toast(kick_is_en() ? "Failed to load channel" : "No se pudo cargar el canal", channel_slug, 60);
        return;
    }

    if (!s_channel.slug[0]) snprintf(s_channel.slug, sizeof s_channel.slug, "%s", channel_slug);
    if (!s_channel.username[0]) snprintf(s_channel.username, sizeof s_channel.username, "%s", (username_hint && *username_hint) ? username_hint : channel_slug);

    int sub_tab = SUBTAB_LIVE;
    int vods_loaded = 0;
    int vod_page = 0;
    int vod_sel = 0;
    int clips_loaded = 0;
    int clip_page = 0;
    int clip_sel = 0;
    int confirm_unfollow_dialog = 0;

    VtNavRepeat nav = {0};
    SceCtrlData old_pad = {0};
    int touch_down_x = -1;
    int touch_down_y = -1;

    while (1) {
        vt_thumbs_pump();

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        UiTouchEvent ev;
        unsigned int ev_flags = ui_touch_poll(&ev);

        unsigned int pressed = pad.buttons & ~old_pad.buttons;
        old_pad = pad;

        /* Confirm Unfollow Dialog Touch Handling */
        if (confirm_unfollow_dialog) {
            int dw = 460, dh = 180;
            int dx = (VT_SCREEN_W - dw) / 2, dy = (VT_SCREEN_H - dh) / 2;
            if (ev_flags & UI_TOUCH_EVENT_TAP) {
                if (ev.x >= dx + 24 && ev.x <= dx + 214 && ev.y >= dy + 110 && ev.y <= dy + 152) {
                    kick_api_unfollow_channel(s_channel.slug);
                    confirm_unfollow_dialog = 0;
                    vt_ui_toast(kick_is_en() ? "Unfollowed" : "Dejaste de seguir a", s_channel.username, 30);
                } else if (ev.x >= dx + 240 && ev.x <= dx + 430 && ev.y >= dy + 110 && ev.y <= dy + 152) {
                    confirm_unfollow_dialog = 0;
                } else if (ev.x < dx || ev.x > dx + dw || ev.y < dy || ev.y > dy + dh) {
                    confirm_unfollow_dialog = 0;
                }
            }
            if (pressed & SCE_CTRL_CROSS) {
                kick_api_unfollow_channel(s_channel.slug);
                confirm_unfollow_dialog = 0;
                vt_ui_toast(kick_is_en() ? "Unfollowed" : "Dejaste de seguir a", s_channel.username, 30);
            } else if (pressed & SCE_CTRL_CIRCLE) {
                confirm_unfollow_dialog = 0;
            }
            goto render_channel;
        }

        /* Exit on Circle */
        if (pressed & SCE_CTRL_CIRCLE) {
            break;
        }

        /* Check if viewing own channel */
        const KickAuthUser *me = kick_auth_get_user();
        int is_me = 0;
        if (me && me->username[0]) {
            if (strcasecmp(me->username, s_channel.slug) == 0 ||
                strcasecmp(me->username, s_channel.username) == 0) {
                is_me = 1;
            }
        }

        /* Toggle follow on Square */
        if (!is_me && (pressed & SCE_CTRL_SQUARE)) {
            if (!kick_auth_is_logged_in()) {
                vt_ui_toast(kick_tr("Inicia sesión para seguir", "Sign in to follow", "Inicie sessão para seguir"), s_channel.username, 60);
            } else if (kick_api_is_channel_followed(s_channel.slug)) {
                confirm_unfollow_dialog = 1;
            } else {
                kick_api_follow_channel_async(s_channel.slug);
                vt_ui_toast(kick_tr("Siguiendo a", "Following", "Seguindo"), s_channel.username, 30);
            }
        }

        /* Refresh channel on Triangle */
        if (pressed & SCE_CTRL_TRIANGLE) {
            volatile int cancel_ref = 0;
            ChannelTask ref_task = { .cancel = &cancel_ref, .out_chan = &s_channel };
            snprintf(ref_task.slug, sizeof ref_task.slug, "%s", s_channel.slug);
            vt_ui_run_task("Actualizando canal...", fetch_channel_task, &ref_task, &cancel_ref);
            vods_loaded = 0;
            clips_loaded = 0;
        }

        /* Switch sub-tabs with L/R triggers */
        if (pressed & SCE_CTRL_LTRIGGER) {
            sub_tab = (sub_tab > 0) ? sub_tab - 1 : SUBTAB_COUNT - 1;
            vod_page = 0; vod_sel = 0;
            clip_page = 0; clip_sel = 0;
        } else if (pressed & SCE_CTRL_RTRIGGER) {
            sub_tab = (sub_tab < SUBTAB_COUNT - 1) ? sub_tab + 1 : 0;
            vod_page = 0; vod_sel = 0;
            clip_page = 0; clip_sel = 0;
        }

        /* Auto-load VODs on entering VODs tab */
        if (sub_tab == SUBTAB_VODS && !vods_loaded) {
            volatile int cancel_v = 0;
            ChannelTask v_task = { .cancel = &cancel_v, .out_streams = &s_vods };
            snprintf(v_task.slug, sizeof v_task.slug, "%s", s_channel.slug);
            vt_ui_run_task("Cargando videos (VODs)...", fetch_vods_task, &v_task, &cancel_v);
            vods_loaded = 1;
            vod_page = 0;
            vod_sel = 0;
        }

        /* Auto-load Clips on entering Clips tab */
        if (sub_tab == SUBTAB_CLIPS && !clips_loaded) {
            volatile int cancel_c = 0;
            ChannelTask c_task = { .cancel = &cancel_c, .out_streams = &s_clips };
            snprintf(c_task.slug, sizeof c_task.slug, "%s", s_channel.slug);
            vt_ui_run_task(kick_tr("Cargando clips...", "Loading clips...", "Carregando clips..."), fetch_clips_task, &c_task, &cancel_c);
            clips_loaded = 1;
            clip_page = 0;
            clip_sel = 0;
        }

        int total_vod_pages = (s_vods.count + VODS_PER_PAGE - 1) / VODS_PER_PAGE;
        if (total_vod_pages <= 0) total_vod_pages = 1;
        if (vod_page >= total_vod_pages) vod_page = total_vod_pages - 1;
        if (vod_page < 0) vod_page = 0;

        int total_clip_pages = (s_clips.count + VODS_PER_PAGE - 1) / VODS_PER_PAGE;
        if (total_clip_pages <= 0) total_clip_pages = 1;
        if (clip_page >= total_clip_pages) clip_page = total_clip_pages - 1;
        if (clip_page < 0) clip_page = 0;

        /* Touch Swipe detection for pages */
        if (ev_flags & UI_TOUCH_EVENT_DOWN) {
            touch_down_x = ev.x;
            touch_down_y = ev.y;
        } else if (ev_flags & UI_TOUCH_EVENT_UP) {
            if (touch_down_x >= 0) {
                int dx = ev.x - touch_down_x;
                int dy = ev.y - touch_down_y;
                if (abs(dx) > 60 && abs(dx) > abs(dy) * 2) {
                    if (sub_tab == SUBTAB_VODS) {
                        if (dx < 0 && vod_page < total_vod_pages - 1) {
                            vod_page++;
                            vod_sel = 0;
                        } else if (dx > 0 && vod_page > 0) {
                            vod_page--;
                            vod_sel = 0;
                        }
                    } else if (sub_tab == SUBTAB_CLIPS) {
                        if (dx < 0 && clip_page < total_clip_pages - 1) {
                            clip_page++;
                            clip_sel = 0;
                        } else if (dx > 0 && clip_page > 0) {
                            clip_page--;
                            clip_sel = 0;
                        }
                    }
                }
            }
            touch_down_x = -1;
            touch_down_y = -1;
        }

        /* Touch Tap selection */
        if (ev_flags & UI_TOUCH_EVENT_TAP) {
            /* 0. Top Back Button (x: 16..115, y: 10..52) */
            if (ev.x >= 16 && ev.x <= 115 && ev.y >= 10 && ev.y <= 52) {
                break;
            }
            /* 1. Sub-tabs bar (y: 110..155) */
            else if (ev.y >= 110 && ev.y <= 155) {
                int stab_w = VT_SCREEN_W / SUBTAB_COUNT;
                int clicked_tab = ev.x / stab_w;
                if (clicked_tab >= 0 && clicked_tab < SUBTAB_COUNT) {
                    sub_tab = clicked_tab;
                    vod_page = 0; vod_sel = 0;
                    clip_page = 0; clip_sel = 0;
                }
            }
            /* 2. Follow button at top right (only if not self) */
            else if (!is_me && ev.x >= VT_SCREEN_W - 190 && ev.x <= VT_SCREEN_W - 20 && ev.y >= 16 && ev.y <= 66) {
                if (!kick_auth_is_logged_in()) {
                    vt_ui_toast(kick_tr("Inicia sesión para seguir", "Sign in to follow", "Inicie sessão para seguir"), s_channel.username, 60);
                } else if (kick_api_is_channel_followed(s_channel.slug)) {
                    confirm_unfollow_dialog = 1;
                } else {
                    kick_api_follow_channel_async(s_channel.slug);
                    vt_ui_toast(kick_tr("Siguiendo a", "Following", "Seguindo"), s_channel.username, 30);
                }
            }
            /* 3. Live Play button */
            else if (sub_tab == SUBTAB_LIVE && s_channel.is_live && ev.y >= 260 && ev.y <= 440) {
                player_screen_run(&s_channel.livestream);
            }
            /* 4. Tap on VOD/Clip items (y: 160..448) */
            else if (sub_tab == SUBTAB_VODS && ev.y >= 160 && ev.y < 448) {
                int row_click = (ev.y - 160) / 96;
                int clicked_idx = vod_page * VODS_PER_PAGE + row_click;
                if (clicked_idx >= 0 && clicked_idx < s_vods.count) {
                    vod_sel = row_click;
                    player_screen_run(&s_vods.items[clicked_idx]);
                }
            }
            else if (sub_tab == SUBTAB_CLIPS && ev.y >= 160 && ev.y < 448) {
                int row_click = (ev.y - 160) / 96;
                int clicked_idx = clip_page * VODS_PER_PAGE + row_click;
                if (clicked_idx >= 0 && clicked_idx < s_clips.count) {
                    clip_sel = row_click;
                    player_screen_run(&s_clips.items[clicked_idx]);
                }
            }
            /* 5. Pagination Buttons (y: 450..510) */
            else if (ev.y >= 450 && ev.y <= 510) {
                if (sub_tab == SUBTAB_VODS) {
                    if (ev.x >= 20 && ev.x <= 180 && vod_page > 0) {
                        vod_page--;
                        vod_sel = 0;
                    } else if (ev.x >= VT_SCREEN_W - 180 && ev.x <= VT_SCREEN_W - 20 && vod_page < total_vod_pages - 1) {
                        vod_page++;
                        vod_sel = 0;
                    }
                } else if (sub_tab == SUBTAB_CLIPS) {
                    if (ev.x >= 20 && ev.x <= 180 && clip_page > 0) {
                        clip_page--;
                        clip_sel = 0;
                    } else if (ev.x >= VT_SCREEN_W - 180 && ev.x <= VT_SCREEN_W - 20 && clip_page < total_clip_pages - 1) {
                        clip_page++;
                        clip_sel = 0;
                    }
                }
            }
        }

        /* Directional D-pad & Analog */
        unsigned int dir = vt_nav_repeat(&nav, pressed, pad.buttons, pad.lx, pad.ly);
        if (sub_tab == SUBTAB_VODS) {
            int items_in_cur_page = s_vods.count - vod_page * VODS_PER_PAGE;
            if (items_in_cur_page > VODS_PER_PAGE) items_in_cur_page = VODS_PER_PAGE;

            if (dir & SCE_CTRL_UP) {
                if (vod_sel > 0) vod_sel--;
                else if (vod_page > 0) {
                    vod_page--;
                    vod_sel = VODS_PER_PAGE - 1;
                }
            } else if (dir & SCE_CTRL_DOWN) {
                if (vod_sel < items_in_cur_page - 1) vod_sel++;
                else if (vod_page < total_vod_pages - 1) {
                    vod_page++;
                    vod_sel = 0;
                }
            } else if (dir & SCE_CTRL_LEFT) {
                if (vod_page > 0) {
                    vod_page--;
                    vod_sel = 0;
                }
            } else if (dir & SCE_CTRL_RIGHT) {
                if (vod_page < total_vod_pages - 1) {
                    vod_page++;
                    vod_sel = 0;
                }
            }
        } else if (sub_tab == SUBTAB_CLIPS) {
            int items_in_cur_page = s_clips.count - clip_page * VODS_PER_PAGE;
            if (items_in_cur_page > VODS_PER_PAGE) items_in_cur_page = VODS_PER_PAGE;

            if (dir & SCE_CTRL_UP) {
                if (clip_sel > 0) clip_sel--;
                else if (clip_page > 0) {
                    clip_page--;
                    clip_sel = VODS_PER_PAGE - 1;
                }
            } else if (dir & SCE_CTRL_DOWN) {
                if (clip_sel < items_in_cur_page - 1) clip_sel++;
                else if (clip_page < total_clip_pages - 1) {
                    clip_page++;
                    clip_sel = 0;
                }
            } else if (dir & SCE_CTRL_LEFT) {
                if (clip_page > 0) {
                    clip_page--;
                    clip_sel = 0;
                }
            } else if (dir & SCE_CTRL_RIGHT) {
                if (clip_page < total_clip_pages - 1) {
                    clip_page++;
                    clip_sel = 0;
                }
            }
        }

        /* Physical Cross (✕) selection */
        if (pressed & SCE_CTRL_CROSS) {
            if (sub_tab == SUBTAB_LIVE && s_channel.is_live) {
                player_screen_run(&s_channel.livestream);
            } else if (sub_tab == SUBTAB_VODS && s_vods.count > 0) {
                int clicked_idx = vod_page * VODS_PER_PAGE + vod_sel;
                if (clicked_idx >= 0 && clicked_idx < s_vods.count) {
                    player_screen_run(&s_vods.items[clicked_idx]);
                }
            } else if (sub_tab == SUBTAB_CLIPS && s_clips.count > 0) {
                int clicked_idx = clip_page * VODS_PER_PAGE + clip_sel;
                if (clicked_idx >= 0 && clicked_idx < s_clips.count) {
                    player_screen_run(&s_clips.items[clicked_idx]);
                }
            }
        }

render_channel:

        /* Render */
        vita2d_start_drawing();
        vita2d_clear_screen();

        /* Background */
        vt_ui_rect(0, 0, VT_SCREEN_W, VT_SCREEN_H, VT_COLOR_BG);

        /* Top Header Card */
        vt_ui_rect(0, 0, VT_SCREEN_W, 110, VT_COLOR_SURFACE);
        vt_ui_rect(0, 109, VT_SCREEN_W, 1, VT_COLOR_BORDER);

        /* Back Button */
        int bb_x = 16, bb_y = 35, bb_w = 95, bb_h = 40;
        vt_ui_rect(bb_x, bb_y, bb_w, bb_h, VT_COLOR_RAISED);
        float cir_sz = 18.0f;
        vt_ui_draw_btn_icon(VT_BTN_CIRCLE, (float)(bb_x + 10), (float)(bb_y + 11), cir_sz);
        vt_ui_text(bb_x + 34, bb_y + 10, VT_COLOR_TEXT, 0.88f, kick_is_en() ? "Back" : "Volver");

        /* Channel Avatar */
        char av_key[64];
        snprintf(av_key, sizeof av_key, "av_%s", s_channel.slug);
        vita2d_texture *avatar_tex = vt_thumbs_get(av_key, s_channel.profile_pic);
        int av_x = 125, av_y = 18, av_size = 74;
        if (avatar_tex) {
            float sx = (float)av_size / vita2d_texture_get_width(avatar_tex);
            float sy = (float)av_size / vita2d_texture_get_height(avatar_tex);
            vita2d_draw_texture_scale(avatar_tex, (float)av_x, (float)av_y, sx, sy);
        } else {
            vt_ui_rect(av_x, av_y, av_size, av_size, VT_COLOR_RAISED);
            vt_ui_text(av_x + 22, av_y + 22, VT_COLOR_MUTED, 1.3f, "K");
        }

        /* Username & Bio */
        vt_ui_text(av_x + av_size + 16, 20, VT_COLOR_TEXT, 1.25f, s_channel.username);
        if (s_channel.bio[0]) {
            vt_ui_text_clipped(av_x + av_size + 16, 50, VT_COLOR_MUTED, 0.80f, 480, s_channel.bio);
        } else {
            char fstr[64];
            snprintf(fstr, sizeof fstr, "@%s", s_channel.slug);
            vt_ui_text(av_x + av_size + 16, 50, VT_COLOR_MUTED, 0.80f, fstr);
        }

        /* Status indicator */
        if (s_channel.is_live) {
            vt_ui_rect(av_x + av_size + 16, 76, 75, 20, VT_COLOR_DANGER);
            vt_ui_text(av_x + av_size + 22, 78, VT_COLOR_TEXT, 0.70f, kick_is_en() ? "LIVE" : "EN VIVO");
        } else {
            vt_ui_text(av_x + av_size + 16, 76, VT_COLOR_MUTED, 0.75f,
                       kick_is_en() ? "Verified channel" : "Canal verificado");
        }

        /* Follow Button (Right Header, hidden if viewing own channel) */
        if (!is_me) {
            int is_following = kick_auth_is_logged_in() && kick_api_is_channel_followed(s_channel.slug);
            int fbtn_w = 165, fbtn_h = 42;
            int fbtn_x = VT_SCREEN_W - fbtn_w - 24;
            int fbtn_y = 34;
            float sq_sz = 20.0f;
            if (is_following) {
                vt_ui_rect(fbtn_x, fbtn_y, fbtn_w, fbtn_h, VT_COLOR_FOCUS);
                vt_ui_rect(fbtn_x, fbtn_y, 4, fbtn_h, VT_COLOR_ACCENT);
                vt_ui_draw_btn_icon(VT_BTN_SQUARE, (float)(fbtn_x + 14), (float)(fbtn_y + 11), sq_sz);
                vt_ui_text(fbtn_x + 38, fbtn_y + 11, VT_COLOR_ACCENT, 0.88f,
                           kick_is_en() ? "FOLLOWING" : "SIGUIENDO");
            } else {
                vt_ui_rect(fbtn_x, fbtn_y, fbtn_w, fbtn_h, VT_COLOR_ACCENT);
                vt_ui_draw_btn_icon(VT_BTN_SQUARE, (float)(fbtn_x + 14), (float)(fbtn_y + 11), sq_sz);
                vt_ui_text(fbtn_x + 38, fbtn_y + 11, RGBA8(0, 0, 0, 255), 0.90f,
                           kick_is_en() ? "FOLLOW" : "SEGUIR");
            }
        }

        /* Sub-tabs bar */
        int stab_y = 112;
        int stab_w = VT_SCREEN_W / SUBTAB_COUNT;
        vt_ui_rect(0, stab_y, VT_SCREEN_W, 44, VT_COLOR_SURFACE);
        /* L and R Trigger icons */
        vt_ui_draw_btn_icon(VT_BTN_L, 12.0f, (float)(stab_y + 12), 20.0f);
        float r_w = vt_ui_btn_icon_width(VT_BTN_R, 20.0f);
        vt_ui_draw_btn_icon(VT_BTN_R, (float)(VT_SCREEN_W - 12 - (int)r_w), (float)(stab_y + 12), 20.0f);

        for (int i = 0; i < SUBTAB_COUNT; i++) {
            int tx = i * stab_w;
            int active = (i == sub_tab);
            const char *st_name = get_subtab_name(i);
            if (active) {
                vt_ui_rect(tx, stab_y + 40, stab_w, 4, VT_COLOR_ACCENT);
                vt_ui_text(tx + (stab_w - vt_ui_text_width(0.98f, st_name)) / 2,
                           stab_y + 12, VT_COLOR_ACCENT, 0.98f, st_name);
            } else {
                vt_ui_text(tx + (stab_w - vt_ui_text_width(0.92f, st_name)) / 2,
                           stab_y + 12, VT_COLOR_MUTED, 0.92f, st_name);
            }
        }
        vt_ui_rect(0, stab_y + 43, VT_SCREEN_W, 1, VT_COLOR_BORDER);

        /* Content Area */
        int content_y = 164;

        if (sub_tab == SUBTAB_LIVE) {
            if (s_channel.is_live) {
                int cx = 30, cy = content_y + 10, cw = VT_SCREEN_W - 60, ch = 310;
                vt_ui_rect(cx, cy, cw, ch, VT_COLOR_SURFACE);
                vt_ui_rect(cx, cy, 4, ch, VT_COLOR_ACCENT);

                /* Live thumbnail preview */
                char live_th_key[64];
                snprintf(live_th_key, sizeof live_th_key, "live_%s_%llu", s_channel.slug, (unsigned long long)s_channel.livestream.id);
                const char *lth_url = s_channel.livestream.thumbnail_url[0] ? s_channel.livestream.thumbnail_url : s_channel.profile_pic;
                vita2d_texture *th = vt_thumbs_get(live_th_key, lth_url);
                int tw = 320, th_h = 180, tx = cx + 24, ty = cy + 24;
                if (th) {
                    float sx = (float)tw / vita2d_texture_get_width(th);
                    float sy = (float)th_h / vita2d_texture_get_height(th);
                    vita2d_draw_texture_scale(th, (float)tx, (float)ty, sx, sy);
                } else {
                    vt_ui_rect(tx, ty, tw, th_h, VT_COLOR_RAISED);
                }

                /* Stream Details */
                int dx = tx + tw + 24;
                vt_ui_text(dx, cy + 24, VT_COLOR_ACCENT, 0.85f,
                           kick_is_en() ? "BROADCASTING LIVE" : "TRANSMITIENDO EN VIVO");
                vt_ui_text_clipped(dx, cy + 54, VT_COLOR_TEXT, 1.15f, cw - (dx - cx) - 20, s_channel.livestream.session_title);
                if (s_channel.livestream.category_name[0]) {
                    char cstr[128];
                    snprintf(cstr, sizeof cstr, kick_is_en() ? "Category: %s" : "Categoría: %s", s_channel.livestream.category_name);
                    vt_ui_text(dx, cy + 96, VT_COLOR_MUTED, 0.90f, cstr);
                }

                char vstr[64];
                snprintf(vstr, sizeof vstr, kick_is_en() ? "Viewers: %u" : "Espectadores: %u", s_channel.livestream.viewer_count);
                vt_ui_text(dx, cy + 126, VT_COLOR_MUTED, 0.90f, vstr);

                /* Big Play Button */
                int pw = 280, ph = 50;
                int px = dx, py = cy + 175;
                vt_ui_rect(px, py, pw, ph, VT_COLOR_ACCENT);
                float cw_sz = 22.0f;
                float cw_w = vt_ui_btn_icon_width(VT_BTN_CROSS, cw_sz);
                const char *w_txt = kick_is_en() ? "WATCH LIVE" : "VER EN DIRECTO";
                int w_tw = vt_ui_text_width(1.05f, w_txt);
                int tot_w = (int)cw_w + 8 + w_tw;
                int sx_w = px + (pw - tot_w) / 2;
                vt_ui_draw_btn_icon(VT_BTN_CROSS, (float)sx_w, (float)(py + 14), cw_sz);
                vt_ui_text(sx_w + (int)cw_w + 8, py + 14, RGBA8(0, 0, 0, 255), 1.05f, w_txt);
            } else {
                /* Channel Offline Card */
                int cx = 140, cy = content_y + 40, cw = VT_SCREEN_W - 280, ch = 190;
                vt_ui_rect(cx, cy, cw, ch, VT_COLOR_SURFACE);
                vt_ui_rect(cx, cy, 4, ch, VT_COLOR_BORDER);

                vt_ui_text(cx + 40, cy + 35, VT_COLOR_TEXT, 1.25f,
                           kick_is_en() ? "Channel offline" : "Canal fuera de línea");
                vt_ui_text(cx + 40, cy + 75, VT_COLOR_MUTED, 0.90f,
                           kick_is_en() ? "The streamer is not live right now." : "El streamer no está transmitiendo en este momento.");
                vt_ui_text(cx + 40, cy + 115, VT_COLOR_ACCENT, 0.88f,
                           kick_is_en() ? "Tap 'Videos (VODs)' to watch past broadcasts." : "Toca 'Videos (VODs)' para ver contenido anterior.");
            }
        } else if (sub_tab == SUBTAB_VODS) {
            if (s_vods.count == 0) {
                vt_ui_text(VT_SCREEN_W / 2 - 140, content_y + 60, VT_COLOR_MUTED, 1.0f,
                           kick_is_en() ? "No videos (VODs) available" : "No hay videos (VODs) disponibles");
            } else {
                int row_y = content_y;
                int start_idx = vod_page * VODS_PER_PAGE;
                for (int i = 0; i < VODS_PER_PAGE && (start_idx + i) < s_vods.count; i++) {
                    int g_idx = start_idx + i;
                    draw_stream_row(&s_vods.items[g_idx], row_y, i == vod_sel);
                    row_y += 96;
                }

                /* Modern Pagination Bar (bottom: y = 456, h = 44) */
                int pbar_y = 456;
                vt_ui_rect(0, pbar_y, VT_SCREEN_W, 44, RGBA8(14, 17, 22, 240));
                vt_ui_rect(0, pbar_y, VT_SCREEN_W, 1, VT_COLOR_BORDER);

                /* [◀ Anterior] Button */
                int pb_w = 140, pb_h = 36;
                int p_prev_x = 24, p_btn_y = pbar_y + 4;
                const char *prev_lbl = kick_is_en() ? "◀ Previous" : "◀ Anterior";
                if (vod_page > 0) {
                    vt_ui_rect(p_prev_x, p_btn_y, pb_w, pb_h, VT_COLOR_FOCUS);
                    vt_ui_rect(p_prev_x, p_btn_y, 3, pb_h, VT_COLOR_ACCENT);
                    vt_ui_text(p_prev_x + 18, p_btn_y + 9, VT_COLOR_ACCENT, 0.85f, prev_lbl);
                } else {
                    vt_ui_rect(p_prev_x, p_btn_y, pb_w, pb_h, VT_COLOR_SURFACE);
                    vt_ui_text(p_prev_x + 18, p_btn_y + 9, VT_COLOR_FAINT, 0.85f, prev_lbl);
                }

                /* Page indicator text in center */
                char page_str[128];
                snprintf(page_str, sizeof page_str,
                         kick_is_en() ? "Page %d of %d  (%d videos)" : "Página %d de %d  (%d videos)",
                         vod_page + 1, total_vod_pages, s_vods.count);
                int pstr_w = vt_ui_text_width(0.88f, page_str);
                vt_ui_text((VT_SCREEN_W - pstr_w) / 2, p_btn_y + 9, VT_COLOR_TEXT, 0.88f, page_str);

                /* [Siguiente ▶] Button */
                int p_next_x = VT_SCREEN_W - pb_w - 24;
                const char *next_lbl = kick_is_en() ? "Next ▶" : "Siguiente ▶";
                if (vod_page < total_vod_pages - 1) {
                    vt_ui_rect(p_next_x, p_btn_y, pb_w, pb_h, VT_COLOR_FOCUS);
                    vt_ui_rect(p_next_x + pb_w - 3, p_btn_y, 3, pb_h, VT_COLOR_ACCENT);
                    vt_ui_text(p_next_x + 22, p_btn_y + 9, VT_COLOR_ACCENT, 0.85f, next_lbl);
                } else {
                    vt_ui_rect(p_next_x, p_btn_y, pb_w, pb_h, VT_COLOR_SURFACE);
                    vt_ui_text(p_next_x + 22, p_btn_y + 9, VT_COLOR_FAINT, 0.85f, next_lbl);
                }
            }
        } else if (sub_tab == SUBTAB_CLIPS) {
            if (s_clips.count == 0) {
                vt_ui_text(VT_SCREEN_W / 2 - 140, content_y + 60, VT_COLOR_MUTED, 1.0f,
                           kick_is_en() ? "No clips available" : "No hay clips disponibles");
            } else {
                int row_y = content_y;
                int start_idx = clip_page * VODS_PER_PAGE;
                for (int i = 0; i < VODS_PER_PAGE && (start_idx + i) < s_clips.count; i++) {
                    int g_idx = start_idx + i;
                    draw_stream_row(&s_clips.items[g_idx], row_y, i == clip_sel);
                    row_y += 96;
                }

                /* Modern Pagination Bar (bottom: y = 456, h = 44) */
                int pbar_y = 456;
                vt_ui_rect(0, pbar_y, VT_SCREEN_W, 44, RGBA8(14, 17, 22, 240));
                vt_ui_rect(0, pbar_y, VT_SCREEN_W, 1, VT_COLOR_BORDER);

                /* [◀ Anterior] Button */
                int pb_w = 140, pb_h = 36;
                int p_prev_x = 24, p_btn_y = pbar_y + 4;
                const char *prev_lbl = kick_is_en() ? "◀ Previous" : "◀ Anterior";
                if (clip_page > 0) {
                    vt_ui_rect(p_prev_x, p_btn_y, pb_w, pb_h, VT_COLOR_FOCUS);
                    vt_ui_rect(p_prev_x, p_btn_y, 3, pb_h, VT_COLOR_ACCENT);
                    vt_ui_text(p_prev_x + 18, p_btn_y + 9, VT_COLOR_ACCENT, 0.85f, prev_lbl);
                } else {
                    vt_ui_rect(p_prev_x, p_btn_y, pb_w, pb_h, VT_COLOR_SURFACE);
                    vt_ui_text(p_prev_x + 18, p_btn_y + 9, VT_COLOR_FAINT, 0.85f, prev_lbl);
                }

                /* Page indicator text in center */
                char page_str[128];
                snprintf(page_str, sizeof page_str,
                         kick_is_en() ? "Page %d of %d  (%d clips)" : "Página %d de %d  (%d clips)",
                         clip_page + 1, total_clip_pages, s_clips.count);
                int pstr_w = vt_ui_text_width(0.88f, page_str);
                vt_ui_text((VT_SCREEN_W - pstr_w) / 2, p_btn_y + 9, VT_COLOR_TEXT, 0.88f, page_str);

                /* [Siguiente ▶] Button */
                int p_next_x = VT_SCREEN_W - pb_w - 24;
                const char *next_lbl = kick_is_en() ? "Next ▶" : "Siguiente ▶";
                if (clip_page < total_clip_pages - 1) {
                    vt_ui_rect(p_next_x, p_btn_y, pb_w, pb_h, VT_COLOR_FOCUS);
                    vt_ui_rect(p_next_x + pb_w - 3, p_btn_y, 3, pb_h, VT_COLOR_ACCENT);
                    vt_ui_text(p_next_x + 22, p_btn_y + 9, VT_COLOR_ACCENT, 0.85f, next_lbl);
                } else {
                    vt_ui_rect(p_next_x, p_btn_y, pb_w, pb_h, VT_COLOR_SURFACE);
                    vt_ui_text(p_next_x + 22, p_btn_y + 9, VT_COLOR_FAINT, 0.85f, next_lbl);
                }
            }
        }

        /* Unfollow Confirmation Dialog */
        if (confirm_unfollow_dialog) {
            int dw = 460, dh = 180;
            int dx = (VT_SCREEN_W - dw) / 2, dy = (VT_SCREEN_H - dh) / 2;
            vt_ui_rect(0, 0, VT_SCREEN_W, VT_SCREEN_H, RGBA8(0, 0, 0, 160));
            vt_ui_rect(dx, dy, dw, dh, RGBA8(14, 18, 24, 250));
            vt_ui_rect(dx, dy, 4, dh, VT_COLOR_DANGER);
            char qline[128];
            snprintf(qline, sizeof qline,
                     kick_is_en() ? "Unfollow @%s?" : "¿Dejar de seguir a @%s?",
                     s_channel.username);
            vt_ui_text_clipped(dx + 24, dy + 28, VT_COLOR_TEXT, 1.05f, dw - 48, qline);
            vt_ui_text(dx + 24, dy + 62, VT_COLOR_MUTED, 0.82f,
                       kick_is_en() ? "You will no longer see this channel in your following list." :
                                      "Dejarás de ver este canal en tu lista de seguidos.");
            vt_ui_rect(dx + 24, dy + 110, 190, 42, VT_COLOR_DANGER);
            vt_ui_text(dx + 35, dy + 122, VT_COLOR_TEXT, 0.88f,
                       kick_is_en() ? "✕ Unfollow" : "✕ Dejar de seguir");
            vt_ui_rect(dx + 240, dy + 110, 190, 42, VT_COLOR_SURFACE);
            vt_ui_text(dx + 295, dy + 122, VT_COLOR_MUTED, 0.88f,
                       kick_is_en() ? "○ Cancel" : "○ Cancelar");
        }

        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
    }
}
