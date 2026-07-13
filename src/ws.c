#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include "conn.h"
#include "ws.h"
#include "cJSON.h"
#include "logic.h"
#include "menu.h"

/* die zwei fixen antworten, dafür braucht man kein cJSON */
static const char *CALL_BUSY_MSG = "{\"type\":\"call_busy\"}";
static const char *CALL_END_MSG  = "{\"type\":\"call_end\"}";

/* forward decl, impl steht unten beim signaling-router */
static int ws_resolve_and_send(int target_fd, const char *msg, size_t len);

#define WS_MSG_MAX 16384

/* server.log lazy aufmachen, nach jeder zeile flushen. sonst ist bei
 * crash/kill mitten im call das log weg */
static FILE *g_log_file = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void ws_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2))); /* printf-format-check vom compiler */

/* immer nach file und stderr, damit man beim debuggen aufm pi live mitliest */
static void ws_logf(const char *fmt, ...) {
    pthread_mutex_lock(&log_lock);
    if (!g_log_file) g_log_file = fopen("server.log", "a");

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

    va_list args;

    if (g_log_file) {
        fprintf(g_log_file, "[%s] ", ts);
        va_start(args, fmt);
        vfprintf(g_log_file, fmt, args);
        va_end(args);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }

    fprintf(stderr, "[%s] ", ts);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    pthread_mutex_unlock(&log_lock);
}

/* client in die liste, unter ws_lock. -1 wenn voll */
int ws_add(Conn *c, int table_number, int is_waiter) {
    pthread_mutex_lock(&ws_lock);
    if (ws_count >= MAX_WS) {
        pthread_mutex_unlock(&ws_lock);
        ws_logf("[ws] capacity full (MAX_WS=%d), rejecting fd=%d", MAX_WS, c->fd);
        return -1;
    }
    ws_clients[ws_count].conn         = *c;
    ws_clients[ws_count].table_number = table_number;
    ws_clients[ws_count].is_waiter    = is_waiter;
    /* eigenes send-lock pro client, sonst zwei threads gleichzeitig im SSL_write */
    pthread_mutex_init(&ws_clients[ws_count].send_lock, NULL);
    ws_count++;
    pthread_mutex_unlock(&ws_lock);
    ws_logf("[ws] client connected: fd=%d table=%d waiter=%d",
            c->fd, table_number, is_waiter);
    return 0;
}

