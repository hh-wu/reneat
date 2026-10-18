/* ui.c — main window: big-icon toolbar, category sidebar, report list.
 * Light theme matching NeatDM 1.4 screenshots. */
#include "ui.h"
#include "util.h"
#include "lang.h"
#include "ws.h"
#include <shellapi.h>
#include <commdlg.h>
#include <time.h>

#define WM_APP_WSJOB  (WM_APP + 1)
#define WM_APP_TRAY   (WM_APP + 2)
#define TRAY_UID      1

static int g_really_quit = 0;
static int g_sizing = 0;             /* drag/resize in progress: skip timer repaints */
static char g_last_clip_url[2048] = "";
static HICON g_side_icons[3] = {0};  /* cached folder/video/audio... sidebar icons */
static UINT g_taskbar_created = 0;
static int g_tray_ok = 0;   /* tray registration succeeded or not */   /* explorer restart broadcast */

/* fwd */
static void add_and_show(const char *url, const char *referer, const char *cookies,
                         const char *ua, const char *filename);
static void tray_add(HWND hwnd);
static void tray_remove(void);
static void tray_menu(HWND hwnd);
static void clipboard_check(HWND hwnd);

HINSTANCE g_inst;
HFONT g_font, g_font_bold, g_font_small, g_font_title;

static HWND   g_hwnd;
static int    g_scroll = 0;
static int    g_selected = -1;       /* selected task row index (in filtered view) */
static int    g_hover_tb = -1;
static int    g_hover_row = -1;
static int    g_hover_side = -1;
static int    g_category = CAT_ALL;  /* active sidebar category */

/* toolbar */
enum { TB_NEW, TB_RESUME, TB_STOP, TB_DELETE, TB_SETTINGS, TB_BROWSERS, TB_ABOUT, TB_QUIT, TB_COUNT };
static const struct { int icon; int label; } TB_DEF[TB_COUNT] = {
    { 108, L_NEW_URL }, { 109, L_RESUME }, { 110, L_STOP }, { 111, L_DELETE },
    { 112, L_SETTINGS }, { 121, L_BROWSERS }, { 113, L_ABOUT }, { 114, L_QUIT }
};
static HICON g_tbico[TB_COUNT];
static RECT  g_tbrect[TB_COUNT];

/* sidebar items in draw order */
static const int SIDE_ITEMS[] = { CAT_ALL, CAT_VIDEO, CAT_AUDIO, CAT_COMPRESSED,
                                  CAT_DOCUMENT, CAT_APPLICATION, CAT_MISC,
                                  CAT_COMPLETE, CAT_INCOMPLETE };
#define SIDE_N 9
static RECT g_siderect[SIDE_N];

/* list columns */
static const int COLS[] = { L_COL_NAME, L_COL_SIZE, L_COL_STATUS, L_COL_BANDWIDTH, L_COL_REMAINING, L_COL_LASTTRY };
static int COLW[] = { 250, 80, 105, 85, 105, 130 };
#define NCOLS 6

/* File Name column absorbs extra client width */
static void compute_colw(int width, int *out)
{
    int extra = width - SIDEBAR_W - (250+80+105+85+105+130) - 10;
    out[0] = COLW[0] + (extra > 0 ? extra : 0);
    if (out[0] < 160) out[0] = 160;
    for (int i = 1; i < NCOLS; i++) out[i] = COLW[i];
}

/* file-type icon cache per extension */
typedef struct ExtIcon { char ext[16]; HICON ico; struct ExtIcon *next; } ExtIcon;
static ExtIcon *g_exticons = NULL;

/* ---------- shared helpers ---------- */

void ui_fill_rect(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

void ui_draw_text(HDC dc, const wchar_t *s, RECT *r, HFONT f, COLORREF c, UINT fmt)
{
    HFONT of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, r, fmt);
    SelectObject(dc, of);
}

