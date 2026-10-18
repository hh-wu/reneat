/* http.c */
#include "http.h"
#include "settings.h"

void http_init(HttpConn *c, const NUrl *url)
{
    memset(c, 0, sizeof(*c));
    c->sock = INVALID_SOCKET;
    c->url = *url;
    c->use_tls = url->use_tls;
    c->state = HTTP_STATE_VOID;
    c->content_length = -1;
    strncpy(c->user_agent, g_cfg.user_agent[0] ? g_cfg.user_agent :
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36",
        sizeof(c->user_agent)-1);
}

/* ---- proxy support (consumes Settings Proxy/Socks page) ---- */

static void b64_encode(const char *in, char *out, int outlen)
{
    static const char *b="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int il=strlen(in), o=0;
    for (int i=0;i<il && o<outlen-4;i+=3){ unsigned v=(unsigned char)in[i]<<16|(i+1<il?(unsigned char)in[i+1]<<8:0)|(i+2<il?(unsigned char)in[i+2]:0);
        out[o++]=b[v>>18]; out[o++]=b[(v>>12)&63]; out[o++]= i+1<il?b[(v>>6)&63]:'='; out[o++]= i+2<il?b[v&63]:'='; }
    out[o]=0;
}

/* read a proxy CONNECT response head (\r\n\r\n terminated), returns status or -1 */
static int proxy_read_status(SOCKET s, int timeout_ms)
{
    char buf[4096]; int len = 0;
    while (len < (int)sizeof(buf)-1) {
        int n = net_recv_timeout(s, buf+len, (int)sizeof(buf)-1-len, timeout_ms);
        if (n <= 0) { nlog("proxy: CONNECT response read failed n=%d", n); return -1; }
        len += n; buf[len] = 0;
        if (strstr(buf, "\r\n\r\n")) {
            int major, minor, status;
            if (sscanf(buf, "HTTP/%d.%d %d", &major, &minor, &status) == 3) return status;
            nlog("proxy: bad CONNECT response: %.60s", buf);
            return -1;
        }
    }
    return -1;
}

