#pragma once
/* Windows POSIX socket compatibility — maps <sys/socket.h> to Winsock2 */
#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/types.h>
#include <io.h>
#include <fcntl.h>

/* Windows opens files in text mode by default (\r\n -> \n), which corrupts
   Content-Length in HTTP responses. Force binary mode for all file opens. */
#undef  O_RDONLY
#define O_RDONLY (_O_RDONLY | _O_BINARY)

/* Defined in src/win_init.c; handles both CRT fds and WinSock sockets */
int _compat_close(int fd);

/* Winsock setsockopt/send/recv take char* but POSIX code passes void* or uint8_t*.
   Wrappers cast to silence signedness warnings without touching source files. */
static inline int _compat_setsockopt(SOCKET s, int level, int optname,
                                     const void *optval, int optlen) {
    return (setsockopt)(s, level, optname, (const char *)optval, optlen);
}
#define setsockopt(s, l, o, v, sl) _compat_setsockopt((s), (l), (o), (v), (sl))

static inline int _compat_send(SOCKET s, const void *buf, int len, int flags) {
    return (send)(s, (const char *)buf, len, flags);
}
#define send(s, b, l, f) _compat_send((SOCKET)(s), (b), (int)(l), (f))

static inline int _compat_recv(SOCKET s, void *buf, int len, int flags) {
    return (recv)(s, (char *)buf, len, flags);
}
#define recv(s, b, l, f) _compat_recv((SOCKET)(s), (b), (int)(l), (f))
