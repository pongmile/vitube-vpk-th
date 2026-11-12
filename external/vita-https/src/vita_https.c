#include "vita_https.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include <psp2/kernel/threadmgr.h>

#include <curl/curl.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <psp2/kernel/processmgr.h>

#include "ca_bundle.h"
#include "net_runtime.h"

/* With background read-ahead (see the RangeStream comment) this no longer
 * needs to be large for smoothness: the prefetch thread has the entire
 * consumption time of the CURRENT window to fetch the next one, and at the
 * throughput actually observed (~2 MiB in ~1s, i.e. ~2 MiB/s) even a 512 KiB
 * fetch finishes in ~250ms — a huge margin under any realistic window size.
 * Its only remaining job is bounding the INITIAL buffer wait before the
 * first byte can even reach the demuxer, which scales directly with this
 * value, so smaller is better there. (Before read-ahead existed, this was
 * bumped to 2 MiB to reduce — not fix — how often a boundary crossing
 * blocked; that was the wrong lever, since a bigger buffer only spaces
 * stalls out further apart rather than eliminating the blocking fetch
 * itself.) */
#define RANGE_CACHE_SIZE (512 * 1024)

struct VitaHttpsClient {
	char user_agent[128];
	char username[256];
	char password[256];
	long connect_timeout_ms;
	long request_timeout_ms;
	long low_speed_limit;
	long low_speed_time;
	char pinned_public_key[128];
	int allow_untrusted_ca_with_pin;
	void (*log)(const char *line);
	int persistent;      /* see VitaHttpsClientConfig.persistent_connection */
	CURL *perform_handle; /* the reused easy handle, created on first use */
};

/* Background read-ahead state machine (see the comment above range_fetch
 * for why this exists). Only the prefetch thread moves IDLE/READY -> the
 * fetch outcome; only the consumer (range_read, via ensure_window) moves
 * things TO FETCHING (it "requests" a window) and consumes READY. STOP is
 * one-way, set only by range_close. */
enum {
	RANGE_PF_IDLE = 0,
	RANGE_PF_FETCHING,
	RANGE_PF_READY,
	RANGE_PF_STOP
};

typedef struct RangeStream {
	VitaHttpsClient *client;
	char *url;
	volatile int *cancel;
	uint64_t size;
	uint64_t position;
	uint64_t cache_start;
	size_t cache_size;
	unsigned char *cache;
	/* One easy handle reused for every range fetch on this stream. Reusing
	 * the handle lets libcurl keep its own connection alive across calls
	 * (its default per-handle connection cache matches by host/port/proto),
	 * so only the FIRST fetch pays a TCP+TLS handshake — every later 512 KiB
	 * chunk reuses it. Recreating the handle per fetch (the previous
	 * behaviour) forced a fresh handshake on every single chunk: harmless
	 * at low bitrate (a 360p chunk covers ~12s of playback), but at 720p's
	 * higher bitrate a chunk covers only ~4-5s, so a handshake every few
	 * seconds was clearly audible/visible as stutter — confirmed against a
	 * real cached 720p file (831 kbps video) on 2026-08-27.
	 */
	CURL *curl;

	/* Background single-window read-ahead. Even with a reused connection
	 * and a generous cache, every boundary crossing still did a BLOCKING
	 * fetch inline with the read — and the decoder opens two independent
	 * cursors per playback (audio + video demux), each hitting its own
	 * boundary on its own schedule, so stalls were roughly twice as
	 * frequent as a single-cursor design and occasionally overlapped into
	 * a longer combined stall. Confirmed via real device logs on
	 * 2026-08-27: every "ready frames" dip in the app's decoder-status log
	 * lined up exactly with a range_fetch timestamp.
	 *
	 * The prefetch thread fetches the NEXT window into next_cache while
	 * range_read() keeps serving from cache; a fetch takes roughly 1s and
	 * covers ~17s of 720p playback, so by the time the reader reaches the
	 * boundary the data is essentially always already there — a lock+swap
	 * instead of a fresh network round trip. */
	pthread_t pf_thread;
	int pf_started;          /* thread was created; must be stopped+joined */
	pthread_mutex_t pf_lock; /* protects every pf_/next_* field below */
	pthread_cond_t pf_cond;
	int pf_state;             /* RANGE_PF_* */
	uint64_t pf_want_start;   /* window the thread should be fetching */
	uint64_t next_start;
	size_t next_size;
	unsigned char *next_cache;
	CURL *pf_curl;            /* dedicated handle: never touched by the
	                            * consumer, so no cross-thread curl use */
	volatile int pf_cancel;   /* range_close()'s own abort signal for an
	                            * in-flight prefetch — separate from
	                            * `cancel` (app-owned) so closing a stream
	                            * never depends on app state, and never
	                            * blocks for longer than one curl progress
	                            * tick regardless of it. */
} RangeStream;

