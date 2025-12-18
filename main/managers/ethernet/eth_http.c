#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "managers/ethernet/eth_http.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>

static bool eth_parse_http_url(const char *url, char *host, size_t host_len, uint16_t *port, char *path, size_t path_len) {
    if (!url || !host || !port || !path || host_len == 0 || path_len == 0) return false;
    host[0] = '\0';
    path[0] = '\0';
    *port = 80;

    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return false;
    p += 7;

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : (p + strlen(p));

    const char *colon = NULL;
    for (const char *t = p; t < host_end; t++) {
        if (*t == ':') {
            colon = t;
            break;
        }
    }

    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_len) hlen = host_len - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';

        int parsed_port = atoi(colon + 1);
        if (parsed_port > 0 && parsed_port <= 65535) *port = (uint16_t)parsed_port;
    } else {
        size_t hlen = (size_t)(host_end - p);
        if (hlen >= host_len) hlen = host_len - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    }

    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= path_len) plen = path_len - 1;
        memcpy(path, slash, plen);
        path[plen] = '\0';
    } else {
        snprintf(path, path_len, "/");
    }

    return host[0] != '\0';
}

int eth_http_get_simple(const char *url, char *out, size_t out_len, int timeout_ms) {
    if (!url || !out || out_len == 0) return -1;
    out[0] = '\0';

    char host[64];
    char path[128];
    uint16_t port = 80;
    if (!eth_parse_http_url(url, host, sizeof(host), &port, path, sizeof(path))) return -1;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    int so_error = 0;
    socklen_t slen = sizeof(so_error);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &slen);
    if (so_error != 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    if (flags >= 0) fcntl(sock, F_SETFL, flags);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path, host);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    if (send(sock, req, req_len, 0) < 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    size_t total = 0;
    while (total + 1 < out_len) {
        int n = recv(sock, out + total, (int)(out_len - total - 1), 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    out[total] = '\0';

    close(sock);
    freeaddrinfo(res);
    return (int)total;
}

#else

#include "managers/ethernet/eth_http.h"

int eth_http_get_simple(const char *url, char *out, size_t out_len, int timeout_ms) {
    (void)url;
    (void)out;
    (void)out_len;
    (void)timeout_ms;
    return -1;
}

#endif
