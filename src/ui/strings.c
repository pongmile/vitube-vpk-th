/* ViTube — which language the catalogue answers in, and where that choice is
 * stored. The catalogue itself is in strings_table.c. */
#include "ui/strings.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "app.h"
#include "log.h"
#include "yt/region.h"

static VtLangChoice g_choice = VT_LANG_AUTO;

/* AUTO follows the region, because a build answering with English content and
 * German chrome is exactly the inconsistency the region setting introduced.
 * Anything that is not a German-speaking region falls back to English rather
 * than to its own language: those translations do not exist, and English is
 * the one every one of those regions can read. */
static int use_english(void) {
    if (g_choice == VT_LANG_DE) return 0;
    if (g_choice == VT_LANG_EN) return 1;
    const char *hl = yt_region_hl();
    return !(hl[0] == 'd' && hl[1] == 'e');
}

const char *vt_str(VtStringId id) {
    const char *s = use_english() ? vt_str_en(id) : vt_str_de(id);
    /* A missing translation falls back to the other language rather than to
     * nothing: blank UI is a worse failure than untranslated UI, and
     * tests/test_strings.c catches the gap long before a user does. */
    if (!s) s = use_english() ? vt_str_de(id) : vt_str_en(id);
    return s ? s : "";
}

const char *vt_lang_label(VtLangChoice choice) {
    if (choice == VT_LANG_DE) return "Deutsch";
    if (choice == VT_LANG_EN) return "English";
    return vt_str(S_LANG_AUTOMATIC);
}

const char *vt_lang_effective_label(void) {
    return use_english() ? "English" : "Deutsch";
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
    vt_log("lang: choice=%d effective=%s", (int)g_choice,
           use_english() ? "en" : "de");
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
    vt_log("lang set: choice=%d effective=%s", (int)choice,
           use_english() ? "en" : "de");
}
