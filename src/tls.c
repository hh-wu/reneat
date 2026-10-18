/* tls.c — Schannel client: AcquireCredentialsHandle -> InitializeSecurityContext
 * loop ("SSLHandShake" in NeatDM logs), then EncryptMessage / DecryptMessage. */
#include "tls.h"
#include "net.h"

static wchar_t *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int ensure_cred(Tls *t)
{
    if (t->has_cred) return 0;
    SCHANNEL_CRED cred = {0};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    /* TLS 1.3 over Schannel has post-handshake quirks (session tickets,
     * KeyUpdate); pin to 1.2 for maximum server compatibility, like NeatDM */
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
    cred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                   SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    SECURITY_STATUS st = AcquireCredentialsHandleW(NULL, (SEC_WCHAR*)UNISP_NAME_W,
        SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, &t->cred, NULL);
    if (st != SEC_E_OK) {
        /* fallback: no validation (some corp proxies / odd chains) */
        cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
        st = AcquireCredentialsHandleW(NULL, (SEC_WCHAR*)UNISP_NAME_W,
            SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, &t->cred, NULL);
        if (st != SEC_E_OK) { nlog("AcquireCredentialsHandle failed: 0x%lx", st); return -1; }
    }
    t->has_cred = 1;
    return 0;
}

int tls_handshake(SOCKET s, Tls *t, const char *hostname)
{
    memset(t, 0, sizeof(*t));
    if (ensure_cred(t) != 0) return -1;
    nlog("Initializing SSLHandShake for Socket (%d)", (int)s);

    wchar_t *whost = to_wide(hostname);
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                  ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
                  ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    SECURITY_STATUS st = SEC_I_CONTINUE_NEEDED;
    int have_input = 0;

    while (1) {
        SecBuffer outbuf = {0, SECBUFFER_TOKEN, NULL};
        SecBufferDesc outdesc = {SECBUFFER_VERSION, 1, &outbuf};
        SecBuffer inbuf[2] = {
            {t->enc_len, SECBUFFER_TOKEN, t->enc},
            {0, SECBUFFER_EMPTY, NULL}
        };
        SecBufferDesc indesc = {SECBUFFER_VERSION, 2, inbuf};

        DWORD retflags = 0;
        st = InitializeSecurityContextW(&t->cred, have_input ? &t->ctx : NULL,
            whost, flags, 0, 0, have_input ? &indesc : NULL, 0,
            have_input ? NULL : &t->ctx, &outdesc, &retflags, NULL);
        nlog("hs: ISC st=0x%08lx outbuf=%u have_input=%d", st, outbuf.cbBuffer, have_input);
        if (FAILED(st) && have_input && t->enc_len > 0) {
            int dumpn = t->enc_len < 300 ? t->enc_len : 300;
            char dump[301]; memcpy(dump, t->enc, dumpn); dump[dumpn] = 0;
            for (int i = 0; i < dumpn; i++) if ((unsigned char)dump[i] < 0x20 && dump[i] != '\r' && dump[i] != '\n') dump[i] = '.';
            nlog("hs: non-TLS input (%d bytes): %s", t->enc_len, dump);
        }
        if (!have_input && (st == SEC_I_CONTINUE_NEEDED || st == SEC_E_OK)) t->has_ctx = 1;

        /* consume extra */
        if (inbuf[1].BufferType == SECBUFFER_EXTRA && inbuf[1].cbBuffer > 0) {
            memmove(t->enc, t->enc + (t->enc_len - inbuf[1].cbBuffer), inbuf[1].cbBuffer);
            t->enc_len = inbuf[1].cbBuffer;
        } else if (st != SEC_E_INCOMPLETE_MESSAGE) {
            t->enc_len = 0;
        }

        if (st == SEC_E_OK) {
            if (outbuf.cbBuffer && outbuf.pvBuffer)
                net_send_all(s, outbuf.pvBuffer, outbuf.cbBuffer);
            if (outbuf.pvBuffer) FreeContextBuffer(outbuf.pvBuffer);
            break;
        }
        if (st == SEC_I_CONTINUE_NEEDED || st == SEC_E_INCOMPLETE_MESSAGE ||
            st == SEC_I_INCOMPLETE_CREDENTIALS) {
            /* TLS 1.3 often returns CONTINUE_NEEDED with no output token:
             * it just wants more handshake bytes */
            if (outbuf.cbBuffer && outbuf.pvBuffer) {
                if (net_send_all(s, outbuf.pvBuffer, outbuf.cbBuffer) != 0) {
                    FreeContextBuffer(outbuf.pvBuffer); nlog("hs: send failed"); goto fail;
                }
            }
            if (outbuf.pvBuffer) FreeContextBuffer(outbuf.pvBuffer);
            /* read more encrypted data */
            int n = net_recv_timeout(s, t->enc + t->enc_len, (int)sizeof(t->enc) - t->enc_len, 15000);
            if (n <= 0) { nlog("hs: recv failed/timeout n=%d wsa=%d", n, WSAGetLastError()); goto fail; }
            t->enc_len += n;
            have_input = 1;
            continue;
        }
        nlog("SSLHandShake Failed for Socket (%d) st=0x%lx", (int)s, st);
        goto fail;
    }
    free(whost);
    if (QueryContextAttributesW(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes) != SEC_E_OK) goto fail0;
    /* log negotiated protocol + cipher, invaluable for TLS debugging */
    {
        SecPkgContext_ConnectionInfo ci = {0};
        if (QueryContextAttributesW(&t->ctx, SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK) {
            const char *proto =
                ci.dwProtocol == SP_PROT_TLS1_3_CLIENT ? "TLS 1.3" :
                ci.dwProtocol == SP_PROT_TLS1_2_CLIENT ? "TLS 1.2" :
                ci.dwProtocol == SP_PROT_TLS1_1_CLIENT ? "TLS 1.1" : "TLS ?";
            nlog("SSLHandShake ended Successfully (%s, cipher 0x%x, exch 0x%x)",
                 proto, ci.aiCipher, ci.aiExch);
        } else {
            nlog("SSLHandShake ended Successfully");
        }
    }
    return 0;
fail:
    free(whost);
fail0:
    tls_free(t);
    return -1;
}

int tls_send(SOCKET s, Tls *t, const char *buf, int len)
{
    int sent = 0;
    int maxmsg = t->sizes.cbMaximumMessage > 0 ? (int)t->sizes.cbMaximumMessage : 16384;
    while (sent < len) {
        int chunk = len - sent; if (chunk > maxmsg) chunk = maxmsg;
        int total = t->sizes.cbHeader + chunk + t->sizes.cbTrailer;
        char *msg = malloc(total);
        SecBuffer bufs[4] = {
            {t->sizes.cbHeader, SECBUFFER_STREAM_HEADER, msg},
            {chunk, SECBUFFER_DATA, msg + t->sizes.cbHeader},
            {t->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, msg + t->sizes.cbHeader + chunk},
            {0, SECBUFFER_EMPTY, NULL}
        };
        memcpy(bufs[1].pvBuffer, buf + sent, chunk);
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, bufs};
        SECURITY_STATUS st = EncryptMessage(&t->ctx, 0, &desc, 0);
        if (st != SEC_E_OK) { nlog("tls: EncryptMessage failed st=0x%lx", st); free(msg); return -1; }
        int outlen = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        int r = net_send_all(s, msg, outlen);
        free(msg);
        if (r != 0) return -1;
        sent += chunk;
    }
    return 0;
}

