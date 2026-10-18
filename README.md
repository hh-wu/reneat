# reneat — Neat Download Manager clone in native Win32 C

reneat 是一个用**纯 Win32 C (C11)** 写的下载管理器，复刻 Neat Download Manager 的
外观与行为：大图标工具栏 + 分类侧栏 + 报表列表主窗口、三页签进度窗口、
动态分段下载引擎、浏览器扩展接管下载、托盘、剪贴板监听、中英双语。

零第三方依赖——TLS 用系统 Schannel (SSPI)，UI 全部 owner-draw 的 Win32 控件，
单 exe 即可运行。

## 功能

- **动态分段下载引擎**：HTTP Range 多连接分段（默认 8，可配 1–32）；
  空闲连接会"窃取"剩余量最大的在跑段（一分为二接管）；失败段回滚重试（上限 8 次）
- **断点续传**：每 2 秒把段表写入 `<file>.segments.bin`，进程被杀也能续传；
  完成后自动删除存档
- **探测请求** `Range: bytes=0-0` 跟随重定向，从 `Content-Range` /
  `Content-Disposition`（含 `filename*=UTF-8''`）取大小、可续传性与文件名
- **主窗口**：New URL / Resume / Stop / Delete / Settings / Browsers / About / Quit
  工具栏；All Downloads 分类侧栏（Video/Audio/Compressed/Document/Application/Misc、
  Complete/Incomplete）；File Name/Size/Status/Bandwidth/Remaining Time/Last Try 列表
- **进度窗口**：Download / Options / Connections 三页签 + 大进度条 +
  `Segments : N` 分段框；Options 页可即时限速
- **Settings**：General（连接数/限速/UA/目录/完成提示/开机自启/语言/剪贴板监听）、
  Proxy/Socks（HTTP CONNECT、SOCKS5、Basic/user-pass 认证）、Passwords
- **浏览器扩展**：`extension/`（Manifest V3，Chrome/Edge/Firefox 均可加载），
  兼容官方 NeatDM 扩展协议：`ws://127.0.0.1:10007/download`、
  子协议 `neatextension.v1`，自动接管浏览器下载 + 右键菜单 + 状态弹窗
- **托盘**：最小化/关闭收进托盘，托盘菜单恢复主窗口
- **剪贴板监听**：复制 http(s) 文件链接自动添加下载（Settings 可开关）
- **中英双语**：默认跟随系统语言，Settings → Language 切换
- **CLI 模式**：`reneat.exe --cli <url> [outdir] [conn]`，进度打到终端
- **调试日志**：默认常开，写 exe 同目录 `reneat.log`（8MB 自动轮转，
  `RENEAT_LOG=<路径>` 可改），覆盖 DNS/connect/TLS 握手/HTTP 头/重定向/分段事件

## 构建

需要 MinGW-w64 GCC（项目用 winlibs）：

```powershell
# 把 mingw64\bin 加进 PATH 后：
mingw32-make            # 产出 reneat.exe
mingw32-make clean
```

## 使用

```
reneat.exe                                    # GUI
reneat.exe <url> [outdir] [connections]       # GUI + 立即开始下载并弹出进度窗口
reneat.exe --cli <url> [outdir] [connections] # 控制台模式
```

设置保存在注册表 `HKCU\Software\reneat`；任务列表保存在 exe 同目录
`tasks.dat`；单实例（互斥体 `reneat_singleton`）。

### 浏览器扩展安装

浏览器打开 `chrome://extensions` → 开发者模式 → 「加载已解压的扩展程序」
→ 选 `extension/` 目录。reneat 运行时扩展会自动接管下载；
reneat 不在线时自动回退浏览器自带下载，不丢任务。

测试客户端：`tests/wsclient_official.py`（模拟官方扩展握手 + 推送）。

## 架构

| 模块 | 文件 | 说明 |
|---|---|---|
| 裸 Winsock 封装 | `src/net.c` | 连接/收发/超时 |
| Schannel(SSPI) TLS 客户端 | `src/tls.c` | 握手/加解密/close_notify |
| HTTP/1.1 状态机 | `src/http.c` | 请求构造、头解析、重定向 |
| 动态分段下载引擎 | `src/engine.c` | SegmentManager + 窃取/回滚 |
| 段表/任务持久化 | `src/store.c` | segments.bin / tasks.dat |
| 官方扩展 WebSocket 服务 | `src/ws.c` | 10007/download + neatextension.v1 |
| 主窗口 | `src/ui.c` | 工具栏/侧栏/列表 + 托盘 + 剪贴板 |
| 进度窗口 | `src/prog.c` | 三页签 + 分段框 |
| 对话框 | `src/dlg.c` | Settings/About/Properties/New URL/完成提示 |
| 注册表设置 + 分类 | `src/settings.c` | HKCU\Software\reneat |
| 中英文字符串表 | `src/lang.c` | en/zh |
| 资源脚本 | `res/reneat.rc` | 图标 + manifest + VERSIONINFO |

## 实测

- 本地 HTTP Range 服务器：1MB / 5 段下载完成，SHA-256 一致 ✓
- HTTPS 8 连接分段下载 ✓ 字节级校验
- 29% 处 kill 进程 → 重启从 segments.bin 续传完成，字节一致 ✓
- HTTP 302 重定向跟随 ✓
- 官方扩展协议握手（101 + neatextension.v1 回显 + greeting + 推送）✓

## Roadmap

- [ ] HLS (m3u8) 下载与 TS 合并
- [ ] WebM 音视频交织合成 MKV
- [ ] SQLite 任务库（当前是二进制 tasks.dat）
- [ ] 定时队列、站点密码（Passwords 页目前是空表）

## 许可

学习用复刻项目。图标来自 NeatDM (c) Javad Motallebi。
