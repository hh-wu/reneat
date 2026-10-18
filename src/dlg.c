/* dlg.c — dialogs: Settings (General/Proxy/Passwords), About, Properties,
 * New URL, Download Complete.  Light theme, NeatDM look. */
#include "ui.h"
#include "lang.h"
#include "util.h"
#include <shellapi.h>
#include <commdlg.h>
#include <time.h>

/* ---------- generic modal window plumbing ---------- */

static void center_on(HWND dlg, HWND parent)
{
    RECT rp, rd; GetWindowRect(parent, &rp); GetWindowRect(dlg, &rd);
    int w = rd.right-rd.left, h = rd.bottom-rd.top;
    SetWindowPos(dlg, NULL, rp.left + (rp.right-rp.left-w)/2, rp.top + (rp.bottom-rp.top-h)/2,
                 0, 0, SWP_NOSIZE|SWP_NOZORDER);
}

static HWND mk(HWND dlg, const wchar_t *cls, const wchar_t *txt, int id,
               int x, int y, int w, int h, DWORD style)
{
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, dlg, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return c;
}

static LRESULT CALLBACK dlg_base_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        static HBRUSH br = NULL;
        if (!br) br = CreateSolidBrush(RGB(255,255,255));
        HDC dc = (HDC)wp;
        SetTextColor(dc, COL_TEXT);
        SetBkColor(dc, RGB(255,255,255));
        return (LRESULT)br;
    }
    case WM_DESTROY:
        /* 注意：不能在这里 PostQuitMessage —— 模态循环靠 IsWindow(dlg)==false
         * 自然结束；PostQuitMessage 会把主窗口的消息循环也终结（整个程序退出） */
        return 0;
    }
    return DefWindowProcW(dlg, msg, wp, lp);
}

static int run_modal(HWND parent, HWND dlg)
{
    center_on(dlg, parent);
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return 0;
}

static void get_text(HWND dlg, int id, char *out, int n)
{
    wchar_t w[1024];
    GetWindowTextW(GetDlgItem(dlg, id), w, 1024);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    str_trim(out);
}

static void set_text(HWND dlg, int id, const char *v)
{
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 1024);
    SetWindowTextW(GetDlgItem(dlg, id), w);
}

static int get_int(HWND dlg, int id)
{
    wchar_t w[64]; GetWindowTextW(GetDlgItem(dlg, id), w, 64);
    return _wtoi(w);
}

/* ================= New URL dialog ================= */

#define IDU_URL 4001
#define IDU_DIR 4002
#define IDU_CONN 4003
#define IDU_BROWSE 4004

static char g_add_url[2048], g_add_dir[MAX_PATH];
static int g_add_conn, g_add_ok;

static LRESULT CALLBACK addurl_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDU_BROWSE: {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn = {0};
            ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = dlg;
            ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L(L_ND_SAVETO);
            ofn.Flags = OFN_NOVALIDATE | OFN_NOTESTFILECREATE | OFN_HIDEREADONLY;
            if (GetSaveFileNameW(&ofn)) {
                wchar_t *sl = wcsrchr(file, L'\\'); if (sl) *sl = 0;
                SetWindowTextW(GetDlgItem(dlg, IDU_DIR), file);
            }
            return 0;
        }
        case IDOK:
            get_text(dlg, IDU_URL, g_add_url, sizeof(g_add_url));
            get_text(dlg, IDU_DIR, g_add_dir, sizeof(g_add_dir));
            g_add_conn = get_int(dlg, IDU_CONN);
            if (!g_add_url[0] || !strstr(g_add_url, "://")) {
                MessageBoxW(dlg, L(L_ND_BADURL), L"reneat", MB_ICONWARNING);
                return 0;
            }
            g_add_ok = 1;
            DestroyWindow(dlg);
            return 0;
        case IDCANCEL: DestroyWindow(dlg); return 0;
        }
        break;
    }
    return dlg_base_proc(dlg, msg, wp, lp);
}

