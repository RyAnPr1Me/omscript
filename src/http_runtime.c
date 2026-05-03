// http_runtime.c — Secure HTTP client runtime for OmScript.
// Uses libcurl with full SSL certificate verification, host verification,
// and per-request timeout enforcement.
//
// Compile with: -lcurl
// Requires libcurl 7.62+ (for CURLOPT_PROTOCOLS_STR / CURLOPT_REDIR_PROTOCOLS_STR;
// falls back gracefully on older versions via CURLOPT_PROTOCOLS bitmask).

#include "http_runtime.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

typedef struct {
    char*  data;
    size_t size;
    size_t capacity;
} HttpBuf;

static void httpbuf_init(HttpBuf* b) {
    b->data = (char*)malloc(4096);
    if (b->data) {
        b->data[0] = '\0';
        b->size = 0;
        b->capacity = b->data ? 4096 : 0;
    }
}

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t incoming = size * nmemb;
    HttpBuf* b = (HttpBuf*)userdata;
    if (!b->data) return incoming; // silently discard if allocation failed

    if (b->size + incoming + 1 > b->capacity) {
        size_t newcap = b->capacity * 2 + incoming + 1;
        char* tmp = (char*)realloc(b->data, newcap);
        if (!tmp) return 0; // signal error to libcurl
        b->data = tmp;
        b->capacity = newcap;
    }
    memcpy(b->data + b->size, ptr, incoming);
    b->size += incoming;
    b->data[b->size] = '\0';
    return incoming;
}

// Returns a malloc'd empty string (never NULL).
static char* empty_string(void) {
    char* s = (char*)malloc(1);
    if (s) s[0] = '\0';
    return s ? s : NULL; // caller must handle NULL defensively
}

// Common CURL setup: SSL verification ON, timeouts, User-Agent.
static void setup_common(CURL* curl, HttpBuf* buf) {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);

    // SSL: verify peer certificate and host name (both ON by default; set explicitly).
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // Follow redirects but stay within http/https.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    // Restrict to http and https only (no file://, ftp://, etc.).
#if LIBCURL_VERSION_NUM >= 0x074e00  // 7.78.0
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    // Timeouts (connect: 10 s, total transfer: 30 s).
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    // Identify ourselves.
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "omscript-http/1.0");

    // Do not emit progress meter.
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
}

// Finalise response: steal the buffer or return empty string.
static char* finalise(HttpBuf* buf, CURLcode rc) {
    if (rc != CURLE_OK || !buf->data) {
        free(buf->data);
        return empty_string();
    }
    return buf->data; // transfer ownership to caller
}

// Parse "Name: Value\nName2: Value2\n..." into a curl_slist.
static struct curl_slist* parse_headers(const char* headers_str) {
    if (!headers_str || headers_str[0] == '\0') return NULL;
    struct curl_slist* list = NULL;
    const char* p = headers_str;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            char* hdr = (char*)malloc(len + 1);
            if (hdr) {
                memcpy(hdr, p, len);
                hdr[len] = '\0';
                struct curl_slist* tmp = curl_slist_append(list, hdr);
                if (tmp) list = tmp;
                free(hdr);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return list;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

char* omsc_http_get(const char* url) {
    if (!url) return empty_string();
    CURL* curl = curl_easy_init();
    if (!curl) return empty_string();

    HttpBuf buf;
    httpbuf_init(&buf);

    setup_common(curl, &buf);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return finalise(&buf, rc);
}

char* omsc_http_post(const char* url, const char* body, const char* content_type) {
    if (!url) return empty_string();
    CURL* curl = curl_easy_init();
    if (!curl) return empty_string();

    HttpBuf buf;
    httpbuf_init(&buf);

    setup_common(curl, &buf);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    const char* safe_body = body ? body : "";
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, safe_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(safe_body));

    // Content-Type header.
    const char* ct = content_type && content_type[0] ? content_type
                                                      : "application/octet-stream";
    size_t ct_hdr_len = strlen(ct) + sizeof("Content-Type: ");
    char* ct_hdr = (char*)malloc(ct_hdr_len);
    if (ct_hdr) {
        snprintf(ct_hdr, ct_hdr_len, "Content-Type: %s", ct);
    }
    struct curl_slist* hdrs = NULL;
    if (ct_hdr) {
        hdrs = curl_slist_append(hdrs, ct_hdr);
        free(ct_hdr);
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return finalise(&buf, rc);
}

char* omsc_http_request(const char* method, const char* url,
                        const char* body, const char* headers_str) {
    if (!url) return empty_string();
    CURL* curl = curl_easy_init();
    if (!curl) return empty_string();

    HttpBuf buf;
    httpbuf_init(&buf);

    setup_common(curl, &buf);
    curl_easy_setopt(curl, CURLOPT_URL, url);

    const char* safe_method = method && method[0] ? method : "GET";
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, safe_method);

    if (body && body[0]) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }

    struct curl_slist* hdrs = parse_headers(headers_str);
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return finalise(&buf, rc);
}

long long omsc_http_get_status(const char* url) {
    if (!url) return 0LL;
    CURL* curl = curl_easy_init();
    if (!curl) return 0LL;

    // Discard response body (we only want the status code).
    // Use CURLOPT_NOBODY for a HEAD-like request but still follow redirects.
    HttpBuf buf;
    httpbuf_init(&buf);

    setup_common(curl, &buf);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    // Use GET rather than HEAD so that servers that reject HEAD still work.
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    free(buf.data);
    curl_easy_cleanup(curl);
    return (long long)status;
}
