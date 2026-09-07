/* ViTube — Google OAuth 2.0 device flow (YouTube on TV client).
 * The client id/secret pair below is the public identity embedded in every
 * "YouTube on TV" device; the device flow shows a code the user confirms on
 * their phone at google.com/device. */
#include "yt/oauth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <jansson.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "app.h"
#include "log.h"
#include "net/http.h"

#define OAUTH_CLIENT_ID \
    "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com"
#define OAUTH_CLIENT_SECRET "SboVhoG9s0rNafixCSGGKXAT"
/* InnerTube only honors tokens carrying the legacy TV scope (gdata),
 * like real YouTube-on-TV devices request. URL-encoded form value. */
#define OAUTH_SCOPE \
    "http%3A%2F%2Fgdata.youtube.com%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fyoutube"
#define OAUTH_DEVICE_URL "https://oauth2.googleapis.com/device/code"
#define OAUTH_TOKEN_URL "https://oauth2.googleapis.com/token"
#define OAUTH_REVOKE_URL "https://oauth2.googleapis.com/revoke"

static char g_refresh_token[512];
static char g_access_token[2048];
static char g_authorization[2112];
static time_t g_access_expires;

/* Returns 0 when the token is safely on disk, -1 otherwise. The caller must
 * not report a successful login on -1: the session still works (the access
 * token is in memory), but the login will be gone after the next restart —
 * exactly the silent failure that made the 0600 bug below so hard to pin. */
static int store_refresh_token(void) {
    json_t *root = json_pack("{ss}", "refresh_token", g_refresh_token);
    char *text = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!text) {
        vt_log("oauth store: json_dumps failed");
        return -1;
    }
    size_t len = strlen(text);
    /* Token refresh can run on a worker thread mid-playback — all ux0: file
     * access must hold the app-wide I/O lock (see log.c). That lock is NOT
     * recursive and vt_log() takes it too, so nothing between lock and unlock
     * may log — doing so deadlocks the thread against itself. */
    vt_io_lock();
    int mk = sceIoMkdir(VITUBE_DATA_DIR, 0777);
    /* The mode MUST be 0777. Until 2026-08-28 this file was created with 0600
     * and the login vanished on every single relaunch; changing nothing but
     * the mode fixed it, confirmed on hardware over two consecutive relaunches
     * (log: "oauth load: read 123 bytes" + a personalized feed instead of the
     * anonymous one). log.c and hd_helper.c write to the same directory with
     * 0777 and never lost a file. The exact platform mechanism is NOT
     * established — we never captured a log from a 0600 build, so whether the
     * write or the read-back was failing is unknown. Do not "clean up" this
     * mode on the theory that 0600 ought to work. */
    SceUID fd = sceIoOpen(VITUBE_OAUTH_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    int wrote = -1, syncfd = -1, close_ret = -1, sync = -1;
    if (fd >= 0) {
        wrote = sceIoWrite(fd, text, len);
        /* Not the cause of the bug above (log.txt persists with no sync at
         * all), but this is the one file whose loss the user actually
         * notices, so it is flushed explicitly rather than left to the cache. */
        syncfd = sceIoSyncByFd(fd, 0);
        close_ret = sceIoClose(fd);
        sync = sceIoSync("ux0:", 0);
    }
    vt_io_unlock();
    int ok = (fd >= 0 && wrote == (int)len);
    if (!ok)
        vt_log("oauth store FAILED: mkdir=%#x fd=%#x wrote=%d/%d syncfd=%#x "
               "close=%#x sync=%#x",
               mk, fd, wrote, (int)len, syncfd, close_ret, sync);
    else
        vt_log("oauth store: saved %d bytes", wrote);
    free(text);
    return ok ? 0 : -1;
}

int yt_oauth_load(void) {
    g_refresh_token[0] = '\0';
    g_access_token[0] = '\0';
    g_access_expires = 0;
    vt_io_lock();
    SceUID fd = sceIoOpen(VITUBE_OAUTH_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        vt_io_unlock();
        /* Also the normal state before the very first login. */
        vt_log("oauth load: no stored login (fd=%#x)", fd);
        return 0;
    }
    char raw[1024];
    int n = sceIoRead(fd, raw, sizeof raw - 1);
    sceIoClose(fd);
    vt_io_unlock();
    vt_log("oauth load: read %d bytes", n);
    if (n <= 0) return 0;
    raw[n] = '\0';
    json_error_t err;
    json_t *root = json_loads(raw, 0, &err);
    if (!root) {
        vt_log("oauth load: json parse failed: %s", err.text);
        return 0;
    }
    const char *token = json_string_value(json_object_get(root, "refresh_token"));
    if (!token) vt_log("oauth load: no refresh_token field in json");
    if (token) snprintf(g_refresh_token, sizeof g_refresh_token, "%s", token);
    json_decref(root);
    return g_refresh_token[0] != '\0';
}

