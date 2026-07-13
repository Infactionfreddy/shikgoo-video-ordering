/* shikgoo backend, http + websocket server
 * nur posix + pthreads + openssl, kein node/nginx, nix externes
 * https auf :8443, plain http :80 leitet weiter (braucht sudo)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "types.h"
#include "conn.h"
#include "state.h"
#include "http.h"
#include "ws.h"
#include "menu.h"
#include "store.h"
#include "api.h"

/* certs liegen auf dem pi hier, lokal musst du das anpassen */
#define CERT_PATH   "/opt/shikgoo/certs/cert.pem"
#define KEY_PATH    "/opt/shikgoo/certs/key.pem"
#define HTTP_PORT   80  /* port 80 braucht root */

static SSL_CTX *g_ssl_ctx = NULL;

/* plain-http rein, 301 auf https raus. kein ssl, wär sinnlos fuer nen redirect */
static void *handle_http_redirect(void *arg) {
    int fd = (int)(intptr_t)arg;
    pthread_detach(pthread_self());

    char buf[2048];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(fd); return NULL; }
    buf[n] = '\0';

    /* pfad aus zeile 1, "GET /foo HTTP/1.1" */
    char path[256] = "/";
    sscanf(buf, "%*s %255s", path);

    /* host-header, port-suffix (:80 etc) abschneiden */
    char host[128] = "shikgoo.local";
    const char *h = strstr(buf, "Host: ");
    if (h) {
        h += 6;
        const char *end = strpbrk(h, ":\r\n");
        size_t len = end ? (size_t)(end - h) : strlen(h);
        if (len > 0 && len < sizeof(host)) {
            memcpy(host, h, len);
            host[len] = '\0';
        }
    }

    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: https://%s:%d%s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        host, PORT, path);
    send(fd, resp, (size_t)rlen, 0);
    close(fd);
    return NULL;
}

/* eigener listener nur fuers umleiten. muss ein extra thread sein weil main
 * dauerhaft im accept() auf :8443 haengt */
static void *http_redirect_loop(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("http socket"); return NULL; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("http bind (port 80 braucht sudo)"); close(srv); return NULL;
    }
    if (listen(srv, 32) < 0) {
        perror("http listen"); close(srv); return NULL;
    }
    printf("SHIKGOO HTTP redirect on :%d → https://...:%d\n", HTTP_PORT, PORT);

    /* wie die hauptschleife, nur ohne tls */
    for (;;) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int fd = accept(srv, (struct sockaddr *)&cli, &cli_len);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); /* kein nagle delay */
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_http_redirect, (void *)(intptr_t)fd) != 0)
            close(fd);
    }
    return NULL;
}

