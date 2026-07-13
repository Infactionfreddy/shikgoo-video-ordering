#pragma once
#include <openssl/ssl.h>
#include <sys/types.h>

/* verbindungs-handle, ssl oder plain socket.
 * ssl==NULL = plain fd (z.b. lokaler test ohne tls) */
typedef struct { int fd; SSL *ssl; } Conn;

ssize_t conn_send      (Conn *c, const void *buf, size_t len);
ssize_t conn_recv      (Conn *c,       void *buf, size_t len);
ssize_t conn_recv_exact(Conn *c,       void *buf, size_t len); /* liest genau len bytes, sonst weniger bei EOF/fehler */
void    conn_close     (Conn *c);