/* client raus. und falls er grad in nem call hing, die andere seite benachrichtigen */
void ws_remove(Conn *c) {
    int fd = c->fd;
    int found = 0;
    int disc_table_number = 0; /* wird unten unter ws_lock gefüllt */
    int disc_is_waiter    = 0;
    /* kopie, weil wir den lock nach dem rausnehmen noch destroyen müssen */
    pthread_mutex_t removed_send_lock;

    pthread_mutex_lock(&ws_lock);
    for (int i = 0; i < ws_count; i++) {
        if (ws_clients[i].conn.fd == fd) {
            removed_send_lock  = ws_clients[i].send_lock;
            disc_table_number  = ws_clients[i].table_number;
            disc_is_waiter     = ws_clients[i].is_waiter;
            found = 1;
            /* send-lock holen bevor der slot überschrieben wird, sonst schreibt
             * ein broadcast-thread noch in nen client den es nicht mehr gibt */
            pthread_mutex_lock(&removed_send_lock);
            ws_clients[i] = ws_clients[--ws_count];
            break;
        }
    }
    pthread_mutex_unlock(&ws_lock);

    /* SSL_free unter send-lock, sonst race mit SSL_write */
    if (found) {
        if (c->ssl) {
            SSL_shutdown(c->ssl);
            SSL_free(c->ssl);
            c->ssl = NULL; /* sonst double-free in conn_close */
        }
        pthread_mutex_unlock(&removed_send_lock);
        pthread_mutex_destroy(&removed_send_lock);
    }

    ws_logf("[ws] client disconnected: fd=%d", fd);

    /* disconnect mitten im call: session platt machen, gegenseite bescheid geben.
     * aktiver call -> customer_fd/waiter_fd sind gesetzt, direkt per fd prüfen.
     * pending -> die fds gibts erst ab call_accept, also über caller_table + is_waiter erkennen */

    /* waiter-fd noch unter ws_lock holen bevor wir g_lock nehmen. nie ws_lock und
     * g_lock zusammen halten, sonst ABBA-deadlock mit ws_route_signal (ordering: ws_lock vor g_lock) */
    pthread_mutex_lock(&ws_lock);
    int wfd = ws_find_waiter();
    pthread_mutex_unlock(&ws_lock);

    pthread_mutex_lock(&g_lock);
    if (g_call_session.active &&
        (g_call_session.customer_fd == fd || g_call_session.waiter_fd == fd)) {
        /* aktiver call, andere seite benachrichtigen */
        int other_fd = (g_call_session.customer_fd == fd)
                       ? g_call_session.waiter_fd
                       : g_call_session.customer_fd;
        memset(&g_call_session, 0, sizeof(g_call_session));
        pthread_mutex_unlock(&g_lock);
        /* g_lock schon frei bevor ws_resolve_and_send */
        if (other_fd > 0) {
            ws_logf("[ws] disconnect fd=%d -> notify other_fd=%d (active call ended)", fd, other_fd);
            ws_resolve_and_send(other_fd, CALL_END_MSG, strlen(CALL_END_MSG));
        }
    } else if (g_call_session.pending &&
               (disc_is_waiter ||
                disc_table_number == g_call_session.caller_table)) {
        /* pending call, kunde oder kellner geht während der ring läuft */
        if (disc_is_waiter) {
            /* kellner weg während ring: kunden-overlay gibts noch gar nicht, nix zu melden */
            memset(&g_call_session, 0, sizeof(g_call_session));
            pthread_mutex_unlock(&g_lock);
        } else {
            /* kunde weg während ring: kellner-overlay ausblenden. wfd haben wir oben schon */
            memset(&g_call_session, 0, sizeof(g_call_session));
            pthread_mutex_unlock(&g_lock);
            if (wfd > 0) {
                ws_logf("[ws] disconnect fd=%d -> notify wfd=%d (pending call ended)", fd, wfd);
                ws_resolve_and_send(wfd, CALL_END_MSG, strlen(CALL_END_MSG));
            }
        }
    } else {
        pthread_mutex_unlock(&g_lock);
    }
}

/* frame selbst zusammenbauen (header + payload), wir haben keine ws-lib */
static void ws_send(Conn *c, const char *msg, size_t len) {
    if (len > 65535) {
        fprintf(stderr, "[ws] outbound message too large (%zu bytes), dropping\n", len);
        return;
    }
    uint8_t hdr[4];
    size_t hlen;
    hdr[0] = 0x81; /* FIN + text-opcode */
    if (len < 126) {
        hdr[1] = (uint8_t)len; hlen = 2;
    } else {
        hdr[1] = 126; hdr[2] = (len >> 8) & 0xFF; hdr[3] = len & 0xFF; hlen = 4;
    }
    conn_send(c, hdr, hlen);
    conn_send(c, msg, len);
}

void ws_send_to(Conn *c, const char *msg) {  /* wrapper wenn man eh nen nullterminierten string hat */
    ws_send(c, msg, strlen(msg));
}

/* forward decl, def kommt erst nach den handlern */
static void ws_route_signal(int sender_fd, const char *msg, size_t len);

void ws_broadcast(const char *msg) {
    size_t len = strlen(msg);
    /* erst snapshot unter lock, senden dann außerhalb. kein ssl_write unter ws_lock */
    Conn conns[MAX_WS];
    int count;
    pthread_mutex_lock(&ws_lock);
    count = ws_count;
    for (int i = 0; i < count; i++) conns[i] = ws_clients[i].conn;
    pthread_mutex_unlock(&ws_lock);
    for (int i = 0; i < count; i++)
        ws_send(&conns[i], msg, len);
}

/* kellner-fd oder -1. nur unter ws_lock aufrufen */
int ws_find_waiter(void) {
    for (int i = 0; i < ws_count; i++)
        if (ws_clients[i].is_waiter) return ws_clients[i].conn.fd;
    return -1;
}

/* kunden-fd am tisch oder -1. auch nur unter ws_lock */
int ws_find_table(int table_number) {
    for (int i = 0; i < ws_count; i++)
        if (!ws_clients[i].is_waiter && ws_clients[i].table_number == table_number)
            return ws_clients[i].conn.fd;
    return -1;
}

