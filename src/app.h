/* Twitch Vita — shared app constants and Twitch theme. */
#ifndef TWITCH_APP_H
#define TWITCH_APP_H

#include <stdint.h>
#include <stddef.h>
#include <vita2d.h>

#define TWITCH_VERSION "1.00"
#define TWITCH_DATA_DIR "ux0:data/TwitchVita"
#define TWITCH_AUTH_PATH TWITCH_DATA_DIR "/auth.json"
#define TWITCH_SETTINGS_PATH TWITCH_DATA_DIR "/settings.json"
#define TWITCH_QUALITY_PREF_PATH TWITCH_DATA_DIR "/quality.txt"
#define TWITCH_LANG_PATH TWITCH_DATA_DIR "/lang.txt"
#define TWITCH_STATE_PATH TWITCH_DATA_DIR "/state.json"

/* Compatibility aliases */
#define KICK_VERSION TWITCH_VERSION
#define KICK_DATA_DIR TWITCH_DATA_DIR
#define KICK_AUTH_PATH TWITCH_AUTH_PATH
#define KICK_SETTINGS_PATH TWITCH_SETTINGS_PATH
#define KICK_QUALITY_PREF_PATH TWITCH_QUALITY_PREF_PATH
#define KICK_LANG_PATH TWITCH_LANG_PATH
#define KICK_STATE_PATH TWITCH_STATE_PATH

#define VT_SCREEN_W 960
#define VT_SCREEN_H 544

/* Supported playback qualities (144p, 240p, 360p, 480p, 720p/HD) */
#define TWITCH_QUALITY_HD      720
#define TWITCH_QUALITY_720P    720
#define TWITCH_QUALITY_480P    480
#define TWITCH_QUALITY_360P    360
#define TWITCH_QUALITY_240P    240
#define TWITCH_QUALITY_144P    144
#define TWITCH_QUALITY_DEFAULT TWITCH_QUALITY_360P

#define KICK_QUALITY_HD      TWITCH_QUALITY_HD
#define KICK_QUALITY_720P    TWITCH_QUALITY_720P
#define KICK_QUALITY_360P    TWITCH_QUALITY_360P
#define KICK_QUALITY_240P    TWITCH_QUALITY_240P
#define KICK_QUALITY_144P    TWITCH_QUALITY_144P
#define KICK_QUALITY_DEFAULT TWITCH_QUALITY_DEFAULT

/* Twitch Official Dark & Vibrant Purple Palette */
#define VT_COLOR_BG        RGBA8(14, 14, 16, 255)       /* #0E0E10 Twitch Deep Dark */
#define VT_COLOR_SURFACE   RGBA8(24, 24, 27, 255)       /* #18181B Surface Cards */
#define VT_COLOR_RAISED    RGBA8(31, 31, 35, 255)       /* #1F1F23 Elevated Surfaces */
#define VT_COLOR_FOCUS     RGBA8(38, 38, 44, 255)       /* #26262C Active / Hover */
#define VT_COLOR_ACCENT    RGBA8(145, 70, 255, 255)     /* #9146FF Twitch Signature Purple */
#define VT_COLOR_ACCENT_HOV RGBA8(169, 112, 255, 255)   /* #A970FF Twitch Light Purple */
#define VT_COLOR_TEXT      RGBA8(239, 239, 241, 255)    /* #EFEFF1 Crisp White */
#define VT_COLOR_MUTED     RGBA8(173, 173, 184, 255)    /* #ADADB8 Secondary Muted */
#define VT_COLOR_FAINT     RGBA8(83, 83, 95, 255)       /* #53535F Inactive */
#define VT_COLOR_BORDER    RGBA8(46, 46, 53, 255)       /* #2E2E35 Subtle Borders */
#define VT_COLOR_DANGER    RGBA8(235, 4, 0, 255)        /* #EB0400 Twitch Red / Live */
#define VT_COLOR_OK        RGBA8(145, 70, 255, 255)     /* Twitch Purple */

#endif
