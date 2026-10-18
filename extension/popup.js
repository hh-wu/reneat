/* popup.js — show reneat bridge status + interception toggle */
const WS_URL = 'ws://127.0.0.1:10007/download';
const WS_PROTOCOL = 'neatextension.v1';

const statusEl = document.getElementById('status');
const enabledEl = document.getElementById('enabled');

chrome.storage.local.get({ enabled: true }, v => { enabledEl.checked = v.enabled; });
enabledEl.addEventListener('change', () => {
  chrome.storage.local.set({ enabled: enabledEl.checked });
});

/* probe the bridge: reneat accepts a WS upgrade and answers text frames */
function probe() {
  let ws;
  try { ws = new WebSocket(WS_URL, WS_PROTOCOL); } catch (e) { show(false); return; }
  const timer = setTimeout(() => { try { ws.close(); } catch (e) {} show(false); }, 1500);
  ws.onopen = () => {
    clearTimeout(timer);
    ws.close();
    show(true);
  };
  ws.onerror = () => { clearTimeout(timer); show(false); };
}

function show(ok) {
  statusEl.textContent = ok ? '● reneat 已连接 / connected' : '● reneat 未运行 / not running';
  statusEl.className = 'status ' + (ok ? 'ok' : 'bad');
}

probe();
