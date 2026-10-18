/* common.h — shared types, logging, small helpers */
#ifndef RENEAT_COMMON_H
#define RENEAT_COMMON_H

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RENEAT_VERSION "0.1"
#define RENEAT_NAME    "reneat"

/* default split parameters, mirroring NeatDM behaviour */
#define RENEAT_DEFAULT_CONN   8     /* connections per download          */
#define RENEAT_MAX_SEG        64    /* hard cap on segments per download */
#define RENEAT_MIN_SPLIT      (256*1024)   /* don't steal below 256KB   */
#define RENEAT_IO_BUF         (64*1024)    /* socket read chunk          */
#define RENEAT_MAX_REDIRECTS  8
#define RENEAT_MAX_RETRIES    8

/* debug log: always on, written to <exedir>\reneat.log (override with
 * RENEAT_LOG env var); in --cli mode also mirrored to the console. */
void nlog(const char *fmt, ...);
void nlog_enable_console(void);

static inline int64_t nmin64(int64_t a, int64_t b){ return a<b?a:b; }
static inline int64_t nmax64(int64_t a, int64_t b){ return a>b?a:b; }

#endif
