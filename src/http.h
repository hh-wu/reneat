/* http.h — hand-rolled HTTP/1.1 client with explicit state machine
 * (mirrors NeatDM's HTTP_STATE_* design) */
#ifndef RENEAT_HTTP_H
#define RENEAT_HTTP_H
#include "common.h"
#include "net.h"
#include "tls.h"
#include "util.h"

typedef enum {
    HTTP_STATE_VOID = 0,
    HTTP_STATE_GET_SENT,
    HTTP_STATE_PARSING_HTTP_HEADER,
    HTTP_STATE_RECEIVING,
    HTTP_STATE_REDIRECTING,
    HTTP_STATE_DONE,
    HTTP_STATE_ERROR
} HttpState;

typedef struct HttpConn {
    SOCKET  sock;
    Tls     tls;
    int     use_tls;
    NUrl    url;
    HttpState state;

    /* response facts */
    int     status;
    int64_t content_length;   /* -1 unknown */
    int     chunked;
    int     accept_ranges;
    int     connection_close;
    char    content_disposition[512];
    char    content_type[128];
    char    location[2048];

    /* header accumulation buffer; leftover body bytes live in body_leftover */
    char    hdr[16384];
    int     hdrlen;
    char   *body_leftover;
    int     leftover_len;

    /* per-request extras */
    char    user_agent[256];
    char    cookies[2048];
    char    referer[2048];
} HttpConn;

void http_init(HttpConn *c, const NUrl *url);
int  http_open(HttpConn *c, int timeout_ms);                       /* connect (+TLS) */
int  http_send_get(HttpConn *c, int64_t range_from, int64_t range_to); /* range_to<0 = open end */
int  http_read_headers(HttpConn *c, int timeout_ms);               /* fills response facts */
int  http_recv(HttpConn *c, char *buf, int len, int timeout_ms);   /* body bytes */
void http_close(HttpConn *c);

/* one-shot probe: GET with Range: bytes=0-0 to learn size / resumability /
 * filename, following redirects.  Returns 0 and fills *final_url + facts. */
int  http_probe_ex(const char *url, const char *ua, const char *referer,
                   const char *cookies, NUrl *final_url, HttpConn *facts,
                   char *filename_out, int fnlen);
#define http_probe(url, ua, final_url, facts, filename_out, fnlen) \
    http_probe_ex(url, ua, NULL, NULL, final_url, facts, filename_out, fnlen)

#endif