int ws_handshake(Conn *c, const char *key) {
    /* rfc 6455 magic uuid anhängen, sha1 drüber, base64 raus */
    char cat[160];
    snprintf(cat, sizeof(cat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    uint8_t digest[SHA_DIGEST_LENGTH];
    SHA1((const uint8_t *)cat, strlen(cat), digest);
    char accept[64];
    int n_enc = EVP_EncodeBlock((unsigned char *)accept, digest, SHA_DIGEST_LENGTH);
    accept[n_enc] = '\0';
    char resp[256];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    return conn_send(c, resp, (size_t)n) > 0 ? 0 : -1;
}

/* obj als json rausschicken und gleich freigeben */
static void ws_send_json(Conn *c, cJSON *obj) {
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (out) { ws_send_to(c, out); cJSON_free(out); }
}

/* ab hier die handler für die normalen bestell-/call-msgs */

static void handle_get_menu(Conn *c) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "menu_data");
    cJSON_AddItemToObject(resp, "items", logic_menu_items());
    ws_send_json(c, resp);
}

static void handle_get_order(Conn *c, cJSON *req) {
    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "order_id");
    int oid = cJSON_IsNumber(id_j) ? (int)id_j->valuedouble : 0;
    cJSON *obj = oid > 0 ? logic_order_detail(oid) : NULL;
    if (!obj) { ws_send_to(c, "{\"type\":\"order_error\",\"message\":\"Order not found\"}"); return; }
    cJSON_AddStringToObject(obj, "type", "order_data");
    ws_send_json(c, obj);
}

static void handle_get_table_order(Conn *c, cJSON *req) {
    cJSON *tbl_j = cJSON_GetObjectItemCaseSensitive(req, "table_number");
    int table = cJSON_IsNumber(tbl_j) ? (int)tbl_j->valuedouble : 0;
    if (table < 1 || table > 20) return; /* ungültige tisch-nr, ignorieren */
    cJSON *obj = logic_active_order_for_table(table);
    if (!obj) return; /* kein aktiver order, stepper bleibt weg */
    cJSON_AddStringToObject(obj, "type", "order_data");
    ws_send_json(c, obj);
}

static void handle_place_order(Conn *c, cJSON *req) {
    cJSON *table_j = cJSON_GetObjectItemCaseSensitive(req, "table_number");
    int table = cJSON_IsNumber(table_j) ? (int)table_j->valuedouble : -1;

    cJSON *items_j = cJSON_GetObjectItemCaseSensitive(req, "items");
    int n = cJSON_IsArray(items_j) ? cJSON_GetArraySize(items_j) : 0;
    if (n < 1 || n > 20) {
        ws_send_to(c, "{\"type\":\"order_error\",\"message\":\"items must be a non-empty array\"}");
        return;
    }
    LogicOrderItem vi[20];
    for (int i = 0; i < n; i++) {
        cJSON *elem  = cJSON_GetArrayItem(items_j, i);
        cJSON *mid_j = cJSON_GetObjectItemCaseSensitive(elem, "menu_item_id");
        cJSON *qty_j = cJSON_GetObjectItemCaseSensitive(elem, "quantity");
        vi[i].menu_item_id = cJSON_IsNumber(mid_j) ? (int)mid_j->valuedouble : -1;
        vi[i].quantity     = cJSON_IsNumber(qty_j) ? (int)qty_j->valuedouble : -1;
    }
    cJSON *cname_j = cJSON_GetObjectItemCaseSensitive(req, "customer_name");
    cJSON *notes_j = cJSON_GetObjectItemCaseSensitive(req, "notes");

    char evt[128] = "";
    int oid = logic_place_order(table, n, vi,
                                cJSON_IsString(cname_j) ? cname_j->valuestring : NULL,
                                cJSON_IsString(notes_j) ? notes_j->valuestring : NULL,
                                evt, sizeof(evt));

    if (oid == LOGIC_CAPACITY) { ws_send_to(c, "{\"type\":\"order_error\",\"message\":\"Server at capacity\"}"); return; }
    if (oid < 0)               { ws_send_to(c, "{\"type\":\"order_error\",\"message\":\"Invalid input\"}");     return; }

    char resp[80];
    snprintf(resp, sizeof(resp), "{\"type\":\"order_placed\",\"order_id\":%d,\"status\":\"pending\"}", oid);
    ws_send_to(c, resp);
    if (evt[0]) ws_broadcast(evt); /* alle benachrichtigen, z.b. kellner-dashboard */
}

