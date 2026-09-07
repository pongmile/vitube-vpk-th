/* ViTube — Google OAuth 2.0 device flow (YouTube on TV client). */
#ifndef VITUBE_YT_OAUTH_H
#define VITUBE_YT_OAUTH_H

#include <stddef.h>

typedef struct {
    char device_code[256];
    char user_code[32];
    char verification_url[128];
    int interval_seconds;
    int expires_in_seconds;
} yt_device_auth_t;

/* Load a stored refresh token from disk. Returns 1 if a login exists. */
int yt_oauth_load(void);
int yt_oauth_logged_in(void);

/* Returns "Bearer xxx" (refreshing the access token when expired) or NULL
 * when logged out / refresh failed. The returned pointer stays valid until
 * the next call. *cancel = 1 aborts the refresh round trip (NULL = none).
 * After a transport-level refresh failure the next 30 s return NULL at once
 * instead of repeating the blocking round trip for every request. */
const char *yt_oauth_authorization(volatile int *cancel);

/* Step 1: request a device + user code. 0 on success. *cancel = 1 aborts. */
int yt_oauth_device_start(yt_device_auth_t *out, volatile int *cancel);
/* Step 2: poll. 1 = still pending, 0 = success (token stored on disk),
 * 2 = signed in but the token could NOT be saved (this session works, the
 * login will be gone after a restart — tell the user), <0 = error, one of
 * the codes below: only EXPIRED and DENIED are final, TRANSIENT is a failed
 * round trip and the caller should simply poll again. */
#define YT_OAUTH_POLL_NOT_PERSISTED 2
#define YT_OAUTH_POLL_TRANSIENT (-1)
#define YT_OAUTH_POLL_EXPIRED (-2)
#define YT_OAUTH_POLL_DENIED (-3)
#define YT_OAUTH_POLL_FAILED (-4)
int yt_oauth_device_poll(const yt_device_auth_t *auth, volatile int *cancel);

/* Asks Google to invalidate the stored refresh token. Blocking (up to the
 * HTTP timeout), so callers should run it behind a spinner; *cancel = 1
 * aborts it. Returns 0 when Google confirmed, <0 otherwise — either way the
 * caller must still call yt_oauth_logout(), which is what actually ends the
 * session locally. */
int yt_oauth_revoke(volatile int *cancel);

/* Ends the session on this device: clears the tokens and deletes oauth.json.
 * Never fails and never touches the network. */
void yt_oauth_logout(void);

#endif
