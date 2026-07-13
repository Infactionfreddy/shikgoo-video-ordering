#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "conn.h"
#include "http.h"

/* liest bis header komplett (\r\n\r\n) oder buffer voll */
static int recv_until_headers(Conn *c, char *buf, int cap) {
    int total = 0, n;
    while (total < cap - 1) {
        n = (int)conn_recv(c, buf + total, (size_t)(cap - total - 1));
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    return total;
}

/* request zerlegen: method, path, body, ws-key in req */
int parse_req(Conn *c, Req *req) {
    char raw[RECV_BUF + 1];
    memset(req, 0, sizeof(*req));
    int hlen = recv_until_headers(c, raw, RECV_BUF);
    if (hlen <= 0) return -1;

    sscanf(raw, "%7s %255s", req->method, req->path);

    const char *p;
    /* beide schreibweisen kommen in der praxis vor */
    if (strstr(raw, "Upgrade: websocket") || strstr(raw, "Upgrade: WebSocket"))
        req->is_ws = 1;

    /* ws-key: brauchen wir fuers accept im handshake */
    if ((p = strstr(raw, "Sec-WebSocket-Key: "))) {
        p += 19;
        const char *e = strpbrk(p, "\r\n");
        if (e) {
            size_t l = (size_t)(e - p);
            if (l < sizeof(req->ws_key)) { memcpy(req->ws_key, p, l); req->ws_key[l] = '\0'; }
        }
    }
    if ((p = strstr(raw, "Content-Length: ")))
        req->content_length = atoi(p + 16);

    const char *body_start = strstr(raw, "\r\n\r\n");
    if (!body_start) return 0;
    body_start += 4;

    /* tcp kann teil vom body schon mit dem header schicken */
    int already = hlen - (int)(body_start - raw);
    if (already < 0) already = 0;

    if (req->content_length > 0) {
        int want = req->content_length;
        /* fester buffer, kein heap. was nicht passt wird gekappt */
        if (want > (int)sizeof(req->body) - 1) want = (int)sizeof(req->body) - 1;
        if (already > 0) {
            int copy = already < want ? already : want;
            memcpy(req->body, body_start, (size_t)copy);
            req->body_len = copy;
        }
        /* rest nachlesen falls noch was fehlt */
        while (req->body_len < want) {
            int n = (int)conn_recv(c, req->body + req->body_len,
                                   (size_t)(want - req->body_len));
            if (n <= 0) break;
            req->body_len += n;
        }
        req->body[req->body_len] = '\0';
    }
    return 0;
}

/* nur die codes die wir brauchen, rest ist 500 */
static const char *status_line(int code) {
    switch (code) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 204: return "204 No Content";
        case 400: return "400 Bad Request";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 503: return "503 Service Unavailable";
        default:  return "500 Internal Server Error";
    }
}

/* generische antwort, ct + body selbst angeben */
void send_resp(Conn *c, int code, const char *ct, const char *body, ssize_t blen) {
    if (blen < 0) blen = (ssize_t)strlen(body); /* -1 = länge selbst berechnen */
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zd\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        status_line(code), ct, blen);
    conn_send(c, hdr, (size_t)hlen);
    if (body && blen > 0)
        conn_send(c, body, (size_t)blen);
}

/* json shortcut, brauchen wir dauernd */
void json_resp(Conn *c, int code, const char *fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    send_resp(c, code, "application/json", body, -1);
}

/* content-type aus endung, nur was wir ausliefern */
static const char *ext_ct(const char *path) {
    const char *d = strrchr(path, '.');
    if (!d) return "application/octet-stream";
    if (strcmp(d, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(d, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcmp(d, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (strcmp(d, ".json") == 0) return "application/json";
    if (strcmp(d, ".png")  == 0) return "image/png";
    return "application/octet-stream";
}

/* datei raus. erst traversal-check, sonst kommt wer an /etc/passwd */
void serve_file(Conn *c, const char *fpath) {
    if (strstr(fpath, "..")) {
        json_resp(c, 404, "{\"error\":\"Not found\"}");
        return;
    }
    int file = open(fpath, O_RDONLY);
    if (file < 0) {
        json_resp(c, 404, "{\"error\":\"Not found\"}");
        return;
    }
    struct stat st;
    fstat(file, &st);
    char hdr[512];
    const char *ct = ext_ct(fpath);
    /* alles no-cache, auch css/js. hatten css/js mal 1h gecacht, aber dann
     * sehen tablets (und ich beim testen) nach nem update bis zu ner stunde
     * die alte version. dateien sind winzig im lan, re-fetch kostet nix. */
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n",
        ct, (long long)st.st_size);
    conn_send(c, hdr, (size_t)hlen);
    /* in 4k-blöcken raus */
    char fbuf[4096];
    ssize_t n;
    while ((n = read(file, fbuf, sizeof(fbuf))) > 0)
        conn_send(c, fbuf, (size_t)n);
    close(file);
}