HICON ui_load_icon(int id, int size)
{
    return (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
}

static HICON ext_icon(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    char ext[16] = "";
    if (dot) { strncpy(ext, dot, 15); }
    for (ExtIcon *e = g_exticons; e; e = e->next)
        if (str_ieq(e->ext, ext)) return e->ico;
    /* ask the shell */
    wchar_t wf[64]; MultiByteToWideChar(CP_UTF8, 0, ext[0] ? ext : ".file", -1, wf, 64);
    SHFILEINFOW sfi = {0};
    HICON ico = NULL;
    if (SHGetFileInfoW(wf, FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES))
        ico = sfi.hIcon;
    ExtIcon *e = calloc(1, sizeof(*e));
    strncpy(e->ext, ext, 15);
    e->ico = ico;
    e->next = g_exticons; g_exticons = e;
    return ico;
}

void ui_open_folder(DownloadTask *t)
{
    if (!t->filepath[0]) return;
    wchar_t wp[MAX_PATH], param[MAX_PATH + 16];
    MultiByteToWideChar(CP_UTF8, 0, t->filepath, -1, wp, MAX_PATH);
    _snwprintf(param, MAX_PATH + 16, L"/select,\"%s\"", wp);
    ShellExecuteW(NULL, L"open", L"explorer.exe", param, NULL, SW_SHOW);
}

static void open_file(DownloadTask *t)
{
    if (!t->filepath[0]) return;
    wchar_t wp[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, t->filepath, -1, wp, MAX_PATH);
    ShellExecuteW(NULL, L"open", wp, NULL, NULL, SW_SHOW);
}

/* ---------- filtered view ---------- */

static int task_visible(DownloadTask *t)
{
    switch (g_category) {
        case CAT_ALL: return 1;
        case CAT_COMPLETE: return t->state == TASK_DONE;
        case CAT_INCOMPLETE: return t->state != TASK_DONE;
        default: return t->category == g_category;
    }
}

static DownloadTask *row_task(int row, int *out_index)
{
    int r = 0;
    for (int i = 0; i < engine_count(); i++) {
        if (task_visible(engine_task_at(i))) {
            if (r == row) { if (out_index) *out_index = i; return engine_task_at(i); }
            r++;
        }
    }
    return NULL;
}

static int visible_count(void)
{
    int r = 0;
    for (int i = 0; i < engine_count(); i++) if (task_visible(engine_task_at(i))) r++;
    return r;
}

/* ---------- text formatting (NeatDM style) ---------- */

static void fmt_status(DownloadTask *t, wchar_t *out, int n)
{
    switch (t->state) {
        case TASK_PROBING:  _snwprintf(out, n, L(L_ST_CONNECTING)); break;
        case TASK_QUEUED:   _snwprintf(out, n, L(L_ST_QUEUED)); break;
        case TASK_RUNNING:  _snwprintf(out, n, L(L_ST_DOWNLOADING)); break;
        case TASK_PAUSED:
            if (t->size > 0) _snwprintf(out, n, L"%s ( %d%% )", L(L_ST_PAUSED), (int)(t->downloaded*100/t->size));
            else _snwprintf(out, n, L"%s", L(L_ST_PAUSED)); break;
        case TASK_DONE:     _snwprintf(out, n, L(L_ST_COMPLETE)); break;
        case TASK_ERROR: {
            /* surface HTTP status when we have one: "Error ( HTTP 403 )" */
            if (t->err[0]) {
                wchar_t we[128];
                MultiByteToWideChar(CP_UTF8, 0, t->err, -1, we, 128);
                _snwprintf(out, n, L"%s ( %s )", L(L_ST_ERROR), we);
            } else if (t->size > 0)
                _snwprintf(out, n, L"%s ( %d%% )", L(L_ST_ERROR), (int)(t->downloaded*100/t->size));
            else
                _snwprintf(out, n, L"%s", L(L_ST_ERROR));
            break;
        }
        case TASK_CANCELED: _snwprintf(out, n, L(L_ST_CANCELED)); break;
    }
}

static COLORREF status_color(DownloadTask *t)
{
    if (t->state == TASK_ERROR) return COL_ERR;
    if (t->state == TASK_PAUSED) return COL_WARN;
    return COL_TEXT;
}

static void fmt_lasttry(DownloadTask *t, wchar_t *out, int n)
{
    static const wchar_t *MON[] = { L"", L"Jan",L"Feb",L"Mar",L"Apr",L"May",L"Jun",
                                    L"Jul",L"Aug",L"Sep",L"Oct",L"Nov",L"Dec" };
    time_t tt = t->state == TASK_RUNNING ? time(NULL) :
                (t->finished_at ? t->finished_at : t->added_at);
    struct tm tmv; localtime_s(&tmv, &tt);
    _snwprintf(out, n, L"%s %02d %02d:%02d:%02d", MON[tmv.tm_mon+1], tmv.tm_mday,
               tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static void fmt_remaining(DownloadTask *t, wchar_t *out, int n)
{
    if (t->state != TASK_RUNNING || t->speed < 1 || t->size <= 0) { out[0] = 0; return; }
    int s = (int)((t->size - t->downloaded) / t->speed);
    int h = s / 3600, m = (s % 3600) / 60; s %= 60;
    if (h) _snwprintf(out, n, L(L_TIME_HR_MIN), h, m);
    else if (m) _snwprintf(out, n, L(L_TIME_MIN_SEC), m, s);
    else _snwprintf(out, n, L(L_TIME_SEC), s);
}

/* ---------- toolbar ---------- */

static void layout_toolbar(int width)
{
    int x = 10;
    for (int i = 0; i < TB_COUNT; i++) {
        if (i == TB_SETTINGS) { /* right-aligned group */
            int rx = width - 10;
            for (int j = TB_COUNT-1; j >= TB_SETTINGS; j--) {
                rx -= 76;
                SetRect(&g_tbrect[j], rx, 4, rx + 76, TOOLBAR_H - 4);
            }
            break;
        }
        SetRect(&g_tbrect[i], x, 4, x + 76, TOOLBAR_H - 4);
        x += 80;
    }
}

static void draw_toolbar(HDC dc, int width)
{
    RECT r = { 0, 0, width, TOOLBAR_H };
    ui_fill_rect(dc, &r, COL_TOOLBAR);
    RECT sep = { 0, TOOLBAR_H - 1, width, TOOLBAR_H };
    ui_fill_rect(dc, &sep, COL_BORDER);

    for (int i = 0; i < TB_COUNT; i++) {
        RECT *b = &g_tbrect[i];
        if (i == g_hover_tb) {
            HBRUSH br = CreateSolidBrush(COL_HOVER);
            HPEN p = CreatePen(PS_SOLID, 1, COL_BORDER);
            HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, p);
            RoundRect(dc, b->left, b->top, b->right, b->bottom, 6, 6);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(br); DeleteObject(p);
        }
        if (g_tbico[i])
            DrawIconEx(dc, b->left + (b->right-b->left-32)/2, b->top + 2, g_tbico[i], 32, 32, 0, NULL, DI_NORMAL);
        RECT tr = { b->left, b->top + 38, b->right, b->bottom };
        ui_draw_text(dc, L(TB_DEF[i].label), &tr, g_font_small, COL_TEXT, DT_CENTER | DT_SINGLELINE);
    }
}

/* ---------- sidebar ---------- */

static void layout_sidebar(int height)
{
    int y = TOOLBAR_H + 8;
    for (int i = 0; i < SIDE_N; i++) {
        int indent = (i >= 1 && i <= 6) ? 22 : 0;
        SetRect(&g_siderect[i], 4 + indent, y, SIDEBAR_W - 8, y + 26);
        y += 27;
        if (i == 0) y += 2;
    }
}

static void draw_sidebar(HDC dc, int height)
{
    RECT r = { 0, TOOLBAR_H, SIDEBAR_W, height };
    ui_fill_rect(dc, &r, COL_SIDEBAR);
    RECT sep = { SIDEBAR_W - 1, TOOLBAR_H, SIDEBAR_W, height };
    ui_fill_rect(dc, &sep, COL_BORDER);

    for (int i = 0; i < SIDE_N; i++) {
        RECT *ir = &g_siderect[i];
        int cat = SIDE_ITEMS[i];
        if (cat == g_category) ui_fill_rect(dc, ir, COL_SEL);
        else if (i == g_hover_side) ui_fill_rect(dc, ir, COL_HOVER);

        /* expand arrow for group items */
        if (cat == CAT_ALL || cat == CAT_COMPLETE || cat == CAT_INCOMPLETE) {
            POINT pts[3] = { { ir->left + 2, ir->top + 8 }, { ir->left + 10, ir->top + 13 }, { ir->left + 2, ir->top + 18 } };
            HPEN p = CreatePen(PS_SOLID, 1, COL_TEXT_DIM);
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, pts[0].x, pts[0].y, NULL);
            LineTo(dc, pts[1].x, pts[1].y);
            LineTo(dc, pts[2].x, pts[2].y);
            SelectObject(dc, op); DeleteObject(p);
        }
        /* folder/type icon (cached — LoadImage per frame made dragging stutter) */
        int icon_id = 104; /* folder */
        if (cat == CAT_VIDEO) icon_id = 105;
        else if (cat == CAT_AUDIO) icon_id = 106;
        else if (cat == CAT_DOCUMENT) icon_id = 107;
        int slot = icon_id - 104;
        if (slot < 0 || slot > 2) slot = 0;
        if (!g_side_icons[slot]) g_side_icons[slot] = ui_load_icon(icon_id, 16);
        if (g_side_icons[slot]) DrawIconEx(dc, ir->left + 14, ir->top + 5, g_side_icons[slot], 16, 16, 0, NULL, DI_NORMAL);

        RECT tr = { ir->left + 36, ir->top, ir->right, ir->bottom };
        ui_draw_text(dc, category_name(cat), &tr, g_font, COL_TEXT, DT_VCENTER | DT_SINGLELINE);
    }
}

/* ---------- list ---------- */

static void list_origin(int *x0, int *y0)
{
    *x0 = SIDEBAR_W;
    *y0 = TOOLBAR_H;
}

static void draw_list(HDC dc, int width, int height)
{
    int x0, y0; list_origin(&x0, &y0);
    RECT bg = { x0, y0, width, height };
    ui_fill_rect(dc, &bg, COL_BG);
    int cw[NCOLS]; compute_colw(width, cw);

    /* header */
    RECT hr = { x0, y0, width, y0 + HEADER_H };
    ui_fill_rect(dc, &hr, COL_HEADER);
    RECT hb = { x0, y0 + HEADER_H - 1, width, y0 + HEADER_H };
    ui_fill_rect(dc, &hb, COL_BORDER);
    int cx = x0 + 4;
    for (int c = 0; c < NCOLS; c++) {
        RECT tr = { cx + 4, y0, cx + cw[c], y0 + HEADER_H };
        ui_draw_text(dc, L(COLS[c]), &tr, g_font_small, COL_TEXT_DIM, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (c) { RECT sep = { cx, y0 + 4, cx + 1, y0 + HEADER_H - 4 }; ui_fill_rect(dc, &sep, COL_BORDER); }
        cx += cw[c];
    }

    /* rows */
    int n = visible_count();
    int y = y0 + HEADER_H - g_scroll;
    for (int row = 0; row < n; row++, y += ROW_H) {
        if (y + ROW_H < y0 + HEADER_H) continue;
        if (y > height) break;
        DownloadTask *t = row_task(row, NULL);
        if (!t) continue;
        RECT rr = { x0, y, width, y + ROW_H };
        if (row == g_selected) ui_fill_rect(dc, &rr, COL_SEL);
        else if (row == g_hover_row) ui_fill_rect(dc, &rr, COL_HOVER);

        cx = x0 + 4;
        for (int c = 0; c < NCOLS; c++) {
            RECT tr = { cx + 4, y, cx + cw[c] - 2, y + ROW_H };
            wchar_t w[512] = L"";
            COLORREF col = COL_TEXT;
            switch (c) {
                case 0: {
                    HICON ico = ext_icon(t->filename);
                    if (ico) DrawIconEx(dc, cx + 2, y + 5, ico, 16, 16, 0, NULL, DI_NORMAL);
                    tr.left += 20;
                    MultiByteToWideChar(CP_UTF8, 0, t->filename[0] ? t->filename : t->url, -1, w, 512);
                    break;
                }
                case 1:
                    if (t->size > 0) format_size(t->size, w, 512);
                    break;
                case 2: fmt_status(t, w, 512); col = status_color(t); break;
                case 3:
                    if (t->state == TASK_RUNNING) format_speed(t->speed, w, 512);
                    break;
                case 4: fmt_remaining(t, w, 512); break;
                case 5: fmt_lasttry(t, w, 512); col = COL_TEXT_DIM; break;
            }
            if (w[0]) ui_draw_text(dc, w, &tr, g_font_small, col, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            cx += cw[c];
        }
        RECT sep = { x0, y + ROW_H - 1, width, y + ROW_H };
        ui_fill_rect(dc, &sep, RGB(242, 243, 245));
    }
}

/* ---------- paint ---------- */

static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    int W = cr.right, H = cr.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ ob = SelectObject(mem, bmp);
    ui_fill_rect(mem, &cr, COL_BG);

    draw_toolbar(mem, W);
    layout_sidebar(H);
    draw_sidebar(mem, H);
    draw_list(mem, W, H);

    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp); DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* ---------- input ---------- */

static int toolbar_hit(POINT pt)
{
    for (int i = 0; i < TB_COUNT; i++)
        if (PtInRect(&g_tbrect[i], pt)) return i;
    return -1;
}

static int sidebar_hit(POINT pt)
{
    for (int i = 0; i < SIDE_N; i++)
        if (PtInRect(&g_siderect[i], pt)) return i;
    return -1;
}

static int row_hit(POINT pt, int height)
{
    int x0, y0; list_origin(&x0, &y0);
    if (pt.x < x0 || pt.y < y0 + HEADER_H) return -1;
    int row = (pt.y - (y0 + HEADER_H) + g_scroll) / ROW_H;
    return (row >= 0 && row < visible_count()) ? row : -1;
}

/* completion dialog tracking */
static void check_completions(void)
{
    static int inited = 0;
    if (!inited) { inited = 1; return; }   /* don't pop dialogs for old tasks on startup */
    for (int i = 0; i < engine_count(); i++) {
        DownloadTask *t = engine_task_at(i);
        if (t->state == TASK_DONE && !t->completion_shown) {
            t->completion_shown = 1;
            if (g_cfg.show_completion) ui_show_completion(t);
        }
    }
}

static void do_toolbar(HWND hwnd, int tb)
{
    DownloadTask *sel = g_selected >= 0 ? row_task(g_selected, NULL) : NULL;
    switch (tb) {
        case TB_NEW: ui_add_download_dialog(hwnd); break;
        case TB_RESUME: if (sel) engine_resume(sel); break;
        case TB_STOP: if (sel) engine_pause(sel); break;
        case TB_DELETE:
            if (sel && MessageBoxW(hwnd, L(L_DELETE_CONFIRM), L"reneat",
                                   MB_YESNO | MB_ICONQUESTION) == IDYES) {
                engine_remove(sel);
                g_selected = -1;
                store_save_tasks();
            }
            break;
        case TB_SETTINGS: ui_show_settings(hwnd); break;
        case TB_BROWSERS: {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L(L_EXT_CHROME));
            AppendMenuW(m, MF_STRING, 2, L(L_EXT_EDGE));
            AppendMenuW(m, MF_STRING, 3, L(L_EXT_FIREFOX));
            AppendMenuW(m, MF_STRING, 4, L(L_EXT_OPERA));
            RECT *b = &g_tbrect[TB_BROWSERS];
            POINT pt = { b->left, b->bottom };
            ClientToScreen(hwnd, &pt);
            TrackPopupMenu(m, TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
            break;
        }
        case TB_ABOUT: ui_show_about(hwnd); break;
        case TB_QUIT:
            g_really_quit = 1;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

enum { CM_RESUME=100, CM_STOP, CM_REDOWNLOAD, CM_SHOWWIN, CM_OPENFOLDER, CM_OPEN, CM_DELETE, CM_PROPS };

static void show_row_menu(HWND hwnd, POINT sc, int row)
{
    DownloadTask *t = row_task(row, NULL);
    if (!t) return;
    HMENU m = CreatePopupMenu();
    UINT run = (t->state == TASK_RUNNING) ? MF_GRAYED : 0;
    UINT stop = (t->state == TASK_RUNNING) ? 0 : MF_GRAYED;
    AppendMenuW(m, MF_STRING | run, CM_RESUME, L(L_RESUME));
    AppendMenuW(m, MF_STRING | stop, CM_STOP, L(L_STOP));
    AppendMenuW(m, MF_STRING, CM_REDOWNLOAD, L(L_CM_REDOWNLOAD));
    AppendMenuW(m, MF_STRING, CM_SHOWWIN, L(L_CM_SHOWWIN));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CM_OPENFOLDER, L(L_CM_OPENFOLDER));
    AppendMenuW(m, MF_STRING, CM_OPEN, L(L_CM_OPEN));
    AppendMenuW(m, MF_STRING, CM_DELETE, L(L_DELETE));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CM_PROPS, L(L_CM_PROPERTIES));
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                             sc.x, sc.y, 0, hwnd, NULL);
    DestroyMenu(m);
    switch (cmd) {
        case CM_RESUME: engine_resume(t); break;
        case CM_STOP: engine_pause(t); break;
        case CM_REDOWNLOAD: engine_redownload(t); break;
        case CM_SHOWWIN: ui_progress_show(t); break;
        case CM_OPENFOLDER: ui_open_folder(t); break;
        case CM_OPEN: open_file(t); break;
        case CM_DELETE: engine_remove(t); g_selected = -1; store_save_tasks(); break;
        case CM_PROPS: ui_show_properties(t); break;
    }
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_font = CreateFontW(-15, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_font_bold = CreateFontW(-15, 0,0,0, FW_SEMIBOLD, 0,0,0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_font_small = CreateFontW(-13, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_font_title = CreateFontW(-24, 0,0,0, FW_LIGHT, 0,0,0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (int i = 0; i < TB_COUNT; i++) g_tbico[i] = ui_load_icon(TB_DEF[i].icon, 32);
        RECT cr; GetClientRect(hwnd, &cr);
        layout_toolbar(cr.right);
        SetTimer(hwnd, 1, 500, NULL);
        HICON app = ui_load_icon(101, 32);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)app);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ui_load_icon(102, 16));
        tray_add(hwnd);
        if (g_cfg.monitor_clipboard) AddClipboardFormatListener(hwnd);
        if (g_cfg.ws_port > 0) ws_start(g_cfg.ws_port);
        return 0;
    }
    case WM_TIMER:
        if (g_sizing) return 0;   /* don't fight the window drag */
        check_completions();
        for (int i = 0; i < engine_count(); i++) {
            TaskState s = engine_task_at(i)->state;
            if (s == TASK_RUNNING || s == TASK_PROBING) {
                InvalidateRect(hwnd, NULL, FALSE);
                break;
            }
        }
        return 0;
    case WM_APP_WSJOB: {
        WsJob *job = (WsJob*)lp;
        if (job) {
            add_and_show(job->url, job->referer[0] ? job->referer : NULL,
                         job->cookies[0] ? job->cookies : NULL,
                         job->useragent[0] ? job->useragent : NULL,
                         job->filename);
            free(job);
        }
        return 0;
    }
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            tray_menu(hwnd);
        }
        return 0;
    case WM_CLIPBOARDUPDATE:
        clipboard_check(hwnd);
        return 0;
    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED && g_tray_ok) {   /* minimize to tray only if tray exists */
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        RECT cr; GetClientRect(hwnd, &cr);
        layout_toolbar(cr.right);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_ENTERSIZEMOVE: g_sizing = 1; return 0;
    case WM_EXITSIZEMOVE:  g_sizing = 0; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        g_scroll -= d / 120 * ROW_H * 3;
        int maxs = visible_count() * ROW_H;
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > maxs) g_scroll = maxs;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        RECT cr; GetClientRect(hwnd, &cr);
        int tb = toolbar_hit(pt);
        int sd = pt.y > TOOLBAR_H && pt.x < SIDEBAR_W ? sidebar_hit(pt) : -1;
        int rw = pt.y > TOOLBAR_H && pt.x >= SIDEBAR_W ? row_hit(pt, cr.bottom) : -1;
        if (tb != g_hover_tb || sd != g_hover_side || rw != g_hover_row) {
            g_hover_tb = tb; g_hover_side = sd; g_hover_row = rw;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover_tb = -1; g_hover_side = -1; g_hover_row = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        RECT cr; GetClientRect(hwnd, &cr);
        int tb = toolbar_hit(pt);
        if (tb >= 0) { do_toolbar(hwnd, tb); return 0; }
        if (pt.x < SIDEBAR_W && pt.y > TOOLBAR_H) {
            int i = sidebar_hit(pt);
            if (i >= 0) { g_category = SIDE_ITEMS[i]; g_selected = -1; g_scroll = 0; }
        } else {
            g_selected = row_hit(pt, cr.bottom);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        RECT cr; GetClientRect(hwnd, &cr);
        int row = row_hit(pt, cr.bottom);
        if (row >= 0) {
            DownloadTask *t = row_task(row, NULL);
            if (t) ui_progress_show(t);
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (pt.x == -1) { pt.x = 100; pt.y = 100; }
        POINT cl = pt; ScreenToClient(hwnd, &cl);
        RECT cr; GetClientRect(hwnd, &cr);
        int row = row_hit(cl, cr.bottom);
        if (row >= 0) {
            g_selected = row;
            InvalidateRect(hwnd, NULL, FALSE);
            show_row_menu(hwnd, pt, row);
        }
        return 0;
    }
    case WM_PAINT: on_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE:
        if (!g_really_quit && g_tray_ok) {   /* hide to tray only if tray exists */
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (!g_really_quit) {
            /* no tray: closing just minimizes to the taskbar instead */
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        ui_progress_close_all();
        RemoveClipboardFormatListener(hwnd);
        tray_remove();
        ws_stop();
        store_save_tasks();
        PostQuitMessage(0);
        return 0;
    default:
        if (g_taskbar_created && msg == g_taskbar_created) {
            tray_add(hwnd);   /* explorer restarted: re-add the icon */
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ui_notify_changed(void)
{
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* called from the WebSocket client thread: marshal the job to the UI thread */
void ui_submit_ws_job(WsJob *job)
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_WSJOB, 0, (LPARAM)job);
    else free(job);
}

/* add + start a download coming from WS / clipboard / cmdline, pop progress win */
static void add_and_show(const char *url, const char *referer, const char *cookies,
                         const char *ua, const char *filename)
{
    /* dedupe: same URL already in the list */
    for (int i = 0; i < engine_count(); i++)
        if (str_ieq(engine_task_at(i)->url, url)) return;
    DownloadTask *t = engine_add(url, g_cfg.download_dir,
                                 (filename && filename[0]) ? filename : NULL,
                                 g_cfg.max_connections);
    if (referer) strncpy(t->referer, referer, sizeof(t->referer)-1);
    if (cookies) strncpy(t->cookies, cookies, sizeof(t->cookies)-1);
    if (ua) strncpy(t->useragent, ua, sizeof(t->useragent)-1);
    engine_start(t);
    store_save_tasks();
    ui_progress_show(t);
    ui_notify_changed();
}

/* ---------- tray ---------- */

static void tray_add(HWND hwnd)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = ui_load_icon(101, GetSystemMetrics(SM_CXSMICON));
    wcscpy(nid.szTip, L"reneat");
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    DWORD gle = GetLastError();
    nlog("tray: NIM_ADD %s (icon=%p, win32=%lu, cbSize=%u)", ok ? "ok" : "FAILED", nid.hIcon, gle, (unsigned)nid.cbSize);
    g_tray_ok = ok ? 1 : 0;
    /* explorer restart: re-register */
    if (!g_taskbar_created)
        g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
}

static void tray_remove(void)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void tray_menu(HWND hwnd)
{
    /* 唯一的真退出入口是主界面工具栏的 Quit 按钮；托盘只提供恢复 */
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L(L_TRAY_SHOW));
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                             pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }
}

/* ---------- clipboard monitor ---------- */

static int looks_like_download(const char *s)
{
    if (_strnicmp(s, "http://", 7) != 0 && _strnicmp(s, "https://", 8) != 0) return 0;
    if (strlen(s) > 2000 || strchr(s, ' ') || strchr(s, '\n') || strchr(s, '\r')) return 0;
    /* file-ish: last path component contains a dot */
    const char *sl = strrchr(s, '/');
    if (!sl || !strchr(sl, '.')) return 0;
    return 1;
}

static void clipboard_check(HWND hwnd)
{
    if (!g_cfg.monitor_clipboard) return;
    if (!OpenClipboard(hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        wchar_t *w = GlobalLock(h);
        if (w) {
            char url[2048];
            WideCharToMultiByte(CP_UTF8, 0, w, -1, url, sizeof(url), NULL, NULL);
            GlobalUnlock(h);
            str_trim(url);
            if (looks_like_download(url) && !str_ieq(url, g_last_clip_url)) {
                strncpy(g_last_clip_url, url, sizeof(g_last_clip_url)-1);
                nlog("clipboard: download link detected: %s", url);
                add_and_show(url, NULL, NULL, NULL, NULL);
            }
        }
    }
    CloseClipboard();
}

int ui_run(HINSTANCE inst)
{
    g_inst = inst;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"reneat_main";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COL_BG);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"reneat 0.2 — NeatDM in C",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 934, 600,
        NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