/* ein thread pro verbindung */
static void *handle_conn(void *arg) {
    int fd = (int)(intptr_t)arg;
    pthread_detach(pthread_self());

    /* handshake zuerst, sonst kann man den request nicht lesen */
    SSL *ssl = SSL_new(g_ssl_ctx);
    if (!ssl) { close(fd); return NULL; }
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    Conn c = { fd, ssl };

    Req req;
    if (parse_req(&c, &req) < 0) { conn_close(&c); return NULL; }

    /* kurze aliase, sonst wird das routing unlesbar */
    const char *m = req.method, *p = req.path, *b = req.body;

    /* ws upgrade? ab in die ws-loop, danach ist der thread durch */
    if (req.is_ws && strncmp(p, "/ws/notifications", 17) == 0) {
        int is_waiter = 0, table_number = 0;
        const char *qs = strchr(p, '?');
        if (qs) {
            if (strstr(qs, "role=waiter")) is_waiter = 1;
            const char *t = strstr(qs, "table=");
            if (t) table_number = (int)strtol(t + 6, NULL, 10);
            if (table_number < 0 || table_number > 20) table_number = 0; /* nur tisch 1..20, sonst 0 = kein tisch */
        }
        if (ws_handshake(&c, req.ws_key) == 0) {
            if (ws_add(&c, table_number, is_waiter) == 0) { /* abweisen wenn zu viele verbunden */
                ws_loop(&c);
                ws_remove(&c);
            }
            /* ws_add hat den grund schon ins stderr geloggt */
        }
        conn_close(&c);
        return NULL;
    }

    /* cors preflight, kommt vor jedem cross-origin request */
    if (strcmp(m, "OPTIONS") == 0) {
        const char *h =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET,POST,PATCH,DELETE,OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n\r\n";
        conn_send(&c, h, strlen(h));
    }
    /* cert.pem, damit neue tablets das self-signed cert installieren */
    else if (strcmp(p, "/cert.pem") == 0)
        serve_file(&c, CERT_PATH);
    else if (strcmp(p, "/api/menu") == 0) {
        if (strcmp(m, "GET") == 0) api_get_menu(&c);
        else json_resp(&c, 405, "{\"error\":\"Method not allowed\"}");
    }
    else if (strcmp(p, "/api/orders") == 0) {
        if (strcmp(m, "POST") == 0) api_post_order(&c, b);
        else json_resp(&c, 405, "{\"error\":\"Method not allowed\"}");
    }
    else if (strncmp(p, "/api/orders/", 12) == 0) {
        if      (strcmp(m, "GET")   == 0) api_get_order(&c, p);
        else if (strcmp(m, "PATCH") == 0) api_patch_order(&c, p, b);
        else json_resp(&c, 405, "{\"error\":\"Method not allowed\"}");
    }
    else if (strcmp(p, "/api/waiter/status") == 0) {
        api_waiter_status(&c, m, b);
    }
    else if (strcmp(p, "/api/waiter/call") == 0) {
        if (strcmp(m, "POST") == 0) api_call_waiter(&c, b);
        else json_resp(&c, 405, "{\"error\":\"Method not allowed\"}");
    }
    else if (strcmp(p, "/api/waiter/calls") == 0) {
        if (strcmp(m, "GET") == 0) api_get_calls(&c);
        else json_resp(&c, 405, "{\"error\":\"Method not allowed\"}");
    }
    else if (strncmp(p, "/api/waiter/calls/", 18) == 0) {
        if (strcmp(m, "DELETE") == 0) api_resolve_call(&c, p);
        else json_resp(&c, 405, "{\"error\":\"Method not allowed\"}");
    }
    else if (strcmp(p, "/api/waiter/queue") == 0) {
        if (strcmp(m, "GET") == 0) api_get_queue(&c);
        else json_resp(&c, 405, "{\"error\":\"Method not allowed\"}");
    }
    /* frontend ausliefern */
    else if (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0)
        serve_file(&c, FRONTEND_DIR "/index.html");
    else if (strcmp(p, "/customer") == 0 || strncmp(p, "/customer/", 10) == 0)
        serve_file(&c, FRONTEND_DIR "/customer.html");
    else if (strcmp(p, "/waiter") == 0 || strcmp(p, "/waiter/") == 0)
        serve_file(&c, FRONTEND_DIR "/waiter.html");
    else if (p[0] == '/') {
        /* rest (css/js/bilder) direkt aus dem frontend-ordner */
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s%s", FRONTEND_DIR, p);
        serve_file(&c, fpath);
    }
    else {
        json_resp(&c, 404, "{\"error\":\"Not found\"}");
    }

    conn_close(&c);
    return NULL;
}

int main(void) {
    /* sigpipe ignorieren, sonst killt ein abbrechender client den prozess */
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    /* menü + state laden bevor wir lauschen */
    if (load_menu() <= 0) {
        fprintf(stderr, "Cannot open menu.json — run from project root.\n");
        return 1;
    }
    printf("Loaded %d menu items.\n", menu_count);

    int restored = load_orders();
    if (restored > 0)
        printf("Restored %d orders from orders.json.\n", restored);

    int restored_calls = load_calls();
    if (restored_calls > 0)
        printf("Restored %d active calls from calls.json.\n", restored_calls);

    /* ssl ctx einmal, alle threads teilen ihn */
    g_ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!g_ssl_ctx) { ERR_print_errors_fp(stderr); return 1; }
    if (SSL_CTX_use_certificate_file(g_ssl_ctx, CERT_PATH, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr); return 1;
    }
    if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, KEY_PATH, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr); return 1;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); /* neustart nach crash ohne wartezeit */

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }

    printf("SHIKGOO listening on :%d\n", PORT);

    /* redirect-listener im hintergrund, haengt an port 80 */
    pthread_t redirect_tid;
    pthread_create(&redirect_tid, NULL, http_redirect_loop, NULL);
    pthread_detach(redirect_tid);

    /* accept, thread, weiter */
    for (;;) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int fd = accept(srv, (struct sockaddr *)&cli, &cli_len);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); /* latenz runter */
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_conn, (void *)(intptr_t)fd) != 0)
            close(fd); /* thread ging schief, fd trotzdem zu */
    }
    return 0;
}
