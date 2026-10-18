/* engine.c — SegmentManager implementation with dynamic work stealing */
#include "engine.h"
#include "http.h"
#include "util.h"
#include "settings.h"
#include <time.h>

/* ---------------- task registry ---------------- */

static DownloadTask **g_tasks = NULL;
static int g_ntasks = 0, g_cap = 0;
static CRITICAL_SECTION g_cs;
static int g_next_id = 1;

void engine_init(void){ InitializeCriticalSection(&g_cs); }
void engine_lock(void){ EnterCriticalSection(&g_cs); }
void engine_unlock(void){ LeaveCriticalSection(&g_cs); }

DownloadTask *engine_task_at(int i){ return (i>=0 && i<g_ntasks) ? g_tasks[i] : NULL; }
int engine_count(void){ return g_ntasks; }

DownloadTask *engine_add(const char *url, const char *outdir, const char *filename, int connections)
{
    DownloadTask *t = calloc(1, sizeof(*t));
    t->id = g_next_id++;
    strncpy(t->url, url, sizeof(t->url)-1);
    strncpy(t->outdir, outdir, sizeof(t->outdir)-1);
    if (filename) strncpy(t->filename, filename, sizeof(t->filename)-1);
    t->connections = connections > 0 ? connections : g_cfg.max_connections;
    t->bw_limit = (int64_t)g_cfg.bw_limit_kbs * 1024;
    t->category = CAT_ALL;
    t->size = -1;
    t->state = TASK_QUEUED;
    t->file = INVALID_HANDLE_VALUE;
    t->added_at = time(NULL);
    InitializeCriticalSection(&t->lock);

    EnterCriticalSection(&g_cs);
    if (g_ntasks == g_cap) {
        g_cap = g_cap ? g_cap*2 : 8;
        g_tasks = realloc(g_tasks, g_cap * sizeof(*g_tasks));
    }
    g_tasks[g_ntasks++] = t;
    LeaveCriticalSection(&g_cs);
    nlog("[task %d] added: %s -> %s (conn=%d)", t->id, url, outdir, t->connections);
    return t;
}

/* ---------------- segment table ops (call with t->lock held) ---------------- */

int engine_seg_add(DownloadTask *t, int64_t start, int64_t pos, int64_t end, int state)
{
    if (t->nseg == t->ncap) {
        t->ncap = t->ncap ? t->ncap*2 : 16;
        t->segs = realloc(t->segs, t->ncap * sizeof(Segment));
    }
    Segment *s = &t->segs[t->nseg];
    s->start = start; s->pos = pos; s->end = end;
    s->state = state; s->retries = 0; s->owner = -1;
    return t->nseg++;
}

/* find work: prefer IDLE segment; otherwise steal by splitting the largest
 * running segment whose remaining range is big enough.  Returns seg index
 * or -1 when no work remains. */
static int seg_acquire(DownloadTask *t, int worker)
{
    for (int i = 0; i < t->nseg; i++) {
        if (t->segs[i].state == SEG_IDLE) {
            t->segs[i].state = SEG_RUNNING;
            t->segs[i].owner = worker;
            return i;
        }
    }
    /* steal: split the largest running segment */
    int best = -1; int64_t bestrem = RENEAT_MIN_SPLIT * 2;
    for (int i = 0; i < t->nseg; i++) {
        Segment *s = &t->segs[i];
        if (s->state != SEG_RUNNING || s->owner == worker) continue;
        int64_t rem = s->end - s->pos;
        if (rem > bestrem) { bestrem = rem; best = i; }
    }
    if (best >= 0 && t->nseg < RENEAT_MAX_SEG) {
        Segment *victim = &t->segs[best];
        int64_t mid = victim->pos + (victim->end - victim->pos) / 2;
        int ni = engine_seg_add(t, mid, mid, victim->end, SEG_RUNNING);
        t->segs[ni].owner = worker;
        victim->end = mid;   /* old connection stops at mid ("Merged To Segment") */
        nlog("[task %d] SegmentManager Created a New Segment and now has %d Segments.", t->id, t->nseg);
        nlog("[task %d]   worker %d Took a partial Segment: seg %d [%lld..%lld) stolen from seg %d (owner %d)",
             t->id, worker, ni, mid, t->segs[ni].end, best, victim->owner);
        return ni;
    }
    return -1;
}

static int64_t seg_downloaded_total(DownloadTask *t)
{
    int64_t d = 0;
    for (int i = 0; i < t->nseg; i++) d += t->segs[i].pos - t->segs[i].start;
    return d;
}