int tls_recv(SOCKET s, Tls *t, char *buf, int len, int timeout_ms)
{
    /* serve from buffered plaintext first */
    if (t->plain_off < t->plain_len) {
        int n = t->plain_len - t->plain_off; if (n > len) n = len;
        memcpy(buf, t->plain + t->plain_off, n);
        t->plain_off += n;
        if (t->plain_off == t->plain_len) t->plain_off = t->plain_len = 0;
        return n;
    }
    while (1) {
        int n = net_recv_timeout(s, t->enc + t->enc_len, (int)sizeof(t->enc) - t->enc_len, timeout_ms);
        if (n == 0) { nlog("tls: recv timeout after %dms (sock %d, enc_pending=%d)", timeout_ms, (int)s, t->enc_len); return 0; }
        if (n < 0) return t->enc_len ? 0 : -1; /* closed (already logged by net layer) */
        t->enc_len += n;
        while (t->enc_len > 0) {
            SecBuffer bufs[4] = {
                {t->enc_len, SECBUFFER_DATA, t->enc},
                {0, SECBUFFER_EMPTY, NULL},
                {0, SECBUFFER_EMPTY, NULL},
                {0, SECBUFFER_EMPTY, NULL}
            };
            SecBufferDesc desc = {SECBUFFER_VERSION, 4, bufs};
            SECURITY_STATUS st = DecryptMessage(&t->ctx, &desc, 0, NULL);
            if (st == SEC_E_INCOMPLETE_MESSAGE) break; /* need more bytes */
            if (st == SEC_I_CONTEXT_EXPIRED) { nlog("tls: context expired / close_notify (sock %d)", (int)s); return 0; }
            if (st == SEC_I_RENEGOTIATE) {
                nlog("tls: server requested renegotiation (sock %d) — unsupported", (int)s);
                return -1;
            }
            if (st != SEC_E_OK && st != SEC_I_CONTEXT_EXPIRED) {
                nlog("tls: DecryptMessage failed st=0x%lx (sock %d, enc_pending=%d)", st, (int)s, t->enc_len);
                return -1;
            }
            /* figure out how much input was consumed: data + extra */
            int extra = 0;
            for (int i = 1; i < 4; i++)
                if (bufs[i].BufferType == SECBUFFER_EXTRA) extra = bufs[i].cbBuffer;
            int consumed = t->enc_len - extra;
            /* copy plaintext BEFORE any memmove: pdata points into t->enc! */
            int plen = 0;
            char *pdata = NULL;
            for (int i = 0; i < 4; i++)
                if (bufs[i].BufferType == SECBUFFER_DATA) { pdata = bufs[i].pvBuffer; plen = bufs[i].cbBuffer; }
            if (plen > 0) {
                int out = plen < len ? plen : len;
                memcpy(buf, pdata, out);
                int rem = plen - out;
                if (rem > 0 && rem <= (int)sizeof(t->plain)) {
                    memcpy(t->plain, pdata + out, rem);
                    t->plain_off = 0; t->plain_len = rem;
                }
                /* drop consumed bytes only after plaintext is copied out */
                if (consumed > 0 && extra > 0) {
                    memmove(t->enc, t->enc + consumed, extra);
                    t->enc_len = extra;
                } else {
                    t->enc_len = 0;
                }
                return out;
            }
            if (consumed > 0) {
                memmove(t->enc, t->enc + consumed, t->enc_len - consumed);
                t->enc_len -= consumed;
            }
            if (consumed == 0) break; /* no progress */
        }
    }
}

void tls_free(Tls *t)
{
    if (t->has_ctx) { DeleteSecurityContext(&t->ctx); t->has_ctx = 0; }
    if (t->has_cred) { FreeCredentialsHandle(&t->cred); t->has_cred = 0; }
}