int ui_add_download_dialog(HWND parent)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = addurl_proc; wc.hInstance = g_inst;
    wc.lpszClassName = L"reneat_addurl";
    wc.hbrBackground = CreateSolidBrush(COL_BG);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = ui_load_icon(101, 32);
    RegisterClassW(&wc);

    g_add_ok = 0; g_add_url[0] = 0;
    strncpy(g_add_dir, g_cfg.download_dir, sizeof(g_add_dir)-1);
    g_add_conn = g_cfg.max_connections;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L(L_ND_TITLE),
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 580, 230, parent, NULL, g_inst, NULL);

    /* prefill URL from clipboard if it looks like a link */
    if (OpenClipboard(dlg)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            wchar_t *w = GlobalLock(h);
            if (w && (wcsncmp(w, L"http://", 7)==0 || wcsncmp(w, L"https://", 8)==0))
                WideCharToMultiByte(CP_UTF8, 0, w, -1, g_add_url, sizeof(g_add_url), NULL, NULL);
            if (w) GlobalUnlock(h);
        }
        CloseClipboard();
    }

    mk(dlg, L"STATIC", L(L_ND_ADDRESS), 0, 20, 18, 120, 20, 0);
    mk(dlg, L"EDIT", L"", IDU_URL, 20, 40, 536, 26, WS_BORDER | ES_AUTOHSCROLL);
    set_text(dlg, IDU_URL, g_add_url);
    mk(dlg, L"STATIC", L(L_ND_SAVETO), 0, 20, 78, 120, 20, 0);
    mk(dlg, L"EDIT", L"", IDU_DIR, 20, 100, 424, 26, WS_BORDER | ES_AUTOHSCROLL);
    set_text(dlg, IDU_DIR, g_add_dir);
    mk(dlg, L"BUTTON", L"...", IDU_BROWSE, 452, 99, 40, 27, 0);
    mk(dlg, L"STATIC", L(L_ND_CONN), 0, 500, 78, 60, 20, 0);
    mk(dlg, L"EDIT", L"", IDU_CONN, 500, 100, 56, 26, WS_BORDER | ES_NUMBER);
    {
        wchar_t w[16]; _snwprintf(w, 16, L"%d", g_add_conn);
        SetWindowTextW(GetDlgItem(dlg, IDU_CONN), w);
    }
    mk(dlg, L"BUTTON", L(L_ND_START), IDOK, 336, 152, 104, 32, BS_DEFPUSHBUTTON);
    mk(dlg, L"BUTTON", L(L_CANCEL), IDCANCEL, 452, 152, 104, 32, 0);

    run_modal(parent, dlg);
    if (!g_add_ok) return 0;
    if (!g_add_dir[0]) strncpy(g_add_dir, g_cfg.download_dir, sizeof(g_add_dir)-1);
    if (g_add_conn <= 0) g_add_conn = g_cfg.max_connections;
    DownloadTask *t = engine_add(g_add_url, g_add_dir, NULL, g_add_conn);
    engine_start(t);
    store_save_tasks();
    ui_progress_show(t);   /* NeatDM: pop the download status window on start */
    ui_notify_changed();
    return 1;
}

/* ================= About ================= */

static LRESULT CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { DestroyWindow(dlg); return 0; }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(dlg, &ps);
        RECT cr; GetClientRect(dlg, &cr);
        ui_fill_rect(dc, &cr, COL_BG);
        HICON n = ui_load_icon(103, 64);
        if (n) { DrawIconEx(dc, 30, 22, n, 64, 64, 0, NULL, DI_NORMAL); DestroyIcon(n); }
        RECT r = { 115, 30, 228, 54 };
        wchar_t ver[64];
        _snwprintf(ver, 64, L"reneat %s", L"0.2");
        ui_draw_text(dc, ver, &r, g_font_bold, COL_TEXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        r.top += 22; r.bottom += 22;
        ui_draw_text(dc, L(L_ABOUT_LICENSE), &r, g_font, COL_TEXT_DIM, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        r.left = 30; r.top = 104; r.bottom = 126; r.right = cr.right - 16;
        ui_draw_text(dc, L(L_ABOUT_WEBSITE), &r, g_font, COL_TEXT_BLUE, DT_LEFT);
        r.top += 22; r.bottom += 22;
        ui_draw_text(dc, L"Pure-C clone for study. Icons (c) Javad Motallebi.", &r, g_font_small, COL_TEXT_DIM, DT_LEFT);
        EndPaint(dlg, &ps);
        return 0;
    }
    }
    return dlg_base_proc(dlg, msg, wp, lp);
}

void ui_show_about(HWND parent)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = about_proc; wc.hInstance = g_inst;
    wc.lpszClassName = L"reneat_about";
    wc.hbrBackground = CreateSolidBrush(COL_BG);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = ui_load_icon(101, 32);
    RegisterClassW(&wc);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L(L_ABOUT_TITLE),
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 424, 240, parent, NULL, g_inst, NULL);
    mk(dlg, L"BUTTON", L(L_ABOUT_CHECK), 0, 232, 24, 160, 28, 0);
    mk(dlg, L"BUTTON", L(L_OK), IDOK, 167, 150, 90, 30, BS_DEFPUSHBUTTON);
    run_modal(parent, dlg);
}

/* ================= Properties ================= */

typedef struct { DownloadTask *t; } PropsCtx;