static int seg_all_done(DownloadTask *t)
{
    for (int i = 0; i < t->nseg; i++)
        if (t->segs[i].state != SEG_DONE) return 0;
    return 1;
}

/* ---------------- file IO ---------------- */

static int file_prealloc(DownloadTask *t)
{
    /* open (or create) target, preallocate full size */
    HANDLE f = CreateFileA(t->filepath, GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        nlog("[task %d] CreateFile(%s) failed, win32 error %lu", t->id, t->filepath, GetLastError());
        return -1;
    }
    if (t->size > 0) {
        LARGE_INTEGER li; li.QuadPart = t->size;
        SetFilePointerEx(f, li, NULL, FILE_BEGIN);
        if (!SetEndOfFile(f)) {
            nlog("[task %d] SetEndOfFile(%lld) failed, win32 error %lu", t->id, t->size, GetLastError());
            CloseHandle(f);
            return -1;
        }
    }
    t->file = f;
    return 0;
}

/* pwrite via OVERLAPPED on a synchronous handle */
static int file_write_at(DownloadTask *t, int64_t off, const char *buf, int len)
{
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)(off & 0xffffffff);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD wr = 0;
    if (!WriteFile(t->file, buf, len, &wr, &ov) || wr != (DWORD)len) return -1;
    return 0;
}

/* ---------------- worker ---------------- */

typedef struct { DownloadTask *t; int idx; } WorkerArg;

static int worker_run_segment(DownloadTask *t, int si)
{
    Segment *s = &t->segs[si];
    NUrl url;
    if (url_parse(t->final_url, &url) != 0) { nlog("[task %d] seg %d: bad url", t->id, si); return -1; }

    HttpConn c; http_init(&c, &url);
    if (t->useragent[0]) strncpy(c.user_agent, t->useragent, sizeof(c.user_agent)-1);
    if (t->referer[0]) strncpy(c.referer, t->referer, sizeof(c.referer)-1);
    if (t->cookies[0]) strncpy(c.cookies, t->cookies, sizeof(c.cookies)-1);
    if (http_open(&c, 15000) != 0) { nlog("[task %d] seg %d: connect/handshake failed", t->id, si); return -1; }

    int64_t req_end = s->end - 1;
    if (http_send_get(&c, s->pos, req_end) != 0) { nlog("[task %d] seg %d: send failed", t->id, si); http_close(&c); return -1; }
    int r = http_read_headers(&c, 20000);
    if (r != 0 || (c.status != 200 && c.status != 206)) {
        nlog("[task %d] seg %d: headers failed r=%d status=%d", t->id, si, r, c.status);
        http_close(&c); return -1;
    }
    nlog("[task %d] seg %d: receiving [%lld..%lld) via HTTP %d", t->id, si, s->pos, s->end, c.status);

    char *buf = malloc(RENEAT_IO_BUF);
    int fail = 0;
    int64_t thr_bytes = 0;
    DWORD thr_start = GetTickCount();
    while (!t->cancel && !t->pause) {
        /* lock to refresh end (a steal may have shrunk it) */
        EnterCriticalSection(&t->lock);
        int64_t end = s->end, pos = s->pos;
        LeaveCriticalSection(&t->lock);
        if (pos >= end) break;

        int want = (int)nmin64(RENEAT_IO_BUF, end - pos);
        int n = http_recv(&c, buf, want, 30000);
        if (n <= 0) { fail = 1; nlog("[task %d] seg %d: recv failed at pos %lld/%lld (n=%d)", t->id, si, s->pos, s->end, n); break; }
        if (file_write_at(t, pos, buf, n) != 0) {
            fail = 1; snprintf(t->err, sizeof(t->err), "disk write failed at %lld", pos);
            nlog("[task %d] seg %d: %s", t->id, si, t->err);
            break;
        }
        /* crude bandwidth limiter: per-worker share of the task limit */
        if (t->bw_limit > 0) {
            EnterCriticalSection(&t->lock);
            int64_t share = t->bw_limit / nmax64(1, t->active_conns);
            LeaveCriticalSection(&t->lock);
            if (share < 4096) share = 4096;
            thr_bytes += n;
            DWORD el = GetTickCount() - thr_start;
            if (el >= 200) {
                int64_t allowed = share * el / 1000;
                if (thr_bytes > allowed)
                    Sleep((DWORD)((thr_bytes - allowed) * 1000 / share));
                thr_bytes = 0; thr_start = GetTickCount();
            }
        }
        EnterCriticalSection(&t->lock);
        s->pos += n;
        InterlockedAdd64(&t->downloaded, n);
        LeaveCriticalSection(&t->lock);
    }
    free(buf);
    http_close(&c);
    return fail ? -1 : 0;
}

