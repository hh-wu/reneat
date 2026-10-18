/* prog.c — per-download progress window, NeatDM style:
 * tabs (Download / Options / Connections), big green progress bar,
 * "Segments : N" + per-segment boxes bar, Pause / Cancel buttons. */
#include "ui.h"
#include "lang.h"
#include "util.h"
#include <shellapi.h>

#define PROG_W 573
#define PROG_H 400
#define TAB_H  24
#define BTN_ID_PAUSE  3001
#define BTN_ID_CANCEL 3002
#define BTN_ID_APPLY_BW 3003
#define BTN_ID_BW_EDIT  3004

typedef struct ProgWin {
    DownloadTask *t;
    HWND hwnd;
    int tab;                 /* 0 Download, 1 Options, 2 Connections */
    HWND bw_edit, apply_btn, pause_btn, cancel_btn, remember_chk, completion_chk, conn_drop, conn_apply;
    struct ProgWin *next;
} ProgWin;

static ProgWin *g_wins = NULL;
static int g_class_registered = 0;

/* ---------- segmented boxes bar (NeatDM signature) ---------- */

static void draw_segments_boxes(HDC dc, DownloadTask *t, RECT *r)
{
    if (t->nseg <= 0) return;
    int W = r->right - r->left;
    EnterCriticalSection(&t->lock);
    int n = t->nseg;
    for (int i = 0; i < n; i++) {
        int x0 = r->left + (int)((int64_t)i * W / n);
        int x1 = r->left + (int)((int64_t)(i+1) * W / n) - (i < n-1 ? 1 : 0);
        Segment *s = &t->segs[i];
        double frac = (s->end > s->start) ? (double)(s->pos - s->start) / (s->end - s->start) : 0;
        RECT box = { x0, r->top, x1, r->bottom };
        ui_fill_rect(dc, &box, RGB(255,255,255));
        if (frac > 0) {
            RECT f = { x0, r->top, x0 + (int)((x1-x0) * frac), r->bottom };
            ui_fill_rect(dc, &f, s->state == SEG_DONE ? COL_SEGBLUE : RGB(70, 130, 200));
        }
        /* border */
        HPEN p = CreatePen(PS_SOLID, 1, COL_SEGBLUE);
        HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, box.left, box.top, box.right, box.bottom);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(p);
    }
    LeaveCriticalSection(&t->lock);
}

static void draw_big_progress(HDC dc, DownloadTask *t, RECT *r)
{
    /* track */
    ui_fill_rect(dc, r, RGB(238, 238, 238));
    HPEN p = CreatePen(PS_SOLID, 1, COL_BORDER);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r->left, r->top, r->right, r->bottom);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(p);
    if (t->size > 0 && t->downloaded > 0) {
        int fw = (int)((double)t->downloaded / t->size * (r->right - r->left - 2));
        if (fw > 0) {
            RECT f = { r->left + 1, r->top + 1, r->left + 1 + fw, r->bottom - 1 };
            ui_fill_rect(dc, &f, COL_GREEN);
        }
    }
}

/* ---------- per-tab content ---------- */