int yt_oauth_logged_in(void) {
    return g_refresh_token[0] != '\0';
}

/* POST application/x-www-form-urlencoded to the token endpoint. */
static json_t *token_request(const char *form, volatile int *cancel) {
    http_buf_t buf = {0};
    long status = 0;
    int ret = http_post(OAUTH_TOKEN_URL, "application/x-www-form-urlencoded",
                        form, strlen(form), NULL, cancel, &buf, &status);
    if (ret < 0 || !buf.data) {
        http_buf_free(&buf);
        return NULL;
    }
    json_error_t err;
    json_t *root = json_loads(buf.data, 0, &err);
    http_buf_free(&buf);
    return root;
}

/* Negative cache for a refresh that failed at the transport level: without
 * it every personalised request re-ran the blocking round trip (up to the
 * full connect timeout, twice per feed with the client fallback). */
static time_t g_refresh_failed_until;

static int refresh_access_token(volatile int *cancel) {
    if (g_refresh_failed_until && time(NULL) < g_refresh_failed_until)
        return -1;
    char form[1024];
    snprintf(form, sizeof form,
             "client_id=%s&client_secret=%s&refresh_token=%s"
             "&grant_type=refresh_token",
             OAUTH_CLIENT_ID, OAUTH_CLIENT_SECRET, g_refresh_token);
    json_t *root = token_request(form, cancel);
    if (!root) {
        if (!(cancel && *cancel)) g_refresh_failed_until = time(NULL) + 30;
        return -1;
    }
    g_refresh_failed_until = 0;
    const char *access = json_string_value(json_object_get(root, "access_token"));
    int expires = (int)json_integer_value(json_object_get(root, "expires_in"));
    int ok = -1;
    if (access) {
        snprintf(g_access_token, sizeof g_access_token, "%s", access);
        g_access_expires = time(NULL) + (expires > 120 ? expires - 60 : 60);
        ok = 0;
    } else {
        const char *error = json_string_value(json_object_get(root, "error"));
        /* Google always sends error_description alongside error; logging only
         * a generic "failed" here made an invalid_grant (which silently wipes
         * the stored login below) indistinguishable from a network hiccup. */
        const char *desc =
            json_string_value(json_object_get(root, "error_description"));
        vt_log("oauth refresh failed: error=%s desc=%s",
               error ? error : "(none)", desc ? desc : "(none)");
        if (error && strcmp(error, "invalid_grant") == 0) {
            /* Refresh token revoked — force logout. */
            yt_oauth_logout();
        }
    }
    json_decref(root);
    return ok;
}

const char *yt_oauth_authorization(volatile int *cancel) {
    if (!g_refresh_token[0]) return NULL;
    if (!g_access_token[0] || time(NULL) >= g_access_expires) {
        if (refresh_access_token(cancel) < 0) return NULL;
    }
    snprintf(g_authorization, sizeof g_authorization,
             "Authorization: Bearer %s", g_access_token);
    return g_authorization;
}

int yt_oauth_device_start(yt_device_auth_t *out, volatile int *cancel) {
    memset(out, 0, sizeof *out);
    char form[512];
    snprintf(form, sizeof form, "client_id=%s&scope=%s",
             OAUTH_CLIENT_ID, OAUTH_SCOPE);
    http_buf_t buf = {0};
    long status = 0;
    int ret = http_post(OAUTH_DEVICE_URL, "application/x-www-form-urlencoded",
                        form, strlen(form), NULL, cancel, &buf, &status);
    if (ret < 0 || !buf.data || status != 200) {
        http_buf_free(&buf);
        return -1;
    }
    json_error_t err;
    json_t *root = json_loads(buf.data, 0, &err);
    http_buf_free(&buf);
    if (!root) return -1;
    const char *device = json_string_value(json_object_get(root, "device_code"));
    const char *user = json_string_value(json_object_get(root, "user_code"));
    const char *url = json_string_value(json_object_get(root, "verification_url"));
    if (device && user) {
        snprintf(out->device_code, sizeof out->device_code, "%s", device);
        snprintf(out->user_code, sizeof out->user_code, "%s", user);
        snprintf(out->verification_url, sizeof out->verification_url, "%s",
                 url ? url : "google.com/device");
        out->interval_seconds =
            (int)json_integer_value(json_object_get(root, "interval"));
        if (out->interval_seconds < 1) out->interval_seconds = 5;
        out->expires_in_seconds =
            (int)json_integer_value(json_object_get(root, "expires_in"));
        if (out->expires_in_seconds < 1) out->expires_in_seconds = 1800;
    }
    json_decref(root);
    vt_log("oauth device_start: %s", out->device_code[0] ? "ok" : "failed");
    return out->device_code[0] ? 0 : -1;
}