static DWORD WINAPI worker_main(void *pv)
{
    WorkerArg *wa = pv;
    DownloadTask *t = wa->t;
    int me = wa->idx;
    free(wa);

    EnterCriticalSection(&t->lock);
    t->active_conns++;
    LeaveCriticalSection(&t->lock);

    while (!t->cancel && !t->pause) {
        EnterCriticalSection(&t->lock);
        int si = seg_acquire(t, me);
        LeaveCriticalSection(&t->lock);
        if (si < 0) break;

        int r = worker_run_segment(t, si);

        EnterCriticalSection(&t->lock);
        Segment *s = &t->segs[si];
        if (r == 0 && s->pos >= s->end) {
            s->state = SEG_DONE;
            nlog("[task %d] Segment Completed: seg %d [%lld..%lld)", t->id, si, s->start, s->end);
        } else if (t->cancel || t->pause) {
            s->state = SEG_IDLE;    /* will resume from s->pos */
            s->owner = -1;
        } else {
            s->retries++;
            s->owner = -1;
            if (s->retries > RENEAT_MAX_RETRIES) {
                s->state = SEG_ERROR;
                snprintf(t->err, sizeof(t->err), "segment %d failed after %d retries", si, s->retries);
                nlog("[task %d] %s", t->id, t->err);
            } else {
                s->state = SEG_IDLE;  /* "Segment Rolled Back" */
                nlog("[task %d] Segment Rolled Back: seg %d (retry %d/%d, pos=%lld)",
                     t->id, si, s->retries, RENEAT_MAX_RETRIES, s->pos);
            }
        }
        int done = seg_all_done(t);
        LeaveCriticalSection(&t->lock);
        if (done) break;
        if (r != 0 && !t->cancel && !t->pause) Sleep(500); /* backoff */
    }

    EnterCriticalSection(&t->lock);
    t->active_conns--;
    LeaveCriticalSection(&t->lock);
    return 0;
}

/* ---------------- manager thread ---------------- */