typedef struct WriteBridge {
	VitaHttpsWriteCallback callback;
	void *opaque;
} WriteBridge;

typedef struct FixedBuffer {
	unsigned char *data;
	size_t size;
	size_t capacity;
} FixedBuffer;

static int s_init_count;

static int curl_error(CURLcode code) {
	if (code == CURLE_PEER_FAILED_VERIFICATION ||
	    code == CURLE_SSL_CACERT_BADFILE)
		return VITA_HTTPS_ERROR_UNTRUSTED_CERTIFICATE;
	if (code == CURLE_SSL_PINNEDPUBKEYNOTMATCH)
		return VITA_HTTPS_ERROR_PIN_MISMATCH;
	return VITA_HTTPS_ERROR_CURL_BASE - (int)code;
}

static int valid_https_url(const char *url) {
	return url && strncmp(url, "https://", 8) == 0 && url[8] != '\0';
}

static size_t discard_write(char *data, size_t size, size_t count, void *opaque) {
	(void)data;
	(void)opaque;
	return size * count;
}

static size_t bridge_write(char *data, size_t size, size_t count, void *opaque) {
	WriteBridge *bridge = (WriteBridge *)opaque;
	size_t bytes = size * count;
	if (!bridge || !bridge->callback) return bytes;
	return bridge->callback(data, bytes, bridge->opaque);
}

static size_t fixed_write(char *data, size_t size, size_t count, void *opaque) {
	FixedBuffer *buffer = (FixedBuffer *)opaque;
	size_t bytes = size * count;
	if (!buffer || bytes > buffer->capacity - buffer->size) return 0;
	memcpy(buffer->data + buffer->size, data, bytes);
	buffer->size += bytes;
	return bytes;
}

static int progress_cancel(void *opaque, curl_off_t down_total,
	                       curl_off_t down_now, curl_off_t up_total,
	                       curl_off_t up_now) {
	(void)down_total;
	(void)down_now;
	(void)up_total;
	(void)up_now;
	volatile int *cancel = (volatile int *)opaque;
	return cancel && *cancel;
}

/* State of the header callback (VitaHttpsRequest.on_response): the status
 * line, Content-Length and Content-Range of the block being received. */
typedef struct HeaderState {
	const VitaHttpsRequest *request;
	long status;
	int64_t content_length;
	char content_range[96];
} HeaderState;

static int header_is(const char *line, size_t len, const char *name) {
	size_t n = strlen(name);
	if (len < n) return 0;
	for (size_t i = 0; i < n; i++) {
		char ch = line[i];
		if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
		if (ch != name[i]) return 0;
	}
	return 1;
}

static size_t header_write(char *data, size_t size, size_t count,
	                       void *opaque) {
	HeaderState *state = (HeaderState *)opaque;
	size_t bytes = size * count;
	size_t len = bytes;
	while (len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n')) len--;
	if (len == 0) {
		/* Blank line: the block is complete. Reset status so a trailer's
		 * closing blank line after the body does not report it twice, and
		 * stay silent for 1xx interim answers. */
		long status = state->status;
		state->status = 0;
		if (status >= 200 && state->request->on_response &&
		    state->request->on_response(state->request->on_response_opaque,
		                                status, state->content_length,
		                                state->content_range))
			return 0;
		return bytes;
	}
	if (len >= 5 && memcmp(data, "HTTP/", 5) == 0) {
		size_t at = 5;
		while (at < len && data[at] != ' ') at++;
		while (at < len && data[at] == ' ') at++;
		state->status = 0;
		while (at < len && data[at] >= '0' && data[at] <= '9')
			state->status = state->status * 10 + (data[at++] - '0');
		state->content_length = -1;
		state->content_range[0] = '\0';
	} else if (header_is(data, len, "content-length:")) {
		size_t at = 15;
		while (at < len && data[at] == ' ') at++;
		int64_t value = 0;
		int digits = 0;
		while (at < len && data[at] >= '0' && data[at] <= '9' &&
		       value < ((int64_t)1 << 56)) {
			value = value * 10 + (data[at++] - '0');
			digits++;
		}
		state->content_length = digits ? value : -1;
	} else if (header_is(data, len, "content-range:")) {
		size_t at = 14;
		while (at < len && data[at] == ' ') at++;
		size_t n = len - at;
		if (n >= sizeof(state->content_range)) n = sizeof(state->content_range) - 1;
		memcpy(state->content_range, data + at, n);
		state->content_range[n] = '\0';
	}
	return bytes;
}

/* Replaces progress_cancel when a request carries an abort callback. */
typedef struct AbortHook {
	volatile int *cancel;
	VitaHttpsAbortCallback check;
	void *opaque;
} AbortHook;

static int hook_progress(void *opaque, curl_off_t down_total,
	                     curl_off_t down_now, curl_off_t up_total,
	                     curl_off_t up_now) {
	(void)down_total;
	(void)down_now;
	(void)up_total;
	(void)up_now;
	AbortHook *hook = (AbortHook *)opaque;
	if (hook->cancel && *hook->cancel) return 1;
	return hook->check && hook->check(hook->opaque) ? 1 : 0;
}

static void apply_common(CURL *curl, const VitaHttpsClient *client,
	                     volatile int *cancel) {
	struct curl_blob ca = {
		.data = (void *)ca_bundle_pem,
		.len = ca_bundle_pem_len,
		.flags = CURL_BLOB_NOCOPY
	};
	curl_easy_setopt(curl, CURLOPT_USERAGENT, client->user_agent);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, client->connect_timeout_ms);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, client->request_timeout_ms);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, client->low_speed_limit);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, client->low_speed_time);
	int pinned_self_signed = client->allow_untrusted_ca_with_pin &&
	                         client->pinned_public_key[0];
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,
	                 pinned_self_signed ? 0L : 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
	curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
	if (client->pinned_public_key[0])
		curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY,
		                 client->pinned_public_key);
	if (client->username[0]) {
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
		curl_easy_setopt(curl, CURLOPT_USERNAME, client->username);
		curl_easy_setopt(curl, CURLOPT_PASSWORD, client->password);
	}
	if (cancel) {
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cancel);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
	}
}

