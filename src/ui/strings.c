/* Kick Vita — interface language catalog and persistent choice */
#include "ui/strings.h"
#include "store.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>

#include "app.h"
#include "log.h"

static VtLangChoice g_choice = VT_LANG_ES;

static VtLangChoice effective(void) {
    if (g_choice == VT_LANG_EN) return VT_LANG_EN;
    if (g_choice == VT_LANG_PT) return VT_LANG_PT;
    return VT_LANG_ES;
}

int kick_is_en(void) {
    return effective() == VT_LANG_EN;
}

int kick_is_pt(void) {
    return effective() == VT_LANG_PT;
}

const char *kick_tr(const char *es, const char *en, const char *pt) {
    if (kick_is_en()) return en ? en : es;
    if (kick_is_pt()) return pt ? pt : es;
    return es;
}

const char *vt_str(VtStringId id) {
    const char *s = vt_str_in(effective(), id);
    if (!s) s = vt_str_in(VT_LANG_ES, id);
    if (!s) s = vt_str_in(VT_LANG_EN, id);
    return s ? s : "";
}

/* Interface language display labels */
const char *vt_lang_label(VtLangChoice choice) {
    if (choice == VT_LANG_EN) return "English";
    if (choice == VT_LANG_PT) return "Português";
    return "Español";
}

const char *vt_lang_effective_label(void) {
    return vt_lang_label(effective());
}

VtLangChoice vt_lang_choice(void) { return effective(); }

void vt_lang_load(void) {
    kick_store_load();
    const char *al = kick_store_get_app_lang();
    if (al && strcmp(al, "en") == 0) {
        g_choice = VT_LANG_EN;
    } else if (al && strcmp(al, "pt") == 0) {
        g_choice = VT_LANG_PT;
    } else {
        g_choice = VT_LANG_ES;
    }
    vt_log("lang: choice=%s effective=%s", vt_lang_code(g_choice),
           vt_lang_code(effective()));
}

void vt_lang_set_choice(VtLangChoice choice) {
    if (choice == VT_LANG_EN) g_choice = VT_LANG_EN;
    else if (choice == VT_LANG_PT) g_choice = VT_LANG_PT;
    else g_choice = VT_LANG_ES;

    kick_store_set_app_lang(vt_lang_code(g_choice));

    vt_io_lock();
    sceIoMkdir(KICK_DATA_DIR, 0777);
    SceUID fd = sceIoOpen(KICK_LANG_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        char c = (char)('0' + (int)g_choice);
        sceIoWrite(fd, &c, 1);
        sceIoClose(fd);
    }
    vt_io_unlock();
    vt_log("lang set: choice=%s effective=%s", vt_lang_code(g_choice),
           vt_lang_code(effective()));
}