static DWORD WINAPI manager_main(void *pv)
{
    DownloadTask *t = pv;
    t->state = TASK_PROBING;

    /* probe: follow redirects, get size + resumability + filename */
    NUrl final; HttpConn facts; char fname[512] = {0};
    int pr = http_probe_ex(t->url, t->useragent[0] ? t->useragent : NULL,
                           t->referer[0] ? t->referer : NULL,
                           t->cookies[0] ? t->cookies : NULL,
                           &final, &facts, fname, sizeof(fname));
    if (pr < 0) {
        if (pr <= -200)      /* -(2+status): server answered with an error code */
            snprintf(t->err, sizeof(t->err), "HTTP %d", -2 - pr);
        else
            snprintf(t->err, sizeof(t->err), "connect failed (%d)", pr);
        nlog("probe failed: %s (%s)", t->err, t->url);
        t->state = TASK_ERROR;
        return 0;
    }
    strncpy(t->final_url, t->url, sizeof(t->final_url)-1);
    /* rebuild final URL string */
    {
        char hp[300]; url_hostport(&final, hp, sizeof(hp));
        snprintf(t->final_url, sizeof(t->final_url), "%s://%s%s",
                 final.use_tls ? "https" : "http", hp, final.path);
    }
    t->size = facts.content_length;
    t->resumable = (pr == 1);
    if (!t->filename[0]) strncpy(t->filename, fname, sizeof(t->filename)-1);
    if (!t->filename[0]) strcpy(t->filename, "download.bin");
    nlog("probe ok: size=%lld resumable=%d file=%s final=%s",
         t->size, t->resumable, t->filename, t->final_url);
    t->category = category_of_file(t->filename);
    if (g_cfg.category_folders && str_ieq(t->outdir, g_cfg.download_dir)) {
        /* NeatDM: create category subfolders under the download dir */
        char sub[MAX_PATH];
        char catname[32];
        WideCharToMultiByte(CP_UTF8, 0, category_name(t->category), -1, catname, 32, NULL, NULL);
        snprintf(sub, sizeof(sub), "%s\\%s", t->outdir, catname);
        CreateDirectoryA(sub, NULL);
        strncpy(t->outdir, sub, sizeof(t->outdir)-1);
    }
    snprintf(t->filepath, sizeof(t->filepath), "%s\\%s", t->outdir, t->filename);

    CreateDirectoryA(t->outdir, NULL);

    EnterCriticalSection(&t->lock);
    int resumed = 0;
    if (t->size > 0 && t->resumable) {
        if (store_load_segments(t) == 0) {
            resumed = 1;
            int64_t d = seg_downloaded_total(t);
            InterlockedExchange64(&t->downloaded, d);
            nlog("[task %d] %d Segments were loaded from segments.bin file (%lld/%lld bytes).",
                 t->id, t->nseg, d, t->size);
        } else {
            nlog("[task %d] no valid segments.bin, starting fresh", t->id);
        }
    } else {
        nlog("[task %d] %s", t->id,
             t->size <= 0 ? "size unknown, single connection" : "server not resumable, single connection");
    }
    if (!resumed) {
        t->nseg = 0;
        InterlockedExchange64(&t->downloaded, 0);
        if (t->size > 0 && t->resumable) {
            int n = t->connections;
            if (n > RENEAT_MAX_SEG) n = RENEAT_MAX_SEG;
            int64_t chunk = t->size / n;
            if (chunk < RENEAT_MIN_SPLIT) { n = (int)(t->size / RENEAT_MIN_SPLIT) + 1; if (n < 1) n = 1; chunk = t->size / n; }
            for (int i = 0; i < n; i++) {
                int64_t st = i * chunk;
                int64_t en = (i == n-1) ? t->size : (i+1) * chunk;
                engine_seg_add(t, st, st, en, SEG_IDLE);
            }
            nlog("[task %d] SegmentManager Created %d Segments (chunk=%lld).", t->id, t->nseg, chunk);
        } else {
            /* unknown size or no ranges: single connection */
            engine_seg_add(t, 0, 0, t->size > 0 ? t->size : INT64_MAX, SEG_IDLE);
        }
    }
    LeaveCriticalSection(&t->lock);

    if (file_prealloc(t) != 0 && t->temp_fallback_done == 0) {
        /* target dir not writable (locked-down folders, permissions...):
         * fall back to the temp dir, like NeatDM's Temporary Directory */
        t->temp_fallback_done = 1;
        nlog("[task %d] falling back to temp dir %s", t->id, g_cfg.temp_dir);
        CreateDirectoryA(g_cfg.temp_dir, NULL);
        strncpy(t->outdir, g_cfg.temp_dir, sizeof(t->outdir)-1);
        snprintf(t->filepath, sizeof(t->filepath), "%s\\%s", t->outdir, t->filename);
        if (file_prealloc(t) != 0) {
            /* second fallback: next to the exe */
            char exedir[MAX_PATH];
            GetModuleFileNameA(NULL, exedir, MAX_PATH);
            char *sl = strrchr(exedir, '\\'); if (sl) *sl = 0;
            nlog("[task %d] falling back to exe dir %s", t->id, exedir);
            strncpy(t->outdir, exedir, sizeof(t->outdir)-1);
            snprintf(t->filepath, sizeof(t->filepath), "%s\\%s", t->outdir, t->filename);
            file_prealloc(t);
        }
    }
    if (t->file == INVALID_HANDLE_VALUE) {
        snprintf(t->err, sizeof(t->err), "cannot create output file: %s", t->filepath);
        nlog("[task %d] %s", t->id, t->err);
        t->state = TASK_ERROR;
        return 0;
    }
    nlog("[task %d] output preallocated: %s (%lld bytes)", t->id, t->filepath, t->size);

    t->state = TASK_RUNNING;
    nlog("[task %d] DownloadEngine is Starting (%d workers)...", t->id,
         (t->connections > t->nseg && t->resumable) ? t->nseg : t->connections);

    /* spawn workers */
    int nw = t->connections;
    if (nw > t->nseg && t->resumable) nw = t->nseg;
    if (nw < 1) nw = 1;
    HANDLE *ths = malloc(nw * sizeof(HANDLE));
    for (int i = 0; i < nw; i++) {
        WorkerArg *wa = malloc(sizeof(*wa));
        wa->t = t; wa->idx = i;
        ths[i] = CreateThread(NULL, 0, worker_main, wa, 0, NULL);
    }

    /* speed sampler + periodic checkpoint (segments.bin, like NeatDM) */
    int64_t last = 0;
    int ticks = 0;
    while (1) {
        Sleep(500);
        int64_t now = t->downloaded;
        double inst = (now - last) * 2.0;   /* bytes per second */
        t->speed = t->speed * 0.6 + inst * 0.4;
        last = now;
        if (++ticks % 4 == 0 && t->resumable) store_save_segments(t);
        EnterCriticalSection(&t->lock);
        int done = seg_all_done(t);
        int err = 0;
        for (int i = 0; i < t->nseg; i++) if (t->segs[i].state == SEG_ERROR) err = 1;
        LeaveCriticalSection(&t->lock);
        if (t->cancel || t->pause || done || err) break;
    }

    WaitForMultipleObjects(nw, ths, TRUE, INFINITE);
    for (int i = 0; i < nw; i++) CloseHandle(ths[i]);
    free(ths);

    if (t->file != INVALID_HANDLE_VALUE) {
        if (t->downloaded >= t->size && t->size > 0) {
            /* truncate to exact size (non-resumable case may overshoot cap) */
            LARGE_INTEGER li; li.QuadPart = t->downloaded < t->size ? t->downloaded : t->size;
            SetFilePointerEx(t->file, li, NULL, FILE_BEGIN);
            SetEndOfFile(t->file);
        }
        CloseHandle(t->file);
        t->file = INVALID_HANDLE_VALUE;
    }

    if (t->cancel) {
        t->state = TASK_CANCELED;
        nlog("task %d canceled (%lld/%lld bytes)", t->id, t->downloaded, t->size);
    } else if (t->pause) {
        store_save_segments(t);
        t->state = TASK_PAUSED;
        nlog("task %d paused (%lld/%lld bytes)", t->id, t->downloaded, t->size);
    } else if (seg_all_done(t)) {
        t->state = TASK_DONE;
        t->finished_at = time(NULL);
        store_delete_segments(t);
        nlog("File completely Downloaded. (%s)", t->filepath);
    } else {
        t->state = TASK_ERROR;
        store_save_segments(t);
        nlog("task %d error: %s", t->id, t->err);
    }
    t->pause = 0; t->cancel = 0;
    store_save_tasks();
    return 0;
}

