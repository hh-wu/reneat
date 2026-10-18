/* util.h — URL parsing, string helpers */
#ifndef RENEAT_UTIL_H
#define RENEAT_UTIL_H
#include "common.h"

typedef struct {
    int  use_tls;
    char host[256];
    int  port;                 /* 0 = default (80/443) */
    char path[2048];           /* includes query; "/" if empty */
    char auth[256];            /* user:pass or empty */
} NUrl;

int  url_parse(const char *url, NUrl *out);           /* 0 ok */
void url_join_relative(const NUrl *base, const char *loc, NUrl *out); /* resolve Location */
void url_hostport(const NUrl *u, char *out, int outlen); /* "host" or "host:port" */
const char *url_basename(const char *path, char *out, int outlen); /* filename guess from URL path */
void url_decode(const char *in, char *out, int outlen);
void str_trim(char *s);
int  str_ieq(const char *a, const char *b);
void format_size(int64_t bytes, wchar_t *out, int outlen);   /* "12.4 MB" */
void format_speed(double bps, wchar_t *out, int outlen);     /* "1.2 MB/s" */
void format_eta(double secs, wchar_t *out, int outlen);      /* "00:01:23" */

#endif