int vita_https_init(void) {
	if (s_init_count > 0) {
		s_init_count++;
		return 0;
	}
	int result = vita_https_net_init();
	if (result < 0) return result;
	CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (code != CURLE_OK) {
		vita_https_net_term();
		return curl_error(code);
	}
	s_init_count = 1;
	return 0;
}

void vita_https_shutdown(void) {
	if (s_init_count <= 0 || --s_init_count > 0) return;
	curl_global_cleanup();
	vita_https_net_term();
}

int vita_https_is_connected(void) {
	return s_init_count > 0 && vita_https_net_is_connected();
}

int vita_https_wifi_signal_percent(int *percent) {
	return s_init_count > 0
	     ? vita_https_net_wifi_signal_percent(percent)
	     : VITA_HTTPS_ERROR_NOT_INITIALIZED;
}

VitaHttpsClient *vita_https_client_create(const VitaHttpsClientConfig *config) {
	if (s_init_count <= 0) return NULL;
	VitaHttpsClient *client = calloc(1, sizeof(*client));
	if (!client) return NULL;
	snprintf(client->user_agent, sizeof(client->user_agent), "%s",
	         config && config->user_agent ? config->user_agent : "vita-https/1.0");
	if (config && config->username)
		snprintf(client->username, sizeof(client->username), "%s", config->username);
	if (config && config->password)
		snprintf(client->password, sizeof(client->password), "%s", config->password);
	if (config && config->pinned_public_key)
		snprintf(client->pinned_public_key,
		         sizeof(client->pinned_public_key), "%s",
		         config->pinned_public_key);
	client->allow_untrusted_ca_with_pin =
		config && config->allow_untrusted_ca_with_pin &&
		client->pinned_public_key[0];
	client->connect_timeout_ms = config && config->connect_timeout_ms > 0
	                           ? config->connect_timeout_ms : 10000;
	client->request_timeout_ms = config && config->request_timeout_ms > 0
	                           ? config->request_timeout_ms : 20000;
	client->low_speed_limit = config && config->low_speed_bytes_per_second > 0
	                         ? config->low_speed_bytes_per_second : 1024;
	client->low_speed_time = config && config->low_speed_seconds > 0
	                        ? config->low_speed_seconds : 15;
	client->log = config ? config->log : NULL;
	client->persistent = config && config->persistent_connection;
	return client;
}

void vita_https_client_destroy(VitaHttpsClient *client) {
	if (!client) return;
	if (client->perform_handle) curl_easy_cleanup(client->perform_handle);
	memset(client->password, 0, sizeof(client->password));
	free(client);
}

