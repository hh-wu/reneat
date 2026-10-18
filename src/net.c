/* net.c */
#include "net.h"

int net_init(void)
{
    static int done = 0;
    if (done) return 0;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2,2), &wd) != 0) { nlog("WSAStartup() failed"); return -1; }
    done = 1;
    return 0;
}

SOCKET net_connect(const char *host, int port, int timeout_ms)
{
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    nlog("net: resolving %s:%d ...", host, port);
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0) { nlog("net: getaddrinfo(%s) failed: %d", host, gai); return INVALID_SOCKET; }

    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        char ip[64] = "?";
        if (ai->ai_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in*)ai->ai_addr)->sin_addr, ip, sizeof(ip));
        else if (ai->ai_family == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)ai->ai_addr)->sin6_addr, ip, sizeof(ip));
        nlog("net: trying %s [%s]:%d ...", host, ip, port);
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) { nlog("net: socket() failed wsa=%d", WSAGetLastError()); continue; }
        /* non-blocking connect with timeout */
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set w; FD_ZERO(&w); FD_SET(s, &w);
            struct timeval tv = { timeout_ms/1000, (timeout_ms%1000)*1000 };
            if (select(0, NULL, &w, NULL, &tv) == 1) {
                int err=0, len=sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                if (err == 0) { nb=0; ioctlsocket(s, FIONBIO, &nb); nlog("net: connected to [%s]:%d (sock %d)", ip, port, (int)s); break; }
                nlog("net: connect to [%s] refused/err %d", ip, err);
            } else {
                nlog("net: connect to [%s] timed out after %dms", ip, timeout_ms);
            }
            closesocket(s); s = INVALID_SOCKET; continue;
        } else if (r == 0) { nb=0; ioctlsocket(s, FIONBIO, &nb); break; }
        nlog("net: connect to [%s] failed wsa=%d", ip, WSAGetLastError());
        closesocket(s); s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        int rcvbuf = 256*1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
    } else {
        nlog("net: all addresses for %s failed", host);
    }
    return s;
}

int net_send_all(SOCKET s, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int r = send(s, buf+sent, len-sent, 0);
        if (r <= 0) { nlog("net: send(sock %d) failed after %d/%d bytes, wsa=%d", (int)s, sent, len, WSAGetLastError()); return -1; }
        sent += r;
    }
    return 0;
}

int net_recv_timeout(SOCKET s, char *buf, int len, int timeout_ms)
{
    fd_set r; FD_ZERO(&r); FD_SET(s, &r);
    struct timeval tv = { timeout_ms/1000, (timeout_ms%1000)*1000 };
    int sel = select(0, &r, NULL, NULL, timeout_ms >= 0 ? &tv : NULL);
    if (sel <= 0) {
        if (sel < 0) nlog("net: select(sock %d) error wsa=%d", (int)s, WSAGetLastError());
        return sel; /* 0 timeout, <0 err */
    }
    int n = recv(s, buf, len, 0);
    if (n == 0) { nlog("net: peer closed connection (sock %d)", (int)s); return -1; }
    if (n < 0) { nlog("net: recv(sock %d) failed wsa=%d", (int)s, WSAGetLastError()); return -1; }
    return n;
}

void net_close(SOCKET s)
{
    if (s != INVALID_SOCKET) { shutdown(s, SD_BOTH); closesocket(s); }
}