static LRESULT CALLBACK props_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    PropsCtx *cx = (PropsCtx*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
            case IDOK: case IDCANCEL: DestroyWindow(dlg); return 0;
            case 4101: if (cx) ui_open_folder(cx->t); return 0;   /* Open Folder */
        }
        break;
    case WM_PAINT: {
        if (!cx) break;
        PAINTSTRUCT ps; HDC dc = BeginPaint(dlg, &ps);
        RECT cr; GetClientRect(dlg, &cr);
        ui_fill_rect(dc, &cr, COL_BG);
        DownloadTask *t = cx->t;
        int x = 20, y = 16;

        HICON f = ui_load_icon(107, 32);
        if (f) { DrawIconEx(dc, x, y, f, 32, 32, 0, NULL, DI_NORMAL); DestroyIcon(f); }
        wchar_t w[1024];
        MultiByteToWideChar(CP_UTF8, 0, t->filename, -1, w, 1024);
        RECT r = { x + 44, y + 6, cr.right - 16, y + 30 };
        ui_draw_text(dc, w, &r, g_font_bold, COL_TEXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 48;
        RECT sep = { 0, y, cr.right, y + 1 }; ui_fill_rect(dc, &sep, COL_BORDER);
        y += 12;

        wchar_t sz[128];
        const wchar_t *st;
        if (t->size > 0) {
            wchar_t fs[64]; format_size(t->size, fs, 64);
            _snwprintf(sz, 128, L"%lld Bytes ( %s )", t->size, fs);
        } else wcscpy(sz, L"?");
        switch (t->state) {
            case TASK_DONE: st = L(L_ST_COMPLETE); break;
            case TASK_RUNNING: st = L(L_ST_DOWNLOADING); break;
            case TASK_PAUSED: st = L(L_ST_PAUSED); break;
            case TASK_ERROR: st = L(L_ST_ERROR); break;
            default: st = L(L_ST_QUEUED); break;
        }
        struct { const wchar_t *k; const wchar_t *v; } rows[] = {
            { L(L_PW_FILESIZE), sz }, { L(L_COL_STATUS), st },
        };
        for (int i = 0; i < 2; i++) {
            RECT kr = { x, y, x + 90, y + 22 };
            ui_draw_text(dc, rows[i].k, &kr, g_font, COL_TEXT, DT_LEFT);
            RECT vr = { x + 100, y, cr.right - 16, y + 22 };
            ui_draw_text(dc, rows[i].v, &vr, g_font,
                         i == 1 && t->state == TASK_DONE ? COL_GREEN : COL_TEXT, DT_LEFT);
            y += 26;
        }
        /* paths */
        wchar_t wp[MAX_PATH];
        const char *vals[] = { t->outdir, t->url };
        const wchar_t *keys[] = { L(L_PROP_SAVEDTO), L(L_PROP_URL) };
        for (int i = 0; i < 2; i++) {
            RECT kr = { x, y, x + 90, y + 22 };
            ui_draw_text(dc, keys[i], &kr, g_font, COL_TEXT, DT_LEFT);
            MultiByteToWideChar(CP_UTF8, 0, vals[i], -1, w, 1024);
            RECT vr = { x + 100, y, cr.right - 110, y + 22 };
            ui_draw_text(dc, w, &vr, g_font, i == 1 ? COL_TEXT_BLUE : COL_TEXT,
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS);
            y += 28;
        }
        /* added / last try */
        static const wchar_t *MON[] = { L"", L"Jan",L"Feb",L"Mar",L"Apr",L"May",L"Jun",
                                        L"Jul",L"Aug",L"Sep",L"Oct",L"Nov",L"Dec" };
        struct tm tmv; localtime_s(&tmv, &t->added_at);
        _snwprintf(w, 1024, L"%s %02d %02d:%02d:%02d  %d", MON[tmv.tm_mon+1], tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tmv.tm_year + 1900);
        RECT kr = { x, y, x + 90, y + 22 };
        ui_draw_text(dc, L(L_PROP_ADDED), &kr, g_font, COL_TEXT, DT_LEFT);
        RECT vr = { x + 100, y, cr.right - 16, y + 22 };
        ui_draw_text(dc, w, &vr, g_font, COL_TEXT, DT_LEFT);
        EndPaint(dlg, &ps);
        return 0;
    }
    }
    return dlg_base_proc(dlg, msg, wp, lp);
}

void ui_show_properties(DownloadTask *t)
{
    static int reg = 0;
    WNDCLASSW wc = {0};
    if (!reg) {
        wc.lpfnWndProc = props_proc; wc.hInstance = g_inst;
        wc.lpszClassName = L"reneat_props";
        wc.hbrBackground = CreateSolidBrush(COL_BG);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = ui_load_icon(101, 32);
        RegisterClassW(&wc);
        reg = 1;
    }
    HWND parent = NULL;
    PropsCtx *cx = calloc(1, sizeof(*cx));
    cx->t = t;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"reneat_props", L(L_PROP_TITLE),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        520, 330, parent, NULL, g_inst, NULL);
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)cx);
    mk(dlg, L"BUTTON", L(L_CM_OPENFOLDER), 4101, 330, 118, 160, 26, 0);
    mk(dlg, L"BUTTON", L(L_OK), IDOK, 215, 250, 90, 30, BS_DEFPUSHBUTTON);
    /* non-modal like NeatDM's */
}