static int public_key_pin_from_pem(const char *pem, char *pin,
	                               size_t pin_size) {
	if (!pem || !pin || pin_size < 53) return VITA_HTTPS_ERROR_INVALID_ARGUMENT;
	mbedtls_x509_crt certificate;
	mbedtls_x509_crt_init(&certificate);
	int result = mbedtls_x509_crt_parse(&certificate,
		                              (const unsigned char *)pem,
		                              strlen(pem) + 1);
	if (result < 0) {
		mbedtls_x509_crt_free(&certificate);
		return VITA_HTTPS_ERROR_CERTIFICATE_INFO;
	}
	unsigned char der[2048];
	int der_size = mbedtls_pk_write_pubkey_der(&certificate.pk, der, sizeof(der));
	if (der_size <= 0 || (size_t)der_size > sizeof(der)) {
		mbedtls_x509_crt_free(&certificate);
		return VITA_HTTPS_ERROR_CERTIFICATE_INFO;
	}
	unsigned char digest[32];
	result = mbedtls_sha256(der + sizeof(der) - (size_t)der_size,
	                        (size_t)der_size, digest, 0);
	mbedtls_x509_crt_free(&certificate);
	if (result < 0) return VITA_HTTPS_ERROR_CERTIFICATE_INFO;
	unsigned char encoded[48];
	size_t encoded_size = 0;
	result = mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_size,
	                               digest, sizeof(digest));
	if (result < 0 || encoded_size != 44 || pin_size < 9 + encoded_size)
		return VITA_HTTPS_ERROR_CERTIFICATE_INFO;
	memcpy(pin, "sha256//", 8);
	memcpy(pin + 8, encoded, encoded_size);
	pin[8 + encoded_size] = '\0';
	return 0;
}

int vita_https_probe_public_key(VitaHttpsClient *client, const char *url,
	                            char *pin, size_t pin_size) {
	if (s_init_count <= 0) return VITA_HTTPS_ERROR_NOT_INITIALIZED;
	if (!client || !valid_https_url(url) || !pin || pin_size < 53)
		return VITA_HTTPS_ERROR_INVALID_ARGUMENT;
	pin[0] = '\0';
	CURL *curl = curl_easy_init();
	if (!curl) return VITA_HTTPS_ERROR_OUT_OF_MEMORY;
	apply_common(curl, client, NULL);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, NULL);
	curl_easy_setopt(curl, CURLOPT_CERTINFO, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
	CURLcode code = curl_easy_perform(curl);
	int result = code == CURLE_OK ? VITA_HTTPS_ERROR_CERTIFICATE_INFO
	                             : curl_error(code);
	if (code == CURLE_OK) {
		struct curl_certinfo *info = NULL;
		if (curl_easy_getinfo(curl, CURLINFO_CERTINFO, &info) == CURLE_OK &&
		    info && info->num_of_certs > 0) {
			for (struct curl_slist *item = info->certinfo[0]; item;
			     item = item->next) {
				if (item->data && strncmp(item->data, "Cert:", 5) == 0) {
					result = public_key_pin_from_pem(item->data + 5,
					                                 pin, pin_size);
					break;
				}
			}
		}
	}
	curl_easy_cleanup(curl);
	return result;
}

int vita_https_perform(VitaHttpsClient *client,
	                   const VitaHttpsRequest *request,
	                   VitaHttpsResponse *response) {
	if (s_init_count <= 0) return VITA_HTTPS_ERROR_NOT_INITIALIZED;
	if (!client || !request || !valid_https_url(request->url))
		return VITA_HTTPS_ERROR_INVALID_ARGUMENT;
	CURL *curl;
	if (client->persistent) {
		/* curl_easy_reset() clears every option but keeps the connection
		 * cache, DNS cache and TLS session cache — exactly the state a
		 * fresh handle would have to rebuild with a full handshake. */
		if (!client->perform_handle) client->perform_handle = curl_easy_init();
		curl = client->perform_handle;
		if (curl) curl_easy_reset(curl);
	} else {
		curl = curl_easy_init();
	}
	if (!curl) return VITA_HTTPS_ERROR_OUT_OF_MEMORY;
	apply_common(curl, client, request->cancel_flag);
	curl_easy_setopt(curl, CURLOPT_URL, request->url);
	const char *method = request->method ? request->method : "GET";
	if (strcmp(method, "HEAD") == 0) {
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	} else if (strcmp(method, "GET") != 0) {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	}
	if (request->body) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
		                 (curl_off_t)request->body_size);
	}
	struct curl_slist *headers = NULL;
	for (const char *const *header = request->headers; header && *header; header++)
		headers = curl_slist_append(headers, *header);
	if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	WriteBridge bridge = { request->write, request->write_opaque };
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
	                 request->write ? bridge_write : discard_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA,
	                 request->write ? (void *)&bridge : NULL);
	HeaderState header_state = { request, 0, -1, "" };
	if (request->on_response) {
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_write);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)&header_state);
	}
	AbortHook abort_hook = { request->cancel_flag, request->should_abort,
	                         request->should_abort_opaque };
	if (request->should_abort) {
		/* After apply_common on purpose: this replaces its cancel-only
		 * progress function with one that also asks the caller. */
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, hook_progress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)&abort_hook);
	}
	CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_off_t length = -1;
	char *effective = NULL;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
	if (response) {
		memset(response, 0, sizeof(*response));
		response->status_code = status;
		response->content_length = (int64_t)length;
		if (effective)
			snprintf(response->effective_url, sizeof(response->effective_url),
			         "%s", effective);
	}
	curl_slist_free_all(headers);
	if (client->persistent && code == CURLE_OK && status < 400) {
		/* The header list is freed above; drop the handle's pointer to it
		 * (and to the caller's stack bridge) before the next reuse. */
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
	} else {
		/* After any failure the next request starts from a fresh handle:
		 * a handshake is cheap next to a connection in an odd state, and
		 * it keeps reuse out of the picture when a failure is diagnosed. */
		curl_easy_cleanup(curl);
		if (client->persistent) client->perform_handle = NULL;
	}
	if (code != CURLE_OK) return curl_error(code);
	return status >= 200 && status < 400 ? 0 : VITA_HTTPS_ERROR_HTTP_STATUS;
}