/* establish a tunnel to c->url through the configured proxy. 0 ok */
static int proxy_tunnel(HttpConn *c, int timeout_ms)
{
    const char *pa = g_cfg.proxy_addr;
    int pp = g_cfg.proxy_port > 0 ? g_cfg.proxy_port : 7890;
    nlog("proxy: connecting %s proxy %s:%d ...", g_cfg.proxy_mode == 1 ? "HTTP" : "SOCKS", pa, pp);
    c->sock = net_connect(pa, pp, timeout_ms);
    if (c->sock == INVALID_SOCKET) { nlog("proxy: connect failed"); return -1; }

    if (g_cfg.proxy_mode == 1) {
        /* HTTP proxy: plain http goes through with absolute URI (no tunnel);
         * https needs a CONNECT tunnel first */
        if (!c->use_tls) return 0;
        /* CONNECT 必须显式带端口（url_hostport 会省略默认端口，
         * 不带端口代理会当成 80 —— TLS 字节打到 HTTP 服务器上就是 400） */
        char hp[300];
        snprintf(hp, sizeof(hp), "%s:%d", c->url.host, c->url.port);
        char req[1024];
        int n = snprintf(req, sizeof(req), "CONNECT %s HTTP/1.1\r\nHost: %s\r\n", hp, hp);
        if (g_cfg.proxy_user[0]) {
            char up[300], b64[512];
            snprintf(up, sizeof(up), "%s:%s", g_cfg.proxy_user, g_cfg.proxy_pass);
            b64_encode(up, b64, sizeof(b64));
            n += snprintf(req+n, sizeof(req)-n, "Proxy-Authorization: Basic %s\r\n", b64);
        }
        n += snprintf(req+n, sizeof(req)-n, "\r\n");
        nlog("proxy: CONNECT %s ...", hp);
        if (net_send_all(c->sock, req, n) != 0) return -1;
        int st = proxy_read_status(c->sock, timeout_ms);
        nlog("proxy: CONNECT answered %d", st);
        if (st != 200) { net_close(c->sock); c->sock = INVALID_SOCKET; return -1; }
        return 0;
    }

    /* SOCKS5 */
    {
        char greet[4] = { 0x05, 0x01, 0x00 };
        if (g_cfg.proxy_user[0]) { greet[1] = 0x02; greet[2] = 0x02; }
        if (net_send_all(c->sock, greet, 3) != 0) return -1;
        char resp[300];
        if (net_recv_timeout(c->sock, resp, 2, timeout_ms) != 2) { nlog("proxy: socks greeting no reply"); return -1; }
        nlog("proxy: socks greeting -> ver=%d method=%d", (unsigned char)resp[0], (unsigned char)resp[1]);
        if (resp[0] != 0x05) return -1;
        if ((unsigned char)resp[1] == 0x02) {
            char auth[600]; int ul = strlen(g_cfg.proxy_user), pl = strlen(g_cfg.proxy_pass);
            if (ul > 255 || pl > 255) return -1;
            int an = 0;
            auth[an++] = 0x01; auth[an++] = (char)ul;
            memcpy(auth+an, g_cfg.proxy_user, ul); an += ul;
            auth[an++] = (char)pl;
            memcpy(auth+an, g_cfg.proxy_pass, pl); an += pl;
            if (net_send_all(c->sock, auth, an) != 0) return -1;
            if (net_recv_timeout(c->sock, resp, 2, timeout_ms) != 2 || resp[1] != 0) {
                nlog("proxy: socks auth failed"); return -1;
            }
        } else if ((unsigned char)resp[1] != 0x00) {
            nlog("proxy: socks: no acceptable auth method"); return -1;
        }
        char conn[600]; int cn = 0, hl = strlen(c->url.host);
        if (hl > 255) return -1;
        conn[cn++] = 0x05; conn[cn++] = 0x01; conn[cn++] = 0x00; conn[cn++] = 0x03;
        conn[cn++] = (char)hl;
        memcpy(conn+cn, c->url.host, hl); cn += hl;
        conn[cn++] = (char)(c->url.port >> 8); conn[cn++] = (char)(c->url.port & 0xff);
        nlog("proxy: SOCKS5 CONNECT %s:%d ...", c->url.host, c->url.port);
        if (net_send_all(c->sock, conn, cn) != 0) return -1;
        int got = net_recv_timeout(c->sock, resp, 4, timeout_ms);
        if (got != 4 || resp[1] != 0) { nlog("proxy: socks connect failed, rep=%d", got==4?(unsigned char)resp[1]:-1); return -1; }
        /* skip bound addr: ipv4=4+2, domain=1+len+2, ipv6=16+2 */
        int skip = resp[3]==1 ? 6 : resp[3]==4 ? 18 : -1;
        if (resp[3]==3) { if (net_recv_timeout(c->sock, resp, 1, timeout_ms)!=1) return -1; skip = 2 + (unsigned char)resp[0]; }
        if (skip < 0 || net_recv_timeout(c->sock, resp, skip, timeout_ms) != skip) return -1;
        nlog("proxy: SOCKS5 tunnel established");
        return 0;
    }
}

int http_open(HttpConn *c, int timeout_ms)
{
    net_init();
    int use_proxy = g_cfg.proxy_mode > 0 && g_cfg.proxy_addr[0];
    if (use_proxy) {
        if (proxy_tunnel(c, timeout_ms) != 0) {
            c->state = HTTP_STATE_ERROR;
            return -1;
        }
    } else {
        c->sock = net_connect(c->url.host, c->url.port, timeout_ms);
        if (c->sock == INVALID_SOCKET) { c->state = HTTP_STATE_ERROR; return -1; }
    }
    if (c->use_tls) {
        if (tls_handshake(c->sock, &c->tls, c->url.host) != 0) {
            nlog("SSL-HandShake Failed At First Try.");
            net_close(c->sock); c->sock = INVALID_SOCKET;
            c->state = HTTP_STATE_ERROR;
            return -1;
        }
    }
    return 0;
}

