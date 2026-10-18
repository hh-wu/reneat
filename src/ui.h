/* ui.h — NeatDM-look UI: light theme, big toolbar, category sidebar,
 * report list, per-download progress windows, dialogs. */
#ifndef RENEAT_UI_H
#define RENEAT_UI_H
#include "common.h"
#include "engine.h"
#include "settings.h"

/* palette sampled from NeatDM screenshots (light theme) */
#define COL_BG        RGB(255, 255, 255)
#define COL_TOOLBAR   RGB(255, 255, 255)
#define COL_SIDEBAR   RGB(255, 255, 255)
#define COL_HEADER    RGB(250, 250, 250)
#define COL_TEXT      RGB(30, 30, 30)
#define COL_TEXT_DIM  RGB(110, 110, 110)
#define COL_TEXT_BLUE RGB(0, 102, 204)
#define COL_SEL       RGB(204, 232, 255)
#define COL_SEL_INACT RGB(229, 241, 251)
#define COL_HOVER     RGB(229, 241, 251)
#define COL_BORDER    RGB(218, 220, 224)
#define COL_GREEN     RGB(0, 176, 60)
#define COL_SEGBLUE   RGB(0, 60, 140)
#define COL_ERR       RGB(220, 60, 50)
#define COL_WARN      RGB(230, 126, 34)

#define TOOLBAR_H   66
#define SIDEBAR_W   176
#define ROW_H       26
#define HEADER_H    26

int  ui_run(HINSTANCE inst);
void ui_notify_changed(void);

/* shared drawing helpers (ui.c) */
extern HFONT  g_font, g_font_bold, g_font_small, g_font_title;
extern HINSTANCE g_inst;
void ui_fill_rect(HDC dc, const RECT *r, COLORREF c);
void ui_draw_text(HDC dc, const wchar_t *s, RECT *r, HFONT f, COLORREF c, UINT fmt);
HICON ui_load_icon(int id, int size);
void ui_open_folder(DownloadTask *t);
void ui_show_properties(DownloadTask *t);
void ui_show_settings(HWND parent);
void ui_show_settings_proxy(HWND parent);
void ui_show_settings_passwords(HWND parent);
void ui_show_about(HWND parent);
void ui_show_completion(DownloadTask *t);   /* download-finished dialog */
void ui_progress_show(DownloadTask *t);     /* per-download window (prog.c) */
void ui_progress_show_tab(DownloadTask *t, int tab);
void ui_progress_close_all(void);
int  ui_add_download_dialog(HWND parent);   /* New URL dialog (dlg.c) */

#endif
