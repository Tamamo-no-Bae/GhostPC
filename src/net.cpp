// xnet: the Xbox Winsock subset maps 1:1 onto ws2_32 (used by the Havok visual debugger)
#include "common.h"
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

XSTD(int, XNetStartup, 4, (const void *)) { return 0; }
XSTD(int, WSAStartup, 8, (WORD v, LPWSADATA d)) { return WSAStartup(v, d); }
XSTD(int, WSACleanup, 0, ()) { return WSACleanup(); }
XSTD(int, WSAGetLastError, 0, ()) { return WSAGetLastError(); }
XSTD(int, __WSAFDIsSet, 8, (SOCKET s, fd_set *f)) { return __WSAFDIsSet(s, f); }
// No sockets: the only users are the Havok visual debugger server and system link play;
// opening a listening port would also trigger a Windows Firewall prompt.
XSTD(SOCKET, socket, 12, (int a, int t, int p)) { WSASetLastError(WSAENETDOWN); return INVALID_SOCKET; }
XSTD(int, bind, 12, (SOCKET s, const sockaddr *a, int n)) { return bind(s, a, n); }
XSTD(int, listen, 8, (SOCKET s, int b)) { return listen(s, b); }
XSTD(SOCKET, accept, 12, (SOCKET s, sockaddr *a, int *n)) { return accept(s, a, n); }
XSTD(int, closesocket, 4, (SOCKET s)) { return closesocket(s); }
XSTD(u_short, htons, 4, (u_short v)) { return htons(v); }
XSTD(int, recv, 16, (SOCKET s, char *b, int n, int f)) { return recv(s, b, n, f); }
XSTD(int, send, 16, (SOCKET s, const char *b, int n, int f)) { return send(s, b, n, f); }
XSTD(int, select, 20, (int n, fd_set *r, fd_set *w, fd_set *e, const timeval *t)) { return select(n, r, w, e, t); }
XSTD(int, setsockopt, 20, (SOCKET s, int l, int o, const char *v, int n)) { return setsockopt(s, l, o, v, n); }
