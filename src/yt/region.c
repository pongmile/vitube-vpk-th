/* ViTube — stored hl/gl choice. See region.h. */
#include "yt/region.h"

#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/system_param.h>

#include "app.h"
#include "log.h"

/* Deliberately short. This is a handheld with a d-pad, and the list only has
 * to cover "the language I read" — YouTube itself falls back sensibly for
 * everything the pair does not pin down. */
static const yt_region_t g_regions[] = {
    { "Deutschland",       "de", "DE" },
    { "\xc3\x96sterreich", "de", "AT" },
    { "Schweiz",           "de", "CH" },
    { "USA",               "en", "US" },
    /* Split literal: a 'b' after \x9f would be read as part of the escape. */
    { "Gro\xc3\x9f" "britannien", "en", "GB" },
    { "France",            "fr", "FR" },
    { "Espa\xc3\xb1\x61",  "es", "ES" },
    { "Italia",            "it", "IT" },
};
#define REGION_COUNT ((int)(sizeof g_regions / sizeof *g_regions))

static int g_index; /* 0 until yt_region_load() says otherwise */

int yt_region_count(void) { return REGION_COUNT; }

const yt_region_t *yt_region_at(int index) {
    if (index < 0 || index >= REGION_COUNT) index = 0;
    return &g_regions[index];
}

/* First run, no stored choice: follow the console. A German console starts
 * in Deutschland; everything else starts in USA/en, so the chrome, the
 * toasts and the content agree (use_english() derives the UI language from
 * hl). fr/es/it stay an explicit pick — age_minutes() cannot parse their
 * date words yet, and auto-selecting them would push a broken newest-first
 * sort onto users who never asked for it. Every failure falls through to
 * en/US: a locale probe must never be able to break a launch. */
static int region_from_console(void) {
    int fallback = 3; /* USA / en */
    SceAppUtilInitParam init = {0};
    SceAppUtilBootParam boot = {0};
    int init_ret = sceAppUtilInit(&init, &boot);
    int lang = -1;
    int get_ret = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    if (init_ret == 0) sceAppUtilShutdown();
    if (get_ret < 0) {
        vt_log("region: system language unavailable (init=0x%08X get=0x%08X)",
               (unsigned)init_ret, (unsigned)get_ret);
        return fallback;
    }
    vt_log("region: console language %d", lang);
    return lang == SCE_SYSTEM_PARAM_LANG_GERMAN ? 0 : fallback;
}

void yt_region_load(void) {
    vt_io_lock();
    SceUID fd = sceIoOpen(VITUBE_REGION_PATH, SCE_O_RDONLY, 0);
    char c = 0;
    int have_file = fd >= 0;
    if (have_file) {
        if (sceIoRead(fd, &c, 1) != 1) have_file = 0;
        sceIoClose(fd);
    }
    vt_io_unlock();
    int index = have_file ? c - '0' : region_from_console();
    g_index = (index >= 0 && index < REGION_COUNT) ? index : 3;
    vt_log("region: %s (%s/%s)%s", g_regions[g_index].label,
           g_regions[g_index].hl, g_regions[g_index].gl,
           have_file ? "" : " (from the console, not stored)");
}

int yt_region_index(void) { return g_index; }

void yt_region_set_index(int index) {
    if (index < 0 || index >= REGION_COUNT) return;
    g_index = index;
    /* Single digit, same shape as hd.flag — and the same reason the mode is
     * 0777: a file written 0600 in this directory did not survive a relaunch
     * (see store_refresh_token in oauth.c). */
    vt_io_lock();
    sceIoMkdir(VITUBE_DATA_DIR, 0777);
    SceUID fd = sceIoOpen(VITUBE_REGION_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        char c = (char)('0' + index);
        sceIoWrite(fd, &c, 1);
        sceIoClose(fd);
    }
    vt_io_unlock();
    vt_log("region set: %s (%s/%s)", g_regions[index].label,
           g_regions[index].hl, g_regions[index].gl);
}

const char *yt_region_hl(void) { return g_regions[g_index].hl; }
const char *yt_region_gl(void) { return g_regions[g_index].gl; }
