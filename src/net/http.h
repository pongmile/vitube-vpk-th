/* ViTube — small convenience layer over vita-https. */
#ifndef VITUBE_NET_HTTP_H
#define VITUBE_NET_HTTP_H

#include <stddef.h>

typedef struct {
    char *data;   /* NUL-terminated */
    size_t len;
    size_t cap;
} http_buf_t;

void http_buf_free(http_buf_t *buf);

/* Global API client (User-Agent of the YouTube Android app). Returns 0. */
int http_init(void);
void http_shutdown(void);
int http_connected(void);

/* Running count of requests that failed below HTTP — DNS, connect, TLS,
 * timeout — on any thread; cancelled requests are not counted. A caller
 * compares it before and after an attempt to tell "the network failed" from
 * "YouTube answered with an error", which the yt layer folds into one -1.
 * Shared by every thread, so a thumbnail failing at the same moment counts
 * too; that only ever errs towards one more retry. */
unsigned http_transport_failures(void);

/* All calls return 0 on success (any HTTP status), <0 on transport error.
 * *status receives the HTTP status code. extra_headers is a NULL-terminated
 * list of "Name: value" strings, may be NULL. Setting *cancel_flag = 1 from
 * another thread aborts the transfer (may be NULL). */
int http_post(const char *url, const char *content_type, const void *body,
              size_t body_len, const char *const *extra_headers,
              volatile int *cancel_flag, http_buf_t *out, long *status);
int http_get(const char *url, const char *const *extra_headers,
             volatile int *cancel_flag, http_buf_t *out, long *status);

/* URL-escape helper (returns malloc'd string; caller frees with free()). */
char *http_escape(const char *text);

#endif