/* ================= Download Complete ================= */

typedef struct { DownloadTask *t; } CompCtx;

static LRESULT CALLBACK comp_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CompCtx *cx = (CompCtx*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 4201: if (cx) { wchar_t wp2[MAX_PATH]; MultiByteToWideChar(CP_UTF8,0,cx->t->filepath,-1,wp2,MAX_PATH); ShellExecuteW(NULL,L(L_CM_OPEN),wp2,NULL,NULL,SW_SHOW); } DestroyWindow(dlg); return 0;
        case 4202: if (cx) ui_open_folder(cx->t); DestroyWindow(dlg); return 0;
        case IDOK: case IDCANCEL: DestroyWindow(dlg); return 0;
        }
        break;
    case WM_PAINT: {
        if (!cx) break;
        PAINTSTRUCT ps; HDC dc = BeginPaint(dlg, &ps);
        RECT cr; GetClientRect(dlg, &cr);
        ui_fill_rect(dc, &cr, COL_BG);
        HICON n = ui_load_icon(103, 48);
        if (n) { DrawIconEx(dc, 24, 24, n, 48, 48, 0, NULL, DI_NORMAL); DestroyIcon(n); }
        RECT r = { 90, 26, cr.right - 16, 50 };
        ui_draw_text(dc, L(L_COMP_TITLE), &r, g_font_bold, COL_GREEN, DT_LEFT);
        wchar_t w[1024];
        MultiByteToWideChar(CP_UTF8, 0, cx->t->filename, -1, w, 1024);
        r.top += 28; r.bottom += 28;
        ui_draw_text(dc, w, &r, g_font, COL_TEXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        wchar_t sz[64]; format_size(cx->t->size, sz, 64);
        r.top += 24; r.bottom += 24;
        ui_draw_text(dc, sz, &r, g_font_small, COL_TEXT_DIM, DT_LEFT);
        EndPaint(dlg, &ps);
        return 0;
    }
    case WM_DESTROY:
        free(cx); SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
        return dlg_base_proc(dlg, msg, wp, lp);
    }
    return dlg_base_proc(dlg, msg, wp, lp);
}

void ui_show_completion(DownloadTask *t)
{
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = comp_proc; wc.hInstance = g_inst;
        wc.lpszClassName = L"reneat_comp";
        wc.hbrBackground = CreateSolidBrush(COL_BG);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = ui_load_icon(101, 32);
        RegisterClassW(&wc);
        reg = 1;
    }
    CompCtx *cx = calloc(1, sizeof(*cx));
    cx->t = t;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"reneat_comp", L"reneat",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        420, 210, NULL, NULL, g_inst, NULL);
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)cx);
    mk(dlg, L"BUTTON", L(L_CM_OPEN), 4201, 40, 130, 100, 30, BS_DEFPUSHBUTTON);
    mk(dlg, L"BUTTON", L(L_CM_OPENFOLDER), 4202, 150, 130, 110, 30, 0);
    mk(dlg, L"BUTTON", L(L_COMP_CLOSE), IDCANCEL, 270, 130, 100, 30, 0);
    /* non-modal */
}

/* ================= Settings (3 tabs) ================= */

#define IDS_TAB_GENERAL 0
#define IDS_TAB_PROXY   1
#define IDS_TAB_PASS    2

typedef struct {
    int tab;
    Settings tmp;
    /* general */
    HWND conn_drop, bw_edit, ua_edit, dir_edit, tmp_edit, lang_drop, chk_clip;
    HWND chk_ua, chk_cat, chk_comp, chk_start, radio_all, radio_one;
    /* proxy */
    HWND radio_none, radio_http, radio_socks, radio_v5, radio_v4;
    HWND pa_edit, pp_edit, pu_edit, pw_edit;
    HWND pa2_edit, pp2_edit, pa3_edit, pp3_edit;
    HWND chk_https, chk_ftp;
    /* page ownership lists */
    HWND gens[24]; int ngens;
    HWND prxs[24]; int nprxs;
} SettingsCtx;

static void settings_layout(SettingsCtx *cx, HWND dlg)
{
    int g = cx->tab == IDS_TAB_GENERAL, p = cx->tab == IDS_TAB_PROXY;
    for (int i = 0; i < cx->ngens; i++) ShowWindow(cx->gens[i], g ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < cx->nprxs; i++) ShowWindow(cx->prxs[i], p ? SW_SHOW : SW_HIDE);
    InvalidateRect(dlg, NULL, TRUE);
}

