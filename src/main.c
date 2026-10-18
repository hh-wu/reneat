/* main.c — single exe, two modes:
 *   reneat.exe                              GUI
 *   reneat.exe <url> [outdir] [conn]        GUI + start download
 *   reneat.exe --cli <url> [outdir] [conn]  console mode
 */
#include "common.h"
#include "engine.h"
#include "ui.h"
#include "settings.h"
#include "lang.h"
#include <shellapi.h>

int cli_main(const char *url, const char *outdir, int conn);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    engine_init();
    settings_load();
    lang_init();
    nlog("settings: max_conn=%d bw_limit=%dKB/s dl_dir=%s temp_dir=%s proxy_mode=%d(%s:%d) ua_for_all=%d",
         g_cfg.max_connections, g_cfg.bw_limit_kbs, g_cfg.download_dir, g_cfg.temp_dir,
         g_cfg.proxy_mode, g_cfg.proxy_addr, g_cfg.proxy_port, g_cfg.ua_for_all);

    int argc = 0;
    LPWSTR *argvw = CommandLineToArgvW(GetCommandLineW(), &argc);

    /* console mode */
    if (argvw && argc >= 2 && wcscmp(argvw[1], L"--cli") == 0) {
        char url[2048] = "", dir[MAX_PATH] = "";
        int conn = 0;
        if (argc >= 3) WideCharToMultiByte(CP_UTF8, 0, argvw[2], -1, url, sizeof(url), NULL, NULL);
        if (argc >= 4) WideCharToMultiByte(CP_UTF8, 0, argvw[3], -1, dir, sizeof(dir), NULL, NULL);
        if (argc >= 5) conn = _wtoi(argvw[4]);
        int r = cli_main(url, dir, conn);
        if (argvw) LocalFree(argvw);
        return r;
    }

    /* GUI: single instance, like NeatDM */
    HANDLE mx = CreateMutexW(NULL, TRUE, L"reneat_singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND h = FindWindowW(L"reneat_main", NULL);
        if (h) { SetForegroundWindow(h); }
        if (argvw) LocalFree(argvw);
        return 0;
    }
    store_load_tasks();
    g_inst = inst;   /* dialogs invoked via CLI flags need this before ui_run */

    /* reneat.exe <url> [outdir] [connections] — add + start from command line
     * (this is how the browser extension will hand off downloads) */
    if (argvw && argc >= 2) {
        char url[2048], dir[MAX_PATH];
        strncpy(dir, g_cfg.download_dir, sizeof(dir)-1);
        WideCharToMultiByte(CP_UTF8, 0, argvw[1], -1, url, sizeof(url), NULL, NULL);
        if (argc >= 3) WideCharToMultiByte(CP_UTF8, 0, argvw[2], -1, dir, sizeof(dir), NULL, NULL);
        int conn = argc >= 4 ? _wtoi(argvw[3]) : g_cfg.max_connections;
        if (strstr(url, "://")) {
            nlog("cmdline add: %s", url);
            DownloadTask *t = engine_add(url, dir, NULL, conn > 0 ? conn : g_cfg.max_connections);
            engine_start(t);
            store_save_tasks();
            /* NeatDM: browser-sent / command-line downloads pop the status window */
            int noprog = 0;
            for (int i = 1; i < argc; i++)
                if (wcscmp(argvw[i], L"--noprogwin") == 0) noprog = 1;
            if (!noprog) ui_progress_show(t);
            /* --progopt/--progconn: open progress window on a specific tab (UI testing) */
            for (int i = 1; i < argc; i++) {
                if (wcscmp(argvw[i], L"--progopt") == 0) ui_progress_show_tab(t, 1);
                if (wcscmp(argvw[i], L"--progconn") == 0) ui_progress_show_tab(t, 2);
            }
        }
    }
    /* hidden UI-test flags */
    for (int i = 1; argvw && i < argc; i++) {
        if (wcscmp(argvw[i], L"--settings") == 0) ui_show_settings(NULL);
        if (wcscmp(argvw[i], L"--setproxy") == 0) ui_show_settings_proxy(NULL);
        if (wcscmp(argvw[i], L"--setpass") == 0) ui_show_settings_passwords(NULL);
        if (wcscmp(argvw[i], L"--about") == 0) ui_show_about(NULL);
        if (wcscmp(argvw[i], L"--props") == 0 && engine_count() > 0) ui_show_properties(engine_task_at(0));
        if (wcscmp(argvw[i], L"--newurl") == 0) ui_add_download_dialog(NULL);
    }
    if (argvw) LocalFree(argvw);
    int r = ui_run(inst);
    ReleaseMutex(mx);
    CloseHandle(mx);
    return r;
}
