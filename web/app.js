'use strict';

const $ = (id) => document.getElementById(id);
const SCREEN_W = 1280;
const SCREEN_H = 720;
let draft = null;
let published = null;
let lastValidated = null;
let conflictRevision = null;

const api = async (path, options = {}) => {
  const response = await fetch(path, {
    headers: { 'Content-Type': 'application/json', ...(options.headers || {}) },
    ...options,
  });
  const text = await response.text();
  const data = text ? JSON.parse(text) : {};
  if (!response.ok) throw Object.assign(new Error(data.error?.message || response.statusText), { data, status: response.status });
  return data;
};

function show(message, kind = '') {
  const el = $('message');
  el.textContent = message;
  el.className = `message ${kind}`;
}

function buttonFields(action) {
  const fs = document.querySelector(`fieldset[data-action="${action}"]`);
  return Object.fromEntries([...fs.querySelectorAll('input')].map((input) => [input.dataset.field, Number(input.value)]));
}

function buildLayout() {
  const exit = buttonFields('exit');
  return {
    title: $('title').value,
    title_font: { family: $('fontFamily').value, size: Number($('fontSize').value) },
    background: { mode: $('bgMode').value, darken: Number($('darken').value) },
    safe_area: { x: 16, y: 16, w: 1248, h: 688 },
    title_rect: { x: 24, y: 20, w: Math.max(320, exit.x - 40), h: Number($('titleH').value) },
    buttons: {
      start: { ...buttonFields('start'), label: '开始' },
      settings: { ...buttonFields('settings'), label: '设置' },
      exit: { ...exit, label: '退出' },
    },
    focus_order: ['start', 'settings', 'exit'],
  };
}

function fillForm(layout) {
  $('title').value = layout.title;
  $('fontSize').value = layout.title_font.size;
  $('fontFamily').value = layout.title_font.family;
  $('bgMode').value = layout.background.mode;
  $('darken').value = layout.background.darken;
  $('titleH').value = layout.title_rect.h;
  $('baseRevision').value = layout.revision ?? '';
  for (const action of ['start', 'settings', 'exit']) {
    const fs = document.querySelector(`fieldset[data-action="${action}"]`);
    for (const key of ['x', 'y', 'w', 'h']) {
      fs.querySelector(`input[data-field="${key}"]`).value = layout.buttons[action][key];
    }
  }
}

function touchHit(rect) {
  const w = Math.max(rect.w, 48);
  const h = Math.max(rect.h, 48);
  return { x: rect.x + Math.floor(rect.w / 2) - Math.floor(w / 2), y: rect.y + Math.floor(rect.h / 2) - Math.floor(h / 2), w, h };
}

function applyPreview(source) {
  const layout = source.layout || source;
  lastValidated = layout.touch_hits ? layout : { ...layout, touch_hits: Object.fromEntries(Object.entries(layout.buttons).map(([k, v]) => [k, touchHit(v)])) };
  const scale = $('preview').clientWidth / SCREEN_W;
  const place = (el, r) => {
    el.style.left = `${(r.x / SCREEN_W) * 100}%`;
    el.style.top = `${(r.y / SCREEN_H) * 100}%`;
    el.style.width = `${(r.w / SCREEN_W) * 100}%`;
    el.style.height = `${(r.h / SCREEN_H) * 100}%`;
  };
  $('previewTitle').textContent = layout.title;
  place($('previewTitle'), layout.title_rect);
  $('previewTitle').style.fontSize = `${layout.title_font.size * scale}px`;
  $('scrim').style.background = layout.background.mode === 'light'
    ? 'rgba(255,255,255,.08)'
    : layout.background.mode === 'dark'
      ? `rgba(0,0,0,${Number(layout.background.darken || 0.65)})`
      : `rgba(0,0,0,${Number(layout.background.darken)})`;
  place($('safe-box'), layout.safe_area);
  for (const action of ['start', 'settings', 'exit']) {
    const visual = layout.buttons[action];
    place($(`preview-${action}`), visual);
    place($(`hit-${action}`), lastValidated.touch_hits[action]);
    $(`preview-${action}`).style.fontSize = `${Math.max(11, 16 * scale)}px`;
  }
  $('previewTitle').style.maxWidth = `${(layout.title_rect.w / SCREEN_W) * 100}%`;
}

async function refresh() {
  try {
    const [d, p] = await Promise.all([api('/api/layout/draft'), api('/api/layout/published')]);
    draft = d;
    published = p;
    $('health').textContent = '服务器在线';
    $('revision').textContent = `草稿 r${d.revision}`;
    $('published').textContent = `发布 v${p.version}`;
    conflictRevision = null;
    $('forceBtn').classList.add('hidden');
    if (!document.activeElement || document.activeElement.tagName !== 'INPUT') fillForm(d);
    applyPreview(p);
    show('已从数据库载入草稿和已发布布局。', 'ok');
  } catch (err) {
    $('health').textContent = '服务器不可达';
    show(err.message, 'error');
  }
}

