/* tls.h — Schannel (SSPI) TLS client, like NeatDM's NeatSocket SSL layer */
#ifndef RENEAT_TLS_H
#define RENEAT_TLS_H
#include "common.h"
#include <winsock2.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

typedef struct {
    CredHandle  cred;
    CtxtHandle  ctx;
    int         has_cred;
    int         has_ctx;
    SecPkgContext_StreamSizes sizes;
    /* decrypted-app-data buffered from a previous DecryptMessage */
    char   plain[16384 + 1024];
    int    plain_len;
    int    plain_off;
    /* encrypted bytes received but not yet consumed */
    char   enc[32768];
    int    enc_len;
} Tls;

int  tls_handshake(SOCKET s, Tls *t, const char *hostname);   /* 0 ok */
int  tls_send(SOCKET s, Tls *t, const char *buf, int len);    /* 0 ok */
int  tls_recv(SOCKET s, Tls *t, char *buf, int len, int timeout_ms); /* >0, 0 closed, -1 err */
void tls_free(Tls *t);

#endif