int http_send_get(HttpConn *c, int64_t from, int64_t to)
{
    char host[300]; url_hostport(&c->url, host, sizeof(host));
    char req[4096];
    /* through an HTTP proxy, plain http must use the absolute URI form */
    int http_proxy_plain = g_cfg.proxy_mode == 1 && g_cfg.proxy_addr[0] && !c->use_tls;
    char target[2200];
    if (http_proxy_plain)
        snprintf(target, sizeof(target), "http://%s%s", host, c->url.path);
    else
        snprintf(target, sizeof(target), "%s", c->url.path);
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: */*\r\n"
        "Accept-Encoding: identity\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Connection: keep-alive\r\n",
        target, host, c->user_agent);
    if (http_proxy_plain && g_cfg.proxy_user[0]) {
        char up[300], b64[512];
        snprintf(up, sizeof(up), "%s:%s", g_cfg.proxy_user, g_cfg.proxy_pass);
        b64_encode(up, b64, sizeof(b64));
        n += snprintf(req+n, sizeof(req)-n, "Proxy-Authorization: Basic %s\r\n", b64);
    }
    if (from >= 0) {
        if (to >= 0)
            n += snprintf(req+n, sizeof(req)-n, "Range: bytes=%lld-%lld\r\n", from, to);
        else
            n += snprintf(req+n, sizeof(req)-n, "Range: bytes=%lld-\r\n", from);
    }
    if (c->referer[0]) n += snprintf(req+n, sizeof(req)-n, "Referer: %s\r\n", c->referer);
    if (c->cookies[0]) n += snprintf(req+n, sizeof(req)-n, "Cookie: %s\r\n", c->cookies);
    if (c->url.auth[0]) {
        /* Basic auth for userinfo in URL */
        char b64[512]; static const char *b="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const unsigned char *in=(const unsigned char*)c->url.auth; int il=strlen(c->url.auth), o=0;
        for (int i=0;i<il;i+=3){ unsigned v=in[i]<<16|(i+1<il?in[i+1]<<8:0)|(i+2<il?in[i+2]:0);
            b64[o++]=b[v>>18]; b64[o++]=b[(v>>12)&63]; b64[o++]= i+1<il?b[(v>>6)&63]:'='; b64[o++]= i+2<il?b[v&63]:'='; }
        b64[o]=0;
        n += snprintf(req+n, sizeof(req)-n, "Authorization: Basic %s\r\n", b64);
    }
    n += snprintf(req+n, sizeof(req)-n, "\r\n");
    nlog("Sending Http-GET (Socket %d) Range = %lld-%lld", (int)c->sock, from, to);
    int r = c->use_tls ? tls_send(c->sock, &c->tls, req, n)
                       : net_send_all(c->sock, req, n);
    if (r != 0) { c->state = HTTP_STATE_ERROR; return -1; }
    c->state = HTTP_STATE_GET_SENT;
    return 0;
}

static int conn_read(HttpConn *c, char *buf, int len, int timeout_ms)
{
    return c->use_tls ? tls_recv(c->sock, &c->tls, buf, len, timeout_ms)
                      : net_recv_timeout(c->sock, buf, len, timeout_ms);
}

