#ifdef _WIN32

/* winsock init/cleanup, laeuft automatisch per constructor/destructor.
 * nur fuer windows-builds, pi/linux braucht das nicht */

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <io.h>

/* WSAStartup muss vor dem ersten socket laufen, so braucht main() kein init */
__attribute__((constructor))
static void wsa_init(void) {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
}

__attribute__((destructor))
static void wsa_cleanup(void) {
    WSACleanup();
}

/* close() auf windows muss CRT-fd und winsock-handle auseinanderhalten.
 * unter linux ist beides close(), drum gibts den wrapper nur hier */
int _compat_close(int fd) {
    /* echter CRT-fd -> _close. sonst socket -> closesocket, sonst leakt das handle */
    if (_get_osfhandle(fd) != (intptr_t)(-1))
        return _close(fd);
    return closesocket((SOCKET)(uintptr_t)(unsigned int)fd);
}

#endif /* _WIN32 */
