/* ws.c — hand-rolled WebSocket server on 127.0.0.1 (like NeatDM's
 * NeatWebSocketServer / NeatWebSocketListener).  Browser extensions POST
 * downloads as JSON text frames: {"url":"...", "referer":"...", "cookies":"...",
 * "useragent":"...", "filename":"..."} and get {"result":"ok"} back.
 *
 * Only what's needed: HTTP upgrade handshake (SHA1 via BCrypt, like NeatDM's
 * bcrypt usage), text/ping/close frames, masked client payloads. */
#include "ws.h"
#include "net.h"
#include "util.h"
#include "settings.h"
#include <bcrypt.h>

static SOCKET  g_listen = INVALID_SOCKET;
static HANDLE  g_thread = NULL;
static volatile int g_stop = 0;
static int     g_port = 0;

/* ---- SHA1 + base64 for Sec-WebSocket-Accept ---- */

static int sha1(const char *data, int len, unsigned char out[20])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = -1;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) != 0) return -1;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) != 0) goto done;
    if (BCryptHashData(h, (PUCHAR)data, len, 0) != 0) goto done;
    if (BCryptFinishHash(h, out, 20, 0) == 0) ok = 0;
done:
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static void b64enc(const unsigned char *in, int len, char *out, int outlen)
{
    static const char *b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < len && o < outlen-4; i += 3) {
        unsigned v = in[i]<<16 | (i+1<len ? in[i+1]<<8 : 0) | (i+2<len ? in[i+2] : 0);
        out[o++] = b[v>>18]; out[o++] = b[(v>>12)&63];
        out[o++] = i+1<len ? b[(v>>6)&63] : '=';
        out[o++] = i+2<len ? b[v&63] : '=';
    }
    out[o] = 0;
}

/* ---- minimal JSON string-field extractor ---- */

static int json_str(const char *json, const char *key, char *out, int outlen)
{
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return -1;
    p++;
    int o = 0;
    while (*p && *p != '"' && o < outlen-1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                case 'r': out[o++] = '\r'; break;
                case '/': out[o++] = '/'; break;
                case '\\': out[o++] = '\\'; break;
                case '"': out[o++] = '"'; break;
                case 'u':
                    if (p[1] && p[2] && p[3] && p[4]) {
                        int cp; sscanf(p+1, "%4x", &cp);
                        if (cp < 0x80) out[o++] = (char)cp;
                        else { /* encode as UTF-8 */
                            if (cp < 0x800) { out[o++] = (char)(0xC0|(cp>>6)); out[o++] = (char)(0x80|(cp&63)); }
                            else { out[o++] = (char)(0xE0|(cp>>12)); out[o++] = (char)(0x80|((cp>>6)&63)); out[o++] = (char)(0x80|(cp&63)); }
                        }
                        p += 4;
                    }
                    break;
                default: out[o++] = *p; break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return 0;
}

/* ---- websocket framing ---- */

static int ws_send_frame(SOCKET s, int opcode, const char *payload, int len)
{
    char hdr[10]; int h = 0;
    hdr[h++] = (char)(0x80 | opcode);
    if (len < 126) hdr[h++] = (char)len;
    else if (len <= 0xffff) { hdr[h++] = 126; hdr[h++] = (char)(len>>8); hdr[h++] = (char)(len&0xff); }
    else return -1;
    if (net_send_all(s, hdr, h) != 0) return -1;
    return len ? net_send_all(s, payload, len) : 0;
}

/* read one frame; returns opcode, payload (unmasked) into buf, or -1 */
static int ws_read_frame(SOCKET s, char *buf, int bufsz, int *outlen)
{
    char h[2];
    if (net_recv_timeout(s, h, 2, 60000) != 2) return -1;
    int opcode = h[0] & 0x0f;
    int masked = (h[1] & 0x80) != 0;
    int64_t len = h[1] & 0x7f;
    if (len == 126) {
        char e[2]; if (net_recv_timeout(s, e, 2, 60000) != 2) return -1;
        len = ((unsigned char)e[0]<<8) | (unsigned char)e[1];
    } else if (len == 127) {
        char e[8]; if (net_recv_timeout(s, e, 8, 60000) != 8) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len<<8) | (unsigned char)e[i];
    }
    char mask[4] = {0};
    if (masked && net_recv_timeout(s, mask, 4, 60000) != 4) return -1;
    if (len > bufsz-1) { /* drain and bail */
        nlog("ws: frame too big (%lld)", len);
        int64_t left = len; char tmp[1024];
        while (left > 0) { int n = net_recv_timeout(s, tmp, (int)nmin64(1024,left), 60000); if (n<=0) break; left -= n; }
        return -1;
    }
    int got = 0;
    while (got < len) {
        int n = net_recv_timeout(s, buf+got, (int)(len-got), 60000);
        if (n <= 0) return -1;
        got += n;
    }
    if (masked)
        for (int i = 0; i < len; i++) buf[i] ^= mask[i%4];
    buf[len] = 0;
    *outlen = (int)len;
    return opcode;
}

