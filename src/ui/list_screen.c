/* Kick Vita — Main list screen implementation (Touch-first Kick Mobile/Web UI) */
#include "ui/list_screen.h"
#include "app.h"
#include "log.h"
#include "store.h"
#include "twitch/twitch_api.h"
#include "twitch/twitch_auth.h"
#include "ui/ui.h"
#include "ui/thumbs.h"
#include "ui/rows_view.h"
#include "ui/touch.h"
#include "ui/text_input.h"
#include "ui/player_screen.h"
#include "ui/channel_screen.h"
#include "ui/login_screen.h"
#include "ui/strings.h"

#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/rtc.h>

enum {
    TAB_LIVE = 0,
    TAB_FOLLOWING,
    TAB_SETTINGS,
    TAB_COUNT
};

static const char *kick_get_tab_name(int tab) {
    switch (tab) {
    case TAB_LIVE: return kick_tr("🔴 En vivo", "🔴 Live", "🔴 Ao vivo");
    case TAB_FOLLOWING: return kick_tr("⭐ Siguiendo", "⭐ Following", "⭐ Seguindo");
    case TAB_SETTINGS: return kick_tr("⚙ Ajustes", "⚙ Settings", "⚙ Ajustes");
    default: return "";
    }
}

static const VtLangChoice s_app_langs[] = {
    VT_LANG_ES, VT_LANG_EN, VT_LANG_PT
};
#define APP_LANG_TOTAL 3

static void cycle_quality(int dir) {
    int q = kick_store_get_quality();
    static const int qs[] = { 144, 240, 360, 720 };
    int idx = 2; /* 360p default */
    for (int i = 0; i < 4; i++) {
        if (qs[i] == q) { idx = i; break; }
    }
    idx = (idx + dir + 4) % 4;
    kick_store_set_quality(qs[idx]);
}

static void cycle_app_lang(int dir) {
    VtLangChoice c = vt_lang_choice();
    int idx = 0;
    for (int i = 0; i < APP_LANG_TOTAL; i++) {
        if (s_app_langs[i] == c) { idx = i; break; }
    }
    idx = (idx + dir + APP_LANG_TOTAL) % APP_LANG_TOTAL;
    vt_lang_set_choice(s_app_langs[idx]);
}

/* Allocated in BSS (zero stack overhead) */
static KickStreamList s_live_streams;
static KickStreamList s_following_streams;
static KickStreamList s_search_results;

typedef struct {
    int page;
    char query[96];
    volatile int *cancel;
    KickStreamList *out_streams;
} FetchTask;

static int fetch_live_task(void *arg) {
    FetchTask *t = arg;
    return kick_api_get_featured(kick_store_get_lang(), t->page, t->cancel, t->out_streams);
}

static int fetch_following_task(void *arg) {
    FetchTask *t = arg;
    return kick_api_get_followed(t->cancel, t->out_streams);
}

static int fetch_search_task(void *arg) {
    FetchTask *t = arg;
    return kick_api_search(t->query, t->cancel, t->out_streams);
}

static void trim_spaces(char *str) {
    if (!str) return;
    char *start = str;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (start != str) memmove(str, start, strlen(start) + 1);
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' || str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[len - 1] = '\0';
        len--;
    }
}

static void trigger_search_flow(char *last_search, int *is_search_active, int *selected_idx, int *scroll_offset) {
    char query[96] = {0};
    const char *prompt = kick_is_en() ? "Search Streamer / Channel" : "Buscar Streamer / Canal";
    if (ui_text_input(prompt, last_search, query, sizeof query) > 0) {
        trim_spaces(query);
        if (query[0]) {
            snprintf(last_search, 96, "%s", query);
            kick_store_search_add(query);
            *is_search_active = 1;

            volatile int cancel_srch = 0;
            FetchTask task = { .cancel = &cancel_srch, .out_streams = &s_search_results };
            snprintf(task.query, sizeof task.query, "%s", query);
            vt_ui_run_task(kick_is_en() ? "Searching Twitch..." : "Buscando en Twitch...", fetch_search_task, &task, &cancel_srch);
            *selected_idx = 0;
            *scroll_offset = 0;
        }
    }
}

static void switch_to_tab(int new_tab, int *current_tab, int *selected_idx, int *scroll_offset) {
    if (new_tab < 0 || new_tab >= TAB_COUNT) return;
    *current_tab = new_tab;
    *selected_idx = 0;
    *scroll_offset = 0;

    if (*current_tab == TAB_FOLLOWING) {
        if (kick_auth_is_logged_in()) {
            volatile int cancel_f = 0;
            FetchTask f_task = { .cancel = &cancel_f, .out_streams = &s_following_streams };
            vt_ui_run_task(kick_tr("Cargando seguidos...", "Loading followed...", "Carregando seguidos..."), fetch_following_task, &f_task, &cancel_f);
        } else {
            s_following_streams.count = 0;
        }
    }
}

