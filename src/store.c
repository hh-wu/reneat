/* store.c — persistence: segments.bin per download + tasks.dat list.
 * Binary format, little-endian, like NeatDM's segments.bin. */
#include "engine.h"

#define SEG_MAGIC  0x4253434e  /* 'NCSB' */
#define TASK_MAGIC 0x4b53434e  /* 'NCSK' */

static void segfile_path(DownloadTask *t, char *out, int n)
{
    snprintf(out, n, "%s\\%s.segments.bin", t->outdir, t->filename);
}

static void wr_u32(FILE *f, uint32_t v){ fwrite(&v, 4, 1, f); }
static void wr_u64(FILE *f, uint64_t v){ fwrite(&v, 8, 1, f); }
static uint32_t rd_u32(FILE *f){ uint32_t v; if(fread(&v,4,1,f)!=1) return 0xffffffff; return v; }
static uint64_t rd_u64(FILE *f){ uint64_t v; if(fread(&v,8,1,f)!=1) return 0xffffffffffffffffULL; return v; }

int store_save_segments(DownloadTask *t)
{
    char p[MAX_PATH]; segfile_path(t, p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    EnterCriticalSection(&t->lock);
    wr_u32(f, SEG_MAGIC);
    wr_u32(f, 1);                        /* version */
    wr_u64(f, (uint64_t)t->size);
    wr_u32(f, t->resumable);
    wr_u32(f, (uint32_t)strlen(t->final_url));
    fwrite(t->final_url, 1, strlen(t->final_url), f);
    wr_u32(f, (uint32_t)t->nseg);
    for (int i = 0; i < t->nseg; i++) {
        wr_u64(f, (uint64_t)t->segs[i].start);
        wr_u64(f, (uint64_t)t->segs[i].pos);
        wr_u64(f, (uint64_t)t->segs[i].end);
        wr_u32(f, (uint32_t)t->segs[i].state);
    }
    LeaveCriticalSection(&t->lock);
    fclose(f);
    nlog("[task %d] segments.bin saved (%d segs)", t->id, t->nseg);
    return 0;
}

int store_load_segments(DownloadTask *t)
{
    char p[MAX_PATH]; segfile_path(t, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) { nlog("[task %d] segments.bin not found", t->id); return -1; }
    if (rd_u32(f) != SEG_MAGIC) { nlog("[task %d] segments.bin bad magic", t->id); fclose(f); return -1; }
    rd_u32(f); /* version */
    int64_t size = (int64_t)rd_u64(f);
    int resumable = (int)rd_u32(f);
    uint32_t urllen = rd_u32(f);
    if (urllen >= sizeof(t->final_url)) { fclose(f); return -1; }
    char url[2048] = {0};
    if (fread(url, 1, urllen, f) != urllen) { fclose(f); return -1; }
    if (size != t->size || !resumable) { nlog("[task %d] segments.bin size mismatch (%lld vs %lld), fresh start", t->id, size, t->size); fclose(f); return -1; }
    /* verify output file exists with correct size */
    HANDLE hf = CreateFileA(t->filepath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) { nlog("[task %d] output file missing, fresh start", t->id); fclose(f); return -1; }
    LARGE_INTEGER fs; GetFileSizeEx(hf, &fs); CloseHandle(hf);
    if (fs.QuadPart < size) { nlog("[task %d] output file truncated, fresh start", t->id); fclose(f); return -1; }

    uint32_t nseg = rd_u32(f);
    if (nseg > RENEAT_MAX_SEG) { fclose(f); return -1; }
    t->nseg = 0;
    for (uint32_t i = 0; i < nseg; i++) {
        int64_t st = (int64_t)rd_u64(f);
        int64_t pos = (int64_t)rd_u64(f);
        int64_t en = (int64_t)rd_u64(f);
        int state = (int)rd_u32(f);
        if (en <= st || pos < st || pos > en) { fclose(f); t->nseg = 0; return -1; }
        engine_seg_add(t, st, pos, en, state == SEG_DONE ? SEG_DONE : SEG_IDLE);
    }
    fclose(f);
    /* final URL might have changed host; still reuse unless empty */
    if (url[0] && !t->final_url[0]) strncpy(t->final_url, url, sizeof(t->final_url)-1);
    return 0;
}

void store_delete_segments(DownloadTask *t)
{
    char p[MAX_PATH]; segfile_path(t, p, sizeof(p));
    DeleteFileA(p);
}

static void taskfile_path(char *out, int n)
{
    char app[MAX_PATH];
    GetModuleFileNameA(NULL, app, MAX_PATH);
    char *sl = strrchr(app, '\\'); if (sl) *sl = 0;
    snprintf(out, n, "%s\\tasks.dat", app);
}

void store_save_tasks(void)
{
    char p[MAX_PATH]; taskfile_path(p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return;
    engine_lock();
    wr_u32(f, TASK_MAGIC);
    wr_u32(f, 1);
    wr_u32(f, (uint32_t)engine_count());
    for (int i = 0; i < engine_count(); i++) {
        DownloadTask *t = engine_task_at(i);
        wr_u32(f, (uint32_t)strlen(t->url)); fwrite(t->url, 1, strlen(t->url), f);
        wr_u32(f, (uint32_t)strlen(t->outdir)); fwrite(t->outdir, 1, strlen(t->outdir), f);
        wr_u32(f, (uint32_t)strlen(t->filename)); fwrite(t->filename, 1, strlen(t->filename), f);
        wr_u32(f, (uint32_t)t->connections);
        wr_u32(f, (uint32_t)t->state);
        wr_u64(f, (uint64_t)t->size);
        wr_u64(f, (uint64_t)t->downloaded);
        wr_u64(f, (uint64_t)t->added_at);
    }
    engine_unlock();
    fclose(f);
}

void store_load_tasks(void)
{
    char p[MAX_PATH]; taskfile_path(p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return;
    if (rd_u32(f) != TASK_MAGIC) { fclose(f); return; }
    rd_u32(f);
    uint32_t n = rd_u32(f);
    if (n > 10000) { fclose(f); return; }
    for (uint32_t i = 0; i < n; i++) {
        char url[2048]={0}, outdir[MAX_PATH]={0}, fname[512]={0};
        uint32_t l;
        l = rd_u32(f); if (l >= sizeof(url) || fread(url,1,l,f)!=l) break;
        l = rd_u32(f); if (l >= sizeof(outdir) || fread(outdir,1,l,f)!=l) break;
        l = rd_u32(f); if (l >= sizeof(fname) || fread(fname,1,l,f)!=l) break;
        int conn = (int)rd_u32(f);
        int state = (int)rd_u32(f);
        int64_t size = (int64_t)rd_u64(f);
        int64_t dl = (int64_t)rd_u64(f);
        uint64_t added = rd_u64(f);
        DownloadTask *t = engine_add(url, outdir, fname, conn);
        t->size = size;
        t->downloaded = dl;
        t->added_at = (time_t)added;
        t->completion_shown = 1;   /* don't pop completion dialogs for restored tasks */
        /* running/probing tasks become paused on restart */
        t->state = (state == TASK_RUNNING || state == TASK_PROBING) ? TASK_PAUSED : (TaskState)state;
        snprintf(t->filepath, sizeof(t->filepath), "%s\\%s", t->outdir, t->filename);
    }
    fclose(f);
}
