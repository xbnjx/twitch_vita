/* Twitch Vita — Native Login Screen (TV QR Code & Link Authentication) */
#include "ui/login_screen.h"
#include "twitch/twitch_auth.h"
#include "twitch/twitch_api.h"
#include "app.h"
#include "log.h"
#include "ui/ui.h"
#include "ui/text_input.h"
#include "ui/touch.h"
#include "ui/qrcodegen.h"
#include "ui/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

typedef struct {
    char uuid[128]; /* device_code */
    char key[32];   /* user_code */
    char url[512];  /* qr url */
    volatile int stop_requested;
    volatile int cancel_flag;
    volatile int auth_success;
    char err_msg[128];
} TvAuthContext;

static int tv_auth_poll_thread(SceSize args, void *argp) {
    TvAuthContext *ctx = *(TvAuthContext **)argp;
    vt_log("tv_auth_poll: started for code=%s", ctx->key);

    while (!ctx->stop_requested && !ctx->auth_success) {
        int r = twitch_auth_tv_poll_once(ctx->uuid, &ctx->cancel_flag,
                                         ctx->err_msg, sizeof ctx->err_msg);
        if (r == 1) {
            ctx->auth_success = 1;
            vt_log("tv_auth_poll: successfully authenticated!");
            twitch_api_sync_followed_async();
            break;
        }
        /* Poll every 3000ms */
        for (int i = 0; i < 30 && !ctx->stop_requested; i++) {
            sceKernelDelayThread(100 * 1000);
        }
    }

    return 0;
}