char *vita_https_escape(const char *text, size_t length) {
	if (s_init_count <= 0 || !text || length > INT_MAX) return NULL;
	CURL *curl = curl_easy_init();
	if (!curl) return NULL;
	char *escaped = curl_easy_escape(curl, text, (int)length);
	curl_easy_cleanup(curl);
	return escaped;
}

char *vita_https_unescape(const char *text, size_t *length) {
	if (s_init_count <= 0 || !text) return NULL;
	CURL *curl = curl_easy_init();
	if (!curl) return NULL;
	int decoded_length = 0;
	char *decoded = curl_easy_unescape(curl, text, 0, &decoded_length);
	curl_easy_cleanup(curl);
	if (length) *length = decoded_length > 0 ? (size_t)decoded_length : 0;
	return decoded;
}

void vita_https_free(void *pointer) {
	curl_free(pointer);
}

static int range_fetch(RangeStream *stream, uint64_t start) {
	if (!stream || !stream->curl || start >= stream->size) return 0;
	uint64_t end = start + RANGE_CACHE_SIZE - 1;
	if (end >= stream->size) end = stream->size - 1;
	char range[64];
	snprintf(range, sizeof(range), "%llu-%llu",
	         (unsigned long long)start, (unsigned long long)end);
	/* Reuse the stream's one easy handle (see the RangeStream comment) —
	 * only the Range header and write target change between fetches. */
	curl_easy_setopt(stream->curl, CURLOPT_RANGE, range);
	FixedBuffer buffer = { stream->cache, 0, RANGE_CACHE_SIZE };
	curl_easy_setopt(stream->curl, CURLOPT_WRITEFUNCTION, fixed_write);
	curl_easy_setopt(stream->curl, CURLOPT_WRITEDATA, &buffer);
	uint64_t t0 = sceKernelGetProcessTimeWide();
	CURLcode code = curl_easy_perform(stream->curl);
	uint64_t elapsed_us = sceKernelGetProcessTimeWide() - t0;
	long status = 0;
	curl_easy_getinfo(stream->curl, CURLINFO_RESPONSE_CODE, &status);
	if (stream->client->log) {
		char line[160];
		snprintf(line, sizeof(line),
		         "range_fetch %llu-%llu status=%ld got=%u in %llu ms",
		         (unsigned long long)start, (unsigned long long)end, status,
		         (unsigned)buffer.size,
		         (unsigned long long)(elapsed_us / 1000));
		stream->client->log(line);
	}
	if (code != CURLE_OK) return curl_error(code);
	if (status != 206) return VITA_HTTPS_ERROR_RANGE_UNSUPPORTED;
	stream->cache_start = start;
	stream->cache_size = buffer.size;
	return buffer.size > 0 ? 0 : VITA_HTTPS_ERROR_RANGE_UNSUPPORTED;
}

/* Runs for the whole life of the stream. Waits for the consumer to request
 * a window (pf_want_start, state -> FETCHING), fetches it into next_cache
 * on the dedicated pf_curl handle, publishes it (state -> READY) unless the
 * consumer retargeted mid-fetch (a seek), in which case the result is
 * discarded and the new target picked up immediately — never publishes a
 * window nobody asked for anymore. */