int yt_oauth_device_poll(const yt_device_auth_t *auth, volatile int *cancel) {
    char form[1024];
    snprintf(form, sizeof form,
             "client_id=%s&client_secret=%s&device_code=%s"
             "&grant_type=urn:ietf:params:oauth:grant-type:device_code",
             OAUTH_CLIENT_ID, OAUTH_CLIENT_SECRET, auth->device_code);
    json_t *root = token_request(form, cancel);
    /* No parsable answer: a dropped request, a timeout, a 5xx — the device
     * code is still valid for many minutes, so this is a poll to repeat, not
     * a failed sign-in. */
    if (!root) return YT_OAUTH_POLL_TRANSIENT;
    const char *access = json_string_value(json_object_get(root, "access_token"));
    const char *refresh = json_string_value(json_object_get(root, "refresh_token"));
    const char *error = json_string_value(json_object_get(root, "error"));
    int result;
    if (access && refresh) {
        snprintf(g_refresh_token, sizeof g_refresh_token, "%s", refresh);
        snprintf(g_access_token, sizeof g_access_token, "%s", access);
        int expires = (int)json_integer_value(json_object_get(root, "expires_in"));
        g_access_expires = time(NULL) + (expires > 120 ? expires - 60 : 60);
        result = store_refresh_token() == 0 ? 0 : YT_OAUTH_POLL_NOT_PERSISTED;
    } else if (error && (strcmp(error, "authorization_pending") == 0 ||
                         strcmp(error, "slow_down") == 0)) {
        result = 1;
    } else if (error && strcmp(error, "expired_token") == 0) {
        result = YT_OAUTH_POLL_EXPIRED;
    } else if (error && strcmp(error, "access_denied") == 0) {
        result = YT_OAUTH_POLL_DENIED;
    } else {
        vt_log("oauth device_poll: error=%s", error ? error : "(none)");
        result = YT_OAUTH_POLL_FAILED;
    }
    json_decref(root);
    return result;
}

/* Deleting only the local file leaves the grant standing in the account's
 * third-party app list — from Google's side the Vita stays signed in, which
 * matters for a handheld that gets lent out or sold. Kept separate from
 * yt_oauth_logout() so that a failed or cancelled revoke still ends the
 * session on this device. */
int yt_oauth_revoke(volatile int *cancel) {
    if (!g_refresh_token[0]) return 0;
    char *escaped = http_escape(g_refresh_token);
    /* A refresh token can contain '/' and '+', which a form value must not
     * carry unescaped. sizeof g_refresh_token * 3 covers worst-case %XX
     * expansion of every byte. */
    char form[sizeof g_refresh_token * 3 + 16];
    int n = snprintf(form, sizeof form, "token=%s",
                     escaped ? escaped : g_refresh_token);
    free(escaped);
    if (n <= 0 || n >= (int)sizeof form) {
        vt_log("oauth revoke: form too long (%d)", n);
        return -1;
    }
    http_buf_t buf = {0};
    long status = 0;
    int ret = http_post(OAUTH_REVOKE_URL, "application/x-www-form-urlencoded",
                        form, strlen(form), NULL, cancel, &buf, &status);
    http_buf_free(&buf);
    vt_log("oauth revoke: ret=%d status=%ld", ret, status);
    return (ret == 0 && status == 200) ? 0 : -1;
}

void yt_oauth_logout(void) {
    g_refresh_token[0] = '\0';
    g_access_token[0] = '\0';
    g_access_expires = 0;
    vt_io_lock();
    sceIoRemove(VITUBE_OAUTH_PATH);
    vt_io_unlock();
}
