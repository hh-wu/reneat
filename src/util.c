/* util.c */
#include "util.h"
#include <wchar.h>
#include <time.h>

static FILE *g_logf = NULL;
static CRITICAL_SECTION g_logcs;
static int g_loginit = 0;
static int g_log_console = 0;

void nlog_enable_console(void){ g_log_console = 1; }

static void nlog_init(void)
{
    if (g_loginit) return;
    g_loginit = 1;
    InitializeCriticalSection(&g_logcs);
    static char path[MAX_PATH];
    if (GetEnvironmentVariableA("RENEAT_LOG", path, MAX_PATH) == 0) {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *sl = strrchr(path, '\\');
        if (sl) sl[1] = 0;
        strcat(path, "reneat.log");
    }
    /* simple rotation: restart the log when it grows past 8 MB */
    HANDLE h = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz; GetFileSizeEx(h, &sz); CloseHandle(h);
        if (sz.QuadPart > 8*1024*1024) {
            char bak[MAX_PATH];
            snprintf(bak, sizeof(bak), "%s.1", path);
            MoveFileExA(path, bak, MOVEFILE_REPLACE_EXISTING);
        }
    }
    g_logf = fopen(path, "a");
    if (g_logf) {
        time_t now = time(NULL);
        fprintf(g_logf, "\n===== reneat " RENEAT_VERSION " session %s", ctime(&now));
        fflush(g_logf);
    }
}

void nlog(const char *fmt, ...)
{
    nlog_init();
    if (!g_logf && !g_log_console) return;
    EnterCriticalSection(&g_logcs);
    char line[2048];
    SYSTEMTIME st; GetLocalTime(&st);
    int off = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d][%lu] ",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + off, sizeof(line) - off, fmt, ap);
    va_end(ap);
    if (g_logf) { fprintf(g_logf, "%s\n", line); fflush(g_logf); }
    if (g_log_console) { fprintf(stderr, "%s\n", line); fflush(stderr); }
    LeaveCriticalSection(&g_logcs);
}

int str_ieq(const char *a, const char *b){ return _stricmp(a,b)==0; }

void str_trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
    while (*s==' '||*s=='\t') memmove(s, s+1, strlen(s));
}

int url_parse(const char *url, NUrl *o)
{
    memset(o, 0, sizeof(*o));
    const char *p = url;
    if (_strnicmp(p, "https://", 8)==0){ o->use_tls=1; o->port=443; p+=8; }
    else if (_strnicmp(p, "http://", 7)==0){ o->use_tls=0; o->port=80; p+=7; }
    else return -1;
    /* optional userinfo */
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) {
        int al = (int)(at-p); if (al >= (int)sizeof(o->auth)) al = sizeof(o->auth)-1;
        memcpy(o->auth, p, al); o->auth[al]=0;
        p = at+1;
    }
    const char *hostend = p;
    while (*hostend && *hostend!='/' && *hostend!=':') hostend++;
    int hl = (int)(hostend-p);
    if (hl<=0 || hl >= (int)sizeof(o->host)) return -1;
    memcpy(o->host, p, hl); o->host[hl]=0;
    if (*hostend==':') o->port = atoi(hostend+1);
    if (o->port<=0) o->port = o->use_tls?443:80;
    const char *path = strchr(p, '/');
    if (path){ strncpy(o->path, path, sizeof(o->path)-1); }
    else strcpy(o->path, "/");
    return 0;
}

void url_join_relative(const NUrl *base, const char *loc, NUrl *out)
{
    if (strstr(loc, "://")) { url_parse(loc, out); return; }
    *out = *base;
    if (loc[0]=='/') {
        strncpy(out->path, loc, sizeof(out->path)-1);
    } else {
        /* relative to base dir */
        char tmp[2048]; strncpy(tmp, base->path, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
        char *sl = strrchr(tmp, '/');
        if (sl) sl[1]=0; else strcpy(tmp,"/");
        strncat(tmp, loc, sizeof(tmp)-strlen(tmp)-1);
        strncpy(out->path, tmp, sizeof(out->path)-1);
    }
}

void url_hostport(const NUrl *u, char *out, int outlen)
{
    int def = u->use_tls?443:80;
    if (u->port==def) snprintf(out,outlen,"%s",u->host);
    else snprintf(out,outlen,"%s:%d",u->host,u->port);
}

void url_decode(const char *in, char *out, int outlen)
{
    int o=0;
    for (int i=0; in[i] && o<outlen-1; i++) {
        if (in[i]=='%' && in[i+1] && in[i+2]) {
            int v; if (sscanf(in+i+1, "%2x", &v)==1){ out[o++]=(char)v; i+=2; continue; }
        }
        out[o++] = in[i];
    }
    out[o]=0;
}

const char *url_basename(const char *path, char *out, int outlen)
{
    char tmp[2048]; strncpy(tmp, path, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
    char *q = strchr(tmp, '?'); if (q) *q=0;
    q = strchr(tmp, '#'); if (q) *q=0;
    const char *base = strrchr(tmp, '/');
    base = base ? base+1 : tmp;
    if (!*base){ strcpy(out, "index.html"); return out; }
    char dec[1024]; url_decode(base, dec, sizeof(dec));
    strncpy(out, dec, outlen-1); out[outlen-1]=0;
    return out;
}

void format_size(int64_t b, wchar_t *out, int n)
{
    if (b < 1024) _snwprintf(out,n,L"%lld B",b);
    else if (b < 1<<20) _snwprintf(out,n,L"%.1f KB",b/1024.0);
    else if (b < 1<<30) _snwprintf(out,n,L"%.1f MB",b/1048576.0);
    else _snwprintf(out,n,L"%.2f GB",b/1073741824.0);
}

void format_speed(double bps, wchar_t *out, int n)
{
    if (bps < 1024) _snwprintf(out,n,L"%.0f B/s",bps);
    else if (bps < 1<<20) _snwprintf(out,n,L"%.1f KB/s",bps/1024.0);
    else _snwprintf(out,n,L"%.2f MB/s",bps/1048576.0);
}

void format_eta(double s, wchar_t *out, int n)
{
    if (s < 0 || s > 360000){ _snwprintf(out,n,L"--:--"); return; }
    int h=(int)(s/3600), m=((int)s/60)%60, sec=(int)s%60;
    if (h) _snwprintf(out,n,L"%d:%02d:%02d",h,m,sec);
    else _snwprintf(out,n,L"%02d:%02d",m,sec);
}