void engine_start(DownloadTask *t)
{
    if (t->state == TASK_RUNNING || t->state == TASK_PROBING) return;
    nlog("[task %d] start (state %d -> probing)", t->id, t->state);
    t->cancel = 0; t->pause = 0;
    t->mgr_thread = CreateThread(NULL, 0, manager_main, t, 0, NULL);
}

void engine_pause(DownloadTask *t)
{
    if (t->state == TASK_RUNNING) { nlog("[task %d] pause requested", t->id); t->pause = 1; }
}

void engine_resume(DownloadTask *t)
{
    if (t->state == TASK_PAUSED || t->state == TASK_ERROR || t->state == TASK_CANCELED) {
        nlog("[task %d] Resume (from state %d, %lld/%lld bytes)", t->id, t->state, t->downloaded, t->size);
        engine_start(t);
    } else if (t->state == TASK_QUEUED) {
        engine_start(t);
    }
}

void engine_cancel(DownloadTask *t)
{
    nlog("[task %d] cancel requested", t->id);
    t->cancel = 1;
}

void engine_remove(DownloadTask *t)
{
    engine_cancel(t);
    if (t->mgr_thread) { WaitForSingleObject(t->mgr_thread, 5000); CloseHandle(t->mgr_thread); t->mgr_thread = NULL; }
    store_delete_segments(t);
    if (t->filepath[0]) DeleteFileA(t->filepath);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_ntasks; i++) {
        if (g_tasks[i] == t) {
            memmove(g_tasks+i, g_tasks+i+1, (g_ntasks-i-1)*sizeof(*g_tasks));
            g_ntasks--;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&t->lock);
    free(t->segs);
    free(t);
}

void engine_redownload(DownloadTask *t)
{
    if (t->state == TASK_RUNNING || t->state == TASK_PROBING) return;
    store_delete_segments(t);
    if (t->filepath[0]) DeleteFileA(t->filepath);
    EnterCriticalSection(&t->lock);
    for (int i = 0; i < t->nseg; i++) {
        t->segs[i].pos = t->segs[i].start;
        t->segs[i].state = SEG_IDLE;
        t->segs[i].retries = 0;
    }
    LeaveCriticalSection(&t->lock);
    InterlockedExchange64(&t->downloaded, 0);
    t->err[0] = 0;
    t->state = TASK_QUEUED;
    engine_start(t);
}