static void handle_update_order(Conn *c, cJSON *req) {
    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "order_id");
    cJSON *st_j = cJSON_GetObjectItemCaseSensitive(req, "status");
    int oid    = cJSON_IsNumber(id_j) ? (int)id_j->valuedouble : 0;
    int new_st = cJSON_IsString(st_j) ? status_idx(st_j->valuestring) : -1;

    char evt[128] = "";
    int result = logic_update_order(oid, new_st, evt, sizeof(evt));

    if (result == LOGIC_BACKWARD) { ws_send_to(c, "{\"type\":\"order_error\",\"message\":\"Status must advance forward\"}"); return; }
    if (result == LOGIC_NOTFOUND) { ws_send_to(c, "{\"type\":\"order_error\",\"message\":\"Order not found\"}");             return; }
    if (result < 0)               { ws_send_to(c, "{\"type\":\"order_error\",\"message\":\"Invalid fields\"}");              return; }

    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"type\":\"order_updated\",\"order_id\":%d,\"status\":\"%s\"}", oid, STATUS_STR[result]);
    ws_send_to(c, resp);
    if (evt[0]) ws_broadcast(evt);
}

static void handle_get_queue(Conn *c) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "queue_data");
    cJSON_AddItemToObject(resp, "orders", logic_queue_orders());
    ws_send_json(c, resp);
}

static void handle_call_waiter(Conn *c, cJSON *req) {
    cJSON *tbl_j = cJSON_GetObjectItemCaseSensitive(req, "table_number");
    int table    = cJSON_IsNumber(tbl_j) ? (int)tbl_j->valuedouble : -1;

    int is_existing = 0;
    char evt[128] = "";
    int cid = logic_call_waiter(table, &is_existing, evt, sizeof(evt));

    if (cid == LOGIC_CAPACITY) { ws_send_to(c, "{\"type\":\"call_error\",\"message\":\"Too many active calls\"}");  return; }
    if (cid < 0)               { ws_send_to(c, "{\"type\":\"call_error\",\"message\":\"table_number must be 1-20\"}"); return; }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"type\":\"call_ack\",\"call_id\":%d}", cid);
    ws_send_to(c, resp);
    if (evt[0]) ws_broadcast(evt);
}

static void handle_get_calls(Conn *c) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "calls_data");
    cJSON_AddItemToObject(resp, "calls", logic_active_calls());
    ws_send_json(c, resp);
}

static void handle_resolve_call(Conn *c, cJSON *req) {
    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "call_id");
    int cid = cJSON_IsNumber(id_j) ? (int)id_j->valuedouble : 0;

    char evt[128] = "";
    int result = logic_resolve_call(cid, evt, sizeof(evt));

    if (result < 0) { ws_send_to(c, "{\"type\":\"resolve_error\",\"message\":\"Call not found\"}"); return; }
    ws_send_to(c, "{\"type\":\"resolve_ok\"}");
    if (evt[0]) ws_broadcast(evt);
}

static void handle_set_availability(Conn *c, cJSON *req) {
    cJSON *busy_j = cJSON_GetObjectItemCaseSensitive(req, "busy");
    if (!cJSON_IsBool(busy_j)) {
        ws_send_to(c, "{\"type\":\"availability_error\",\"message\":\"busy field required\"}");
        return;
    }
    char evt[64] = "";
    logic_set_availability(cJSON_IsTrue(busy_j), evt, sizeof(evt));
    ws_send_to(c, "{\"type\":\"availability_ok\"}");
    if (evt[0]) ws_broadcast(evt);
}

