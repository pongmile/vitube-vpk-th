/* ViTube — a native YouTube client for the PlayStation Vita.
 * GPL-3.0-only. Built on vita-https and vita-hw-decoder by spyro-98, and
 * on wiliwili's h264_vita FFmpeg backend. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include "app.h"
#include "log.h"
#include "store.h"
#include "media/vita_decoder.h"
#include "net/http.h"
#include "ui/list_screen.h"
#include "ui/strings.h"
#include "ui/thumbs.h"
#include "ui/touch.h"
#include "ui/ui.h"
#include "yt/oauth.h"
#include "yt/region.h"

/* Generous newlib heap: FFmpeg demux state, JSON trees and JPEG decode all
 * live here. ATTRIBUTE2=12 (memory expansion) makes room for it. */
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;

/* Padding that exists only to keep vita-elf-create working.
 *
 * That tool appends ~3.7 KB of SCE metadata to the end of the first LOAD
 * segment and aborts with "segment 1 overlaps" when the gap to the next
 * segment is smaller. The gap is whatever the code size happens to leave
 * below the next segment boundary, so with the default 4 KB pages only about
 * one code size in ten links at all — this project had simply been lucky
 * until an unrelated change left it 88 bytes short.
 *
 * Segments are aligned to 64 KB (see CMakeLists.txt) and this array pushes
 * the read-only segment just past a boundary, so the gap is a full 64 KB
 * rather than a leftover. That is headroom for roughly 60 KB of further code
 * growth; if the link ever fails this way again, enlarge this by 4 KB.
 *
 * It must stay referenced: --gc-sections together with -flto discards an
 * unused array even when it is marked "used", so main() reads a byte of it. */
const volatile char vt_segment_pad[4096] = { 1 };
/* Video lists live in FetchTask structs on the main thread's stack. */
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;

int main(void) {
    if (vt_segment_pad[0] == 0) return 0; /* keeps the padding linked */
    sceIoMkdir(VITUBE_DATA_DIR, 0777);
    vt_log_init();
    vt_log("ViTube " VITUBE_VERSION " start");

    if (vt_ui_init() < 0) {
        sceKernelExitProcess(0);
        return 0;
    }
    ui_touch_init();

    /* Before any request: every InnerTube context carries hl/gl, and asking
     * the memory card for them mid-request is not an option. */
    yt_region_load();
    /* After the region: VT_LANG_AUTO resolves against it. */
    vt_lang_load();
    vt_store_load();

    int network_ready = http_init() == 0;
    if (network_ready) {
        yt_oauth_load();
        vt_thumbs_init();
        /* preload reAvPlayer.suprx while the user is still browsing */
        vt_decoder_prepare_hardware_runtime();
        if (!http_connected())
            vt_ui_toast(vt_str(S_NO_NETWORK), vt_str(S_CONNECT_WIFI), 180);
        list_screen_run();
        vt_thumbs_term();
    } else {
        vt_ui_toast(vt_str(S_NET_INIT_FAILED), NULL, 240);
    }

    http_shutdown();
    ui_touch_term();
    vt_ui_term();
    sceKernelExitProcess(0);
    return 0;
}
