/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.  There is NO WARRANTY, to
 * the extent permitted by law; see the LICENSE file for details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* HTTP client as a language-extension package (#68), on the mvx_ext ABI.
 *
 * Adds three BASIC-callable functions — the transport the mv package manager
 * uses on MVX to reach the registry and pull release tarballs:
 *   HTTPGET(url)          -> the response body (binary-safe; "" on error)
 *   HTTPGETFILE(url,path) -> the HTTP status (-1 connect error, -2 write
 *                            error), writing a 2xx body to `path`
 *   HTTPPOST(url,type,body,path)
 *                         -> the HTTP status, the same way, for a request
 *                            that carries a body
 *
 * Plain HTTP/1.1 over POSIX sockets, so the package has no external
 * dependency and builds/loads everywhere the runtime does.  HTTPS (TLS) is a
 * follow-up.  HTTPGET composes with JSONDECODE (the json package) for registry
 * metadata.
 *
 * WHY POST IS HERE AND NOT ONLY IN curl (#286).  The mvx-lang/curl package
 * supersedes this one where TLS is needed, and it published HTTPPOST while
 * this package did not — so the NAME existed only once curl was installed, and
 * a plain checkout could not compile a source that called it.  mv_package's
 * MVPKG.TRACK is written on the stated rule that the thing reporting an install
 * cannot depend on the install having happened; that rule was quietly broken on
 * mvx.  The signature matches curl's published one exactly (4 args, status
 * back), so the two remain interchangeable and superseding stays invisible.
 */
#include "mvx_ext.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { char *p; size_t len, cap; } buf;

static void bappend(buf *b, const char *s, size_t n) {
    if (b->len + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->len + n) c *= 2;
        char *np = realloc(b->p, c);
        if (!np) return;
        b->p = np;
        b->cap = c;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

/* Parse "http://host[:port][/path]".  A non-http scheme (e.g. https) is
   rejected.  Returns 0 on success. */
static int parse_url(const char *url, char *host, size_t hcap, char *port,
                     size_t pcap, char *path, size_t pathcap) {
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7, *hs = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t hl = (size_t)(p - hs);
    if (hl == 0 || hl >= hcap) return -1;
    memcpy(host, hs, hl);
    host[hl] = '\0';
    snprintf(port, pcap, "80");
    if (*p == ':') {
        const char *ps = ++p;
        while (*p && *p != '/') p++;
        size_t pl = (size_t)(p - ps);
        if (pl == 0 || pl >= pcap) return -1;
        memcpy(port, ps, pl);
        port[pl] = '\0';
    }
    snprintf(path, pathcap, "%s", *p ? p : "/");
    return 0;
}

/* Send `method` to `url`, with `body` (and its Content-Type) when there is
   one.  On success returns 0, sets *status to the HTTP status code and
   out/blen to the malloc'd (binary-safe) response body.  Returns -1 on a URL,
   DNS, connect, or I/O error.  The response is read the same way whatever the
   method, so GET and POST differ only in the request line and those two
   headers. */
static int http_req(const char *method, const char *url, const char *ctype,
                    const char *reqbody, size_t reqlen, char **out,
                    size_t *blen, int *status) {
    *out = NULL;
    *blen = 0;
    *status = 0;
    char host[256], port[16], path[2048];
    if (parse_url(url, host, sizeof host, port, sizeof port, path,
                  sizeof path) != 0)
        return -1;

    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    char req[3072];
    int rl;
    if (reqbody) {
        rl = snprintf(req, sizeof req,
                      "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: mvx-http/1.0"
                      "\r\nAccept: */*\r\nContent-Type: %s"
                      "\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      method, path, host, ctype ? ctype : "application/octet-stream",
                      reqlen);
    } else {
        rl = snprintf(req, sizeof req,
                      "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: mvx-http/1.0"
                      "\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                      method, path, host);
    }
    /* A header block that did not fit is a truncated request, which a server
       answers in its own way -- so refuse it here rather than send half of
       one. */
    if (rl < 0 || (size_t)rl >= sizeof req) { close(fd); return -1; }
    for (int off = 0; off < rl;) {
        ssize_t w = send(fd, req + off, (size_t)(rl - off), 0);
        if (w <= 0) { close(fd); return -1; }
        off += (int)w;
    }
    /* THE BODY IS SENT SEPARATELY, not composed into req: it is caller data of
       any length, and the header buffer is fixed. */
    for (size_t off = 0; off < reqlen;) {
        ssize_t w = send(fd, reqbody + off, reqlen - off, 0);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }

    buf raw = {0, 0, 0};
    char tmp[8192];
    ssize_t r;
    while ((r = recv(fd, tmp, sizeof tmp, 0)) > 0) bappend(&raw, tmp, (size_t)r);
    close(fd);
    if (!raw.p) return -1;

    if (raw.len > 12 && strncmp(raw.p, "HTTP/", 5) == 0) {
        const char *sp = memchr(raw.p, ' ', raw.len);
        if (sp) *status = atoi(sp + 1);
    }
    const char *body = NULL;
    size_t bodylen = 0, hlen = 0;
    for (size_t i = 0; i + 3 < raw.len; i++)
        if (memcmp(raw.p + i, "\r\n\r\n", 4) == 0) {
            hlen = i;
            body = raw.p + i + 4;
            bodylen = raw.len - (i + 4);
            break;
        }
    if (!body) { free(raw.p); return -1; }

    int chunked = 0;
    for (size_t i = 0; i + 7 <= hlen; i++)
        if (strncasecmp(raw.p + i, "chunked", 7) == 0) { chunked = 1; break; }

    if (chunked) {
        buf b = {0, 0, 0};
        const char *p = body, *end = body + bodylen;
        while (p < end) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            long sz = strtol(p, NULL, 16);
            p = nl + 1;
            if (sz <= 0 || p + sz > end) break;
            bappend(&b, p, (size_t)sz);
            p += sz;
            if (p + 2 <= end && p[0] == '\r' && p[1] == '\n') p += 2;
        }
        free(raw.p);
        *out = b.p ? b.p : calloc(1, 1);
        *blen = b.len;
    } else {
        char *cp = malloc(bodylen ? bodylen : 1);
        if (cp && bodylen) memcpy(cp, body, bodylen);
        free(raw.p);
        *out = cp;
        *blen = bodylen;
    }
    return 0;
}

static int64_t arg_str(mv_value *v, char *dst, size_t cap) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(v, nb, sizeof nb, &p);
    if (n >= (int64_t)cap) n = (int64_t)cap - 1;
    memcpy(dst, p, (size_t)n);
    dst[n] = '\0';
    return n;
}

/* HTTPGET(url) -> the response body on a 2xx status; "" otherwise (a non-2xx
   status, or a connection error) so a caller can treat empty as "not found". */
static void ext_httpget(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                        mv_value **argv) {
    (void)ctx;
    (void)argc;
    char url[2048];
    arg_str(argv[0], url, sizeof url);
    char *body;
    size_t blen;
    int status;
    if (http_req("GET", url, NULL, NULL, 0, &body, &blen, &status) == 0 && body &&
        status >= 200 && status < 300) {
        mv_set_str(ret, body, (int64_t)blen);
    } else {
        mv_set_str(ret, "", 0);
    }
    free(body);
}

/* Write a 2xx body to `path`, answering -2 when it cannot be opened.  Shared
   by HTTPGETFILE and HTTPPOST so both report a write failure the same way. */
static int body_to_file(const char *path, const char *body, size_t blen,
                        int status) {
    if (status < 200 || status >= 300) return status;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    if (blen) fwrite(body, 1, blen, f);
    fclose(f);
    return status;
}

/* HTTPGETFILE(url, path) -> HTTP status (-1 connect, -2 write); a 2xx body is
   written to `path`. */
static void ext_httpgetfile(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                            mv_value **argv) {
    (void)ctx;
    (void)argc;
    char url[2048], path[2048];
    arg_str(argv[0], url, sizeof url);
    arg_str(argv[1], path, sizeof path);
    char *body;
    size_t blen;
    int status;
    char num[16];
    if (http_req("GET", url, NULL, NULL, 0, &body, &blen, &status) == 0) {
        status = body_to_file(path, body, blen, status);
        free(body);
        snprintf(num, sizeof num, "%d", status);
    } else {
        snprintf(num, sizeof num, "-1");
    }
    mv_set_str(ret, num, (int64_t)strlen(num));
}

/* HTTPPOST(url, content-type, body, path) -> HTTP status (-1 connect, -2
   write); a 2xx body is written to `path`.  The signature mvx-lang/curl
   publishes, so a caller cannot tell which of the two answered (#286).
   The request body is taken with its own length, not as a C string, so a
   payload holding a NUL still goes out whole. */
static void ext_httppost(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                         mv_value **argv) {
    (void)ctx;
    (void)argc;
    char url[2048], ctype[256], path[2048];
    arg_str(argv[0], url, sizeof url);
    arg_str(argv[1], ctype, sizeof ctype);
    char nb[40];
    const char *reqbody;
    int64_t reqlen = mv_val_chars(argv[2], nb, sizeof nb, &reqbody);
    if (reqlen < 0) reqlen = 0;
    arg_str(argv[3], path, sizeof path);
    char *body;
    size_t blen;
    int status;
    char num[16];
    if (http_req("POST", url, ctype, reqbody, (size_t)reqlen, &body, &blen,
                 &status) == 0) {
        status = body_to_file(path, body, blen, status);
        free(body);
        snprintf(num, sizeof num, "%d", status);
    } else {
        snprintf(num, sizeof num, "-1");
    }
    mv_set_str(ret, num, (int64_t)strlen(num));
}

static const mvx_extfn http_fns[] = {
    {"HTTPGET", 1, 1, ext_httpget},
    {"HTTPGETFILE", 2, 2, ext_httpgetfile},
    {"HTTPPOST", 4, 4, ext_httppost},
};
static const mvx_ext http_ext = {"http", 3, http_fns};

const mvx_ext *mvx_ext_entry(int abi) {
    return abi == MVX_EXT_ABI ? &http_ext : NULL;
}
