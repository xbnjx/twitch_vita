/* Twitch Vita — Native Twitch Client for PlayStation Vita */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include "app.h"
#include "log.h"
#include "store.h"
#include "twitch/twitch_auth.h"
#include "twitch/twitch_api.h"
#include "media/vita_decoder.h"
#include "net/http.h"
#include "ui/list_screen.h"
#include "ui/login_screen.h"
#include "ui/strings.h"
#include "ui/thumbs.h"
#include "ui/touch.h"
#include "ui/ui.h"

unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;
const volatile char vt_segment_pad[4096] = { 1 };
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;

int main(void) {
    if (vt_segment_pad[0] == 0) return 0;
    sceIoMkdir(TWITCH_DATA_DIR, 0777);
    vt_log_init();
    vt_log("Twitch Vita " TWITCH_VERSION " starting up");

    if (vt_ui_init() < 0) {
        sceKernelExitProcess(0);
        return 0;
    }
    ui_touch_init();

    kick_store_load();
    twitch_auth_init();

    int network_ready = (http_init() == 0);
    if (network_ready) {
        vt_thumbs_init();
        vt_decoder_init();

        if (!http_connected()) {
            vt_ui_toast(kick_tr("Sin conexión a Internet", "No Internet connection", "Sem conexão à Internet"),
                        kick_tr("Conéctate a una red Wi-Fi", "Connect to a Wi-Fi network", "Conecte-se a uma rede Wi-Fi"),
                        180);
        } else if (twitch_auth_is_logged_in()) {
            const TwitchAuthUser *u = twitch_auth_get_user();
            if (!u->username[0]) {
                twitch_auth_refresh_profile();
            }
            twitch_api_sync_followed_async();
        }

        list_screen_run();

        vt_thumbs_term();
    } else {
        vt_ui_toast(kick_tr("Error al inicializar red", "Network init error", "Erro ao inicializar rede"),
                    kick_tr("Comprueba el estado de red", "Check network status", "Verifique o status da rede"),
                    240);
    }

    http_shutdown();
    ui_touch_term();
    vt_ui_term();
    sceKernelExitProcess(0);
    return 0;
}