/* dispatcher: schaut nur auf "type" und ruft den passenden handler */
static void ws_handle_message(Conn *c, const char *text) {
    cJSON *req = cJSON_Parse(text);
    if (!req) return;
    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(req, "type");
    if (cJSON_IsString(type_j)) {
        const char *t = type_j->valuestring;
        if      (!strcmp(t, "get_menu"))        handle_get_menu(c);
        else if (!strcmp(t, "get_order"))        handle_get_order(c, req);
        else if (!strcmp(t, "get_table_order")) handle_get_table_order(c, req);
        else if (!strcmp(t, "place_order"))      handle_place_order(c, req);
        else if (!strcmp(t, "update_order"))     handle_update_order(c, req);
        else if (!strcmp(t, "get_queue"))        handle_get_queue(c);
        else if (!strcmp(t, "call_waiter"))      handle_call_waiter(c, req);
        else if (!strcmp(t, "get_calls"))        handle_get_calls(c);
        else if (!strcmp(t, "resolve_call"))     handle_resolve_call(c, req);
        else if (!strcmp(t, "set_availability")) handle_set_availability(c, req);
    }
    cJSON_Delete(req);
}

/* webrtc-signaling-router, jede eingehende text-msg kommt hier zuerst rein */

/* target_fd in der liste suchen und die msg schicken. send_lock wird noch unter
 * ws_lock geholt, damit ws_remove nicht dazwischen SSL_free macht. 1 wenn gesendet, sonst 0 */
static int ws_resolve_and_send(int target_fd, const char *msg, size_t len) {
    if (target_fd < 0) return 0;
    Conn target = {-1, NULL};
    pthread_mutex_t *target_send_lock = NULL;

    pthread_mutex_lock(&ws_lock);
    for (int i = 0; i < ws_count; i++) {
        if (ws_clients[i].conn.fd == target_fd) {
            target           = ws_clients[i].conn;
            target_send_lock = &ws_clients[i].send_lock;
            /* noch unter ws_lock, ws_remove muss dann warten bis das SSL_write durch ist */
            pthread_mutex_lock(target_send_lock);
            break;
        }
    }
    pthread_mutex_unlock(&ws_lock);

    if (target.fd >= 0) {
        ws_send(&target, msg, len);
        pthread_mutex_unlock(target_send_lock);
    }
    ws_logf("[route] deliver to fd=%d: %s", target_fd, target.fd >= 0 ? "OK" : "NOT FOUND (stale/disconnected fd)");
    return target.fd >= 0 ? 1 : 0;
}

