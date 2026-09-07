/* ViTube — HTTPS range-stream factory for the decoder (googlevideo). */
#ifndef VITUBE_MEDIA_REMOTE_STREAM_H
#define VITUBE_MEDIA_REMOTE_STREAM_H

#include "media/vita_decoder.h"

typedef struct VtRemoteStreamFactory {
    char url[4096];
    struct VitaHttpsClient *client; /* owns one client per playback */
    volatile int *cancel_flag;
    VtDecoderStreamFactory factory; /* pass .factory to the decoder */
} VtRemoteStreamFactory;

/* Creates the HTTPS client. The factory (and its client) must outlive the
 * whole playback including all seeks. Returns 0 on success. */
int vt_remote_stream_factory_init(VtRemoteStreamFactory *factory,
                                  const char *url, volatile int *cancel_flag);
/* Destroy after the decoder player is destroyed. */
void vt_remote_stream_factory_term(VtRemoteStreamFactory *factory);

#endif
