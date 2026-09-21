/*
 * http_client.c — Minimal synchronous HTTP GET client
 *
 * Error codes returned (not HTTP status):
 *   -1  generic / network failure
 *   -2  connection refused (ThreadWeaver not running)
 *   -3  auth rejected (HTTP 401/403)
 *   -4  response parse error
 */
#include "foundation/http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET cbm_sock_t;
#define CBM_SOCK_BAD INVALID_SOCKET
#define cbm_sock_close closesocket
static int cbm_winsock_init(void) {
    WSADATA wsa;
    /* WSAStartup is idempotent — safe to call multiple times per process. */
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
typedef int cbm_sock_t;
#define CBM_SOCK_BAD (-1)
#define cbm_sock_close close
static int cbm_winsock_init(void) { return 0; } /* no-op on POSIX */
#endif

int cbm_http_client_get(const char *host, int port, const char *path,
                        const char *token, int timeout_ms,
                        char **out_body, size_t *out_len) {
    if (!host || !path || !out_body || !out_len) return -1;
    *out_body = NULL;
    *out_len = 0;

    if (cbm_winsock_init() != 0) {
        fprintf(stderr, "http_client: WSAStartup failed\n");
        return -1;
    }

    cbm_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == CBM_SOCK_BAD) {
        fprintf(stderr, "http_client: socket() failed\n");
        return -1;
    }

#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "http_client: invalid host address: %s\n", host);
        cbm_sock_close(fd);
        return -1;
    }

    /* Non-blocking connect — we check readiness with select/poll below. */
    int conn_rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    (void)conn_rc; /* EINPROGRESS / WSAEWOULDBLOCK is expected on non-blocking */

#ifdef _WIN32
    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(fd, &wfds);
    FD_SET(fd, &efds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int res = select((int)fd + 1, NULL, &wfds, &efds, &tv);
    /* On Windows, a refused connection appears in the error set. */
    if (res <= 0 || FD_ISSET(fd, &efds)) {
        cbm_sock_close(fd);
        return CBM_HTTP_ERR_REFUSED;
    }
#else
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int res = poll(&pfd, 1, timeout_ms);
    if (res <= 0) {
        cbm_sock_close(fd);
        return CBM_HTTP_ERR_REFUSED;
    }
    /* Verify the connect succeeded — check SO_ERROR */
    int so_err = 0;
    socklen_t so_len = sizeof(so_err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
    if (so_err != 0) {
        cbm_sock_close(fd);
        return CBM_HTTP_ERR_REFUSED;
    }
#endif

    /* Restore blocking mode and set I/O timeouts. */
#ifdef _WIN32
    mode = 0;
    ioctlsocket(fd, FIONBIO, &mode);
    DWORD rcv_tv = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_tv, sizeof(rcv_tv));
    DWORD snd_tv = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&snd_tv, sizeof(snd_tv));
#else
    fcntl(fd, F_SETFL, flags); /* restore original (blocking) flags */
    struct timeval to;
    to.tv_sec = timeout_ms / 1000;
    to.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
#endif

    char req[4096];
    int req_len;
    if (token) {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Connection: close\r\n"
            "Authorization: Bearer %s\r\n\r\n",
            path, host, port, token);
    } else {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Connection: close\r\n\r\n",
            path, host, port);
    }

    if (req_len >= (int)sizeof(req) || req_len < 0) {
        fprintf(stderr, "http_client: request path too long\n");
        cbm_sock_close(fd);
        return -1;
    }

    /* Retry send() until all bytes are sent (partial sends are possible). */
    int sent = 0;
    while (sent < req_len) {
        int n = send(fd, req + sent, req_len - sent, 0);
        if (n <= 0) {
            fprintf(stderr, "http_client: send() failed after %d/%d bytes\n", sent, req_len);
            cbm_sock_close(fd);
            return -1;
        }
        sent += n;
    }

    /* Read the full response. Grow buffer as needed. */
    size_t cap = 8192;
    char *buf = malloc(cap);
    size_t len = 0;
    if (!buf) {
        cbm_sock_close(fd);
        return -1;
    }

    while (1) {
        if (len == cap) {
            size_t new_cap = cap * 2;
            char *nb = realloc(buf, new_cap);
            if (!nb) {
                fprintf(stderr, "http_client: out of memory at %zu bytes\n", cap);
                free(buf);
                cbm_sock_close(fd);
                return -1;
            }
            buf = nb;
            cap = new_cap;
        }
        int n = recv(fd, buf + len, (int)(cap - len), 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    cbm_sock_close(fd);

    if (len == 0) {
        fprintf(stderr, "http_client: empty response from server\n");
        free(buf);
        return -1;
    }

    /* Null-terminate; grow by one byte only if buffer is exactly full. */
    if (len >= cap) {
        char *nb = realloc(buf, cap + 1);
        if (!nb) { free(buf); return -1; }
        buf = nb;
    }
    buf[len] = '\0';

    /* Validate HTTP response line. */
    if (strncmp(buf, "HTTP/1.", 7) != 0) {
        fprintf(stderr, "http_client: response does not look like HTTP\n");
        free(buf);
        return CBM_HTTP_ERR_PARSE;
    }

    int status = atoi(buf + 9);
    if (status <= 0 || status > 999) {
        fprintf(stderr, "http_client: unparseable status code in response\n");
        free(buf);
        return CBM_HTTP_ERR_PARSE;
    }

    /* Return auth-error code early so callers can produce good messages. */
    if (status == 401 || status == 403) {
        free(buf);
        return CBM_HTTP_ERR_AUTH;
    }

    char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) {
        fprintf(stderr, "http_client: no header/body separator found\n");
        free(buf);
        return CBM_HTTP_ERR_PARSE;
    }
    body_start += 4;

    /* Detect chunked transfer-encoding (case-insensitive). */
    char *hdr_end = body_start - 4;
    *hdr_end = '\0';
    bool chunked = (strstr(buf, "Transfer-Encoding: chunked") != NULL
                 || strstr(buf, "transfer-encoding: chunked") != NULL);
    *hdr_end = '\r';

    if (chunked) {
        char *p = body_start;
        char *dest = body_start;
        while (1) {
            char *endptr;
            long chunk_len = strtol(p, &endptr, 16);
            if (endptr == p || chunk_len < 0) {
                fprintf(stderr, "http_client: malformed chunk size\n");
                free(buf);
                return CBM_HTTP_ERR_PARSE;
            }
            p = strstr(endptr, "\r\n");
            if (!p) {
                fprintf(stderr, "http_client: missing CRLF after chunk size\n");
                free(buf);
                return CBM_HTTP_ERR_PARSE;
            }
            p += 2;

            if (chunk_len == 0) break; /* last-chunk */
            if ((size_t)(p - buf) + (size_t)chunk_len > len) {
                fprintf(stderr, "http_client: chunk claims data beyond response\n");
                free(buf);
                return CBM_HTTP_ERR_PARSE;
            }

            memmove(dest, p, (size_t)chunk_len);
            dest += chunk_len;
            p += chunk_len;

            if (strncmp(p, "\r\n", 2) == 0) p += 2;
        }
        *out_len = (size_t)(dest - body_start);
    } else {
        *out_len = len - (size_t)(body_start - buf);
    }

    *out_body = malloc(*out_len + 1);
    if (!*out_body) {
        fprintf(stderr, "http_client: out of memory allocating body\n");
        free(buf);
        return -1;
    }
    memcpy(*out_body, body_start, *out_len);
    (*out_body)[*out_len] = '\0';
    free(buf);

    return status;
}