static void *range_prefetch_thread(void *opaque) {
	RangeStream *stream = (RangeStream *)opaque;
	pthread_mutex_lock(&stream->pf_lock);
	for (;;) {
		while (stream->pf_state == RANGE_PF_IDLE ||
		       stream->pf_state == RANGE_PF_READY)
			pthread_cond_wait(&stream->pf_cond, &stream->pf_lock);
		if (stream->pf_state == RANGE_PF_STOP) break;
		uint64_t want = stream->pf_want_start;
		pthread_mutex_unlock(&stream->pf_lock);

		int ok = 0;
		size_t got = 0;
		if (want < stream->size) {
			uint64_t end = want + RANGE_CACHE_SIZE - 1;
			if (end >= stream->size) end = stream->size - 1;
			char range[64];
			snprintf(range, sizeof(range), "%llu-%llu",
			         (unsigned long long)want, (unsigned long long)end);
			curl_easy_setopt(stream->pf_curl, CURLOPT_RANGE, range);
			FixedBuffer buffer = { stream->next_cache, 0, RANGE_CACHE_SIZE };
			curl_easy_setopt(stream->pf_curl, CURLOPT_WRITEFUNCTION, fixed_write);
			curl_easy_setopt(stream->pf_curl, CURLOPT_WRITEDATA, &buffer);
			CURLcode code = curl_easy_perform(stream->pf_curl);
			long status = 0;
			curl_easy_getinfo(stream->pf_curl, CURLINFO_RESPONSE_CODE, &status);
			ok = (code == CURLE_OK && status == 206 && buffer.size > 0);
			got = buffer.size;
			if (stream->client->log) {
				char line[160];
				snprintf(line, sizeof(line),
				         "prefetch %llu-%llu status=%ld got=%u ok=%d",
				         (unsigned long long)want, (unsigned long long)end,
				         status, (unsigned)got, ok);
				stream->client->log(line);
			}
		}

		pthread_mutex_lock(&stream->pf_lock);
		if (stream->pf_state == RANGE_PF_STOP) break;
		/* Only publish if still fetching for the SAME target — a seek may
		 * have retargeted us while curl was in flight. */
		if (stream->pf_state == RANGE_PF_FETCHING &&
		    stream->pf_want_start == want) {
			if (ok) {
				stream->next_start = want;
				stream->next_size = got;
				stream->pf_state = RANGE_PF_READY;
			} else {
				stream->pf_state = RANGE_PF_IDLE;
			}
			pthread_cond_broadcast(&stream->pf_cond);
		}
		/* else: target moved on; loop back around and pick it up. */
	}
	pthread_mutex_unlock(&stream->pf_lock);
	return NULL;
}

/* Always retargets the prefetch thread to `want_start`, replacing whatever
 * it was doing (a stale in-flight fetch is discarded on completion — see
 * range_prefetch_thread). Safe to call redundantly. */
static void kick_prefetch(RangeStream *stream, uint64_t want_start) {
	if (!stream->pf_started) return;
	pthread_mutex_lock(&stream->pf_lock);
	if (stream->pf_state != RANGE_PF_STOP) {
		stream->pf_want_start = want_start;
		stream->pf_state = RANGE_PF_FETCHING;
		pthread_cond_broadcast(&stream->pf_cond);
	}
	pthread_mutex_unlock(&stream->pf_lock);
}

/* Makes sure `start` is covered by stream->cache, preferring an
 * already-in-flight or already-finished background prefetch over a fresh
 * blocking fetch. Returns 0 ok, <0 on error (same codes as range_fetch). */
static int ensure_window(RangeStream *stream, uint64_t start) {
	if (start >= stream->cache_start &&
	    start < stream->cache_start + stream->cache_size)
		return 0;

	if (stream->pf_started) {
		pthread_mutex_lock(&stream->pf_lock);
		if ((stream->pf_state == RANGE_PF_FETCHING ||
		     stream->pf_state == RANGE_PF_READY) &&
		    stream->pf_want_start == start) {
			while (stream->pf_state == RANGE_PF_FETCHING)
				pthread_cond_wait(&stream->pf_cond, &stream->pf_lock);
			if (stream->pf_state == RANGE_PF_READY &&
			    stream->next_start == start) {
				unsigned char *tmp = stream->cache;
				stream->cache = stream->next_cache;
				stream->next_cache = tmp;
				stream->cache_start = stream->next_start;
				stream->cache_size = stream->next_size;
				uint64_t follow = stream->cache_start + stream->cache_size;
				stream->pf_want_start = follow;
				stream->pf_state = RANGE_PF_FETCHING;
				pthread_cond_broadcast(&stream->pf_cond);
				pthread_mutex_unlock(&stream->pf_lock);
				return 0;
			}
			/* The targeted prefetch failed; fall through to a fresh
			 * blocking fetch below. */
		}
		pthread_mutex_unlock(&stream->pf_lock);
	}

	int ret = range_fetch(stream, start);
	/* A Range request is idempotent and the stream is fully usable after a
	 * failed one (position untouched, handle reused), so one transient
	 * failure — a reset mid-transfer, a TLS hiccup, one timed-out refill —
	 * need not end playback: the decoder maps every negative read to EIO and
	 * libavformat latches that. Bounded, short backoff, never across a
	 * cancel, so a dead source still fails within seconds. */
	for (int attempt = 0; ret < 0 && attempt < 2; attempt++) {
		if (stream->cancel && *stream->cancel) break;
		sceKernelDelayThread(300 * 1000);
		if (stream->cancel && *stream->cancel) break;
		ret = range_fetch(stream, start);
	}
	if (ret < 0) return ret;
	kick_prefetch(stream, stream->cache_start + stream->cache_size);
	return 0;
}

