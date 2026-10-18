/* settings.h — registry-backed settings (HKCU\Software\reneat) + categories */
#ifndef RENEAT_SETTINGS_H
#define RENEAT_SETTINGS_H
#include "common.h"

typedef enum { CAT_ALL=-1, CAT_VIDEO=0, CAT_AUDIO, CAT_COMPRESSED, CAT_DOCUMENT,
               CAT_APPLICATION, CAT_MISC, CAT_COMPLETE, CAT_INCOMPLETE } Category;

typedef struct {
    int  max_connections;        /* default connections per download     */
    int  conn_one_by_one;        /* 0 = all at once, 1 = one by one      */
    int  bw_limit_kbs;           /* default per-download limit, 0 = none */
    char user_agent[256];
    int  ua_for_all;
    char download_dir[MAX_PATH];
    int  category_folders;
    char temp_dir[MAX_PATH];
    int  show_completion;
    int  start_on_startup;

    int  proxy_mode;             /* 0 none, 1 http, 2 socks              */
    int  socks_v5;
    char proxy_addr[256];
    int  proxy_port;
    char proxy_user[128];
    char proxy_pass[128];
    int  https_via_proxy;
    int  ftp_via_proxy;
    int  language;               /* -1 = auto (follow OS), 0 = en, 1 = zh */
    int  monitor_clipboard;      /* watch clipboard for download links   */
    int  ws_port;                /* WebSocket server port (0 = disabled) */
} Settings;

extern Settings g_cfg;

void settings_load(void);
void settings_save(void);
void settings_apply_startup(void);      /* write/remove Run key per start_on_startup */
int  category_of_file(const char *filename);  /* CAT_VIDEO..CAT_MISC */
const wchar_t *category_name(int cat);
const char *category_exts(int cat);

#endif
