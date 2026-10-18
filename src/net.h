/* net.h — raw Winsock wrapper (NeatSocket equivalent) */
#ifndef RENEAT_NET_H
#define RENEAT_NET_H
#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>

int  net_init(void);                      /* WSAStartup, once */
SOCKET net_connect(const char *host, int port, int timeout_ms); /* INVALID_SOCKET on fail */
int  net_send_all(SOCKET s, const char *buf, int len);          /* 0 ok */
int  net_recv_timeout(SOCKET s, char *buf, int len, int timeout_ms); /* >0 read, 0 timeout/closed, -1 err */
void net_close(SOCKET s);

#endif