int kick_login_screen_run(void) {
    if (twitch_auth_is_logged_in()) {
        /* Already logged in: show profile info and logout option */
        const TwitchAuthUser *u = twitch_auth_get_user();
        if (!u->username[0]) {
            twitch_auth_refresh_profile();
            u = twitch_auth_get_user();
        }
        while (1) {
            SceCtrlData pad;
            sceCtrlPeekBufferPositive(0, &pad, 1);
            UiTouchEvent ev;
            int ev_flags = ui_touch_poll(&ev);

            static unsigned int old_b = 0;
            unsigned int pressed = pad.buttons & ~old_b;
            old_b = pad.buttons;

            if (pressed & SCE_CTRL_CIRCLE) break;

            if ((pressed & SCE_CTRL_CROSS) ||
                ((ev_flags & UI_TOUCH_EVENT_UP) && ev.y >= 280 && ev.y <= 340)) {
                twitch_auth_logout();
                vt_ui_toast("Sesión cerrada", "", 60);
                return 0;
            }

            vita2d_start_drawing();
            vita2d_clear_screen();

            vt_ui_top_bar(kick_tr("Cuenta de Twitch", "Twitch Account", "Conta Twitch"), 1);

            int box_w = 640;
            int box_h = 300;
            int box_x = (VT_SCREEN_W - box_w) / 2;
            int box_y = 100;

            vt_ui_rect(box_x, box_y, box_w, box_h, VT_COLOR_SURFACE);
            vt_ui_rect(box_x, box_y, 4, box_h, VT_COLOR_ACCENT);

            vt_ui_text(box_x + 30, box_y + 35, VT_COLOR_ACCENT, 1.20f,
                       kick_tr("Sesión Activa en Twitch", "Active Twitch Session", "Sessão Ativa na Twitch"));
            char uline[192];
            snprintf(uline, sizeof uline, "%s: @%s (%s)",
                     kick_tr("Usuario", "User", "Usuário"), u->username, u->display_name);
            vt_ui_text(box_x + 30, box_y + 80, VT_COLOR_TEXT, 1.10f, uline);

            if (u->email[0]) {
                char eline[192];
                snprintf(eline, sizeof eline, "%s: %s",
                         kick_tr("Correo", "Email", "E-mail"), u->email);
                vt_ui_text(box_x + 30, box_y + 115, VT_COLOR_MUTED, 0.90f, eline);
            }

            vt_ui_text(box_x + 30, box_y + 155, VT_COLOR_MUTED, 0.80f,
                       kick_tr("Tu cuenta está sincronizada. Puedes seguir canales y ver transmisiones.",
                               "Your account is synchronized. You can follow channels and watch streams.",
                               "Sua conta está sincronizada. Você pode seguir canais e assistir transmissões."));

            /* Logout button */
            int btn_y = box_y + 205;
            vt_ui_rect(box_x + 30, btn_y, box_w - 60, 48, VT_COLOR_DANGER);
            vt_ui_text(box_x + 50, btn_y + 12, VT_COLOR_TEXT, 1.05f,
                       kick_tr("✕ Cerrar sesión / Desvincular cuenta",
                               "✕ Log Out / Unlink account",
                               "✕ Encerrar sessão / Desvincular"));

            vt_ui_hint_bar(kick_tr("✕ Cerrar Sesión   ○ Volver",
                                   "✕ Log Out   ○ Back",
                                   "✕ Encerrar Sessão   ○ Voltar"));
            vita2d_end_drawing();
            vita2d_wait_rendering_done();
            vita2d_swap_buffers();
        }
        return 0;
    }

    /* Helper to cycle app interface language */
    static const VtLangChoice s_login_langs[] = {
        VT_LANG_ES,
        VT_LANG_EN,
        VT_LANG_PT
    };

    /* Guest: Start TV Link QR Flow via Twitch Device Code */
    TvAuthContext ctx = {0};
    twitch_auth_tv_start(ctx.uuid, sizeof ctx.uuid, ctx.key, sizeof ctx.key, ctx.url, sizeof ctx.url);

    /* Generate QR Code buffer */
    uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    bool qr_ok = qrcodegen_encodeText(ctx.url, tempBuffer, qrcode,
                                      qrcodegen_Ecc_LOW, 1, 10,
                                      qrcodegen_Mask_AUTO, true);

    int qr_modules = qr_ok ? qrcodegen_getSize(qrcode) : 0;

    /* Start polling background thread */
    TvAuthContext *p_ctx = &ctx;
    SceUID thid = sceKernelCreateThread("twitch_tv_poll", tv_auth_poll_thread,
                                        0x10000100, 0x10000, 0, 0, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, sizeof(TvAuthContext *), &p_ctx);
    }

    enum {
        FOCUS_GEN_CODE = 0,
        FOCUS_LANG = 1,
        FOCUS_BACK = 2
    };

    int anim_tick = 0;
    int success_delay = 0;
    int focus_idx = FOCUS_GEN_CODE;
    int stick_cooldown = 0;

    while (1) {
        anim_tick++;
        if (stick_cooldown > 0) stick_cooldown--;

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        UiTouchEvent ev;
        int ev_flags = ui_touch_poll(&ev);

        static unsigned int old_b = 0;
        unsigned int pressed = pad.buttons & ~old_b;
        old_b = pad.buttons;

        /* If authenticated, wait a moment to show confirmation then return */
        if (ctx.auth_success) {
            success_delay++;
            if (success_delay > 90 || (pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE))) {
                break;
            }
        }

        /* Physical Circle (O) exits from anywhere */
        if (pressed & SCE_CTRL_CIRCLE) {
            break;
        }

        /* Button Coordinates */
        int right_x = 388, right_y = 70, right_w = 524, right_h = 454;
        int new_code_btn_x = right_x + 24;
        int new_code_btn_y = right_y + 356;
        int new_code_btn_w = right_w - 48;
        int new_code_btn_h = 48;

        int back_btn_w = 114, back_btn_h = 36;
        int back_btn_x = VT_SCREEN_W - 16 - back_btn_w; /* 830 */
        int back_btn_y = 14;

        int lang_btn_w = 172, lang_btn_h = 36;
        int lang_btn_x = back_btn_x - 14 - lang_btn_w;  /* 644 */
        int lang_btn_y = 14;

        /* D-Pad & Analog Stick Focus Navigation */
        if (pressed & SCE_CTRL_UP) {
            if (focus_idx == FOCUS_GEN_CODE) {
                focus_idx = FOCUS_LANG;
            }
        } else if (pressed & SCE_CTRL_DOWN) {
            if (focus_idx == FOCUS_LANG || focus_idx == FOCUS_BACK) {
                focus_idx = FOCUS_GEN_CODE;
            }
        } else if (pressed & SCE_CTRL_LEFT) {
            if (focus_idx == FOCUS_BACK) {
                focus_idx = FOCUS_LANG;
            } else if (focus_idx == FOCUS_LANG) {
                VtLangChoice c = vt_lang_choice();
                int cur_i = 0;
                for (int i = 0; i < 3; i++) {
                    if (s_login_langs[i] == c) { cur_i = i; break; }
                }
                cur_i = (cur_i + 2) % 3;
                vt_lang_set_choice(s_login_langs[cur_i]);
            }
        } else if (pressed & SCE_CTRL_RIGHT) {
            if (focus_idx == FOCUS_LANG) {
                focus_idx = FOCUS_BACK;
            }
        } else if (stick_cooldown == 0) {
            if (pad.ly < 60) {
                if (focus_idx == FOCUS_GEN_CODE) {
                    focus_idx = (pad.lx > 180) ? FOCUS_BACK : FOCUS_LANG;
                    stick_cooldown = 15;
                }
            } else if (pad.ly > 195) {
                if (focus_idx != FOCUS_GEN_CODE) {
                    focus_idx = FOCUS_GEN_CODE;
                    stick_cooldown = 15;
                }
            } else if (pad.lx < 60) {
                if (focus_idx == FOCUS_BACK) {
                    focus_idx = FOCUS_LANG;
                    stick_cooldown = 15;
                }
            } else if (pad.lx > 195) {
                if (focus_idx == FOCUS_LANG) {
                    focus_idx = FOCUS_BACK;
                    stick_cooldown = 15;
                }
            }
        }

        /* Touch actions */
        int trigger_generate_code = 0;
        if (ev_flags & UI_TOUCH_EVENT_TAP) {
            if (ev.x >= back_btn_x && ev.x <= back_btn_x + back_btn_w &&
                ev.y >= back_btn_y && ev.y <= back_btn_y + back_btn_h) {
                break;
            }
            if (ev.x >= lang_btn_x && ev.x <= lang_btn_x + lang_btn_w &&
                ev.y >= lang_btn_y && ev.y <= lang_btn_y + lang_btn_h) {
                VtLangChoice c = vt_lang_choice();
                int cur_i = 0;
                for (int i = 0; i < 3; i++) {
                    if (s_login_langs[i] == c) { cur_i = i; break; }
                }
                cur_i = (cur_i + 1) % 3;
                vt_lang_set_choice(s_login_langs[cur_i]);
            }
            if (ev.x >= new_code_btn_x && ev.x <= new_code_btn_x + new_code_btn_w &&
                ev.y >= new_code_btn_y && ev.y <= new_code_btn_y + new_code_btn_h) {
                trigger_generate_code = 1;
            }
        }

        /* Cross (X) confirms active focus */
        if (pressed & SCE_CTRL_CROSS) {
            if (focus_idx == FOCUS_BACK) {
                break;
            } else if (focus_idx == FOCUS_LANG) {
                VtLangChoice c = vt_lang_choice();
                int cur_i = 0;
                for (int i = 0; i < 3; i++) {
                    if (s_login_langs[i] == c) { cur_i = i; break; }
                }
                cur_i = (cur_i + 1) % 3;
                vt_lang_set_choice(s_login_langs[cur_i]);
            } else if (focus_idx == FOCUS_GEN_CODE) {
                trigger_generate_code = 1;
            }
        }

        /* Triangle button generates code */
        if (pressed & SCE_CTRL_TRIANGLE) {
            trigger_generate_code = 1;
        }

        /* Regenerate QR Code */
        if (trigger_generate_code) {
            ctx.stop_requested = 1;
            ctx.cancel_flag = 1;
            if (thid >= 0) {
                sceKernelWaitThreadEnd(thid, NULL, NULL);
                sceKernelDeleteThread(thid);
                thid = -1;
            }
            ctx.stop_requested = 0;
            ctx.cancel_flag = 0;
            ctx.err_msg[0] = '\0';
            twitch_auth_tv_start(ctx.uuid, sizeof ctx.uuid, ctx.key, sizeof ctx.key, ctx.url, sizeof ctx.url);
            qr_ok = qrcodegen_encodeText(ctx.url, tempBuffer, qrcode,
                                         qrcodegen_Ecc_LOW, 1, 10,
                                         qrcodegen_Mask_AUTO, true);
            qr_modules = qr_ok ? qrcodegen_getSize(qrcode) : 0;
            thid = sceKernelCreateThread("twitch_tv_poll", tv_auth_poll_thread,
                                         0x10000100, 0x10000, 0, 0, NULL);
            if (thid >= 0) {
                sceKernelStartThread(thid, sizeof(TvAuthContext *), &p_ctx);
            }
        }

        /* Drawing */
        vita2d_start_drawing();
        vita2d_clear_screen();

        vt_ui_top_bar(kick_tr("Vincular Cuenta de Twitch", "Link Twitch Account", "Vincular Conta Twitch"), 0);

        /* Volver Button (Selectable & Clickable) */
        int is_back_focus = (focus_idx == FOCUS_BACK);
        vt_ui_rect(back_btn_x, back_btn_y, back_btn_w, back_btn_h,
                   is_back_focus ? VT_COLOR_FOCUS : VT_COLOR_SURFACE);
        if (is_back_focus) {
            vt_ui_rect(back_btn_x, back_btn_y, back_btn_w, 2, VT_COLOR_ACCENT);
            vt_ui_rect(back_btn_x, back_btn_y + back_btn_h - 2, back_btn_w, 2, VT_COLOR_ACCENT);
            vt_ui_rect(back_btn_x, back_btn_y, 2, back_btn_h, VT_COLOR_ACCENT);
            vt_ui_rect(back_btn_x + back_btn_w - 2, back_btn_y, 2, back_btn_h, VT_COLOR_ACCENT);
        }
        float cir_sz = 18.0f;
        float cir_w = vt_ui_btn_icon_width(VT_BTN_CIRCLE, cir_sz);
        const char *back_txt = kick_tr("Volver", "Back", "Voltar");
        int b_tw = vt_ui_text_width(0.85f, back_txt);
        int tot_b = (int)cir_w + 8 + b_tw;
        int start_bx = back_btn_x + (back_btn_w - tot_b) / 2;
        vt_ui_draw_btn_icon(VT_BTN_CIRCLE, (float)start_bx, (float)(back_btn_y + 9), cir_sz);
        vt_ui_text(start_bx + (int)cir_w + 8, back_btn_y + 9,
                   is_back_focus ? VT_COLOR_ACCENT : VT_COLOR_TEXT, 0.85f, back_txt);

        /* Language Switcher Badge on Top Bar */
        int is_lang_focus = (focus_idx == FOCUS_LANG);
        vt_ui_rect(lang_btn_x, lang_btn_y, lang_btn_w, lang_btn_h,
                   is_lang_focus ? VT_COLOR_FOCUS : VT_COLOR_RAISED);
        if (is_lang_focus) {
            vt_ui_rect(lang_btn_x, lang_btn_y, lang_btn_w, 2, VT_COLOR_ACCENT);
            vt_ui_rect(lang_btn_x, lang_btn_y + lang_btn_h - 2, lang_btn_w, 2, VT_COLOR_ACCENT);
            vt_ui_rect(lang_btn_x, lang_btn_y, 2, lang_btn_h, VT_COLOR_ACCENT);
            vt_ui_rect(lang_btn_x + lang_btn_w - 2, lang_btn_y, 2, lang_btn_h, VT_COLOR_ACCENT);
        }
        char lang_disp[64];
        snprintf(lang_disp, sizeof lang_disp, "🌐 %s", vt_lang_label(vt_lang_choice()));
        int ltw = vt_ui_text_width(0.85f, lang_disp);
        vt_ui_text(lang_btn_x + (lang_btn_w - ltw) / 2, lang_btn_y + 9,
                   is_lang_focus ? VT_COLOR_ACCENT : VT_COLOR_TEXT, 0.85f, lang_disp);

        /* Left Side: QR Code Container (x: 48, y: 70, w: 320, h: 454) */
        int left_x = 48, left_y = 70, left_w = 320, left_h = 454;
        vt_ui_rect(left_x, left_y, left_w, left_h, VT_COLOR_SURFACE);
        vt_ui_rect(left_x, left_y, 4, left_h, VT_COLOR_ACCENT);

        /* White QR canvas card */
        int card_sz = 240;
        int card_x = left_x + (left_w - card_sz) / 2;
        int card_y = left_y + 20;
        vt_ui_rect(card_x, card_y, card_sz, card_sz, RGBA8(255, 255, 255, 255));

        /* Draw QR modules */
        if (qr_ok && qr_modules > 0) {
            int pixel_scale = (card_sz - 16) / qr_modules;
            if (pixel_scale < 1) pixel_scale = 1;
            int qr_draw_w = qr_modules * pixel_scale;
            int off_x = card_x + (card_sz - qr_draw_w) / 2;
            int off_y = card_y + (card_sz - qr_draw_w) / 2;

            for (int y = 0; y < qr_modules; y++) {
                for (int x = 0; x < qr_modules; x++) {
                    if (qrcodegen_getModule(qrcode, x, y)) {
                        vt_ui_rect(off_x + x * pixel_scale,
                                   off_y + y * pixel_scale,
                                   pixel_scale, pixel_scale,
                                   RGBA8(0, 0, 0, 255));
                    }
                }
            }
        }

        /* Short link text */
        vt_ui_text_clipped(left_x + 16, card_y + card_sz + 18, VT_COLOR_MUTED, 0.75f, left_w - 32,
                           kick_tr("O ingresa desde tu móvil a:",
                                   "Or visit on your phone:",
                                   "Ou acesse no seu celular:"));
        vt_ui_text(left_x + 16, card_y + card_sz + 42, VT_COLOR_ACCENT, 0.90f, "twitch.tv/activate");

        /* Code Badge */
        int badge_y = card_y + card_sz + 74;
        vt_ui_rect(left_x + 16, badge_y, left_w - 32, 50, VT_COLOR_FOCUS);
        vt_ui_rect(left_x + 16, badge_y, 4, 50, VT_COLOR_ACCENT);
        vt_ui_text(left_x + 28, badge_y + 16, VT_COLOR_TEXT, 0.88f,
                   kick_tr("Código:", "Code:", "Código:"));

        char formatted_code[32];
        if (strlen(ctx.key) == 8) {
            snprintf(formatted_code, sizeof formatted_code, "%c%c%c%c - %c%c%c%c",
                     ctx.key[0], ctx.key[1], ctx.key[2], ctx.key[3],
                     ctx.key[4], ctx.key[5], ctx.key[6], ctx.key[7]);
        } else {
            snprintf(formatted_code, sizeof formatted_code, "%s", ctx.key);
        }
        vt_ui_text(left_x + 105, badge_y + 14, VT_COLOR_ACCENT, 1.20f, formatted_code);

        /* Right Side: Step-by-step instructions & Action button (x: 388, y: 70, w: 524, h: 454) */
        vt_ui_rect(right_x, right_y, right_w, right_h, VT_COLOR_SURFACE);
        vt_ui_rect(right_x, right_y, 4, right_h, VT_COLOR_ACCENT);

        vt_ui_text(right_x + 24, right_y + 20, VT_COLOR_ACCENT, 1.15f,
                   kick_tr("Vincular tu cuenta con un toque",
                           "Link your account with one touch",
                           "Vincule sua conta com um toque"));
        vt_ui_text(right_x + 24, right_y + 46, VT_COLOR_MUTED, 0.75f,
                   kick_tr("Compatible con cuentas de Twitch en cualquier dispositivo",
                           "Compatible with Twitch accounts on any device",
                           "Compatível com contas Twitch em qualquer dispositivo"));

        /* Step 1 */
        int s1_y = right_y + 78;
        vt_ui_rect(right_x + 24, s1_y, 28, 28, VT_COLOR_FOCUS);
        vt_ui_text(right_x + 32, s1_y + 5, VT_COLOR_ACCENT, 0.85f, "1");
        vt_ui_text(right_x + 64, s1_y + 4, VT_COLOR_TEXT, 0.95f,
                   kick_tr("Escanea el código QR con tu móvil",
                           "Scan the QR code with your phone",
                           "Escaneie o código QR com seu celular"));
        vt_ui_text(right_x + 64, s1_y + 26, VT_COLOR_MUTED, 0.72f,
                   kick_tr("Abre la cámara de tu teléfono para ir directamente a la activación.",
                           "Open your phone camera to go directly to activation.",
                           "Abra a câmera do celular para ir direto à ativação."));

        /* Step 2 */
        int s2_y = right_y + 144;
        vt_ui_rect(right_x + 24, s2_y, 28, 28, VT_COLOR_FOCUS);
        vt_ui_text(right_x + 32, s2_y + 5, VT_COLOR_ACCENT, 0.85f, "2");
        vt_ui_text(right_x + 64, s2_y + 4, VT_COLOR_TEXT, 0.95f,
                   kick_tr("Autoriza la vinculación",
                           "Authorize the connection",
                           "Autorize a vinculação"));
        vt_ui_text(right_x + 64, s2_y + 26, VT_COLOR_MUTED, 0.72f,
                   kick_tr("Confirma el inicio de sesión en tu cuenta de Twitch.",
                           "Confirm sign-in on your Twitch account.",
                           "Confirme o início de sessão na sua conta Twitch."));

        /* Step 3 */
        int s3_y = right_y + 210;
        vt_ui_rect(right_x + 24, s3_y, 28, 28, VT_COLOR_FOCUS);
        vt_ui_text(right_x + 32, s3_y + 5, VT_COLOR_ACCENT, 0.85f, "3");
        vt_ui_text(right_x + 64, s3_y + 4, VT_COLOR_TEXT, 0.95f,
                   kick_tr("¡Listo!", "All set!", "Tudo pronto!"));
        vt_ui_text(right_x + 64, s3_y + 26, VT_COLOR_MUTED, 0.72f,
                   kick_tr("La PS Vita se conectará sola en segundos. No necesitas PC.",
                           "PS Vita will connect automatically in seconds. No PC needed.",
                           "O PS Vita se conectará sozinho em segundos. Sem PC."));

        /* Status Banner (y: 280, height: 56) */
        int stat_y = right_y + 280;
        if (ctx.auth_success) {
            vt_ui_rect(right_x + 24, stat_y, right_w - 48, 56, RGBA8(28, 18, 70, 255));
            vt_ui_rect(right_x + 24, stat_y, 4, 56, VT_COLOR_ACCENT);
            char ok_msg[128];
            snprintf(ok_msg, sizeof ok_msg,
                     kick_tr("✔ ¡Sesión vinculada con éxito! Bienvenido @%s",
                             "✔ Successfully linked! Welcome @%s",
                             "✔ Sessão vinculada com sucesso! Bem-vindo @%s"),
                     twitch_auth_get_user()->username);
            vt_ui_text(right_x + 40, stat_y + 18, VT_COLOR_ACCENT, 0.95f, ok_msg);
        } else {
            vt_ui_rect(right_x + 24, stat_y, right_w - 48, 56, VT_COLOR_RAISED);
            vt_ui_rect(right_x + 24, stat_y, 4, 56, VT_COLOR_ACCENT);

            /* Pulsing indicator */
            const char *dots_es[] = { "Esperando confirmación en tu móvil",
                                      "Esperando confirmación en tu móvil.",
                                      "Esperando confirmación en tu móvil..",
                                      "Esperando confirmación en tu móvil..." };
            const char *dots_en[] = { "Waiting for confirmation on your phone",
                                      "Waiting for confirmation on your phone.",
                                      "Waiting for confirmation on your phone..",
                                      "Waiting for confirmation on your phone..." };
            const char *dots_pt[] = { "Aguardando confirmação no celular",
                                      "Aguardando confirmação no celular.",
                                      "Aguardando confirmação no celular..",
                                      "Aguardando confirmação no celular..." };
            int d_idx = (anim_tick / 20) % 4;
            const char *dots_txt = kick_tr(dots_es[d_idx], dots_en[d_idx], dots_pt[d_idx]);
            vt_ui_text(right_x + 40, stat_y + 18, VT_COLOR_ACCENT, 0.90f, dots_txt);
        }

        /* Action Button: "Generar nuevo código" directly under the confirmation banner */
        int btn_is_focused = (focus_idx == FOCUS_GEN_CODE);
        vt_ui_rect(new_code_btn_x, new_code_btn_y, new_code_btn_w, new_code_btn_h,
                   btn_is_focused ? VT_COLOR_FOCUS : VT_COLOR_RAISED);
        if (btn_is_focused) {
            vt_ui_rect(new_code_btn_x, new_code_btn_y, new_code_btn_w, 2, VT_COLOR_ACCENT);
            vt_ui_rect(new_code_btn_x, new_code_btn_y + new_code_btn_h - 2, new_code_btn_w, 2, VT_COLOR_ACCENT);
            vt_ui_rect(new_code_btn_x, new_code_btn_y, 2, new_code_btn_h, VT_COLOR_ACCENT);
            vt_ui_rect(new_code_btn_x + new_code_btn_w - 2, new_code_btn_y, 2, new_code_btn_h, VT_COLOR_ACCENT);
        }
        const char *gen_txt = kick_tr("Generar nuevo código",
                                      "Generate new code",
                                      "Gerar novo código");
        int gtw = vt_ui_text_width(0.92f, gen_txt);
        float tri_sz = 20.0f;
        float tri_w = vt_ui_btn_icon_width(VT_BTN_TRIANGLE, tri_sz);
        int tot_g = (int)tri_w + 8 + gtw;
        int sx_g = new_code_btn_x + (new_code_btn_w - tot_g) / 2;
        vt_ui_draw_btn_icon(VT_BTN_TRIANGLE, (float)sx_g, (float)(new_code_btn_y + 14), tri_sz);
        vt_ui_text(sx_g + (int)tri_w + 8, new_code_btn_y + 14,
                   btn_is_focused ? VT_COLOR_ACCENT : VT_COLOR_TEXT, 0.92f, gen_txt);

        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
    }

    /* Cleanup polling thread */
    ctx.stop_requested = 1;
    ctx.cancel_flag = 1;
    if (thid >= 0) {
        sceKernelWaitThreadEnd(thid, NULL, NULL);
        sceKernelDeleteThread(thid);
    }

    return ctx.auth_success ? 1 : 0;
}