static int range_read(void *opaque, void *data, size_t size) {
	RangeStream *stream = (RangeStream *)opaque;
	if (!stream || !data) return VITA_HTTPS_ERROR_INVALID_ARGUMENT;
	if (stream->position >= stream->size) return 0;
	size_t total = 0;
	while (total < size && stream->position < stream->size) {
		if (stream->position < stream->cache_start ||
		    stream->position >= stream->cache_start + stream->cache_size) {
			int result = ensure_window(stream, stream->position);
			if (result < 0) return total ? (int)total : result;
		}
		size_t offset = (size_t)(stream->position - stream->cache_start);
		size_t available = stream->cache_size - offset;
		size_t wanted = size - total;
		if (wanted > available) wanted = available;
		memcpy((unsigned char *)data + total, stream->cache + offset, wanted);
		stream->position += wanted;
		total += wanted;
	}
	return (int)total;
}

static int64_t range_seek(void *opaque, int64_t offset, int origin) {
	RangeStream *stream = (RangeStream *)opaque;
	if (!stream) return VITA_HTTPS_ERROR_INVALID_ARGUMENT;
	int64_t base = origin == SEEK_SET ? 0 :
	               origin == SEEK_CUR ? (int64_t)stream->position :
	               origin == SEEK_END ? (int64_t)stream->size : -1;
	if (base < 0 || offset < -base) return VITA_HTTPS_ERROR_INVALID_ARGUMENT;
	int64_t next = base + offset;
	if (next < 0 || (uint64_t)next > stream->size)
		return VITA_HTTPS_ERROR_INVALID_ARGUMENT;
	stream->position = (uint64_t)next;
	return next;
}

static void range_close(void *opaque) {
	RangeStream *stream = (RangeStream *)opaque;
	if (!stream) return;
	if (stream->pf_started) {
		/* Abort a fetch that may be blocked in curl_easy_perform() right
		 * now — pf_cancel is stream-owned and set only here, so closing
		 * never depends on (or fights over) the app's own cancel flag, and
		 * never blocks longer than one curl progress-callback tick, not
		 * the up-to-30s request timeout. */
		stream->pf_cancel = 1;
		pthread_mutex_lock(&stream->pf_lock);
		stream->pf_state = RANGE_PF_STOP;
		pthread_cond_broadcast(&stream->pf_cond);
		pthread_mutex_unlock(&stream->pf_lock);
		pthread_join(stream->pf_thread, NULL);
		pthread_mutex_destroy(&stream->pf_lock);
		pthread_cond_destroy(&stream->pf_cond);
	}
	if (stream->pf_curl) curl_easy_cleanup(stream->pf_curl);
	if (stream->curl) curl_easy_cleanup(stream->curl);
	free(stream->next_cache);
	free(stream->cache);
	free(stream->url);
	free(stream);
}