/* parse one header line (without CRLF) */
static void parse_header_line(HttpConn *c, char *line)
{
    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = 0;
    char *val = colon+1; str_trim(val); str_trim(line);
    if (str_ieq(line, "Content-Length")) { c->content_length = _strtoi64(val, NULL, 10); nlog("hdr < Content-Length: %lld", c->content_length); }
    else if (str_ieq(line, "Transfer-Encoding")) { if (strstr(val, "chunked")) { c->chunked = 1; nlog("hdr < Transfer-Encoding: chunked (NOTE: chunked bodies not supported by engine)"); } }
    else if (str_ieq(line, "Accept-Ranges")) { if (strstr(val, "bytes")) { c->accept_ranges = 1; nlog("hdr < Accept-Ranges: bytes"); } }
    else if (str_ieq(line, "Content-Range")) {
        /* bytes start-end/total */
        char *sl = strchr(val, '/');
        if (sl && sl[1] != '*') c->content_length = _strtoi64(sl+1, NULL, 10);
        c->accept_ranges = 1;
        nlog("hdr < Content-Range: %s (total=%lld)", val, c->content_length);
    }
    else if (str_ieq(line, "Content-Disposition")) { strncpy(c->content_disposition, val, sizeof(c->content_disposition)-1); nlog("hdr < Content-Disposition: %s", val); }
    else if (str_ieq(line, "Content-Type")) { strncpy(c->content_type, val, sizeof(c->content_type)-1); nlog("hdr < Content-Type: %s", val); }
    else if (str_ieq(line, "Location")) { strncpy(c->location, val, sizeof(c->location)-1); nlog("hdr < Location: %s", val); }
    else if (str_ieq(line, "Connection")) { if (str_ieq(val,"close")) c->connection_close = 1; }
}

int http_read_headers(HttpConn *c, int timeout_ms)
{
    c->state = HTTP_STATE_PARSING_HTTP_HEADER;
    c->hdrlen = 0;
    while (c->hdrlen < (int)sizeof(c->hdr)-1) {
        int n = conn_read(c, c->hdr + c->hdrlen, (int)sizeof(c->hdr)-1 - c->hdrlen, timeout_ms);
        if (n <= 0) { nlog("hdr read failed: n=%d wsa=%d (sock %d)", n, WSAGetLastError(), (int)c->sock); c->state = HTTP_STATE_ERROR; return -1; }
        c->hdrlen += n;
        c->hdr[c->hdrlen] = 0;
        char *end = strstr(c->hdr, "\r\n\r\n");
        if (end) {
            /* status line */
            int major, minor, status;
            if (sscanf(c->hdr, "HTTP/%d.%d %d", &major, &minor, &status) != 3) {
                nlog("bad status line: %.40s (sock %d)", c->hdr, (int)c->sock);
                c->state = HTTP_STATE_ERROR; return -1;
            }
            c->status = status;
            nlog("hdr < HTTP/%d.%d %d (sock %d, %d header bytes)", major, minor, status, (int)c->sock, c->hdrlen);
            /* parse header lines */
            *end = 0;
            char *save = NULL;
            for (char *ln = strtok_s(c->hdr, "\r\n", &save); ln; ln = strtok_s(NULL, "\r\n", &save))
                if (strchr(ln, ':')) parse_header_line(c, ln);
            /* leftover body bytes */
            int used = (int)((end + 4) - c->hdr);
            int left = c->hdrlen - used;
            if (left > 0) {
                c->body_leftover = malloc(left);
                memcpy(c->body_leftover, end + 4, left);
                c->leftover_len = left;
            }
            if (status >= 300 && status < 400 && c->location[0]) {
                c->state = HTTP_STATE_REDIRECTING;
                nlog("Redirect Location Detected By Socket (%d): %d -> %s", (int)c->sock, status, c->location);
                return 1; /* redirect */
            }
            c->state = HTTP_STATE_RECEIVING;
            return 0;
        }
    }
    c->state = HTTP_STATE_ERROR;
    return -1;
}

int http_recv(HttpConn *c, char *buf, int len, int timeout_ms)
{
    if (c->leftover_len > 0) {
        int n = c->leftover_len < len ? c->leftover_len : len;
        memcpy(buf, c->body_leftover, n);
        memmove(c->body_leftover, c->body_leftover + n, c->leftover_len - n);
        c->leftover_len -= n;
        if (c->leftover_len == 0) { free(c->body_leftover); c->body_leftover = NULL; }
        return n;
    }
    return conn_read(c, buf, len, timeout_ms);
}

