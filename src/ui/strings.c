/* ViTube — which language the catalogue answers in, and where that choice is
 * stored. The catalogue itself is in strings_table.c and strings_<lang>.c. */
#include "ui/strings.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "app.h"
#include "log.h"
#include "yt/region.h"

static VtLangChoice g_choice = VT_LANG_AUTO;

/* AUTO follows the region, because a build answering with English content and
 * German chrome is exactly the inconsistency the region setting introduced.
 * Every language the catalogue speaks is matched on the hl's language part
 * ("pt" and "pt-PT" both read Portuguese); anything else falls back to
 * English, the one language every remaining region can read. */
static VtLangChoice effective(void) {
    if (g_choice != VT_LANG_AUTO) return g_choice;
    static const struct { char a, b; VtLangChoice lang; } by_hl[] = {
        { 'd', 'e', VT_LANG_DE }, { 'e', 'n', VT_LANG_EN },
        { 'e', 's', VT_LANG_ES }, { 'p', 't', VT_LANG_PT },
        { 'i', 't', VT_LANG_IT }, { 'f', 'r', VT_LANG_FR },
    };
    const char *hl = yt_region_hl();
    for (unsigned i = 0; i < sizeof by_hl / sizeof *by_hl; i++)
        if (hl[0] == by_hl[i].a && hl[1] == by_hl[i].b) return by_hl[i].lang;
    return VT_LANG_EN;
}

const char *vt_str(VtStringId id) {
    const char *s = vt_str_in(effective(), id);
    /* A missing translation falls back to English, then to the German
     * original, rather than to nothing: blank UI is a worse failure than
     * untranslated UI, and tests/test_strings.c catches the gap long before a
     * user does. */
    if (!s) s = vt_str_in(VT_LANG_EN, id);
    if (!s) s = vt_str_in(VT_LANG_DE, id);
    return s ? s : "";
}

/* Each language's own name, the one convention every user can find their
 * language under — unlike the current UI language's word for it. */
const char *vt_lang_label(VtLangChoice choice) {
    switch (choice) {
    case VT_LANG_DE: return "Deutsch";
    case VT_LANG_EN: return "English";
    case VT_LANG_ES: return "Espa\xc3\xb1ol";
    case VT_LANG_PT: return "Portugu\xc3\xaas";
    case VT_LANG_IT: return "Italiano";
    case VT_LANG_FR: return "Fran\xc3\xa7\x61is"; /* \x61 = 'a': it would
                                                     * otherwise extend \xa7 */
    default:         return vt_str(S_LANG_AUTOMATIC);
    }
}

const char *vt_lang_effective_label(void) {
    return vt_lang_label(effective());
}

VtLangChoice vt_lang_choice(void) { return g_choice; }

void vt_lang_load(void) {
    vt_io_lock();
    SceUID fd = sceIoOpen(VITUBE_LANG_PATH, SCE_O_RDONLY, 0);
    char c = '0';
    if (fd >= 0) {
        sceIoRead(fd, &c, 1);
        sceIoClose(fd);
    }
    vt_io_unlock();
    int v = c - '0';
    g_choice = (v > 0 && v < VT_LANG_CHOICE_COUNT) ? (VtLangChoice)v
                                                   : VT_LANG_AUTO;
    vt_log("lang: choice=%s effective=%s", vt_lang_code(g_choice),
           vt_lang_code(effective()));
}

void vt_lang_set_choice(VtLangChoice choice) {
    if (choice < 0 || choice >= VT_LANG_CHOICE_COUNT) return;
    g_choice = choice;
    /* 0777 for the same reason as oauth.json and hd.flag: a file written 0600
     * in this directory did not survive a relaunch. */
    vt_io_lock();
    sceIoMkdir(VITUBE_DATA_DIR, 0777);
    SceUID fd = sceIoOpen(VITUBE_LANG_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        char c = (char)('0' + (int)choice);
        sceIoWrite(fd, &c, 1);
        sceIoClose(fd);
    }
    vt_io_unlock();
    vt_log("lang set: choice=%s effective=%s", vt_lang_code(choice),
           vt_lang_code(effective()));
}
