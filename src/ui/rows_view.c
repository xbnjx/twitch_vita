/* Kick Vita — Row rendering implementation */
#include "ui/rows_view.h"
#include "app.h"
#include "ui/thumbs.h"
#include "ui/strings.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

#define ROW_H 96
#define THUMB_W 152
#define THUMB_H 85

static void format_viewers(uint32_t viewers, char *out, size_t cap) {
    const char *vword = kick_is_en() ? "viewers" : "espectadores";
    if (viewers >= 1000000) {
        snprintf(out, cap, "%.1fM %s", (double)viewers / 1000000.0, vword);
    } else if (viewers >= 1000) {
        snprintf(out, cap, "%.1fk %s", (double)viewers / 1000.0, vword);
    } else if (viewers > 0) {
        snprintf(out, cap, "%u %s", viewers, vword);
    } else {
        snprintf(out, cap, "%s", kick_tr("En vivo", "Live", "Ao vivo"));
    }
}

void draw_stream_row(const KickStream *stream, int y, int selected) {
    if (selected) {
        vt_ui_rect(8, y, VT_SCREEN_W - 16, ROW_H - 4, VT_COLOR_FOCUS);
        vt_ui_rect(8, y, 4, ROW_H - 4, VT_COLOR_ACCENT);
    } else {
        vt_ui_rect(8, y, VT_SCREEN_W - 16, ROW_H - 4, VT_COLOR_SURFACE);
    }

    char key[128];
    if (stream->is_live && stream->id) {
        snprintf(key, sizeof key, "live_%s_%llu", stream->channel_slug, (unsigned long long)stream->id);
    } else if (stream->is_clip && stream->clip_slug[0]) {
        snprintf(key, sizeof key, "clip_%s", stream->clip_slug);
    } else if (stream->id) {
        snprintf(key, sizeof key, "vod_%s_%llu", stream->channel_slug, (unsigned long long)stream->id);
    } else {
        snprintf(key, sizeof key, "st_%s", stream->channel_slug[0] ? stream->channel_slug : "stream");
    }
    const char *img_url = stream->thumbnail_url[0] ? stream->thumbnail_url : stream->profile_pic;
    vita2d_texture *thumb = vt_thumbs_get(key, img_url);
    int tx = 24, ty = y + (ROW_H - 4 - THUMB_H) / 2;

    if (thumb && vita2d_texture_get_width(thumb) > 0 && vita2d_texture_get_height(thumb) > 0) {
        float sx = (float)THUMB_W / vita2d_texture_get_width(thumb);
        float sy = (float)THUMB_H / vita2d_texture_get_height(thumb);
        vita2d_draw_texture_scale(thumb, (float)tx, (float)ty, sx, sy);
    } else {
        vt_ui_rect(tx, ty, THUMB_W, THUMB_H, VT_COLOR_RAISED);
    }

    /* Live badge, Clip badge or Offline badge */
    if (stream->is_live) {
        vt_ui_rect(tx + 6, ty + 6, 38, 18, VT_COLOR_DANGER);
        vt_ui_text(tx + 10, ty + 5, VT_COLOR_TEXT, 0.70f, "LIVE");
    } else if (stream->is_clip) {
        vt_ui_rect(tx + 6, ty + 6, 38, 18, VT_COLOR_ACCENT);
        vt_ui_text(tx + 8, ty + 5, RGBA8(0, 0, 0, 255), 0.70f, "CLIP");
    } else {
        vt_ui_rect(tx + 6, ty + 6, 62, 18, VT_COLOR_RAISED);
        vt_ui_text(tx + 10, ty + 5, VT_COLOR_MUTED, 0.70f, "OFFLINE");
    }

    int text_x = tx + THUMB_W + 16;
    int text_w = selected ? (VT_SCREEN_W - text_x - 56) : (VT_SCREEN_W - text_x - 24);

    if (selected) {
        float cw = vt_ui_btn_icon_width(VT_BTN_CROSS, 22.0f);
        vt_ui_draw_btn_icon(VT_BTN_CROSS, (float)(VT_SCREEN_W - 20 - (int)cw), (float)(y + (ROW_H - 4 - 22) / 2), 22.0f);
    }

    /* Stream Title */
    const char *title = stream->session_title[0] ? stream->session_title :
                        (stream->is_live ? stream->username : (kick_is_en() ? "Channel Offline" : "Canal Desconectado"));
    vt_ui_text_clipped(text_x, y + 8, stream->is_live ? VT_COLOR_TEXT : VT_COLOR_MUTED, 1.0f, text_w, title);

    /* Streamer Name */
    vt_ui_text_clipped(text_x, y + 36, VT_COLOR_ACCENT, 0.85f, text_w, stream->username);

    /* Category & Viewers (Only show viewers for live streams and clips) */
    char subtext[256];
    char vstr[64];
    if (stream->is_live) {
        format_viewers(stream->viewer_count, vstr, sizeof vstr);
        if (stream->category_name[0]) {
            snprintf(subtext, sizeof subtext, "%s • %s", stream->category_name, vstr);
        } else {
            snprintf(subtext, sizeof subtext, "%s", vstr);
        }
    } else if (stream->is_clip) {
        format_viewers(stream->viewer_count, vstr, sizeof vstr);
        if (stream->duration_ms > 0) {
            unsigned s = (unsigned)(stream->duration_ms / 1000ULL);
            char dstr[32];
            snprintf(dstr, sizeof dstr, "%u:%02u", s / 60, s % 60);
            snprintf(subtext, sizeof subtext, "%s • %s", vstr, dstr);
        } else {
            snprintf(subtext, sizeof subtext, "%s", vstr);
        }
    } else {
        if (stream->category_name[0]) {
            snprintf(subtext, sizeof subtext, "%s", stream->category_name);
        } else {
            snprintf(subtext, sizeof subtext, kick_is_en() ? "Tap to view channel & VODs" : "Pulsa para ver canal y VODs");
        }
    }
    if (subtext[0]) {
        vt_ui_text_clipped(text_x, y + 58, VT_COLOR_MUTED, 0.80f, text_w, subtext);
    }
}

void draw_category_row(const KickCategory *cat, int y, int selected) {
    if (selected) {
        vt_ui_rect(8, y, VT_SCREEN_W - 16, 68, VT_COLOR_FOCUS);
        vt_ui_rect(8, y, 4, 68, VT_COLOR_ACCENT);
    } else {
        vt_ui_rect(8, y, VT_SCREEN_W - 16, 68, VT_COLOR_SURFACE);
    }
    vt_ui_text_clipped(24, y + 10, VT_COLOR_TEXT, 1.0f, VT_SCREEN_W - 48, cat->name);

    char vstr[64];
    format_viewers(cat->viewers, vstr, sizeof vstr);
    vt_ui_text_clipped(24, y + 36, VT_COLOR_MUTED, 0.85f, VT_SCREEN_W - 48, vstr);
}

void draw_empty_home(int list_top_y, int logged_in) {
    int y = list_top_y + 80;
    vt_ui_text(VT_SCREEN_W / 2 - 130, y, VT_COLOR_TEXT, 1.1f,
               kick_is_en() ? "No live streams found" : "No se encontraron streams");
    vt_ui_text(VT_SCREEN_W / 2 - 160, y + 35, VT_COLOR_MUTED, 0.85f,
               kick_is_en() ? "Tap the screen to search or explore channels" : "Toca la pantalla para buscar o explorar canales");
}
