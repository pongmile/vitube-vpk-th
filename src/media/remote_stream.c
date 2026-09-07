/* ViTube — HTTPS range-stream factory for the decoder (googlevideo). */
#include "media/remote_stream.h"

#include <stdio.h>
#include <string.h>

#include <vita_https.h>

#include "log.h"

#define MEDIA_USER_AGENT \
    "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip"

/* Bridges vita_https's generic (non-printf) log callback to vt_log. Left
 * unwired by default: it logs a line per range fetch, several a second on a
 * progressive stream, and each one is a synchronous card write on the
 * decoder's own cursor thread — the cost the diagnostic was meant to measure.
 * Wire it into the config below while chasing a stall on the direct path,
 * where it says whether the stall lines up with a fetch (network-bound) or
 * happens well inside an already-cached window (decoder-side instead). */
__attribute__((unused))
static void https_log_bridge(const char *line) {
    vt_log("%s", line);
}

/* VitaHttpsStream and VtDecoderStreamHandle are field-identical; every
 * factory open() creates a fresh independent range cursor (the decoder
 * opens two per playback, and two more on every seek). */
static int remote_open(void *opaque, VtDecoderStreamHandle *out) {
    VtRemoteStreamFactory *factory = opaque;
    if (!factory || !factory->client || !out) return -1;
    VitaHttpsStream stream;
    int ret = vita_https_open_range_stream(factory->client, factory->url,
                                           factory->cancel_flag, &stream);
    if (ret < 0) return ret;
    /* Define the whole handle, not just the fields this factory uses: the
     * ones it leaves out (fetch/url, which mark a source as an HLS playlist)
     * must read as "not a playlist", never as whatever was on the stack. */
    memset(out, 0, sizeof *out);
    out->opaque = stream.opaque;
    out->read = stream.read;
    out->seek = stream.seek;
    out->close = stream.close;
    out->size = stream.size;
    return 0;
}

int vt_remote_stream_factory_init(VtRemoteStreamFactory *factory,
                                  const char *url,
                                  volatile int *cancel_flag) {
    memset(factory, 0, sizeof *factory);
    snprintf(factory->url, sizeof factory->url, "%s", url);
    VitaHttpsClientConfig config = {
        .user_agent = MEDIA_USER_AGENT,
        .connect_timeout_ms = 10000,
        .request_timeout_ms = 30000, /* caps each range refill */
    };
    factory->client = vita_https_client_create(&config);
    if (!factory->client) return -1;
    factory->cancel_flag = cancel_flag;
    factory->factory.opaque = factory;
    factory->factory.open = remote_open;
    return 0;
}

void vt_remote_stream_factory_term(VtRemoteStreamFactory *factory) {
    if (!factory) return;
    if (factory->client) vita_https_client_destroy(factory->client);
    factory->client = NULL;
}