async function validateOnly() {
  try {
    const result = await api('/api/layout/validate', { method: 'POST', body: JSON.stringify(buildLayout()) });
    applyPreview(result.layout);
    show(`校验通过：触摸命中区域、可用区域、焦点顺序、字体回退与退出入口均已纳入同一布局。\n回退链：${result.layout.title_font.fallback_chain.join(' → ')}`, 'ok');
    return result.layout;
  } catch (err) {
    show(`${err.data?.error?.code || '校验失败'}：${err.message}\n${JSON.stringify(err.data?.error?.details || {}, null, 2)}`, 'error');
  }
}

async function saveDraft(publish = false) {
  const body = buildLayout();
  try {
    let saved;
    if (publish) {
      saved = await api('/api/layout/publish', { method: 'POST', body: JSON.stringify({ revision: Number($('baseRevision').value) }) });
    } else {
      saved = await api('/api/layout/draft', { method: 'PUT', body: JSON.stringify({ base_revision: Number($('baseRevision').value), layout: body }) });
    }
    await refresh();
    show(publish ? `已发布布局 v${saved.version}，终端命令必须携带该版本。` : '草稿已保存并写入数据库。', 'ok');
  } catch (err) {
    if (err.status === 409 && err.data?.error?.code === 'draft_revision_conflict') {
      const details = err.data.error.details || {};
      conflictRevision = details.server_revision;
      $('forceBtn').classList.remove('hidden');
      draft = details.server_draft || draft;
      show(`草稿冲突：另一个控制台已经保存到 r${conflictRevision}。\n请比较当前预览/服务器草稿；保留本地输入时点击“按最新修订重提”，放弃改动请刷新。\n${err.message}`, 'warn');
    } else if (err.status === 400) {
      show(`接口校验失败：${err.message}\n${JSON.stringify(err.data?.error?.details || {}, null, 2)}`, 'error');
    } else {
      show(err.message, 'error');
    }
  }
}


async function refreshPolicy() {
  try {
    const data = await api('/api/policy?device_id=terminal-01');
    $('policy-start').checked = !!data.server.start;
    $('policy-settings').checked = !!data.server.settings;
    $('policy-exit').checked = true;
  } catch (_) {}
}
async function savePolicy() {
  const actions = {
    start: $('policy-start').checked,
    settings: $('policy-settings').checked,
    exit: true,
  };
  try {
    await api('/api/policy', { method: 'PUT', body: JSON.stringify({ device_id: 'terminal-01', actions }) });
    show('服务器策略已更新；终端下次轮询生效。本地策略在断网时只开放设置查看与受控退出。', 'ok');
    refreshRecovery();
  } catch (err) { show(err.message, 'error'); }
}

async function refreshActual() {
  try {
    const data = await api('/api/devices/actual?device_id=terminal-01');
    $('actualJson').textContent = data.actual ? JSON.stringify(data.actual, null, 2) : '尚未收到终端上报。';
  } catch (err) { $('actualJson').textContent = err.message; }
}

async function refreshRecovery() {
  try {
    const data = await api('/api/recovery?device_id=terminal-01');
    $('recoveryList').innerHTML = data.events.length ? '' : '尚无记录。';
    for (const e of data.events) {
      const div = document.createElement('div');
      div.className = `event ${e.kind}`;
      div.innerHTML = `<strong>${e.kind}</strong>${e.action ? ` · ${e.action}` : ''}<br>${e.message}<br><span class="time">${new Date(e.created_at).toLocaleString()}</span>`;
      $('recoveryList').appendChild(div);
    }
  } catch (err) { $('recoveryList').textContent = err.message; }
}

$('validateBtn').addEventListener('click', validateOnly);
$('saveBtn').addEventListener('click', () => saveDraft(false));
$('publishBtn').addEventListener('click', async () => {
  const valid = await validateOnly();
  if (valid) await saveDraft(true);
});
$('reloadBtn').addEventListener('click', refresh);
$('refreshActual').addEventListener('click', refreshActual);
$('refreshRecovery').addEventListener('click', refreshRecovery);
$('savePolicy').addEventListener('click', savePolicy);
refreshPolicy();
$('forceBtn').addEventListener('click', () => {
  if (!conflictRevision) return;
  $('baseRevision').value = conflictRevision;
  conflictRevision = null;
  $('forceBtn').classList.add('hidden');
  saveDraft(false);
});
$('stateSelect').addEventListener('change', (e) => {
  const btn = $('preview-start');
  btn.className = `preview-button state-${e.target.value}`;
  btn.disabled = e.target.value === 'busy';
  btn.textContent = e.target.value === 'busy' ? '执行中…' : e.target.value === 'success' ? '成功' : e.target.value === 'failure' ? '失败' : '开始';
});
document.querySelector('#layout-form').addEventListener('input', () => {
  try { applyPreview(buildLayout()); } catch (_) {}
});
window.addEventListener('resize', () => { if (lastValidated) applyPreview(lastValidated); });

refresh();
refreshActual();
refreshRecovery();
setInterval(refreshActual, 4000);
setInterval(refreshRecovery, 8000);