int vita_https_open_range_stream(VitaHttpsClient *client, const char *url,
	                             volatile int *cancel_flag,
	                             VitaHttpsStream *out) {
	if (!client || !out || !valid_https_url(url))
		return VITA_HTTPS_ERROR_INVALID_ARGUMENT;
	memset(out, 0, sizeof(*out));
	VitaHttpsResponse head = {0};
	VitaHttpsRequest request = {
		.method = "HEAD", .url = url, .cancel_flag = cancel_flag
	};
	int result = vita_https_perform(client, &request, &head);
	if (result < 0) return result;
	if (head.content_length <= 0) return VITA_HTTPS_ERROR_RANGE_UNSUPPORTED;
	RangeStream *stream = calloc(1, sizeof(*stream));
	if (!stream) return VITA_HTTPS_ERROR_OUT_OF_MEMORY;
	stream->url = strdup(url);
	stream->cache = malloc(RANGE_CACHE_SIZE);
	if (!stream->url || !stream->cache) {
		range_close(stream);
		return VITA_HTTPS_ERROR_OUT_OF_MEMORY;
	}
	stream->client = client;
	stream->cancel = cancel_flag;
	stream->size = (uint64_t)head.content_length;
	/* One easy handle for the whole stream's lifetime (see the RangeStream
	 * comment) — apply_common's options never change between fetches, so
	 * they're set once here rather than on every range_fetch(). */
	stream->curl = curl_easy_init();
	if (!stream->curl) result = VITA_HTTPS_ERROR_OUT_OF_MEMORY;
	else {
		apply_common(stream->curl, client, cancel_flag);
		curl_easy_setopt(stream->curl, CURLOPT_URL, url);
		curl_easy_setopt(stream->curl, CURLOPT_RANGE, "0-0");
		FixedBuffer probe = { stream->cache, 0, 1 };
		curl_easy_setopt(stream->curl, CURLOPT_WRITEFUNCTION, fixed_write);
		curl_easy_setopt(stream->curl, CURLOPT_WRITEDATA, &probe);
		CURLcode code = curl_easy_perform(stream->curl);
		long status = 0;
		curl_easy_getinfo(stream->curl, CURLINFO_RESPONSE_CODE, &status);
		result = code == CURLE_OK && status == 206 && probe.size == 1
		       ? 0 : VITA_HTTPS_ERROR_RANGE_UNSUPPORTED;
		stream->cache_start = 0;
		stream->cache_size = result == 0 ? 1 : 0;
	}
	if (result < 0) {
		range_close(stream);
		return result;
	}
	/* Background read-ahead (see the RangeStream comment) is a performance
	 * optimization, not a correctness requirement: if any piece of its
	 * setup fails, leave pf_started at 0 (its calloc'd default) and fall
	 * back to the plain synchronous-fetch behaviour rather than failing
	 * the whole stream open over it. */
	stream->next_cache = malloc(RANGE_CACHE_SIZE);
	stream->pf_curl = stream->next_cache ? curl_easy_init() : NULL;
	if (stream->pf_curl) {
		apply_common(stream->pf_curl, client, &stream->pf_cancel);
		curl_easy_setopt(stream->pf_curl, CURLOPT_URL, url);
		if (pthread_mutex_init(&stream->pf_lock, NULL) == 0) {
			if (pthread_cond_init(&stream->pf_cond, NULL) == 0) {
				if (pthread_create(&stream->pf_thread, NULL,
				                   range_prefetch_thread, stream) == 0) {
					stream->pf_started = 1;
					kick_prefetch(stream,
					             stream->cache_start + stream->cache_size);
				} else {
					pthread_cond_destroy(&stream->pf_cond);
					pthread_mutex_destroy(&stream->pf_lock);
				}
			} else {
				pthread_mutex_destroy(&stream->pf_lock);
			}
		}
		if (!stream->pf_started) {
			curl_easy_cleanup(stream->pf_curl);
			stream->pf_curl = NULL;
		}
	}
	if (!stream->pf_started) {
		free(stream->next_cache);
		stream->next_cache = NULL;
	}
	out->opaque = stream;
	out->read = range_read;
	out->seek = range_seek;
	out->close = range_close;
	out->size = (int64_t)stream->size;
	return 0;
}

const char *vita_https_error_string(int error) {
	if (error <= VITA_HTTPS_ERROR_CURL_BASE)
		return curl_easy_strerror((CURLcode)(VITA_HTTPS_ERROR_CURL_BASE - error));
	switch (error) {
	case 0: return "success";
	case VITA_HTTPS_ERROR_INVALID_ARGUMENT: return "invalid argument or non-HTTPS URL";
	case VITA_HTTPS_ERROR_NOT_INITIALIZED: return "vita_https_init was not called";
	case VITA_HTTPS_ERROR_OUT_OF_MEMORY: return "out of memory";
	case VITA_HTTPS_ERROR_HTTP_STATUS: return "HTTP request failed";
	case VITA_HTTPS_ERROR_RANGE_UNSUPPORTED: return "verified byte ranges are unavailable";
	case VITA_HTTPS_ERROR_UNTRUSTED_CERTIFICATE: return "TLS certificate is not trusted";
	case VITA_HTTPS_ERROR_PIN_MISMATCH: return "TLS public-key pin mismatch";
	case VITA_HTTPS_ERROR_CERTIFICATE_INFO: return "TLS public-key fingerprint unavailable";
	default: return "Vita network initialization failed";
	}
}