static void ws_route_signal(int sender_fd, const char *msg, size_t len) {
    /* nur den type lesen, SDP und ICE blobs werden nur durchgereicht, nie geparst */
    cJSON *root = cJSON_Parse(msg);
    if (!root) {
        /* kein json, trotzdem mal beim app-handler versuchen */
        Conn sender = {-1, NULL};
        pthread_mutex_lock(&ws_lock);
        for (int i = 0; i < ws_count; i++)
            if (ws_clients[i].conn.fd == sender_fd) { sender = ws_clients[i].conn; break; }
        pthread_mutex_unlock(&ws_lock);
        if (sender.fd >= 0) ws_handle_message(&sender, msg);
        return;
    }

    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_j)) {
        cJSON_Delete(root);
        /* type kein string, ab an den app-handler */
        Conn sender = {-1, NULL};
        pthread_mutex_lock(&ws_lock);
        for (int i = 0; i < ws_count; i++)
            if (ws_clients[i].conn.fd == sender_fd) { sender = ws_clients[i].conn; break; }
        pthread_mutex_unlock(&ws_lock);
        if (sender.fd >= 0) ws_handle_message(&sender, msg);
        return;
    }
    const char *type = type_j->valuestring;

    /* table_number brauchen wir für manche signaling-typen */
    cJSON *tbl_j = cJSON_GetObjectItemCaseSensitive(root, "table_number");
    int table = cJSON_IsNumber(tbl_j) ? (int)tbl_j->valuedouble : -1;

    /* g_call_session sagt ob grad ein call läuft */
    if (strcmp(type, "call_ring") == 0) {
        /* active ODER pending prüfen, sonst schlüpft ein zweiter ring rein bevor
         * der kellner den ersten accepted hat */
        pthread_mutex_lock(&g_lock);
        if (g_call_session.active || g_call_session.pending) {
            /* schon belegt, call_busy zurück statt weiterleiten */
            pthread_mutex_unlock(&g_lock);
            cJSON_Delete(root);
            ws_resolve_and_send(sender_fd, CALL_BUSY_MSG, strlen(CALL_BUSY_MSG));
            return;
        }
        /* frei, als pending markieren und durchreichen */
        g_call_session.pending      = 1;
        g_call_session.caller_table = table;
        pthread_mutex_unlock(&g_lock);
        /* richtung: kellner ringt -> kunden-tisch, kunde ringt -> kellner.
         * nur ws_lock, g_lock ist oben schon frei */
        pthread_mutex_lock(&ws_lock);
        int is_sender_waiter = 0;
        for (int i = 0; i < ws_count; i++)
            if (ws_clients[i].conn.fd == sender_fd) {
                is_sender_waiter = ws_clients[i].is_waiter;
                break;
            }
        int target_fd = is_sender_waiter ? ws_find_table(table) : ws_find_waiter();
        pthread_mutex_unlock(&ws_lock);
        ws_logf("[route] type=%s sender_fd=%d is_sender_waiter=%d table=%d target_fd=%d",
                type, sender_fd, is_sender_waiter, table, target_fd);
        cJSON_Delete(root);
        ws_resolve_and_send(target_fd, msg, len);
        return;
    }
    if (strcmp(type, "webrtc_offer") == 0) {
        /* offer an die gegenseite, je nach rolle (nicht fix an den kellner).
         * waiter-initiiert: kellner ist offerer, offer geht zum kunden */
        int is_sender_waiter = 0;
        pthread_mutex_lock(&ws_lock);
        for (int i = 0; i < ws_count; i++)
            if (ws_clients[i].conn.fd == sender_fd) { is_sender_waiter = ws_clients[i].is_waiter; break; }
        pthread_mutex_unlock(&ws_lock);
        int offer_target;
        if (is_sender_waiter) {
            pthread_mutex_lock(&g_lock);
            offer_target = g_call_session.active ? g_call_session.customer_fd : -1;
            pthread_mutex_unlock(&g_lock);
        } else {
            pthread_mutex_lock(&g_lock);
            offer_target = g_call_session.active ? g_call_session.waiter_fd : -1;
            pthread_mutex_unlock(&g_lock);
        }
        ws_logf("[route] type=%s sender_fd=%d is_sender_waiter=%d call_active=%d offer_target=%d",
                type, sender_fd, is_sender_waiter, g_call_session.active, offer_target);
        cJSON_Delete(root);
        ws_resolve_and_send(offer_target, msg, len);
        return;
    }
    if (strcmp(type, "call_accept") == 0) {
        /* caller_table lesen bevor wir irgendwas schreiben */
        pthread_mutex_lock(&g_lock);
        int caller_table = g_call_session.caller_table;
        pthread_mutex_unlock(&g_lock);

        /* wer hat accepted? davon hängt ab welcher fd kellner und welcher kunde ist.
         * nur ws_lock, vor g_lock. nie beide gleichzeitig */
        pthread_mutex_lock(&ws_lock);
        int is_acceptor_waiter = 0;
        for (int i = 0; i < ws_count; i++)
            if (ws_clients[i].conn.fd == sender_fd) {
                is_acceptor_waiter = ws_clients[i].is_waiter;
                break;
            }
        int waiter_fd_val   = is_acceptor_waiter ? sender_fd : ws_find_waiter();
        int customer_fd_val = is_acceptor_waiter ? ws_find_table(caller_table) : sender_fd;
        pthread_mutex_unlock(&ws_lock);

        /* alle felder in einem lock-window setzen, sonst gibts kurz active=1 mit customer_fd=0 */
        pthread_mutex_lock(&g_lock);
        g_call_session.active      = 1;
        g_call_session.pending     = 0;
        g_call_session.waiter_fd   = waiter_fd_val;
        g_call_session.customer_fd = customer_fd_val;
        g_call_session.started_at  = time(NULL);
        pthread_mutex_unlock(&g_lock);

        /* den benachrichtigen der den call gestartet hat, nicht den acceptor.
         * customer-initiiert -> an den kunden, waiter-initiiert -> an den kellner */
        int notify_fd = is_acceptor_waiter ? customer_fd_val : waiter_fd_val;
        ws_logf("[route] type=%s sender_fd=%d is_acceptor_waiter=%d caller_table=%d waiter_fd=%d customer_fd=%d notify_fd=%d",
                type, sender_fd, is_acceptor_waiter, caller_table, waiter_fd_val, customer_fd_val, notify_fd);
        cJSON_Delete(root);
        ws_resolve_and_send(notify_fd, msg, len);
        return;
    }
    if (strcmp(type, "call_reject") == 0) {
        /* session platt und kunden bescheid geben */
        pthread_mutex_lock(&g_lock);
        int caller_table = g_call_session.caller_table;
        memset(&g_call_session, 0, sizeof(g_call_session)); /* pending + active weg */
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_lock(&ws_lock);
        int cfd = ws_find_table(caller_table);
        pthread_mutex_unlock(&ws_lock);
        ws_logf("[route] type=%s caller_table=%d cfd=%d", type, caller_table, cfd);
        cJSON_Delete(root);
        ws_resolve_and_send(cfd, msg, len);
        return;
    }
    if (strcmp(type, "client_error") == 0) {
        /* nur zum loggen von client-fehlern (z.b. getUserMedia), kein relay an andere */
        cJSON *where_j = cJSON_GetObjectItemCaseSensitive(root, "where");
        cJSON *name_j = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *message_j = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *where_s = cJSON_IsString(where_j) ? where_j->valuestring : "?";
        const char *name_s = cJSON_IsString(name_j) ? name_j->valuestring : "?";
        const char *message_s = cJSON_IsString(message_j) ? message_j->valuestring : "?";
        ws_logf("[client] sender_fd=%d where=%s name=%s message=%s", sender_fd, where_s, name_s, message_s);
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type, "call_end") == 0) {
        /* gegenseite bescheid geben und session aufräumen, auch noch im pending-zustand
         * (kunde bricht ab bevor der kellner accepted hat) */
        pthread_mutex_lock(&g_lock);
        if (!g_call_session.active && !g_call_session.pending) {
            pthread_mutex_unlock(&g_lock);
            cJSON_Delete(root);
            return;
        }
        int from_active = g_call_session.active;
        int other_fd;
        if (g_call_session.active) {
            /* läuft, andere seite aus den session-fds */
            other_fd = (g_call_session.customer_fd == sender_fd)
                       ? g_call_session.waiter_fd
                       : g_call_session.customer_fd;
        } else {
            /* pending, waiter_fd noch 0, also kellner in ws_clients suchen */
            pthread_mutex_lock(&ws_lock);
            other_fd = ws_find_waiter();
            pthread_mutex_unlock(&ws_lock);
        }
        memset(&g_call_session, 0, sizeof(g_call_session));
        pthread_mutex_unlock(&g_lock);
        ws_logf("[route] type=%s sender_fd=%d other_fd=%d from_active=%d",
                type, sender_fd, other_fd, from_active);
        cJSON_Delete(root);
        if (other_fd > 0)
            ws_resolve_and_send(other_fd, CALL_END_MSG, strlen(CALL_END_MSG));
        return;
    }
    if (strcmp(type, "webrtc_answer") == 0) {
        /* answer an die gegenseite, je nach rolle.
         * routing über g_call_session (bei call_accept gesetzt), in der answer-msg steckt keine table_number */
        int is_sender_waiter = 0;
        pthread_mutex_lock(&ws_lock);
        for (int i = 0; i < ws_count; i++)
            if (ws_clients[i].conn.fd == sender_fd) { is_sender_waiter = ws_clients[i].is_waiter; break; }
        pthread_mutex_unlock(&ws_lock);
        pthread_mutex_lock(&g_lock);
        int answer_target = g_call_session.active
                            ? (is_sender_waiter ? g_call_session.customer_fd : g_call_session.waiter_fd)
                            : -1;
        pthread_mutex_unlock(&g_lock);
        ws_logf("[route] type=%s sender_fd=%d is_sender_waiter=%d call_active=%d answer_target=%d",
                type, sender_fd, is_sender_waiter, g_call_session.active, answer_target);
        cJSON_Delete(root);
        ws_resolve_and_send(answer_target, msg, len);
        return;
    }
    if (strcmp(type, "webrtc_ice") == 0) {
        /* ice candidates, richtung je nachdem wer sendet */
        int is_sender_waiter = 0;
        int counterpart_fd   = -1;
        /* ws_lock nur für den is_waiter-lookup, dann los bevor g_lock kommt. nie beide zusammen */
        pthread_mutex_lock(&ws_lock);
        for (int i = 0; i < ws_count; i++)
            if (ws_clients[i].conn.fd == sender_fd) { is_sender_waiter = ws_clients[i].is_waiter; break; }
        pthread_mutex_unlock(&ws_lock);
        if (is_sender_waiter) {
            /* über g_call_session.customer_fd, nicht ws_find_table: ice-msgs haben keine table_number */
            pthread_mutex_lock(&g_lock);
            counterpart_fd = g_call_session.active ? g_call_session.customer_fd : -1;
            pthread_mutex_unlock(&g_lock);
        } else {
            pthread_mutex_lock(&g_lock);
            counterpart_fd = g_call_session.active ? g_call_session.waiter_fd : -1;
            pthread_mutex_unlock(&g_lock);
        }
        ws_logf("[route] type=%s sender_fd=%d is_sender_waiter=%d counterpart_fd=%d",
                type, sender_fd, is_sender_waiter, counterpart_fd);
        cJSON_Delete(root);
        ws_resolve_and_send(counterpart_fd, msg, len);
        return;
    }

    /* der ganze rest (get_menu, place_order usw.) geht an den app-handler */
    cJSON_Delete(root);
    /* msg noch gültig, cJSON_Delete fasst den originalpuffer nicht an */
    Conn sender = {-1, NULL};
    pthread_mutex_lock(&ws_lock);
    for (int i = 0; i < ws_count; i++)
        if (ws_clients[i].conn.fd == sender_fd) { sender = ws_clients[i].conn; break; }
    pthread_mutex_unlock(&ws_lock);
    if (sender.fd >= 0) ws_handle_message(&sender, msg);
}

