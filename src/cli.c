/* cli.c — console mode of reneat (invoked as: reneat.exe --cli <url> [outdir] [conn]).
 * Attaches to the parent console, mirrors the debug log there, prints progress. */
#include "common.h"
#include "engine.h"
#include "util.h"
#include "settings.h"

int cli_main(const char *url, const char *outdir, int conn)
{
    /* attach to the parent console so output is visible in the terminal */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    nlog_enable_console();

    if (!url || !strstr(url, "://")) {
        printf("reneat %s — NeatDM in C\n"
               "usage:\n"
               "  reneat.exe                                GUI\n"
               "  reneat.exe <url> [outdir] [connections]   GUI + start download\n"
               "  reneat.exe --cli <url> [outdir] [conn]    console mode\n"
               "log: <exedir>\\reneat.log (override with RENEAT_LOG)\n", RENEAT_VERSION);
        return 1;
    }
    const char *dir = (outdir && outdir[0]) ? outdir : ".";
    if (conn <= 0) conn = g_cfg.max_connections;

    nlog("cli: url=%s dir=%s conn=%d", url, dir, conn);
    DownloadTask *t = engine_add(url, dir, NULL, conn);
    engine_start(t);

    int64_t last = 0;
    DWORD t0 = GetTickCount();
    while (t->state == TASK_PROBING || t->state == TASK_RUNNING || t->state == TASK_QUEUED) {
        Sleep(500);
        int64_t dl = t->downloaded;
        double sp = (dl - last) * 2.0;
        last = dl;
        if (t->size > 0)
            printf("\r%.1f%%  %lld/%lld bytes  %.2f MB/s  segs=%d conns=%d   ",
                   t->downloaded * 100.0 / t->size, dl, t->size,
                   sp / 1048576.0, t->nseg, t->active_conns);
        else
            printf("\r%lld bytes  %.2f MB/s   ", dl, sp / 1048576.0);
        fflush(stdout);
    }
    printf("\n");
    if (t->state == TASK_DONE) {
        double secs = (GetTickCount() - t0) / 1000.0;
        printf("DONE: %s (%lld bytes in %.1fs, avg %.2f MB/s)\n",
               t->filepath, t->size, secs, t->size / secs / 1048576.0);
        nlog("cli: done %s (%lld bytes)", t->filepath, t->size);
        return 0;
    }
    printf("STATE=%d err=%s\n", t->state, t->err);
    nlog("cli: failed state=%d err=%s", t->state, t->err);
    return 2;
}