/* ---- handshake ---- */

static int ws_handshake(SOCKET s, char *proto_out, int proto_len)
{
    char req[8192]; int len = 0;
    proto_out[0] = 0;
    while (len < (int)sizeof(req)-1) {
        int n = net_recv_timeout(s, req+len, (int)sizeof(req)-1-len, 15000);
        if (n <= 0) return -1;
        len += n; req[len] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (strncmp(req, "GET ", 4) != 0) { nlog("ws: not a GET: %.30s", req); return -1; }
    char key[256] = "";
    char *kp = strstr(req, "Sec-WebSocket-Key:");
    if (!kp) kp = strstr(req, "sec-websocket-key:");
    if (kp) {
        kp += 18; while (*kp == ' ') kp++;
        int i = 0;
        while (kp[i] && kp[i] != '\r' && i < 255) { key[i] = kp[i]; i++; }
        key[i] = 0;
        str_trim(key);
    }
    if (!key[0]) { nlog("ws: no Sec-WebSocket-Key"); return -1; }
    /* the official NeatDM extension requires the server to echo its
     * subprotocol ("neatextension.v1") or the browser refuses the upgrade */
    char *pp = strstr(req, "Sec-WebSocket-Protocol:");
    if (pp) {
        pp += 23; while (*pp == ' ') pp++;
        int i = 0;
        while (pp[i] && pp[i] != '\r' && i < proto_len-1) { proto_out[i] = pp[i]; i++; }
        proto_out[i] = 0;
        str_trim(proto_out);
        char *comma = strchr(proto_out, ',');   /* take the first offered */
        if (comma) *comma = 0;
    }
    nlog("ws: handshake, key=%s proto=%s", key, proto_out[0] ? proto_out : "(none)");

    char combo[320];
    snprintf(combo, sizeof(combo), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char digest[20];
    if (sha1(combo, (int)strlen(combo), digest) != 0) return -1;
    char accept[64];
    b64enc(digest, 20, accept, sizeof(accept));

    char resp[512];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "%s%s%s"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        accept,
        proto_out[0] ? "Sec-WebSocket-Protocol: " : "",
        proto_out[0] ? proto_out : "",
        proto_out[0] ? "\r\n" : "");
    return net_send_all(s, resp, n);
}

/* ---- client thread ---- */

static void handle_job_json(const char *json, SOCKET s)
{
    WsJob *job = calloc(1, sizeof(*job));
    if (json_str(json, "url", job->url, sizeof(job->url)) != 0 || !strstr(job->url, "://")) {
        nlog("ws: bad job json (no url): %.120s", json);
        free(job);
        ws_send_frame(s, 1, "{\"result\":\"error\",\"reason\":\"no url\"}", 36);
        return;
    }
    json_str(json, "referer", job->referer, sizeof(job->referer));
    json_str(json, "cookies", job->cookies, sizeof(job->cookies));
    json_str(json, "useragent", job->useragent, sizeof(job->useragent));
    json_str(json, "filename", job->filename, sizeof(job->filename));
    nlog("ws: download pushed (json): %s", job->url);
    ui_submit_ws_job(job);   /* ownership passes to UI thread */
    ws_send_frame(s, 1, "{\"result\":\"ok\"}", 15);
}

/* parse the official NeatDM extension protocol:
 * numbered fields, one per line ("1:GET", "2:<url>", "Cookie: ...",
 * "Referer: ...", "__0NeatPostData9__:<data>"). Field map (from bg.js):
 *   1: method, 2: url, 3: ?, 4: title, 5: source page, 6: normal|media|hls,
 *   7: content-length, 8: content-type, 9: panel display name (hittitle),
 *   Origin:/Referer:/Cookie:/Content-Type:/Content-Disposition:/x-* headers  */
static void handle_job_neatdm(const char *text)
{
    WsJob *job = calloc(1, sizeof(*job));
    char *copy = _strdup(text), *save = NULL;
    for (char *ln = strtok_s(copy, "\r\n", &save); ln; ln = strtok_s(NULL, "\r\n", &save)) {
        if (strncmp(ln, "2:", 2) == 0 && ln[2] && strstr(ln+2, "://"))
            strncpy(job->url, ln+2, sizeof(job->url)-1);
        else if (strncmp(ln, "9:", 2) == 0)
            strncpy(job->filename, ln+2, sizeof(job->filename)-1);
        else if (strncmp(ln, "Cookie: ", 8) == 0)
            strncpy(job->cookies, ln+8, sizeof(job->cookies)-1);
        else if (strncmp(ln, "Referer: ", 9) == 0)
            strncpy(job->referer, ln+9, sizeof(job->referer)-1);
        else if (strncmp(ln, "Origin: ", 8) == 0 && !job->referer[0])
            strncpy(job->referer, ln+8, sizeof(job->referer)-1);
        else if (strncmp(ln, "5:", 2) == 0 && !job->referer[0] && strstr(ln+2, "://"))
            strncpy(job->referer, ln+2, sizeof(job->referer)-1);
        else if (strncmp(ln, "User-Agent: ", 12) == 0)
            strncpy(job->useragent, ln+12, sizeof(job->useragent)-1);
    }
    free(copy);
    /* filename like "MP4 File  12.3 MB" is a display title, not a real name;
     * only accept it if it looks file-ish */
    if (job->filename[0] && !strchr(job->filename, '.')) job->filename[0] = 0;
    if (!job->url[0]) { nlog("ws: bad neatdm job (no url): %.120s", text); free(job); return; }
    nlog("ws: download pushed (neatextension.v1): %s", job->url);
    ui_submit_ws_job(job);
}

static void handle_job(const char *payload, SOCKET s)
{
    if (payload[0] == '{') handle_job_json(payload, s);
    else handle_job_neatdm(payload);
}

static DWORD WINAPI client_thread(void *pv)
{
    SOCKET s = (SOCKET)(uintptr_t)pv;
    char proto[128];
    if (ws_handshake(s, proto, sizeof(proto)) == 0) {
        nlog("ws: client connected (sock %d)", (int)s);
        /* greeting expected by the official extension */
        ws_send_frame(s, 1, "Version=reneat " RENEAT_VERSION " OS=WIN", 27);
        ws_send_frame(s, 1, "nowaiting", 9);
        ws_send_frame(s, 1, "ShowPanelChrome=1", 17);
        char *buf = malloc(64*1024);
        while (!g_stop) {
            int outlen = 0;
            int op = ws_read_frame(s, buf, 64*1024, &outlen);
            if (op < 0) break;
            if (op == 1) handle_job(buf, s);
            else if (op == 9) ws_send_frame(s, 10, buf, outlen); /* ping -> pong */
            else if (op == 8) { ws_send_frame(s, 8, "", 0); break; } /* close */
        }
        free(buf);
    }
    nlog("ws: client disconnected (sock %d)", (int)s);
    net_close(s);
    return 0;
}

static DWORD WINAPI listen_thread(void *pv)
{
    (void)pv;
    while (!g_stop) {
        fd_set r; FD_ZERO(&r); FD_SET(g_listen, &r);
        struct timeval tv = { 0, 500*1000 };
        if (select(0, &r, NULL, NULL, &tv) <= 0) continue;
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        HANDLE h = CreateThread(NULL, 0, client_thread, (void*)(uintptr_t)c, 0, NULL);
        if (h) CloseHandle(h);
        else net_close(c);
    }
    return 0;
}

int ws_start(int port)
{
    net_init();
    g_port = port;
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == INVALID_SOCKET) { nlog("ws: socket() failed"); return -1; }
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)port);
    if (bind(g_listen, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        nlog("ws: bind 127.0.0.1:%d failed wsa=%d", port, WSAGetLastError());
        net_close(g_listen); g_listen = INVALID_SOCKET;
        return -1;
    }
    if (listen(g_listen, 8) != 0) { net_close(g_listen); g_listen = INVALID_SOCKET; return -1; }
    g_stop = 0;
    g_thread = CreateThread(NULL, 0, listen_thread, NULL, 0, NULL);
    nlog("Starting WebSocketServer on 127.0.0.1:%d ...", port);
    return 0;
}

void ws_stop(void)
{
    g_stop = 1;
    if (g_listen != INVALID_SOCKET) { net_close(g_listen); g_listen = INVALID_SOCKET; }
    nlog("WebSocketServer Terminated...");
}

int ws_port(void){ return g_port; }
