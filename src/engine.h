/* engine.h — SegmentManager: dynamic segmented download engine.
 *
 * Mirrors NeatDM's design:
 *  - file is split into N segments, each downloaded by its own connection
 *    with a Range request;
 *  - when a connection finishes early, the SegmentManager "steals" work by
 *    splitting the largest still-running segment in half (NeatDM log:
 *    "SegmentManager Created a New Segment", "Took a partial Segment");
 *  - failed segments roll back to IDLE and are retried by other workers
 *    ("Segment Rolled Back To Socket");
 *  - segment table is persisted to segments.bin next to the output file so
 *    downloads survive restarts ("Segments were loaded from segments.bin").
 */
#ifndef RENEAT_ENGINE_H
#define RENEAT_ENGINE_H
#include "common.h"

typedef enum { SEG_IDLE = 0, SEG_RUNNING, SEG_DONE, SEG_ERROR } SegState;
typedef enum {
    TASK_PROBING = 0, TASK_QUEUED, TASK_RUNNING, TASK_PAUSED,
    TASK_DONE, TASK_ERROR, TASK_CANCELED
} TaskState;

typedef struct {
    int64_t  start;      /* inclusive offset in file */
    int64_t  pos;        /* next byte to fetch      */
    int64_t  end;        /* exclusive               */
    int      state;
    int      retries;
    int      owner;      /* worker index, -1 none   */
} Segment;

typedef struct DownloadTask {
    int      id;
    char     url[2048];
    char     final_url[2048];
    char     outdir[MAX_PATH];
    char     filename[512];
    char     filepath[MAX_PATH];      /* outdir\filename */
    int      connections;             /* requested parallelism */
    /* optional per-task request extras (from browser extension) */
    char     referer[2048];
    char     cookies[2048];
    char     useragent[256];

    int64_t  size;                    /* -1 unknown */
    int      resumable;
    TaskState state;
    char     err[256];

    Segment *segs;
    int      nseg;
    int      ncap;

    HANDLE   file;                    /* preallocated output */
    CRITICAL_SECTION lock;
    HANDLE   mgr_thread;
    volatile int cancel;              /* pause/stop request */
    volatile int pause;

    /* stats (read by UI without lock: approximate is fine) */
    volatile int64_t downloaded;      /* sum of completed bytes */
    volatile double  speed;           /* bytes/sec, EMA         */
    int      active_conns;
    time_t   added_at;
    time_t   finished_at;
    int      category;                /* settings.h Category    */
    volatile int64_t bw_limit;        /* bytes/sec, 0 = none    */
    int      completion_shown;        /* UI: completion dialog popped */
    int      temp_fallback_done;      /* engine: already retried in temp dir */
} DownloadTask;

/* engine singleton */
void         engine_init(void);
DownloadTask *engine_add(const char *url, const char *outdir, const char *filename, int connections);
void         engine_start(DownloadTask *t);   /* probe + run (async) */
void         engine_pause(DownloadTask *t);
void         engine_resume(DownloadTask *t);
void         engine_cancel(DownloadTask *t);  /* stop, keep files for resume */
void         engine_remove(DownloadTask *t);  /* stop + delete partial files */
void         engine_redownload(DownloadTask *t); /* wipe progress, start fresh */
DownloadTask *engine_task_at(int i);
int          engine_count(void);
void         engine_lock(void);
void         engine_unlock(void);
int          engine_seg_add(DownloadTask *t, int64_t start, int64_t pos, int64_t end, int state);

/* segments.bin persistence (store.c) */
int  store_save_segments(DownloadTask *t);
int  store_load_segments(DownloadTask *t);    /* 0 = loaded and valid */
void store_delete_segments(DownloadTask *t);

/* task list persistence (tasks.dat, store.c) */
void store_save_tasks(void);
void store_load_tasks(void);

#endif