static void settings_from_ctl(SettingsCtx *cx, HWND dlg)
{
    Settings *s = &cx->tmp;
    s->max_connections = (int)SendMessageW(cx->conn_drop, CB_GETCURSEL, 0, 0) + 1;
    if (s->max_connections < 1) s->max_connections = 8;
    s->conn_one_by_one = SendMessageW(cx->radio_one, BM_GETCHECK, 0, 0) == BST_CHECKED;
    wchar_t w[64]; GetWindowTextW(cx->bw_edit, w, 64); s->bw_limit_kbs = _wtoi(w);
    get_text(dlg, 4303, s->user_agent, sizeof(s->user_agent));
    s->ua_for_all = SendMessageW(cx->chk_ua, BM_GETCHECK, 0, 0) == BST_CHECKED;
    get_text(dlg, 4304, s->download_dir, sizeof(s->download_dir));
    s->category_folders = SendMessageW(cx->chk_cat, BM_GETCHECK, 0, 0) == BST_CHECKED;
    get_text(dlg, 4305, s->temp_dir, sizeof(s->temp_dir));
    s->show_completion = SendMessageW(cx->chk_comp, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->start_on_startup = SendMessageW(cx->chk_start, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (SendMessageW(cx->radio_http, BM_GETCHECK, 0, 0) == BST_CHECKED) s->proxy_mode = 1;
    else if (SendMessageW(cx->radio_socks, BM_GETCHECK, 0, 0) == BST_CHECKED) s->proxy_mode = 2;
    else s->proxy_mode = 0;
    s->socks_v5 = SendMessageW(cx->radio_v5, BM_GETCHECK, 0, 0) == BST_CHECKED;
    get_text(dlg, 4306, s->proxy_addr, sizeof(s->proxy_addr));
    wchar_t pp[16]; GetWindowTextW(cx->pp_edit, pp, 16); s->proxy_port = _wtoi(pp);
    get_text(dlg, 4307, s->proxy_user, sizeof(s->proxy_user));
    get_text(dlg, 4308, s->proxy_pass, sizeof(s->proxy_pass));
    s->https_via_proxy = SendMessageW(cx->chk_https, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->ftp_via_proxy = SendMessageW(cx->chk_ftp, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->language = (int)SendMessageW(cx->lang_drop, CB_GETCURSEL, 0, 0);
    if (s->language < 0) s->language = 0;
    s->monitor_clipboard = SendMessageW(cx->chk_clip, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void settings_paint_general(HWND dlg, HDC dc)
{
    RECT cr; GetClientRect(dlg, &cr);
    int x = 26, y = 52;
    RECT r = { x, y, x + 200, y + 20 };
    ui_draw_text(dc, L(L_SG_MAXCONN), &r, g_font, COL_TEXT, DT_LEFT);
    RECT r2 = { 446, y + 3, cr.right - 4, y + 20 };
    ui_draw_text(dc, L(L_SG_RECOMMENDED), &r2, g_font_small, COL_TEXT_BLUE, DT_LEFT);
    y += 40;
    RECT r3 = { x, y, cr.right - 30, y + 20 };
    ui_draw_text(dc, L(L_SG_WHEN1ST), &r3, g_font_small, COL_TEXT_BLUE, DT_LEFT);
}

static void settings_paint_proxy(HDC dc)
{
    struct { int t; int x, w; } cols[] = {
        { L_SP_ADDR, 196, 110 }, { L_SP_PORT, 312, 44 }, { L_SP_USER, 362, 70 }, { L_SP_PASS, 438, 80 }
    };
    for (int i = 0; i < 4; i++) {
        RECT r = { cols[i].x, 68, cols[i].x + cols[i].w, 88 };
        ui_draw_text(dc, L(cols[i].t), &r, g_font_small, COL_TEXT, DT_CENTER);
    }
}

static void settings_paint_pass(HWND dlg, HDC dc)
{
    /* table header like NeatDM Passwords tab */
    RECT cr; GetClientRect(dlg, &cr);
    RECT hr = { 24, 50, cr.right - 24, 76 };
    HPEN p = CreatePen(PS_SOLID, 1, COL_BORDER);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, hr.left, hr.top, hr.right, hr.bottom);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
    RECT body = { 24, 76, cr.right - 24, cr.bottom - 130 };
    p = CreatePen(PS_SOLID, 1, COL_BORDER);
    op = SelectObject(dc, p); ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, body.left, body.top, body.right, body.bottom);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
    const int cols[] = { L_PW_PROTOCOL, L_PW_TARGET, L_SP_USER, L_SP_PASS };
    int cw = (cr.right - 48) / 4;
    for (int i = 0; i < 4; i++) {
        RECT r = { 28 + i*cw, 52, 28 + (i+1)*cw, 74 };
        ui_draw_text(dc, L(cols[i]), &r, g_font_small, COL_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    RECT empty = { 24, 90, cr.right - 24, 116 };
    ui_draw_text(dc, L(L_PW_EMPTY), &empty, g_font_small, COL_TEXT_DIM, DT_CENTER);
}

static LRESULT CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsCtx *cx = (SettingsCtx*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_LBUTTONDOWN: {
        if (!cx) break;
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (pt.y >= 8 && pt.y <= 34) {
            static const int tabs[3] = { L_TAB_GENERAL, L_TAB_PROXY, L_TAB_PASSWORDS };
            int x = 12;
            HDC dc = GetDC(dlg);
            for (int i = 0; i < 3; i++) {
                SIZE sz; HFONT of = SelectObject(dc, g_font);
                GetTextExtentPoint32W(dc, L(tabs[i]), (int)wcslen(L(tabs[i])), &sz);
                SelectObject(dc, of);
                int tw = sz.cx + 24;
                if (pt.x >= x && pt.x < x + tw) { cx->tab = i; settings_layout(cx, dlg); break; }
                x += tw;
            }
            ReleaseDC(dlg, dc);
        }
        return 0;
    }
    case WM_PAINT: {
        if (!cx) break;
        PAINTSTRUCT ps; HDC dc = BeginPaint(dlg, &ps);
        RECT cr; GetClientRect(dlg, &cr);
        ui_fill_rect(dc, &cr, COL_BG);
        static const int tabs[3] = { L_TAB_GENERAL, L_TAB_PROXY, L_TAB_PASSWORDS };
        int x = 12;
        for (int i = 0; i < 3; i++) {
            SIZE sz; HFONT of = SelectObject(dc, g_font);
            GetTextExtentPoint32W(dc, L(tabs[i]), (int)wcslen(L(tabs[i])), &sz);
            SelectObject(dc, of);
            int tw = sz.cx + 24;
            RECT tr = { x, 8, x + tw, 34 };
            if (cx->tab == i) {
                ui_fill_rect(dc, &tr, COL_BG);
                HPEN p = CreatePen(PS_SOLID, 1, COL_BORDER);
                HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                MoveToEx(dc, tr.left, tr.bottom, NULL);
                LineTo(dc, tr.left, tr.top); LineTo(dc, tr.right, tr.top); LineTo(dc, tr.right, tr.bottom);
                SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
            }
            ui_draw_text(dc, L(tabs[i]), &tr, g_font, COL_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x += tw;
        }
        RECT tl = { 0, 34, cr.right, 35 };
        ui_fill_rect(dc, &tl, COL_BORDER);
        if (cx->tab == IDS_TAB_GENERAL) settings_paint_general(dlg, dc);
        else if (cx->tab == IDS_TAB_PROXY) settings_paint_proxy(dc);
        else settings_paint_pass(dlg, dc);
        EndPaint(dlg, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (!cx) break;
        if (LOWORD(wp) == IDOK) {
            settings_from_ctl(cx, dlg);
            g_cfg = cx->tmp;
            settings_save();
            settings_apply_startup();
            DestroyWindow(dlg);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(dlg); return 0; }
        break;
    case WM_DESTROY:
        free(cx); SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
        return dlg_base_proc(dlg, msg, wp, lp);
    }
    return dlg_base_proc(dlg, msg, wp, lp);
}

static void ui_show_settings_tab(HWND parent, int tab)
{
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = settings_proc; wc.hInstance = g_inst;
        wc.lpszClassName = L"reneat_settings";
        wc.hbrBackground = CreateSolidBrush(COL_BG);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = ui_load_icon(101, 32);
        RegisterClassW(&wc);
        reg = 1;
    }
    SettingsCtx *cx = calloc(1, sizeof(*cx));
    cx->tmp = g_cfg;
    cx->tab = tab;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"reneat_settings", L(L_SET_TITLE),
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 544, 600, parent, NULL, g_inst, NULL);
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)cx);

    /* --- General page controls --- */
    int ng = 0;
    #define GEN(h) do { cx->gens[ng++] = (h); } while (0)
    cx->conn_drop = mk(dlg, L"COMBOBOX", L"", 4300, 370, 48, 66, 200, CBS_DROPDOWNLIST);
    GEN(cx->conn_drop);
    for (int i = 1; i <= 32; i++) { wchar_t w[8]; _snwprintf(w, 8, L"%d", i); SendMessageW(cx->conn_drop, CB_ADDSTRING, 0, (LPARAM)w); }
    SendMessageW(cx->conn_drop, CB_SETCURSEL, cx->tmp.max_connections - 1, 0);
    cx->radio_all = mk(dlg, L"BUTTON", L(L_SG_ALLATONCE), 4301, 46, 108, 320, 20, BS_AUTORADIOBUTTON | WS_GROUP);
    cx->radio_one = mk(dlg, L"BUTTON", L(L_SG_ONEBYONE), 4302, 46, 132, 320, 20, BS_AUTORADIOBUTTON);
    GEN(cx->radio_all); GEN(cx->radio_one);
    SendMessageW(cx->tmp.conn_one_by_one ? cx->radio_one : cx->radio_all, BM_SETCHECK, BST_CHECKED, 0);
    GEN(mk(dlg, L"STATIC", L(L_SG_BWLIMIT), 0, 26, 168, 260, 20, 0));
    cx->bw_edit = mk(dlg, L"EDIT", L"", 4309, 300, 164, 60, 24, WS_BORDER | ES_NUMBER);
    GEN(cx->bw_edit);
    { wchar_t w[16]; _snwprintf(w, 16, L"%d", cx->tmp.bw_limit_kbs); SetWindowTextW(cx->bw_edit, w); }
    GEN(mk(dlg, L"STATIC", L(L_SG_NOLIMIT), 0, 380, 168, 150, 20, 0));
    GEN(mk(dlg, L"STATIC", L(L_SG_UA), 0, 26, 204, 200, 20, 0));
    cx->ua_edit = mk(dlg, L"EDIT", L"", 4303, 180, 200, 280, 24, WS_BORDER | ES_AUTOHSCROLL);
    GEN(cx->ua_edit);
    set_text(dlg, 4303, cx->tmp.user_agent);
    cx->chk_ua = mk(dlg, L"BUTTON", L(L_SG_UAFORALL), 4310, 42, 230, 460, 20, BS_AUTOCHECKBOX);
    GEN(cx->chk_ua);
    SendMessageW(cx->chk_ua, BM_SETCHECK, cx->tmp.ua_for_all ? BST_CHECKED : BST_UNCHECKED, 0);
    GEN(mk(dlg, L"STATIC", L(L_SG_DLDIR), 0, 26, 264, 200, 20, 0));
    cx->dir_edit = mk(dlg, L"EDIT", L"", 4304, 180, 260, 300, 24, WS_BORDER | ES_AUTOHSCROLL);
    GEN(cx->dir_edit);
    set_text(dlg, 4304, cx->tmp.download_dir);
    cx->chk_cat = mk(dlg, L"BUTTON", L(L_SG_CATFOLDERS), 4311, 42, 290, 400, 20, BS_AUTOCHECKBOX);
    GEN(cx->chk_cat);
    SendMessageW(cx->chk_cat, BM_SETCHECK, cx->tmp.category_folders ? BST_CHECKED : BST_UNCHECKED, 0);
    GEN(mk(dlg, L"STATIC", L(L_SG_TMPDIR), 0, 26, 324, 200, 20, 0));
    cx->tmp_edit = mk(dlg, L"EDIT", L"", 4305, 180, 320, 300, 24, WS_BORDER | ES_AUTOHSCROLL);
    GEN(cx->tmp_edit);
    set_text(dlg, 4305, cx->tmp.temp_dir);
    cx->chk_comp = mk(dlg, L"BUTTON", L(L_SG_SHOWCOMP), 4312, 42, 360, 300, 20, BS_AUTOCHECKBOX);
    GEN(cx->chk_comp);
    SendMessageW(cx->chk_comp, BM_SETCHECK, cx->tmp.show_completion ? BST_CHECKED : BST_UNCHECKED, 0);
    cx->chk_start = mk(dlg, L"BUTTON", L(L_SG_STARTUP), 4313, 42, 384, 400, 20, BS_AUTOCHECKBOX);
    GEN(cx->chk_start);
    SendMessageW(cx->chk_start, BM_SETCHECK, cx->tmp.start_on_startup ? BST_CHECKED : BST_UNCHECKED, 0);
    GEN(mk(dlg, L"STATIC", L(L_SG_LANGUAGE), 0, 26, 420, 100, 20, 0));
    cx->lang_drop = mk(dlg, L"COMBOBOX", L"", 4340, 120, 416, 140, 200, CBS_DROPDOWNLIST);
    GEN(cx->lang_drop);
    SendMessageW(cx->lang_drop, CB_ADDSTRING, 0, (LPARAM)L"English");
    SendMessageW(cx->lang_drop, CB_ADDSTRING, 0, (LPARAM)L"中文");
    SendMessageW(cx->lang_drop, CB_SETCURSEL, cx->tmp.language == 1 ? 1 : 0, 0);
    cx->chk_clip = mk(dlg, L"BUTTON", L(L_SG_CLIP), 4341, 42, 448, 400, 20, BS_AUTOCHECKBOX);
    GEN(cx->chk_clip);
    SendMessageW(cx->chk_clip, BM_SETCHECK, cx->tmp.monitor_clipboard ? BST_CHECKED : BST_UNCHECKED, 0);
    cx->ngens = ng;

    /* --- Proxy page controls --- */
    int np = 0;
    #define PRX(h) do { cx->prxs[np++] = (h); } while (0)
    cx->radio_none = mk(dlg, L"BUTTON", L(L_SP_NOPROXY), 4320, 40, 58, 160, 20, BS_AUTORADIOBUTTON | WS_GROUP);
    cx->radio_http = mk(dlg, L"BUTTON", L(L_SP_HTTP), 4321, 40, 96, 140, 20, BS_AUTORADIOBUTTON);
    cx->radio_socks = mk(dlg, L"BUTTON", L(L_SP_SOCKS), 4322, 40, 126, 140, 20, BS_AUTORADIOBUTTON);
    cx->radio_v5 = mk(dlg, L"BUTTON", L(L_SP_V5), 4323, 66, 150, 100, 18, BS_AUTORADIOBUTTON | WS_GROUP);
    cx->radio_v4 = mk(dlg, L"BUTTON", L(L_SP_V4), 4324, 176, 150, 100, 18, BS_AUTORADIOBUTTON);
    PRX(cx->radio_none); PRX(cx->radio_http); PRX(cx->radio_socks); PRX(cx->radio_v5); PRX(cx->radio_v4);
    SendMessageW(cx->tmp.proxy_mode == 1 ? cx->radio_http : cx->tmp.proxy_mode == 2 ? cx->radio_socks : cx->radio_none, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(cx->tmp.socks_v5 ? cx->radio_v5 : cx->radio_v4, BM_SETCHECK, BST_CHECKED, 0);
    /* field grid: Address / Port / User / Password under each protocol row */
    cx->pa_edit = mk(dlg, L"EDIT", L"", 4306, 196, 94, 110, 22, WS_BORDER);
    cx->pp_edit = mk(dlg, L"EDIT", L"", 4325, 312, 94, 44, 22, WS_BORDER | ES_NUMBER);
    cx->pu_edit = mk(dlg, L"EDIT", L"", 4307, 362, 94, 70, 22, WS_BORDER);
    cx->pw_edit = mk(dlg, L"EDIT", L"", 4308, 438, 94, 80, 22, WS_BORDER | ES_PASSWORD);
    PRX(cx->pa_edit); PRX(cx->pp_edit); PRX(cx->pu_edit); PRX(cx->pw_edit);
    set_text(dlg, 4306, cx->tmp.proxy_addr);
    { wchar_t w[16]; _snwprintf(w, 16, L"%d", cx->tmp.proxy_port); SetWindowTextW(cx->pp_edit, w); }
    set_text(dlg, 4307, cx->tmp.proxy_user);
    set_text(dlg, 4308, cx->tmp.proxy_pass);
    cx->chk_https = mk(dlg, L"BUTTON", L(L_SP_HTTPS), 4326, 40, 196, 150, 20, BS_AUTOCHECKBOX);
    cx->chk_ftp = mk(dlg, L"BUTTON", L(L_SP_FTP), 4327, 40, 236, 150, 20, BS_AUTOCHECKBOX);
    PRX(cx->chk_https); PRX(cx->chk_ftp);
    SendMessageW(cx->chk_https, BM_SETCHECK, cx->tmp.https_via_proxy ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(cx->chk_ftp, BM_SETCHECK, cx->tmp.ftp_via_proxy ? BST_CHECKED : BST_UNCHECKED, 0);
    /* second field row for https / ftp protocols */
    cx->pa2_edit = mk(dlg, L"EDIT", L"", 4330, 196, 194, 110, 22, WS_BORDER);
    cx->pp2_edit = mk(dlg, L"EDIT", L"", 4331, 312, 194, 44, 22, WS_BORDER | ES_NUMBER);
    cx->pa3_edit = mk(dlg, L"EDIT", L"", 4332, 196, 234, 110, 22, WS_BORDER);
    cx->pp3_edit = mk(dlg, L"EDIT", L"", 4333, 312, 234, 44, 22, WS_BORDER | ES_NUMBER);
    PRX(cx->pa2_edit); PRX(cx->pp2_edit); PRX(cx->pa3_edit); PRX(cx->pp3_edit);
    cx->nprxs = np;
    #undef GEN
    #undef PRX

    mk(dlg, L"BUTTON", L(L_OK), IDOK, 268, 540, 110, 32, BS_DEFPUSHBUTTON);
    mk(dlg, L"BUTTON", L(L_CANCEL), IDCANCEL, 396, 540, 110, 32, 0);

    settings_layout(cx, dlg);
    run_modal(parent, dlg);
}

void ui_show_settings(HWND parent)
{
    ui_show_settings_tab(parent, 0);
}

void ui_show_settings_proxy(HWND parent) { ui_show_settings_tab(parent, 1); }
void ui_show_settings_passwords(HWND parent) { ui_show_settings_tab(parent, 2); }
