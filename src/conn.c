/* ssl/plain socket wrapper, gleiche api für beide */
#include "conn.h"
#include <sys/socket.h>
#include <unistd.h>

/* schreibt auf ssl oder plain, aufrufer merkt keinen unterschied */
ssize_t conn_send(Conn *c, const void *buf, size_t len) {
    if (c->ssl)
        return (ssize_t)SSL_write(c->ssl, buf, (int)len);
    return send(c->fd, buf, len, MSG_NOSIGNAL);
}

ssize_t conn_recv(Conn *c, void *buf, size_t len) {
    if (c->ssl)
        return (ssize_t)SSL_read(c->ssl, buf, (int)len);
    return recv(c->fd, buf, len, 0);
}

/* liest exakt len bytes, ws frame parsing braucht feste längen */
ssize_t conn_recv_exact(Conn *c, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = conn_recv(c, (char *)buf + got, len - got);
        if (n <= 0) return got > 0 ? (ssize_t)got : n;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* ssl sauber zu machen bevor der fd zugeht */
void conn_close(Conn *c) {
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    close(c->fd);
    c->fd = -1;
}
