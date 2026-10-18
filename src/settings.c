/* settings.c */
#include "settings.h"
#include "util.h"
#include "lang.h"

Settings g_cfg;

#define REGKEY L"Software\\reneat"

static void rd_str(HKEY k, const wchar_t *name, char *out, int outlen, const char *def)
{
    wchar_t w[MAX_PATH * 2]; DWORD sz = sizeof(w), type = 0;
    if (RegGetValueW(k, NULL, name, RRF_RT_REG_SZ, &type, w, &sz) == ERROR_SUCCESS)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outlen, NULL, NULL);
    else
        strncpy(out, def, outlen - 1);
}

static int rd_int(HKEY k, const wchar_t *name, int def)
{
    DWORD v = (DWORD)def, sz = sizeof(v), type = 0;
    RegGetValueW(k, NULL, name, RRF_RT_REG_DWORD, &type, &v, &sz);
    return (int)v;
}

static void wr_str(HKEY k, const wchar_t *name, const char *v)
{
    wchar_t w[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, v, -1, w, MAX_PATH * 2);
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)w, (DWORD)(wcslen(w) + 1) * 2);
}

static void wr_int(HKEY k, const wchar_t *name, int v)
{
    DWORD d = (DWORD)v;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
}

void settings_load(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.max_connections = 8;
    g_cfg.conn_one_by_one = 1;
    g_cfg.socks_v5 = 1;
    strcpy(g_cfg.user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36");
    strcpy(g_cfg.download_dir, "C:\\Users\\user\\Downloads");
    strcpy(g_cfg.temp_dir, "C:\\Users\\user\\AppData\\Roaming\\reneat");
    g_cfg.show_completion = 1;
    g_cfg.language = -1;         /* auto */
    g_cfg.monitor_clipboard = 1;
    g_cfg.ws_port = 10007;     /* official NeatDM extension endpoint port */

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGKEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        g_cfg.max_connections = rd_int(k, L"MaxConnections", 8);
        g_cfg.conn_one_by_one = rd_int(k, L"ConnOneByOne", 1);
        g_cfg.bw_limit_kbs = rd_int(k, L"BandwidthLimitKBs", 0);
        rd_str(k, L"UserAgent", g_cfg.user_agent, sizeof(g_cfg.user_agent), g_cfg.user_agent);
        g_cfg.ua_for_all = rd_int(k, L"UAForAll", 0);
        rd_str(k, L"DownloadDir", g_cfg.download_dir, sizeof(g_cfg.download_dir), g_cfg.download_dir);
        g_cfg.category_folders = rd_int(k, L"CategoryFolders", 0);
        rd_str(k, L"TempDir", g_cfg.temp_dir, sizeof(g_cfg.temp_dir), g_cfg.temp_dir);
        g_cfg.show_completion = rd_int(k, L"ShowCompletion", 1);
        g_cfg.start_on_startup = rd_int(k, L"StartOnStartup", 0);
        g_cfg.proxy_mode = rd_int(k, L"ProxyMode", 0);
        g_cfg.socks_v5 = rd_int(k, L"SocksV5", 1);
        rd_str(k, L"ProxyAddr", g_cfg.proxy_addr, sizeof(g_cfg.proxy_addr), "");
        g_cfg.proxy_port = rd_int(k, L"ProxyPort", 0);
        rd_str(k, L"ProxyUser", g_cfg.proxy_user, sizeof(g_cfg.proxy_user), "");
        rd_str(k, L"ProxyPass", g_cfg.proxy_pass, sizeof(g_cfg.proxy_pass), "");
        g_cfg.https_via_proxy = rd_int(k, L"HttpsViaProxy", 0);
        g_cfg.ftp_via_proxy = rd_int(k, L"FtpViaProxy", 0);
        g_cfg.language = rd_int(k, L"Language", -1);
        g_cfg.monitor_clipboard = rd_int(k, L"MonitorClipboard", 1);
        g_cfg.ws_port = rd_int(k, L"WsPort", 10007);
        RegCloseKey(k);
    }
}

