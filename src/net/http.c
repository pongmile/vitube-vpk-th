/* ViTube — small convenience layer over vita-https. */
#include "net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vita_https.h>

#define HTTP_USER_AGENT "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip"

static VitaHttpsClient *g_client;

void http_buf_free(http_buf_t *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}

static size_t buf_write(const void *data, size_t size, void *opaque) {
    http_buf_t *buf = opaque;
    if (buf->len + size + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 16384;
        while (cap < buf->len + size + 1) cap *= 2;
        char *grown = realloc(buf->data, cap);
        if (!grown) return 0; /* aborts the transfer */
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, data, size);
    buf->len += size;
    buf->data[buf->len] = '\0';
    return size;
}

int http_init(void) {
    int ret = vita_https_init();
    if (ret < 0) return ret;
    VitaHttpsClientConfig config = {
        .user_agent = HTTP_USER_AGENT,
        .connect_timeout_ms = 12000,
        .request_timeout_ms = 45000, /* search responses can be large */
    };
    g_client = vita_https_client_create(&config);
    return g_client ? 0 : -1;
}

void http_shutdown(void) {
    if (g_client) vita_https_client_destroy(g_client);
    g_client = NULL;
    vita_https_shutdown();
}

int http_connected(void) {
    return vita_https_is_connected();
}

static int perform(const char *method, const char *url,
                   const char *content_type, const void *body, size_t body_len,
                   const char *const *extra_headers,
                   volatile int *cancel_flag, http_buf_t *out, long *status) {
    if (out) { out->len = 0; if (out->data) out->data[0] = '\0'; }

    const char *headers[10];
    int n = 0;
    char ct[128];
    if (content_type) {
        snprintf(ct, sizeof ct, "Content-Type: %s", content_type);
        headers[n++] = ct;
    }
    for (const char *const *h = extra_headers; h && *h && n < 9; h++)
        headers[n++] = *h;
    headers[n] = NULL;

    VitaHttpsRequest request = {
        .method = method,
        .url = url,
        .headers = headers,
        .body = body,
        .body_size = body_len,
        .write = out ? buf_write : NULL,
        .write_opaque = out,
        .cancel_flag = cancel_flag,
    };
    VitaHttpsResponse response = {0};
    int ret = vita_https_perform(g_client, &request, &response);
    if (status) *status = response.status_code;
    /* HTTP-level errors (4xx/5xx) still produced a response body. */
    if (ret == VITA_HTTPS_ERROR_HTTP_STATUS && response.status_code > 0)
        return 0;
    return ret;
}

int http_post(const char *url, const char *content_type, const void *body,
              size_t body_len, const char *const *extra_headers,
              volatile int *cancel_flag, http_buf_t *out, long *status) {
    return perform("POST", url, content_type, body, body_len, extra_headers,
                   cancel_flag, out, status);
}

int http_get(const char *url, const char *const *extra_headers,
             volatile int *cancel_flag, http_buf_t *out, long *status) {
    return perform(NULL, url, NULL, NULL, 0, extra_headers, cancel_flag, out,
                   status);
}

char *http_escape(const char *text) {
    char *escaped = vita_https_escape(text, 0);
    if (!escaped) return NULL;
    /* return a plain malloc'd copy so callers can use free() */
    char *copy = strdup(escaped);
    vita_https_free(escaped);
    return copy;
}