static void label_value(HDC dc, const wchar_t *label, const wchar_t *value,
                        int x, int y, int vw, COLORREF vc)
{
    RECT lr = { x, y, x + 120, y + 20 };
    ui_draw_text(dc, label, &lr, g_font, COL_TEXT, DT_LEFT | DT_SINGLELINE);
    RECT vr = { x + 125, y, x + 125 + vw, y + 20 };
    ui_draw_text(dc, value, &vr, g_font, vc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void draw_tab_download(HDC dc, ProgWin *pw, RECT *area)
{
    DownloadTask *t = pw->t;
    int x = area->left + 14, y = area->top + 10;

    /* URL in blue */
    wchar_t wurl[2048];
    MultiByteToWideChar(CP_UTF8, 0, t->url, -1, wurl, 2048);
    RECT ur = { x, y, area->right - 80, y + 22 };
    ui_draw_text(dc, wurl, &ur, g_font_small, COL_TEXT_BLUE, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_PATH_ELLIPSIS);
    y += 30;

    wchar_t w[512];
    const wchar_t *st;
    switch (t->state) {
        case TASK_RUNNING: st = L(L_ST_DOWNLOADING); break;
        case TASK_PAUSED: st = L(L_ST_PAUSED); break;
        case TASK_DONE: st = L(L_ST_COMPLETE); break;
        case TASK_ERROR: st = L(L_ST_ERROR); break;
        case TASK_PROBING: st = L(L_ST_CONNECTING); break;
        default: st = L(L_ST_QUEUED); break;
    }
    label_value(dc, L(L_COL_STATUS), st, x, y, 320, t->state==TASK_ERROR?COL_ERR:COL_TEXT_BLUE); y += 24;
    if (t->size > 0) format_size(t->size, w, 512); else wcscpy(w, L"?");
    label_value(dc, L(L_PW_FILESIZE), w, x, y, 320, COL_TEXT); y += 24;
    wchar_t dl[128]; format_size(t->downloaded, dl, 128);
    if (t->size > 0) _snwprintf(w, 512, L"%s ( %.2f %% )", dl, t->downloaded*100.0/t->size);
    else wcscpy(w, dl);
    label_value(dc, L(L_PW_DOWNLOADED), w, x, y, 320, COL_TEXT); y += 24;
    format_speed(t->state == TASK_RUNNING ? t->speed : 0, w, 512);
    label_value(dc, L(L_PW_BANDWIDTH), w, x, y, 320, COL_TEXT); y += 24;
    if (t->state == TASK_RUNNING && t->speed > 1 && t->size > 0) {
        int s = (int)((t->size - t->downloaded) / t->speed);
        _snwprintf(w, 512, L(L_TIME_MIN_SEC), s/60, s%60);
    } else wcscpy(w, L"--");
    label_value(dc, L(L_PW_REMAINING), w, x, y, 320, COL_TEXT); y += 24;
    label_value(dc, L(L_PW_RESUMABLE), t->resumable ? L(L_YES) : L(L_NO), x, y, 320, COL_GREEN);

    /* folder icon on the right */
    HICON f = ui_load_icon(104, 48);
    if (f) { DrawIconEx(dc, area->right - 72, area->top + 40, f, 48, 48, 0, NULL, DI_NORMAL); DestroyIcon(f); }
}

static void draw_tab_options(HDC dc, ProgWin *pw, RECT *area)
{
    DownloadTask *t = pw->t;
    int x = area->left + 14, y = area->top + 14;
    RECT lr = { x, y, x + 200, y + 22 };
    ui_draw_text(dc, L(L_PW_BANDWIDTH), &lr, g_font_bold, COL_TEXT, DT_LEFT);
    y += 28;
    wchar_t w[128]; format_speed(t->state == TASK_RUNNING ? t->speed : 0, w, 128);
    RECT vr = { x, y, x + 170, y + 30 };
    ui_draw_text(dc, w, &vr, g_font_title, COL_TEXT, DT_LEFT);
    RECT nr = { x + 200, y + 8, area->right - 20, y + 30 };
    ui_draw_text(dc, L(L_OPT_NOLIMIT), &nr, g_font_small, COL_TEXT_BLUE, DT_LEFT);
    /* "Limit Bandwidth to [edit] KB/sec [Apply]" row (controls at y=112) */
    RECT lb = { x, 116, x + 130, 136 };
    ui_draw_text(dc, L(L_OPT_LIMIT_TO), &lb, g_font, COL_TEXT, DT_LEFT);
    RECT kb = { x + 262, 116, x + 320, 136 };
    ui_draw_text(dc, L(L_OPT_KBSEC), &kb, g_font, COL_TEXT, DT_LEFT);
}

static void draw_tab_connections(HDC dc, ProgWin *pw, RECT *area)
{
    DownloadTask *t = pw->t;
    int x = area->left + 14, y = area->top + 10;
    /* mini list: No | Status */
    RECT listr = { x, y, x + 300, y + 150 };
    HPEN p = CreatePen(PS_SOLID, 1, COL_BORDER);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, listr.left, listr.top, listr.right, listr.bottom);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
    RECT hr = { x + 4, y + 2, x + 40, y + 20 };
    ui_draw_text(dc, L(L_CONN_NO), &hr, g_font_small, COL_TEXT, DT_LEFT);
    hr.left = x + 60; hr.right = x + 200;
    ui_draw_text(dc, L(L_COL_STATUS), &hr, g_font_small, COL_TEXT, DT_LEFT);
    EnterCriticalSection(&t->lock);
    int rows = t->nseg < 32 ? t->nseg : 32;
    for (int i = 0; i < rows; i++) {
        Segment *s = &t->segs[i];
        wchar_t w[64];
        _snwprintf(w, 64, L"%d", i + 1);
        RECT nr = { x + 6, y + 22 + i*16, x + 40, y + 38 + i*16 };
        ui_draw_text(dc, w, &nr, g_font_small, COL_TEXT, DT_LEFT);
        const wchar_t *st =
            s->state == SEG_RUNNING ? L(L_CONN_RECEIVING) :
            s->state == SEG_DONE ? L(L_CONN_COMPLETED) :
            s->state == SEG_ERROR ? L(L_ST_ERROR) : L(L_CONN_WAITING);
        nr.left = x + 60; nr.right = x + 296;
        ui_draw_text(dc, st, &nr, g_font_small,
                     s->state == SEG_RUNNING ? COL_TEXT_BLUE : COL_TEXT_DIM, DT_LEFT);
        if (22 + (i+1)*16 > 148) break;
    }
    LeaveCriticalSection(&t->lock);
}

/* ---------- window proc ---------- */

static void prog_update_title(ProgWin *pw)
{
    DownloadTask *t = pw->t;
    wchar_t wname[512], title[600];
    MultiByteToWideChar(CP_UTF8, 0, t->filename[0] ? t->filename : t->url, -1, wname, 512);
    if (t->size > 0)
        _snwprintf(title, 600, L"%d%%  %s", (int)(t->downloaded*100/t->size), wname);
    else
        _snwprintf(title, 600, L"%s", wname);
    SetWindowTextW(pw->hwnd, title);
}

static void prog_layout_controls(ProgWin *pw)
{
    RECT cr; GetClientRect(pw->hwnd, &cr);
    int w = cr.right;
    int ybtns = cr.bottom - 40;
    if (pw->tab == 1) { /* Options */
        SetWindowPos(pw->bw_edit, NULL, 150, 112, 120, 24, SWP_SHOWWINDOW);
        SetWindowPos(pw->apply_btn, NULL, 336, 111, 80, 26, SWP_SHOWWINDOW);
        SetWindowPos(pw->remember_chk, NULL, 20, 152, 200, 22, SWP_SHOWWINDOW);
        SetWindowPos(pw->completion_chk, NULL, 20, 196, 250, 22, SWP_SHOWWINDOW);
    } else {
        ShowWindow(pw->bw_edit, SW_HIDE); ShowWindow(pw->apply_btn, SW_HIDE);
        ShowWindow(pw->remember_chk, SW_HIDE); ShowWindow(pw->completion_chk, SW_HIDE);
    }
    if (pw->tab == 2) { /* Connections */
        SetWindowPos(pw->conn_drop, NULL, 430, 40, 70, 24, SWP_SHOWWINDOW);
        SetWindowPos(pw->conn_apply, NULL, 430, 100, 80, 28, SWP_SHOWWINDOW);
    } else {
        ShowWindow(pw->conn_drop, SW_HIDE); ShowWindow(pw->conn_apply, SW_HIDE);
    }
    SetWindowPos(pw->pause_btn, NULL, w/2 - 165, ybtns, 150, 32, SWP_SHOWWINDOW);
    SetWindowPos(pw->cancel_btn, NULL, w/2 + 15, ybtns, 150, 32, SWP_SHOWWINDOW);
}

static LRESULT CALLBACK prog_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ProgWin *pw = (ProgWin*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_TIMER: {
        if (!pw) break;
        prog_update_title(pw);
        InvalidateRect(hwnd, NULL, FALSE);
        /* auto-close shortly after completion? NeatDM keeps it; keep it. */
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!pw) break;
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (pt.y >= 4 && pt.y <= 4 + TAB_H) {
            static const int names[3] = { L_TAB_DOWNLOAD, L_TAB_OPTIONS, L_TAB_CONNECTIONS };
            int x = 8;
            HDC dc = GetDC(hwnd);
            for (int i = 0; i < 3; i++) {
                SIZE sz; HFONT of = SelectObject(dc, g_font);
                GetTextExtentPoint32W(dc, L(names[i]), (int)wcslen(L(names[i])), &sz);
                SelectObject(dc, of);
                int tw = sz.cx + 22;
                if (pt.x >= x && pt.x < x + tw) { pw->tab = i; prog_layout_controls(pw); InvalidateRect(hwnd, NULL, TRUE); break; }
                x += tw;
            }
            ReleaseDC(hwnd, dc);
        }
        return 0;
    }
    case WM_COMMAND:
        if (!pw) break;
        switch (LOWORD(wp)) {
        case BTN_ID_PAUSE:
            if (pw->t->state == TASK_RUNNING) engine_pause(pw->t);
            else engine_resume(pw->t);
            break;
        case BTN_ID_CANCEL:
            engine_cancel(pw->t);
            DestroyWindow(hwnd);
            return 0;
        case BTN_ID_APPLY_BW: {
            wchar_t w[64]; GetWindowTextW(pw->bw_edit, w, 64);
            int kb = _wtoi(w);
            pw->t->bw_limit = (int64_t)(kb < 0 ? 0 : kb) * 1024;
            break;
        }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        static HBRUSH br = NULL;
        if (!br) br = CreateSolidBrush(RGB(255,255,255));
        HDC dc = (HDC)wp;
        SetTextColor(dc, COL_TEXT);
        SetBkColor(dc, RGB(255,255,255));
        return (LRESULT)br;
    }
    case WM_PAINT: {
        if (!pw) break;
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        ui_fill_rect(mem, &cr, COL_BG);
        DownloadTask *t = pw->t;

        /* tab strip */
        static const int names[3] = { L_TAB_DOWNLOAD, L_TAB_OPTIONS, L_TAB_CONNECTIONS };
        int x = 8;
        for (int i = 0; i < 3; i++) {
            SIZE sz;
            HFONT of = SelectObject(mem, g_font);
            GetTextExtentPoint32W(mem, L(names[i]), (int)wcslen(L(names[i])), &sz);
            SelectObject(mem, of);
            int tw = sz.cx + 22;
            RECT tr = { x, 4, x + tw, 4 + TAB_H };
            if (pw->tab == i) {
                ui_fill_rect(mem, &tr, COL_BG);
                HPEN p = CreatePen(PS_SOLID, 1, COL_BORDER);
                HGDIOBJ op2 = SelectObject(mem, p), ob2 = SelectObject(mem, GetStockObject(NULL_BRUSH));
                MoveToEx(mem, tr.left, tr.bottom, NULL);
                LineTo(mem, tr.left, tr.top); LineTo(mem, tr.right, tr.top); LineTo(mem, tr.right, tr.bottom);
                SelectObject(mem, op2); SelectObject(mem, ob2); DeleteObject(p);
            }
            RECT tx = { x, 4, x + tw, 4 + TAB_H };
            ui_draw_text(mem, L(names[i]), &tx, g_font, COL_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x += tw;
        }
        RECT tabline = { 0, 4 + TAB_H, cr.right, 5 + TAB_H };
        ui_fill_rect(mem, &tabline, COL_BORDER);

        /* content area */
        RECT area = { 0, 6 + TAB_H, cr.right, cr.bottom - 118 };
        if (pw->tab == 0) draw_tab_download(mem, pw, &area);
        else if (pw->tab == 1) draw_tab_options(mem, pw, &area);
        else draw_tab_connections(mem, pw, &area);

        /* big progress bar */
        RECT pb = { 20, cr.bottom - 110, cr.right - 20, cr.bottom - 86 };
        draw_big_progress(mem, t, &pb);

        /* "Segments : N" */
        wchar_t sg[64]; _snwprintf(sg, 64, L(L_SEGMENTS), t->nseg);
        RECT sr = { 0, cr.bottom - 80, cr.right, cr.bottom - 62 };
        ui_draw_text(mem, sg, &sr, g_font_small, COL_TEXT_BLUE, DT_CENTER | DT_SINGLELINE);

        /* segment boxes */
        RECT bb = { 20, cr.bottom - 60, cr.right - 20, cr.bottom - 42 };
        draw_segments_boxes(mem, t, &bb);

        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: {
        if (pw) {
            ProgWin **pp = &g_wins;
            while (*pp && *pp != pw) pp = &(*pp)->next;
            if (*pp) *pp = pw->next;
            free(pw);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ui_progress_show(DownloadTask *t)
{
    ui_progress_show_tab(t, 0);
}

void ui_progress_show_tab(DownloadTask *t, int tab)
{
    for (ProgWin *p = g_wins; p; p = p->next) {
        if (p->t == t) {
            SetForegroundWindow(p->hwnd);
            return;
        }
    }
    if (!g_class_registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = prog_proc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"reneat_prog";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(COL_BG);
        wc.hIcon = ui_load_icon(101, 32);
        RegisterClassW(&wc);
        g_class_registered = 1;
    }
    ProgWin *pw = calloc(1, sizeof(*pw));
    pw->t = t;
    pw->tab = tab;
    /* AdjustWindowRect so the CLIENT area is exactly PROG_W x PROG_H */
    RECT wr = { 0, 0, PROG_W, PROG_H };
    AdjustWindowRect(&wr, WS_CAPTION | WS_SYSMENU, FALSE);
    pw->hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"reneat_prog", L"download",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, g_inst, NULL);
    SetWindowLongPtrW(pw->hwnd, GWLP_USERDATA, (LONG_PTR)pw);

    /* controls (positioned in prog_layout_controls) */
    pw->bw_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_NUMBER,
        0,0,0,0, pw->hwnd, (HMENU)(INT_PTR)3004, g_inst, NULL);
    pw->apply_btn = CreateWindowExW(0, L"BUTTON", L(L_APPLY), WS_CHILD,
        0,0,0,0, pw->hwnd, (HMENU)(INT_PTR)BTN_ID_APPLY_BW, g_inst, NULL);
    pw->remember_chk = CreateWindowExW(0, L"BUTTON", L(L_OPT_REMEMBER), WS_CHILD | BS_AUTOCHECKBOX,
        0,0,0,0, pw->hwnd, (HMENU)(INT_PTR)3010, g_inst, NULL);
    pw->completion_chk = CreateWindowExW(0, L"BUTTON", L(L_OPT_SHOWCOMP), WS_CHILD | BS_AUTOCHECKBOX,
        0,0,0,0, pw->hwnd, (HMENU)(INT_PTR)3011, g_inst, NULL);
    pw->conn_drop = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
        0,0,0,0, pw->hwnd, (HMENU)(INT_PTR)3012, g_inst, NULL);
    pw->conn_apply = CreateWindowExW(0, L"BUTTON", L(L_APPLY), WS_CHILD,
        0,0,0,0, pw->hwnd, (HMENU)(INT_PTR)3013, g_inst, NULL);
    pw->pause_btn = CreateWindowExW(0, L"BUTTON", L(L_PAUSE), WS_CHILD,
        0,0,0,0, pw->hwnd, (HMENU)(INT_PTR)BTN_ID_PAUSE, g_inst, NULL);
    pw->cancel_btn = CreateWindowExW(0, L"BUTTON", L(L_CANCEL), WS_CHILD,
        0,0,0,0, pw->hwnd, (HMENU)(INT_PTR)BTN_ID_CANCEL, g_inst, NULL);
    for (int i = 1; i <= 32; i++) {
        wchar_t w[8]; _snwprintf(w, 8, L"%d", i);
        SendMessageW(pw->conn_drop, CB_ADDSTRING, 0, (LPARAM)w);
    }
    SendMessageW(pw->conn_drop, CB_SETCURSEL, t->connections - 1, 0);
    HFONT fl = g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND ctrls[] = { pw->bw_edit, pw->apply_btn, pw->remember_chk, pw->completion_chk,
                     pw->conn_drop, pw->conn_apply, pw->pause_btn, pw->cancel_btn };
    for (int i = 0; i < 8; i++) SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)fl, TRUE);

    if (g_cfg.show_completion) SendMessageW(pw->completion_chk, BM_SETCHECK, BST_CHECKED, 0);
    wchar_t bw[16]; _snwprintf(bw, 16, L"%d", (int)(t->bw_limit / 1024));
    SetWindowTextW(pw->bw_edit, bw);

    pw->next = g_wins; g_wins = pw;
    prog_layout_controls(pw);
    prog_update_title(pw);
    SetTimer(pw->hwnd, 1, 500, NULL);
}

void ui_progress_close_all(void)
{
    while (g_wins) {
        HWND h = g_wins->hwnd;
        DestroyWindow(h);   /* WM_DESTROY frees the node */
        if (g_wins && g_wins->hwnd == h) break; /* safety */
    }
}