void settings_save(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGKEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    wr_int(k, L"MaxConnections", g_cfg.max_connections);
    wr_int(k, L"ConnOneByOne", g_cfg.conn_one_by_one);
    wr_int(k, L"BandwidthLimitKBs", g_cfg.bw_limit_kbs);
    wr_str(k, L"UserAgent", g_cfg.user_agent);
    wr_int(k, L"UAForAll", g_cfg.ua_for_all);
    wr_str(k, L"DownloadDir", g_cfg.download_dir);
    wr_int(k, L"CategoryFolders", g_cfg.category_folders);
    wr_str(k, L"TempDir", g_cfg.temp_dir);
    wr_int(k, L"ShowCompletion", g_cfg.show_completion);
    wr_int(k, L"StartOnStartup", g_cfg.start_on_startup);
    wr_int(k, L"ProxyMode", g_cfg.proxy_mode);
    wr_int(k, L"SocksV5", g_cfg.socks_v5);
    wr_str(k, L"ProxyAddr", g_cfg.proxy_addr);
    wr_int(k, L"ProxyPort", g_cfg.proxy_port);
    wr_str(k, L"ProxyUser", g_cfg.proxy_user);
    wr_str(k, L"ProxyPass", g_cfg.proxy_pass);
    wr_int(k, L"HttpsViaProxy", g_cfg.https_via_proxy);
    wr_int(k, L"FtpViaProxy", g_cfg.ftp_via_proxy);
    wr_int(k, L"Language", g_cfg.language);
    wr_int(k, L"MonitorClipboard", g_cfg.monitor_clipboard);
    wr_int(k, L"WsPort", g_cfg.ws_port);
    RegCloseKey(k);
}

void settings_apply_startup(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &k) != ERROR_SUCCESS)
        return;
    if (g_cfg.start_on_startup) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        RegSetValueExW(k, L"reneat", 0, REG_SZ, (const BYTE*)exe, (DWORD)(wcslen(exe) + 1) * 2);
    } else {
        RegDeleteValueW(k, L"reneat");
    }
    RegCloseKey(k);
}

/* ---- categories (NeatDM: Video/Audio/Compressed/Document/Application/Misc) ---- */

static const char *const EXTS[] = {
    /* CAT_VIDEO */      "mp4 mkv avi mov wmv flv webm mpg mpeg 3gp m4v ts m2ts vob rmvb",
    /* CAT_AUDIO */      "mp3 wav flac aac ogg wma m4a opus mid midi ape",
    /* CAT_COMPRESSED */ "zip rar 7z tar gz bz2 xz iso dmg cab lzma zst",
    /* CAT_DOCUMENT */   "pdf doc docx xls xlsx ppt pptx pps txt epub odt ods rtf csv xml tex md",
    /* CAT_APPLICATION */"exe msi apk deb rpm appimage bat com scr ps1",
    /* CAT_MISC */       ""
};

int category_of_file(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot || !dot[1]) return CAT_MISC;
    char ext[16]; int i;
    for (i = 0; dot[i+1] && i < 15; i++) ext[i] = (char)tolower((unsigned char)dot[i+1]);
    ext[i] = 0;
    for (int c = 0; c < CAT_MISC; c++) {
        char list[256]; strncpy(list, EXTS[c], sizeof(list)-1); list[sizeof(list)-1]=0;
        char *save = NULL;
        for (char *tok = strtok_s(list, " ", &save); tok; tok = strtok_s(NULL, " ", &save))
            if (str_ieq(tok, ext)) return c;
    }
    return CAT_MISC;
}

const wchar_t *category_name(int cat)
{
    switch (cat) {
        case CAT_ALL:         return L(L_CAT_ALL);
        case CAT_VIDEO:       return L(L_CAT_VIDEO);
        case CAT_AUDIO:       return L(L_CAT_AUDIO);
        case CAT_COMPRESSED:  return L(L_CAT_COMPRESSED);
        case CAT_DOCUMENT:    return L(L_CAT_DOCUMENT);
        case CAT_APPLICATION: return L(L_CAT_APPLICATION);
        case CAT_MISC:        return L(L_CAT_MISC);
        case CAT_COMPLETE:    return L(L_CAT_COMPLETE);
        case CAT_INCOMPLETE:  return L(L_CAT_INCOMPLETE);
    }
    return L"?";
}

const char *category_exts(int cat)
{
    return (cat >= 0 && cat <= CAT_MISC) ? EXTS[cat] : "";
}
