/* reneat Download Manager — background service worker (MV3).
 *
 * 接管浏览器下载：onDeterminingFilename 拿到最终 URL 和文件名后，
 * 通过本机 WebSocket (127.0.0.1:20099) 推给 reneat，成功后取消浏览器下载。
 * reneat 没运行时不做任何事（浏览器照常下载）。
 */

const WS_URL = 'ws://127.0.0.1:10007/download';   /* reneat 与官方扩展同一端点 */
const WS_PROTOCOL = 'neatextension.v1';
const WS_TIMEOUT = 2500;

let gEnabled = true;
chrome.storage.local.get({ enabled: true }, v => { gEnabled = v.enabled; });
chrome.storage.onChanged.addListener((c, area) => {
  if (area === 'local' && 'enabled' in c) gEnabled = c.enabled.newValue;
});

/* 把 job 推给 reneat，返回 true=已接管 */
function pushToReneat(job) {
  return new Promise(resolve => {
    let ws;
    let done = false;
    const finish = ok => {
      if (done) return;
      done = true;
      try { ws && ws.close(); } catch (e) {}
      resolve(ok);
    };
    const timer = setTimeout(() => finish(false), WS_TIMEOUT);
    try {
      ws = new WebSocket(WS_URL, WS_PROTOCOL);
    } catch (e) {
      clearTimeout(timer);
      return resolve(false);
    }
    ws.onopen = () => ws.send(JSON.stringify(job));
    ws.onmessage = ev => {
      clearTimeout(timer);
      try {
        const r = JSON.parse(ev.data);
        finish(r && r.result === 'ok');
      } catch (e) { finish(false); }
    };
    ws.onerror = () => { clearTimeout(timer); finish(false); };
    ws.onclose = () => { clearTimeout(timer); finish(false); };
  });
}

function basename(p) {
  if (!p) return '';
  return p.split(/[\\/]/).pop();
}

async function cookiesFor(url) {
  try {
    const list = await chrome.cookies.getAll({ url });
    return list.map(c => `${c.name}=${c.value}`).join('; ');
  } catch (e) {
    return '';
  }
}

async function handoff(item) {
  const url = item.finalUrl || item.url;
  if (!/^https?:\/\//i.test(url)) return false;
  const job = {
    url,
    referer: item.referrer || '',
    cookies: await cookiesFor(url),
    useragent: navigator.userAgent,
    filename: basename(item.filename) || ''
  };
  const ok = await pushToReneat(job);
  if (ok && item.id !== undefined) {
    try {
      await chrome.downloads.cancel(item.id);
      await chrome.downloads.erase({ id: item.id });
    } catch (e) {}
  }
  return ok;
}

/* 浏览器开始下载时拦截 */
chrome.downloads.onDeterminingFilename.addListener((item, suggest) => {
  suggest(); /* 必须先调用，避免阻塞浏览器 */
  if (!gEnabled) return;
  handoff(item);
});

/* 右键菜单：用 reneat 下载链接 */
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: 'reneat-link',
    title: '用 reneat 下载 / Download with reneat',
    contexts: ['link']
  });
});

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if (info.menuItemId !== 'reneat-link' || !info.linkUrl) return;
  const job = {
    url: info.linkUrl,
    referer: info.pageUrl || (tab && tab.url) || '',
    cookies: await cookiesFor(info.linkUrl),
    useragent: navigator.userAgent,
    filename: ''
  };
  const ok = await pushToReneat(job);
  if (!ok && tab && tab.id !== undefined) {
    /* reneat 不在线就退回浏览器下载 */
    chrome.downloads.download({ url: info.linkUrl });
  }
});
