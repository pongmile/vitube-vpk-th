/* Host stub of external/vita-https's public header — only the subset
 * src/media/hls.c uses, with the same shapes. The test provides the bodies
 * (see tests/test_hls.c) so the playlist and segment logic can be exercised
 * on the build machine against a scripted server. */
#ifndef VITUBE_TEST_STUB_VITA_HTTPS_H
#define VITUBE_TEST_STUB_VITA_HTTPS_H

#include <stddef.h>
#include <stdint.h>

typedef struct VitaHttpsClient VitaHttpsClient;

typedef struct VitaHttpsClientConfig {
    int persistent_connection;
    const char *user_agent;
    long connect_timeout_ms;
    long request_timeout_ms;
    void (*log)(const char *line);
} VitaHttpsClientConfig;

typedef size_t (*VitaHttpsWriteCallback)(const void *data, size_t size,
                                         void *opaque);

typedef struct VitaHttpsRequest {
    const char *method;
    const char *url;
    const char *const *headers;
    const void *body;
    size_t body_size;
    VitaHttpsWriteCallback write;
    void *write_opaque;
    volatile int *cancel_flag;
} VitaHttpsRequest;

typedef struct VitaHttpsResponse {
    long status_code;
    int64_t content_length;
    char effective_url[2048];
} VitaHttpsResponse;

VitaHttpsClient *vita_https_client_create(const VitaHttpsClientConfig *config);
void vita_https_client_destroy(VitaHttpsClient *client);
int vita_https_perform(VitaHttpsClient *client,
                       const VitaHttpsRequest *request,
                       VitaHttpsResponse *response);

#endif