/* frame-reader-schleife: läuft pro verbindung bis sie dicht ist */
void ws_loop(Conn *c) {
    uint8_t hdr[2];
    for (;;) {
        if (conn_recv_exact(c, hdr, 2) != 2) break;
        int opcode = hdr[0] & 0x0F;
        int masked = (hdr[1] & 0x80) != 0;
        uint64_t plen = hdr[1] & 0x7F;

        /* extended payload length lesen falls nötig */
        if (plen == 126) {
            uint8_t ex[2];
            if (conn_recv_exact(c, ex, 2) != 2) break;
            plen = ((uint64_t)ex[0] << 8) | ex[1];
        } else if (plen == 127) {
            uint8_t ex[8];
            if (conn_recv_exact(c, ex, 8) != 8) break;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ex[i];
        }

        if (plen > 65535) break; /* zu große frames ablehnen */

        /* buffer pro frame auf dem heap, nicht auf dem stack */
        char *payload = malloc(plen + 1);
        if (!payload) break;

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && conn_recv_exact(c, mask, 4) != 4) { free(payload); break; }

        if (conn_recv_exact(c, (uint8_t *)payload, plen) != (ssize_t)plen) {
            free(payload); break;
        }

        /* browser-clients müssen immer maskieren (rfc 6455) */
        if (masked)
            for (uint64_t i = 0; i < plen; i++)
                payload[i] ^= mask[i % 4];

        payload[plen] = '\0';

        if (opcode == 0x01) {
            /* text frame, normalfall */
            ws_route_signal(c->fd, payload, plen);
            free(payload);
        } else if (opcode == 0x08) {
            /* close */
            free(payload);
            break;
        } else if (opcode == 0x09) {
            /* ping, pong mit gleichem payload zurück */
            uint8_t pong_hdr[4];
            size_t phlen;
            pong_hdr[0] = 0x8A; /* FIN + pong opcode */
            if (plen < 126) {
                pong_hdr[1] = (uint8_t)plen; phlen = 2;
            } else {
                pong_hdr[1] = 126;
                pong_hdr[2] = (plen >> 8) & 0xFF;
                pong_hdr[3] = plen & 0xFF;
                phlen = 4;
            }
            conn_send(c, pong_hdr, phlen);
            if (plen > 0) conn_send(c, payload, plen);
            free(payload);
        } else {
            /* rest ignorieren */
            free(payload);
        }
    }
}
