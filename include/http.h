#pragma once
#include <sys/types.h>
#include <stdarg.h>
#include "types.h"
#include "conn.h"

/* geparster request, alles was wir aus dem raw input brauchen */
typedef struct {
    char method[8], path[256], body[RECV_BUF];
    char ws_key[64], content_type[64];        /* ws_key nur beim ws-upgrade gesetzt */
    int  content_length, is_ws, body_len;     /* is_ws = upgrade-request, body_len = tatsaechlich gelesen */
} Req;

int  parse_req (Conn *c, Req *req);
void send_resp (Conn *c, int code, const char *ct, const char *body, ssize_t blen);
void json_resp (Conn *c, int code, const char *fmt, ...);
void serve_file(Conn *c, const char *fpath); /* liefert datei aus FRONTEND_DIR, 404 wenn nicht da */