void http_close(HttpConn *c)
{
    if (c->sock != INVALID_SOCKET) {
        if (c->use_tls) tls_free(&c->tls);
        net_close(c->sock);
        c->sock = INVALID_SOCKET;
    }
    if (c->body_leftover) { free(c->body_leftover); c->body_leftover = NULL; c->leftover_len = 0; }
}

/* extract filename*=UTF-8''x or filename="x" from Content-Disposition */
static int filename_from_cd(const char *cd, char *out, int outlen)
{
    const char *p = strstr(cd, "filename*=");
    if (p) {
        p += 10;
        const char *q = strstr(p, "''");
        if (q) p = q+2;
        char tmp[512]; int i=0;
        while (p[i] && p[i]!=';' && p[i]!='\r' && p[i]!='"' && i<(int)sizeof(tmp)-1){ tmp[i]=p[i]; i++; }
        tmp[i]=0;
        url_decode(tmp, out, outlen);
        return 0;
    }
    p = strstr(cd, "filename=");
    if (p) {
        p += 9; if (*p=='"') p++;
        int i=0;
        while (p[i] && p[i]!='"' && p[i]!=';' && p[i]!='\r' && i<outlen-1){ out[i]=p[i]; i++; }
        out[i]=0;
        return 0;
    }
    return -1;
}

int http_probe_ex(const char *url, const char *ua, const char *referer,
                  const char *cookies, NUrl *final_url, HttpConn *facts,
                  char *filename_out, int fnlen)
{
    NUrl cur;
    if (url_parse(url, &cur) != 0) { nlog("probe: cannot parse url '%s'", url); return -1; }
    nlog("probe: %s://%s:%d%s", cur.use_tls?"https":"http", cur.host, cur.port, cur.path);
    for (int hop = 0; hop < RENEAT_MAX_REDIRECTS; hop++) {
        if (hop) nlog("probe: redirect hop %d -> %s://%s:%d%s", hop, cur.use_tls?"https":"http", cur.host, cur.port, cur.path);
        HttpConn c; http_init(&c, &cur);
        if (ua) { strncpy(c.user_agent, ua, sizeof(c.user_agent)-1); }
        if (referer) { strncpy(c.referer, referer, sizeof(c.referer)-1); }
        if (cookies) { strncpy(c.cookies, cookies, sizeof(c.cookies)-1); }
        if (http_open(&c, 15000) != 0) return -1;
        /* probe with a 1-byte range request to detect resumability cheaply */
        http_send_get(&c, 0, 0);
        int r = http_read_headers(&c, 20000);
        if (r < 0) { http_close(&c); return -1; }
        if (r == 1) { /* redirect */
            NUrl next; url_join_relative(&cur, c.location, &next);
            cur = next;
            http_close(&c);
            continue;
        }
        if (c.status != 200 && c.status != 206) { nlog("probe: server answered HTTP %d", c.status); http_close(&c); return -2 - c.status; }
        *final_url = cur;
        *facts = c;               /* shallow copy of facts; socket already... */
        c.sock = INVALID_SOCKET;  /* ...closed below, keep only parsed fields */
        facts->body_leftover = NULL; facts->leftover_len = 0;
        facts->use_tls = cur.use_tls;
        int resumable = (c.status == 206) && c.content_length > 0;
        facts->accept_ranges = resumable;
        if (filename_out) {
            if (filename_from_cd(c.content_disposition, filename_out, fnlen) != 0)
                url_basename(cur.path, filename_out, fnlen);
        }
        http_close(&c);
        return resumable ? 1 : 0;   /* 1 = resumable (206), 0 = plain 200 */
    }
    return -1;
}
