/*
 * http_client.h — Minimal synchronous HTTP GET client for local API polling.
 *
 * Return value convention:
 *   >= 100         : HTTP status code (e.g. 200, 404)
 *   CBM_HTTP_ERR_* : pre-HTTP network/protocol failure (negative)
 */
#ifndef CBM_HTTP_CLIENT_H
#define CBM_HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* Sentinel error codes (all negative, non-overlapping with HTTP status). */
#define CBM_HTTP_ERR_REFUSED  (-2)  /* connection refused — server not running */
#define CBM_HTTP_ERR_AUTH     (-3)  /* HTTP 401 or 403 — bad/expired token */
#define CBM_HTTP_ERR_PARSE    (-4)  /* response is not valid HTTP */

/*
 * Perform a synchronous HTTP/1.1 GET request.
 *
 * host:       Target hostname or IP (e.g. "127.0.0.1").
 * port:       Target port.
 * path:       Request path (e.g. "/api/v1/threads").
 * token:      Optional Bearer token for Authorization header. NULL = omit header.
 * timeout_ms: Connect + receive timeout in milliseconds.
 * out_body:   On success: heap-allocated, null-terminated response body.
 *             Caller must free(). Always NULL on error.
 * out_len:    On success: byte length of out_body (excluding null terminator).
 *
 * Returns:
 *   HTTP status code (>= 100) on a complete response.
 *   CBM_HTTP_ERR_REFUSED if the server port is not open.
 *   CBM_HTTP_ERR_AUTH    if the server returned 401 or 403.
 *   CBM_HTTP_ERR_PARSE   if the response could not be parsed.
 *   -1                   on any other error (OOM, bad args, I/O error).
 */
int cbm_http_client_get(const char *host, int port, const char *path,
                        const char *token, int timeout_ms,
                        char **out_body, size_t *out_len);

#endif /* CBM_HTTP_CLIENT_H */
