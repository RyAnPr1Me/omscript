#pragma once
// http_runtime.h — Secure HTTP client C API for OmScript's stdlib.
//
// All functions return a malloc'd NUL-terminated string on success, or a
// malloc'd empty string on failure (never NULL).  The caller is responsible
// for calling free() on the returned pointer.
//
// Security guarantees:
//   - SSL peer certificate verification is enabled by default (libcurl default).
//   - Host name verification is enabled by default.
//   - No shell invocation — libcurl is called directly (no injection risk).
//   - Redirects are followed only within the same protocol family.
//   - Connect timeout: 10 s.  Transfer timeout: 30 s.

#ifdef __cplusplus
extern "C" {
#endif

/// Perform an HTTP/HTTPS GET request.
/// @param url  NUL-terminated URL string (must not be NULL).
/// @return     malloc'd response body string.  On error, returns a malloc'd
///             empty string.  Caller must free().
char* omsc_http_get(const char* url);

/// Perform an HTTP/HTTPS POST request.
/// @param url           NUL-terminated URL string.
/// @param body          NUL-terminated POST body (may be NULL for empty body).
/// @param content_type  NUL-terminated Content-Type header value
///                      (may be NULL; defaults to "application/octet-stream").
/// @return              malloc'd response body string.  Caller must free().
char* omsc_http_post(const char* url, const char* body, const char* content_type);

/// Perform a generic HTTP/HTTPS request.
/// @param method       NUL-terminated HTTP method ("GET", "POST", "PUT", …).
/// @param url          NUL-terminated URL string.
/// @param body         NUL-terminated request body (may be NULL).
/// @param headers_str  Newline-separated "Name: Value" header lines (may be NULL).
/// @return             malloc'd response body string.  Caller must free().
char* omsc_http_request(const char* method, const char* url,
                        const char* body, const char* headers_str);

/// Return the HTTP status code for a GET request.
/// @param url  NUL-terminated URL string.
/// @return     HTTP status code (e.g. 200), or 0 on connection/TLS error.
long long omsc_http_get_status(const char* url);

#ifdef __cplusplus
} // extern "C"
#endif
