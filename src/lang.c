/* lang.c — string tables; index 0 = English, 1 = 中文 */
#include "lang.h"
#include "settings.h"

static const wchar_t *T[2][L_COUNT] = {
/* ---- English ---- */
{
    L"New URL", L"Resume", L"Stop", L"Delete", L"Settings", L"Browsers", L"About", L"Quit",
    L"All Downloads", L"Video", L"Audio", L"Compressed", L"Document",
    L"Application", L"Misc", L"Complete", L"Incomplete",
    L"File Name", L"Size", L"Status", L"Bandwidth", L"Remaining Time", L"Last Try",
    L"Connecting...", L"Queued", L"Downloading...", L"Paused", L"Complete",
    L"Error", L"Canceled",
    L"%d hr %d min", L"%d min %d sec", L"%d sec",
    L"Redownload", L"Show Window", L"Open Folder", L"Open", L"Properties",
    L"Delete this download?",
    L"Chrome Extension", L"Edge Extension", L"Firefox Extension", L"Opera Extension",
    L"Download", L"Options", L"Connections",
    L"File Size", L"Downloaded", L"Bandwidth", L"Remaining Time", L"Resumable",
    L"Yes", L"No", L"Pause", L"Cancel", L"Apply",
    L"0 or Blank = No Limit", L"Limit Bandwidth to", L"KB/sec",
    L"Remember on Resume", L"Show Completion Dialog",
    L"No", L"Receiving...", L"Completed", L"Waiting...", L"Segments : %d",
    L"New Download", L"Address:", L"Save to:", L"Connections:", L"Download",
    L"Please enter a valid URL (http:// or https://)",
    L"reneat Settings", L"General", L"Proxy/Socks", L"Passwords",
    L"Max Connections per Download", L"recommended", L"When first Connection Starts Downloading  , :",
    L"Create Additional Connections All at Once", L"Create Additional Connections One by One",
    L"Bandwidth Limit per Download (KB/s)", L"0 or blank for No limit", L"Default User-Agent",
    L"Use this UA for both Manual and Browser-Sent Downloads ( Be Careful )",
    L"Download Directory", L"Create Category Folders ( e.g.  Video, Document, ... )",
    L"Temporary Directory", L"Show Download Completion Dialog",
    L"Start Application on System Startup ( Recommended )", L"Language",
    L"No Proxy/Socks", L"HTTP Proxy", L"Socks Proxy", L"Socks V5", L"Socks V4",
    L"Address", L"Port", L"User", L"Password", L"https Protocol", L"ftp Protocol",
    L"Protocol", L"Target", L"(no saved passwords)", L"Remove",
    L"About reneat", L"( Basic Version - Free License )", L"Website : neatdownloadmanager.com",
    L"Check For Update",
    L"Download Properties", L"Saved To :", L"URL :", L"Added On :", L"OK",
    L"Download Complete", L"Close",
    L"Show reneat", L"Monitor clipboard for download links"
},
/* ---- 中文 ---- */
{
    L"新建下载", L"继续", L"停止", L"删除", L"设置", L"浏览器", L"关于", L"退出",
    L"全部下载", L"视频", L"音频", L"压缩包", L"文档",
    L"应用程序", L"其他", L"已完成", L"未完成",
    L"文件名", L"大小", L"状态", L"带宽", L"剩余时间", L"最后尝试",
    L"连接中...", L"排队中", L"下载中...", L"已暂停", L"已完成",
    L"错误", L"已取消",
    L"%d 小时 %d 分", L"%d 分 %d 秒", L"%d 秒",
    L"重新下载", L"显示窗口", L"打开文件夹", L"打开", L"属性",
    L"确定删除这个下载吗？",
    L"Chrome 扩展", L"Edge 扩展", L"Firefox 扩展", L"Opera 扩展",
    L"下载", L"选项", L"连接",
    L"文件大小", L"已下载", L"带宽", L"剩余时间", L"可续传",
    L"是", L"否", L"暂停", L"取消", L"应用",
    L"0 或留空 = 不限速", L"限速为", L"KB/秒",
    L"续传时记住", L"显示完成对话框",
    L"序号", L"接收中...", L"已完成", L"等待中...", L"分段数 : %d",
    L"新建下载", L"地址：", L"保存到：", L"连接数：", L"下载",
    L"请输入有效的 URL（http:// 或 https://）",
    L"reneat 设置", L"常规", L"代理/Socks", L"密码",
    L"每个下载的最大连接数", L"推荐", L"当第一个连接开始下载时 , :",
    L"立即创建全部连接", L"逐个创建连接",
    L"每个下载的带宽限制 (KB/s)", L"0 或留空 = 不限速", L"默认 User-Agent",
    L"手动和浏览器下载都使用此 UA（小心）",
    L"下载目录", L"创建分类文件夹（如 视频、文档…）",
    L"临时目录", L"显示下载完成对话框",
    L"开机自启动（推荐）", L"语言",
    L"不使用代理", L"HTTP 代理", L"Socks 代理", L"Socks V5", L"Socks V4",
    L"地址", L"端口", L"用户名", L"密码", L"https 协议", L"ftp 协议",
    L"协议", L"目标", L"（没有保存的密码）", L"移除",
    L"关于 reneat", L"（基础版 - 免费许可）", L"官网 : neatdownloadmanager.com",
    L"检查更新",
    L"下载属性", L"保存到 :", L"URL :", L"添加时间 :", L"确定",
    L"下载完成", L"关闭",
    L"显示 reneat", L"监视剪贴板里的下载链接"
}
};

void lang_init(void)
{
    /* default: follow the Windows UI language unless the user overrode it */
    if (g_cfg.language < 0) {
        LANGID id = GetUserDefaultUILanguage();
        g_cfg.language = (PRIMARYLANGID(id) == LANG_CHINESE) ? 1 : 0;
    }
}

const wchar_t *L(int id)
{
    if (id < 0 || id >= L_COUNT) return L"?";
    int lang = (g_cfg.language == 1) ? 1 : 0;
    return T[lang][id];
}