void list_screen_run(void) {
    int current_tab = TAB_LIVE;
    int is_search_active = 0;
    int selected_idx = 0;
    int scroll_offset = 0;
    int settings_sel = 0;
    int confirm_logout_modal = 0;

    char last_search[96] = {0};
    VtNavRepeat nav = {0};

    /* Initial fetch for Home feed */
    volatile int cancel_init = 0;
    FetchTask init_task = {
        .page = 1,
        .cancel = &cancel_init,
        .out_streams = &s_live_streams
    };
    vt_ui_run_task(kick_is_en() ? "Loading live streams..." : "Cargando streams en vivo...", fetch_live_task, &init_task, &cancel_init);

    SceCtrlData old_pad = {0};
    int last_touch_y = -1;
    uint32_t last_follow_gen = kick_api_followed_generation();

    while (1) {
        /* Pump downloaded thumbnails so GPU textures decode and upload */
        vt_thumbs_pump();

        /* Auto-refresh Following list if user followed/unfollowed a channel */
        if (current_tab == TAB_FOLLOWING && kick_auth_is_logged_in() &&
            kick_api_followed_generation() != last_follow_gen) {
            last_follow_gen = kick_api_followed_generation();
            volatile int cancel_sync = 0;
            FetchTask sync_task = { .cancel = &cancel_sync, .out_streams = &s_following_streams };
            vt_ui_run_task(kick_tr("Actualizando seguidos...", "Updating followed...", "Atualizando seguidos..."),
                           fetch_following_task, &sync_task, &cancel_sync);
            if (selected_idx >= s_following_streams.count) {
                selected_idx = s_following_streams.count > 0 ? s_following_streams.count - 1 : 0;
            }
            if (scroll_offset > selected_idx) scroll_offset = selected_idx;
            last_follow_gen = kick_api_followed_generation();
        }

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        UiTouchEvent ev;
        unsigned int ev_flags = ui_touch_poll(&ev);

        unsigned int pressed = pad.buttons & ~old_pad.buttons;
        old_pad = pad;

        if (confirm_logout_modal) {
            int mw = 460, mh = 200;
            int mx = (VT_SCREEN_W - mw) / 2;
            int my = (VT_SCREEN_H - mh) / 2;
            int btn_w = 175, btn_h = 48;
            int yes_x = mx + 35, btn_y = my + 118;
            int no_x = mx + mw - 35 - btn_w;

            if (ev_flags & UI_TOUCH_EVENT_TAP) {
                if (ev.x >= yes_x && ev.x <= yes_x + btn_w && ev.y >= btn_y && ev.y <= btn_y + btn_h) {
                    kick_auth_logout();
                    s_following_streams.count = 0;
                    confirm_logout_modal = 0;
                    vt_ui_toast(kick_tr("Sesión cerrada", "Logged out", "Sessão encerrada"), "", 60);
                } else if (ev.x >= no_x && ev.x <= no_x + btn_w && ev.y >= btn_y && ev.y <= btn_y + btn_h) {
                    confirm_logout_modal = 0;
                } else if (ev.x < mx || ev.x > mx + mw || ev.y < my || ev.y > my + mh) {
                    confirm_logout_modal = 0;
                }
            }

            if (pressed & SCE_CTRL_CROSS) {
                kick_auth_logout();
                s_following_streams.count = 0;
                confirm_logout_modal = 0;
                vt_ui_toast(kick_tr("Sesión cerrada", "Logged out", "Sessão encerrada"), "", 60);
            } else if (pressed & SCE_CTRL_CIRCLE) {
                confirm_logout_modal = 0;
            }

            goto do_render;
        }

        /* Switch tabs with L / R buttons */
        if (pressed & SCE_CTRL_LTRIGGER) {
            is_search_active = 0;
            int next_tab = (current_tab > 0) ? current_tab - 1 : TAB_COUNT - 1;
            switch_to_tab(next_tab, &current_tab, &selected_idx, &scroll_offset);
        } else if (pressed & SCE_CTRL_RTRIGGER) {
            is_search_active = 0;
            int next_tab = (current_tab < TAB_COUNT - 1) ? current_tab + 1 : 0;
            switch_to_tab(next_tab, &current_tab, &selected_idx, &scroll_offset);
        }

        /* Refresh with Triangle */
        if (pressed & SCE_CTRL_TRIANGLE) {
            volatile int cancel_ref = 0;
            if (is_search_active) {
                if (last_search[0]) {
                    FetchTask task = { .cancel = &cancel_ref, .out_streams = &s_search_results };
                    snprintf(task.query, sizeof task.query, "%s", last_search);
                    vt_ui_run_task(kick_is_en() ? "Searching Twitch..." : "Buscando en Twitch...", fetch_search_task, &task, &cancel_ref);
                }
            } else if (current_tab == TAB_LIVE) {
                FetchTask task = { .page = 1, .cancel = &cancel_ref, .out_streams = &s_live_streams };
                vt_ui_run_task(kick_tr("Actualizando en vivo...", "Updating live...", "Atualizando ao vivo..."), fetch_live_task, &task, &cancel_ref);
            } else if (current_tab == TAB_FOLLOWING) {
                if (kick_auth_is_logged_in()) {
                    volatile int cancel_ref_f = 0;
                    FetchTask task = { .cancel = &cancel_ref_f, .out_streams = &s_following_streams };
                    vt_ui_run_task(kick_tr("Actualizando seguidos...", "Updating followed...", "Atualizando seguidos..."), fetch_following_task, &task, &cancel_ref_f);
                } else {
                    s_following_streams.count = 0;
                }
            }
            selected_idx = 0;
            scroll_offset = 0;
        }

        /* Search with Square */
        if (pressed & SCE_CTRL_SQUARE) {
            trigger_search_flow(last_search, &is_search_active, &selected_idx, &scroll_offset);
        }

        /* Exit search mode with Circle */
        if ((pressed & SCE_CTRL_CIRCLE) && is_search_active) {
            is_search_active = 0;
            selected_idx = 0;
            scroll_offset = 0;
        }

        /* Determine active item count */
        int item_count = 0;
        if (is_search_active) {
            item_count = s_search_results.count;
        } else if (current_tab == TAB_LIVE) {
            item_count = s_live_streams.count;
        } else if (current_tab == TAB_FOLLOWING) {
            item_count = s_following_streams.count;
        } else if (current_tab == TAB_SETTINGS) {
            item_count = 3;
        }

        /* Touch drag to scroll */
        if (ev_flags & UI_TOUCH_EVENT_DOWN) {
            last_touch_y = ev.y;
        } else if (ev_flags & UI_TOUCH_EVENT_MOVE) {
            if (last_touch_y >= 0 && (is_search_active || current_tab != TAB_SETTINGS)) {
                int dy = ev.y - last_touch_y;
                if (dy < -24 && scroll_offset + 3 < item_count) {
                    scroll_offset++;
                    last_touch_y = ev.y;
                } else if (dy > 24 && scroll_offset > 0) {
                    scroll_offset--;
                    last_touch_y = ev.y;
                }
            }
        } else if (ev_flags & UI_TOUCH_EVENT_UP) {
            last_touch_y = -1;
        }

        /* Touch Tap selection */
        if (ev_flags & UI_TOUCH_EVENT_TAP) {
            /* 1. "Mi canal" Button Tap (top right: x 810..960, y: 0..72) */
            if (ev.y >= 0 && ev.y < 72 && ev.x >= 810 && ev.x <= 960) {
                if (kick_auth_is_logged_in()) {
                    const KickAuthUser *u = kick_auth_get_user();
                    if (!u->username[0]) {
                        kick_auth_refresh_profile();
                        u = kick_auth_get_user();
                    }
                    if (u && u->username[0]) {
                        channel_screen_run(u->username, u->display_name[0] ? u->display_name : u->username);
                    }
                } else {
                    kick_login_screen_run();
                    if (kick_auth_is_logged_in()) {
                        const KickAuthUser *u = kick_auth_get_user();
                        if (u && u->username[0]) {
                            channel_screen_run(u->username, u->display_name[0] ? u->display_name : u->username);
                        }
                        volatile int cancel_f = 0;
                        FetchTask f_task = { .cancel = &cancel_f, .out_streams = &s_following_streams };
                        vt_ui_run_task(kick_is_en() ? "Loading followed..." : "Cargando seguidos...", fetch_following_task, &f_task, &cancel_f);
                    }
                }
            }
            /* 2. Top Search Bar Pill (x: 120..620, y: 0..72) */
            else if (ev.y >= 0 && ev.y < 72 && ev.x >= 120 && ev.x <= 620) {
                trigger_search_flow(last_search, &is_search_active, &selected_idx, &scroll_offset);
            }
            /* 3. Bottom Navigation Bar (y: 488..544) */
            else if (ev.y >= 488 && ev.y <= 544) {
                int l_margin = 64;
                int r_margin = 64;
                int tab_area_w = VT_SCREEN_W - l_margin - r_margin;
                int tab_w = tab_area_w / TAB_COUNT;
                if (ev.x < l_margin) {
                    is_search_active = 0;
                    int next_tab = (current_tab > 0) ? current_tab - 1 : TAB_COUNT - 1;
                    switch_to_tab(next_tab, &current_tab, &selected_idx, &scroll_offset);
                } else if (ev.x >= VT_SCREEN_W - r_margin) {
                    is_search_active = 0;
                    int next_tab = (current_tab < TAB_COUNT - 1) ? current_tab + 1 : 0;
                    switch_to_tab(next_tab, &current_tab, &selected_idx, &scroll_offset);
                } else {
                    int clicked_tab = (ev.x - l_margin) / tab_w;
                    if (clicked_tab >= 0 && clicked_tab < TAB_COUNT) {
                        is_search_active = 0;
                        switch_to_tab(clicked_tab, &current_tab, &selected_idx, &scroll_offset);
                    }
                }
            }
            /* 4. List Rows or Settings (y: 72..488) */
            else if (ev.y >= 72 && ev.y < 488) {
                if (is_search_active) {
                    int row_click = (ev.y - 78) / 96;
                    int clicked_idx = scroll_offset + row_click;
                    if (clicked_idx >= 0 && clicked_idx < s_search_results.count) {
                        selected_idx = clicked_idx;
                        channel_screen_run(s_search_results.items[selected_idx].channel_slug,
                                           s_search_results.items[selected_idx].username);
                    }
                } else if (current_tab == TAB_FOLLOWING && s_following_streams.count == 0) {
                    if (ev.y >= 140 && ev.y <= 270 && ev.x >= 170 && ev.x <= 540) {
                        if (!kick_auth_is_logged_in()) {
                            kick_login_screen_run();
                        }
                        if (kick_auth_is_logged_in()) {
                            volatile int cancel_f = 0;
                            FetchTask f_task = { .cancel = &cancel_f, .out_streams = &s_following_streams };
                            vt_ui_run_task(kick_tr("Cargando seguidos...", "Loading followed...", "Carregando seguidos..."), fetch_following_task, &f_task, &cancel_f);
                        }
                    }
                } else if (current_tab == TAB_SETTINGS) {
                    if (ev.y >= 88 && ev.y < 292) {
                        int opt = (ev.y - 88) / 68;
                        if (opt >= 0 && opt < 3) {
                            settings_sel = opt;
                            if (opt == 0) {
                                cycle_quality(1);
                            } else if (opt == 1) {
                                cycle_app_lang(1);
                            } else if (opt == 2) {
                                if (kick_auth_is_logged_in()) {
                                    confirm_logout_modal = 1;
                                } else {
                                    kick_login_screen_run();
                                    if (kick_auth_is_logged_in()) {
                                        volatile int cancel_f = 0;
                                        FetchTask f_task = { .cancel = &cancel_f, .out_streams = &s_following_streams };
                                        vt_ui_run_task(kick_is_en() ? "Loading followed..." : "Cargando seguidos...", fetch_following_task, &f_task, &cancel_f);
                                    }
                                }
                            }
                        }
                    }
                } else {
                    int row_click = (ev.y - 78) / 96;
                    int clicked_idx = scroll_offset + row_click;
                    if (clicked_idx >= 0 && clicked_idx < item_count) {
                        selected_idx = clicked_idx;
                        if (current_tab == TAB_LIVE) {
                            player_screen_run(&s_live_streams.items[selected_idx]);
                        } else if (current_tab == TAB_FOLLOWING) {
                            if (s_following_streams.items[selected_idx].is_live) {
                                player_screen_run(&s_following_streams.items[selected_idx]);
                            } else {
                                channel_screen_run(s_following_streams.items[selected_idx].channel_slug,
                                                   s_following_streams.items[selected_idx].username);
                            }
                        }
                    }
                }
            }
        }

        /* Directional button navigation */
        unsigned int dir = vt_nav_repeat(&nav, pressed, pad.buttons, pad.lx, pad.ly);
        if (!is_search_active && current_tab == TAB_SETTINGS) {
            if (dir & SCE_CTRL_UP) {
                if (settings_sel > 0) settings_sel--;
            } else if (dir & SCE_CTRL_DOWN) {
                if (settings_sel < 2) settings_sel++;
            } else if (dir & SCE_CTRL_LEFT) {
                if (settings_sel == 0) cycle_quality(-1);
                else if (settings_sel == 1) cycle_app_lang(-1);
            } else if ((dir & SCE_CTRL_RIGHT) || (pressed & SCE_CTRL_CROSS)) {
                if (settings_sel == 0) {
                    cycle_quality(1);
                } else if (settings_sel == 1) {
                    cycle_app_lang(1);
                } else if (settings_sel == 2 && (pressed & SCE_CTRL_CROSS)) {
                    if (kick_auth_is_logged_in()) {
                        confirm_logout_modal = 1;
                    } else {
                        kick_login_screen_run();
                        if (kick_auth_is_logged_in()) {
                            volatile int cancel_f = 0;
                            FetchTask f_task = { .cancel = &cancel_f, .out_streams = &s_following_streams };
                            vt_ui_run_task(kick_is_en() ? "Loading followed..." : "Cargando seguidos...", fetch_following_task, &f_task, &cancel_f);
                        }
                    }
                }
            }
        } else {
            if (dir & SCE_CTRL_UP) {
                if (selected_idx > 0) selected_idx--;
            } else if (dir & SCE_CTRL_DOWN) {
                if (selected_idx < item_count - 1) selected_idx++;
            }

            int visible_rows = 4;
            if (selected_idx < scroll_offset) scroll_offset = selected_idx;
            if (selected_idx >= scroll_offset + visible_rows) scroll_offset = selected_idx - visible_rows + 1;
        }

        /* Physical Cross (✕) selection */
        if (pressed & SCE_CTRL_CROSS) {
            if (is_search_active) {
                if (s_search_results.count > 0 && selected_idx < s_search_results.count) {
                    channel_screen_run(s_search_results.items[selected_idx].channel_slug,
                                       s_search_results.items[selected_idx].username);
                }
            } else if (current_tab == TAB_LIVE && s_live_streams.count > 0) {
                player_screen_run(&s_live_streams.items[selected_idx]);
            } else if (current_tab == TAB_FOLLOWING) {
                if (s_following_streams.count > 0 && selected_idx < s_following_streams.count) {
                    if (s_following_streams.items[selected_idx].is_live) {
                        player_screen_run(&s_following_streams.items[selected_idx]);
                    } else {
                        channel_screen_run(s_following_streams.items[selected_idx].channel_slug,
                                           s_following_streams.items[selected_idx].username);
                    }
                } else if (!kick_auth_is_logged_in()) {
                    kick_login_screen_run();
                    if (kick_auth_is_logged_in()) {
                        volatile int cancel_f = 0;
                        FetchTask f_task = { .cancel = &cancel_f, .out_streams = &s_following_streams };
                        vt_ui_run_task(kick_is_en() ? "Loading followed..." : "Cargando seguidos...", fetch_following_task, &f_task, &cancel_f);
                    }
                }
            }
        }

        /* Physical START button */
        if (pressed & SCE_CTRL_START) {
            if (current_tab == TAB_FOLLOWING && !kick_auth_is_logged_in()) {
                kick_login_screen_run();
                if (kick_auth_is_logged_in()) {
                    volatile int cancel_f = 0;
                    FetchTask f_task = { .cancel = &cancel_f, .out_streams = &s_following_streams };
                    vt_ui_run_task(kick_is_en() ? "Loading followed..." : "Cargando seguidos...", fetch_following_task, &f_task, &cancel_f);
                }
            }
        }

        if (pressed & SCE_CTRL_SELECT) {
            if (kick_auth_is_logged_in()) {
                const KickAuthUser *u = kick_auth_get_user();
                if (!u->username[0]) {
                    kick_auth_refresh_profile();
                    u = kick_auth_get_user();
                }
                if (u && u->username[0]) {
                    channel_screen_run(u->username, u->display_name[0] ? u->display_name : u->username);
                }
            } else {
                kick_login_screen_run();
                if (kick_auth_is_logged_in()) {
                    const KickAuthUser *u = kick_auth_get_user();
                    if (u && u->username[0]) {
                        channel_screen_run(u->username, u->display_name[0] ? u->display_name : u->username);
                    }
                    volatile int cancel_f = 0;
                    FetchTask f_task = { .cancel = &cancel_f, .out_streams = &s_following_streams };
                    vt_ui_run_task(kick_is_en() ? "Loading followed..." : "Cargando seguidos...", fetch_following_task, &f_task, &cancel_f);
                }
            }
        }

        /* Render */
do_render:
        vita2d_start_drawing();
        vita2d_clear_screen();

        /* Background */
        vt_ui_rect(0, 0, VT_SCREEN_W, VT_SCREEN_H, VT_COLOR_BG);

        /* 1. Header (Top Bar, 72px tall) */
        vt_ui_rect(0, 0, VT_SCREEN_W, 72, VT_COLOR_SURFACE);
        vt_ui_rect(0, 71, VT_SCREEN_W, 1, VT_COLOR_BORDER);

        /* Twitch Logo */
        vita2d_texture *logo = vt_ui_get_logo();
        if (logo) {
            float h = 42.0f;
            float w = h * (float)vita2d_texture_get_width(logo) / (float)vita2d_texture_get_height(logo);
            float sx = w / (float)vita2d_texture_get_width(logo);
            float sy = h / (float)vita2d_texture_get_height(logo);
            vita2d_draw_texture_scale(logo, 16.0f, 15.0f, sx, sy);
        } else {
            vt_ui_rect(16, 18, 38, 36, VT_COLOR_ACCENT);
            vt_ui_text(60, 22, VT_COLOR_ACCENT, 1.25f, "Twitch");
        }

        /* Search Input Pill */
        int sb_x = 136, sb_y = 14, sb_w = 470, sb_h = 44;
        vt_ui_rect(sb_x, sb_y, sb_w, sb_h, VT_COLOR_RAISED);
        vt_ui_rect(sb_x, sb_y, 4, sb_h, VT_COLOR_ACCENT);
        float sq_sz = 22.0f;
        vt_ui_draw_btn_icon(VT_BTN_SQUARE, (float)(sb_x + 12), (float)(sb_y + 11), sq_sz);
        if (is_search_active && last_search[0]) {
            char sdisp[128];
            snprintf(sdisp, sizeof sdisp, "%s", last_search);
            vt_ui_text(sb_x + 42, sb_y + 11, VT_COLOR_TEXT, 0.90f, sdisp);
        } else {
            vt_ui_text(sb_x + 42, sb_y + 11, VT_COLOR_MUTED, 0.90f,
                       kick_is_en() ? "Search channel or streamer..." : "Buscar canal o streamer...");
        }

        /* "Mi canal" Green Button (Right side, styled like follow button) */
        int mc_w = 132, mc_h = 42, mc_y = 15;
        int mc_x = VT_SCREEN_W - 14 - mc_w;
        vt_ui_rect(mc_x, mc_y, mc_w, mc_h, VT_COLOR_ACCENT);
        const char *my_chan_txt = kick_tr("Mi canal", "My channel", "Meu canal");
        int my_tw = vt_ui_text_width(0.85f, my_chan_txt);
        float sel_h = 16.0f;
        float sel_w = vt_ui_btn_icon_width(VT_BTN_SELECT, sel_h);
        int tot_mc = (int)sel_w + 6 + my_tw;
        int mc_start_x = mc_x + (mc_w - tot_mc) / 2;
        vt_ui_draw_btn_icon(VT_BTN_SELECT, (float)mc_start_x, (float)(mc_y + 13), sel_h);
        vt_ui_text(mc_start_x + (int)sel_w + 6, mc_y + 11, RGBA8(0, 0, 0, 255), 0.85f, my_chan_txt);

        /* Battery & Clock (Left of "Mi canal") */
        SceDateTime now;
        sceRtcGetCurrentClockLocalTime(&now);
        int batt = vt_ui_get_battery_percent();
        int chrg = vt_ui_is_battery_charging();
        char rstr[64];
        if (chrg) snprintf(rstr, sizeof rstr, "%02d:%02d  ⚡ %d%%", now.hour, now.minute, batt);
        else snprintf(rstr, sizeof rstr, "%02d:%02d  🔋 %d%%", now.hour, now.minute, batt);
        int rw = vt_ui_text_width(0.90f, rstr);
        vt_ui_text(mc_x - 16 - rw, 26, VT_COLOR_TEXT, 0.90f, rstr);

        /* 2. Content Area (y = 78 to 486) */
        int list_top_y = 78;
        int visible_rows = 4;

        if (is_search_active) {
            if (s_search_results.count == 0) {
                int bx = 160, by = list_top_y + 40, bw = 640;
                vt_ui_rect(bx, by, bw, 130, VT_COLOR_SURFACE);
                vt_ui_rect(bx, by, 4, 130, VT_COLOR_ACCENT);
                vt_ui_text(bx + 24, by + 22, VT_COLOR_TEXT, 1.15f,
                           kick_is_en() ? "Search Twitch" : "Búsqueda en Twitch");
                if (last_search[0]) {
                    char sline[160];
                    snprintf(sline, sizeof sline,
                             kick_is_en() ? "No results found for \"%s\"" : "No se encontraron resultados para \"%s\"",
                             last_search);
                    vt_ui_text(bx + 24, by + 58, VT_COLOR_MUTED, 0.90f, sline);
                } else {
                    vt_ui_draw_btn_icon(VT_BTN_SQUARE, (float)(bx + 24), (float)(by + 58), 20.0f);
                    vt_ui_text(bx + 52, by + 58, VT_COLOR_MUTED, 0.90f,
                               kick_is_en() ? "Press Square or tap search bar to search." :
                                              "Pulsa Cuadrado o toca la barra para buscar.");
                }
                vt_ui_draw_btn_icon(VT_BTN_CIRCLE, (float)(bx + 24), (float)(by + 92), 20.0f);
                vt_ui_text(bx + 52, by + 93, VT_COLOR_ACCENT, 0.85f,
                           kick_tr("Volver a la pestaña anterior",
                                   "Return to previous tab",
                                   "Voltar à aba anterior"));
            } else {
                int row_y = list_top_y;
                for (int i = scroll_offset; i < s_search_results.count && i < scroll_offset + visible_rows; i++) {
                    draw_stream_row(&s_search_results.items[i], row_y, i == selected_idx);
                    row_y += 96;
                }
            }
        } else if (current_tab == TAB_LIVE) {
            if (s_live_streams.count == 0) {
                draw_empty_home(list_top_y, 0);
            } else {
                int row_y = list_top_y;
                for (int i = scroll_offset; i < s_live_streams.count && i < scroll_offset + visible_rows; i++) {
                    draw_stream_row(&s_live_streams.items[i], row_y, i == selected_idx);
                    row_y += 96;
                }
            }
        } else if (current_tab == TAB_FOLLOWING) {
            if (s_following_streams.count == 0) {
                int bx = 160, by = list_top_y + 30, bw = 640;
                vt_ui_rect(bx, by, bw, 150, VT_COLOR_SURFACE);
                vt_ui_rect(bx, by, 4, 150, VT_COLOR_ACCENT);
                if (!kick_auth_is_logged_in()) {
                    vt_ui_text(bx + 24, by + 20, VT_COLOR_ACCENT, 1.15f,
                               kick_tr("Inicia sesión en tu cuenta", "Sign in to your account", "Entre na sua conta"));
                    vt_ui_text(bx + 24, by + 52, VT_COLOR_MUTED, 0.85f,
                               kick_tr("Conéctate mediante código QR para sincronizar los canales que sigues.",
                                       "Connect via QR code to sync the channels you follow.",
                                       "Conecte via código QR para sincronizar os canais que você segue."));
                    float st_h = 18.0f;
                    float st_w = vt_ui_btn_icon_width(VT_BTN_START, st_h);
                    const char *login_lbl = kick_tr("Iniciar Sesión con QR", "Sign In with QR", "Entrar com QR");
                    int login_w = vt_ui_text_width(0.90f, login_lbl);
                    int btn_w = (int)st_w + 12 + login_w + 32;
                    if (btn_w < 280) btn_w = 280;
                    vt_ui_rect(bx + 24, by + 90, btn_w, 42, VT_COLOR_FOCUS);
                    vt_ui_rect(bx + 24, by + 90, 3, 42, VT_COLOR_ACCENT);
                    vt_ui_draw_btn_icon(VT_BTN_START, (float)(bx + 36), (float)(by + 102), st_h);
                    vt_ui_text(bx + 36 + (int)st_w + 10, by + 102, VT_COLOR_TEXT, 0.90f, login_lbl);
                } else {
                    vt_ui_text(bx + 24, by + 20, VT_COLOR_TEXT, 1.15f,
                               kick_tr("Tus Canales Seguidos", "Your Followed Channels", "Canais que Você Segue"));
                    vt_ui_text(bx + 24, by + 52, VT_COLOR_MUTED, 0.88f,
                               kick_tr("Aún no tienes canales en tu lista o están cargando.",
                                       "No followed channels found or still loading.",
                                       "Nenhum canal seguido encontrado ou carregando."));
                    vt_ui_rect(bx + 24, by + 90, 280, 42, VT_COLOR_FOCUS);
                    vt_ui_rect(bx + 24, by + 90, 3, 42, VT_COLOR_ACCENT);
                    vt_ui_draw_btn_icon(VT_BTN_TRIANGLE, (float)(bx + 36), (float)(by + 101), 20.0f);
                    vt_ui_text(bx + 64, by + 102, VT_COLOR_ACCENT, 0.90f,
                               kick_tr("Recargar Canales Seguidos", "Reload Followed Channels", "Recarregar Canais Seguidos"));
                }
            } else {
                int row_y = list_top_y;
                for (int i = scroll_offset; i < s_following_streams.count && i < scroll_offset + visible_rows; i++) {
                    draw_stream_row(&s_following_streams.items[i], row_y, i == selected_idx);
                    row_y += 96;
                }
            }
        } else if (current_tab == TAB_SETTINGS) {
            int bx = 120, by = list_top_y + 10, bw = 720;

            /* Option 0: Calidad por defecto */
            int sel0 = (settings_sel == 0);
            vt_ui_rect(bx, by, bw, 60, sel0 ? VT_COLOR_FOCUS : VT_COLOR_SURFACE);
            if (sel0) {
                vt_ui_rect(bx, by, 4, 60, VT_COLOR_ACCENT);
                float cw = vt_ui_btn_icon_width(VT_BTN_CROSS, 22.0f);
                vt_ui_draw_btn_icon(VT_BTN_CROSS, (float)(bx + bw - 20 - (int)cw), (float)(by + 19), 22.0f);
            }
            vt_ui_text(bx + 24, by + 10, VT_COLOR_TEXT, 0.95f,
                       kick_tr("Calidad de reproducción preferida",
                               "Preferred stream quality",
                               "Qualidade de reprodução preferida"));
            char qstr[64];
            int cur_q = kick_store_get_quality();
            if (cur_q == 720) {
                snprintf(qstr, sizeof qstr, "< HD >");
            } else {
                snprintf(qstr, sizeof qstr, "< %dp >", cur_q);
            }
            vt_ui_text(bx + 24, by + 32, VT_COLOR_ACCENT, 0.85f, qstr);

            /* Option 1: Idioma de la Aplicación */
            by += 68;
            int sel1 = (settings_sel == 1);
            vt_ui_rect(bx, by, bw, 60, sel1 ? VT_COLOR_FOCUS : VT_COLOR_SURFACE);
            if (sel1) {
                vt_ui_rect(bx, by, 4, 60, VT_COLOR_ACCENT);
                float cw = vt_ui_btn_icon_width(VT_BTN_CROSS, 22.0f);
                vt_ui_draw_btn_icon(VT_BTN_CROSS, (float)(bx + bw - 20 - (int)cw), (float)(by + 19), 22.0f);
            }
            vt_ui_text(bx + 24, by + 10, VT_COLOR_TEXT, 0.95f,
                       kick_tr("Idioma de la interfaz (App)",
                               "Interface Language (App)",
                               "Idioma da interface (App)"));
            char app_lstr[64];
            snprintf(app_lstr, sizeof app_lstr, "< %s >", vt_lang_label(vt_lang_choice()));
            vt_ui_text(bx + 24, by + 32, VT_COLOR_ACCENT, 0.85f, app_lstr);

            /* Option 2: Cuenta de Twitch (Sesión / Iniciar o Cerrar) */
            by += 68;
            int sel2 = (settings_sel == 2);
            int logged = kick_auth_is_logged_in();
            vt_ui_rect(bx, by, bw, 60, sel2 ? VT_COLOR_FOCUS : VT_COLOR_SURFACE);
            if (sel2) {
                vt_ui_rect(bx, by, 4, 60, logged ? VT_COLOR_DANGER : VT_COLOR_ACCENT);
                float cw = vt_ui_btn_icon_width(VT_BTN_CROSS, 22.0f);
                vt_ui_draw_btn_icon(VT_BTN_CROSS, (float)(bx + bw - 20 - (int)cw), (float)(by + 19), 22.0f);
            }

            const KickAuthUser *u = kick_auth_get_user();
            if (logged && u && u->username[0]) {
                char ustr[128];
                snprintf(ustr, sizeof ustr,
                         kick_tr("Cuenta conectada: @%s", "Connected account: @%s", "Conta conectada: @%s"),
                         u->username);
                vt_ui_text(bx + 24, by + 10, VT_COLOR_TEXT, 0.95f, ustr);
                vt_ui_text(bx + 24, by + 32, VT_COLOR_DANGER, 0.85f,
                           kick_tr("Cerrar sesión / Desvincular cuenta",
                                   "Log out / Unlink account",
                                   "Encerrar sessão / Desvincular conta"));
            } else {
                vt_ui_text(bx + 24, by + 10, VT_COLOR_TEXT, 0.95f,
                           kick_tr("Cuenta de Twitch",
                                   "Twitch Account",
                                   "Conta Twitch"));
                vt_ui_text(bx + 24, by + 32, VT_COLOR_ACCENT, 0.85f,
                           kick_tr("Iniciar Sesión con QR",
                                   "Log In with QR",
                                   "Entrar com QR"));
            }

            /* Static Unselectable Info Card (Credits) */
            by += 68;
            vt_ui_rect(bx, by, bw, 60, RGBA8(16, 20, 26, 200));
            vt_ui_rect(bx, by, 3, 60, VT_COLOR_MUTED);
            vt_ui_text(bx + 24, by + 10, VT_COLOR_TEXT, 0.92f,
                       kick_tr("Proyecto hecho por Benjamin Duarte",
                               "Project made by Benjamin Duarte",
                               "Projeto feito por Benjamin Duarte"));
            vt_ui_text(bx + 24, by + 32, VT_COLOR_ACCENT, 0.82f,
                       kick_tr("Para conocerme más entra a www.benjaminduarte.lat",
                               "To learn more about me visit www.benjaminduarte.lat",
                               "Para me conhecer melhor acesse www.benjaminduarte.lat"));
        }

        /* 3. Modern Touch Bottom Navigation Bar (54px tall) */
        int bbar_y = VT_SCREEN_H - 54;
        vt_ui_rect(0, bbar_y, VT_SCREEN_W, 54, VT_COLOR_SURFACE);
        vt_ui_rect(0, bbar_y, VT_SCREEN_W, 1, VT_COLOR_BORDER);

        int l_margin = 64;
        int r_margin = 64;
        int tab_area_w = VT_SCREEN_W - l_margin - r_margin;
        int tab_w = tab_area_w / TAB_COUNT;

        for (int i = 0; i < TAB_COUNT; i++) {
            int tx = l_margin + i * tab_w;
            int is_active = (!is_search_active && i == current_tab);
            if (is_active) {
                vt_ui_rect(tx + 8, bbar_y + 4, tab_w - 16, 46, VT_COLOR_FOCUS);
                vt_ui_rect(tx + 24, bbar_y + 48, tab_w - 48, 3, VT_COLOR_ACCENT);
            }
            const char *tname = kick_get_tab_name(i);
            int tw = vt_ui_text_width(0.95f, tname);
            vt_ui_text(tx + (tab_w - tw) / 2, bbar_y + 16,
                       is_active ? VT_COLOR_ACCENT : VT_COLOR_MUTED, 0.95f, tname);
        }

        /* L and R Trigger Button Icons at edges (rendered after tabs to stay on top) */
        float l_h = 22.0f;
        vt_ui_draw_btn_icon(VT_BTN_L, 10.0f, (float)(bbar_y + 16), l_h);
        float r_h = 22.0f;
        float r_w = vt_ui_btn_icon_width(VT_BTN_R, r_h);
        vt_ui_draw_btn_icon(VT_BTN_R, (float)(VT_SCREEN_W - 10 - (int)r_w), (float)(bbar_y + 16), r_h);

        /* 4. Logout Confirmation Modal */
        if (confirm_logout_modal) {
            int mw = 460, mh = 200;
            int mx = (VT_SCREEN_W - mw) / 2;
            int my = (VT_SCREEN_H - mh) / 2;

            vt_ui_rect(0, 0, VT_SCREEN_W, VT_SCREEN_H, RGBA8(0, 0, 0, 185));
            vt_ui_rect(mx, my, mw, mh, VT_COLOR_SURFACE);
            vt_ui_rect(mx, my, mw, 4, VT_COLOR_DANGER);

            const char *dlg_title = kick_tr("¿Deseas cerrar la sesión?", "Do you want to log out?", "Deseja encerrar a sessão?");
            int tw = vt_ui_text_width(1.05f, dlg_title);
            vt_ui_text(mx + (mw - tw) / 2, my + 34, VT_COLOR_TEXT, 1.05f, dlg_title);

            const KickAuthUser *u = kick_auth_get_user();
            char sub_acc[96];
            snprintf(sub_acc, sizeof sub_acc, "@%s", (u && u->username[0]) ? u->username : "");
            int sub_tw = vt_ui_text_width(0.85f, sub_acc);
            vt_ui_text(mx + (mw - sub_tw) / 2, my + 68, VT_COLOR_MUTED, 0.85f, sub_acc);

            /* SÍ button */
            int btn_w = 175, btn_h = 48;
            int yes_x = mx + 35, btn_y = my + 118;
            vt_ui_rect(yes_x, btn_y, btn_w, btn_h, VT_COLOR_FOCUS);
            vt_ui_rect(yes_x, btn_y, 3, btn_h, VT_COLOR_DANGER);
            float x_sz = 20.0f;
            float x_w = vt_ui_btn_icon_width(VT_BTN_CROSS, x_sz);
            const char *yes_txt = kick_tr("SÍ", "YES", "SIM");
            int y_tw = vt_ui_text_width(0.95f, yes_txt);
            int yes_tot = (int)x_w + 8 + y_tw;
            int yes_sx = yes_x + (btn_w - yes_tot) / 2;
            vt_ui_draw_btn_icon(VT_BTN_CROSS, (float)yes_sx, (float)(btn_y + 14), x_sz);
            vt_ui_text(yes_sx + (int)x_w + 8, btn_y + 13, VT_COLOR_DANGER, 0.95f, yes_txt);

            /* NO button */
            int no_x = mx + mw - 35 - btn_w;
            vt_ui_rect(no_x, btn_y, btn_w, btn_h, VT_COLOR_SURFACE);
            vt_ui_rect(no_x, btn_y, 3, btn_h, VT_COLOR_BORDER);
            float cir_sz = 20.0f;
            float cir_w = vt_ui_btn_icon_width(VT_BTN_CIRCLE, cir_sz);
            const char *no_txt = kick_tr("NO", "NO", "NÃO");
            int n_tw = vt_ui_text_width(0.95f, no_txt);
            int no_tot = (int)cir_w + 8 + n_tw;
            int no_sx = no_x + (btn_w - no_tot) / 2;
            vt_ui_draw_btn_icon(VT_BTN_CIRCLE, (float)no_sx, (float)(btn_y + 14), cir_sz);
            vt_ui_text(no_sx + (int)cir_w + 8, btn_y + 13, VT_COLOR_TEXT, 0.95f, no_txt);
        }

        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
    }
}
