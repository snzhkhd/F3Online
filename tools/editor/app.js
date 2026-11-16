'use strict';
/* F3 Content Editor: front end of f3_server --editor (docs/editor.md). Plain JS, no build step, no network dependencies.
 * app.js     shell, state, undo / redo, list, schema driven form, base diff, JSON tab, save / validate / reload
 * dialog.js  dialog graph editor, condition and effect builders
 * locale.js  locale tab (en / ru side by side)
 */
(function () {
const Ed = window.Ed = { ext: {} };
const S = Ed.S = {
  token: '', schema: null, info: null, refs: null,
  kind: 'items', tab: 'form', sel: null,           // sel: {key, idx} (a record of a loaded file) | {baseId}
  files: {},                                       // "kind/file" -> {kind,file,doc,saved,etag,generated,generator,hasComments,isNew,parsed,rawText}
  undo: [], redo: [], lastCo: null, lastCoT: 0,
  problems: { errors: [], warnings: [], own: '', key: '' }, saveLog: [], reloadReport: '',
  filters: { q: '', origin: '', cls: '', file: '', base: false },
  protoCls: {}, baseRows: {}, loaded: {}, rawText: {}, baseRec: null, bottomTab: 'problems', showAll: false,
};

// ------------------------------------------------------------------------------------------------------------------ small helpers
function h(tag, props, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k === 'style' && typeof v === 'object') Object.assign(e.style, v);
    else if (k.startsWith('on') && typeof v === 'function') e.addEventListener(k.slice(2), v);
    else if (k === 'value' || k === 'checked' || k === 'disabled' || k === 'selected' || k === 'indeterminate') e[k] = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat(Infinity)) { if (c === null || c === undefined || c === false) continue; e.append(c.nodeType ? c : document.createTextNode(String(c))); }
  return e;
}
const clone = (v) => v === undefined ? undefined : JSON.parse(JSON.stringify(v));
const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const lower = (s) => String(s == null ? '' : s).toLowerCase();
function getIn(o, path) { let c = o; for (const k of path) { if (c === null || c === undefined) return undefined; c = c[k]; } return c; }
function setIn(o, path, v) { let c = o; for (let i = 0; i < path.length - 1; i++) { const k = path[i]; if (c[k] === undefined || c[k] === null) c[k] = typeof path[i + 1] === 'number' ? [] : {}; c = c[k]; } c[path[path.length - 1]] = v; }
function delIn(o, path) {
  const par = getIn(o, path.slice(0, -1));
  if (par === undefined || par === null) return;
  if (Array.isArray(par)) par.splice(path[path.length - 1], 1); else delete par[path[path.length - 1]];
  // an object that became empty because its last field was removed disappears with it (explicit "{}" is created by the Add buttons only)
  if (path.length > 1 && !Array.isArray(par) && Object.keys(par).length === 0 && typeof path[path.length - 2] === 'string') delIn(o, path.slice(0, -1));
}
const pstr = (path) => path.map((k, i) => typeof k === 'number' ? '[' + k + ']' : (i ? '.' : '') + k).join('');
Object.assign(Ed, { h, clone, eq, lower, getIn, setIn, delIn, pstr });
const $ = (id) => document.getElementById(id);
const fkey = (kind, file) => kind + '/' + file;

function toast(msg, type) {
  const t = $('toast');
  t.textContent = msg; t.className = type || ''; t.style.display = 'block';
  clearTimeout(toast.t); toast.t = setTimeout(() => { t.style.display = 'none'; }, type === 'err' ? 7000 : 3500);
}
function modal(title, bodyEls, buttons) {   // buttons: [{label, primary, danger, onclick -> true keeps the modal open}]
  const root = $('modalRoot');
  const close = () => { root.textContent = ''; };
  const acts = h('div', { class: 'actions' }, (buttons || [{ label: 'Close' }]).map((b) => h('button', {
    class: (b.primary ? 'primary ' : '') + (b.danger ? 'danger' : ''),
    onclick: async () => { const keep = b.onclick ? await b.onclick() : false; if (!keep) close(); },
  }, b.label)));
  root.textContent = '';
  const back = h('div', { class: 'back', onmousedown: (e) => { if (e.target === back) close(); } }, h('div', { class: 'modal' }, h('h3', {}, title), bodyEls, acts));
  root.append(back);
  const first = back.querySelector('input,select,textarea'); if (first) first.focus();
  return close;
}
Object.assign(Ed, { toast, modal, fkey });

async function api(method, path, body, query) {
  const url = '/api/' + path + (query ? '?' + new URLSearchParams(query) : '');
  const opt = { method, headers: { 'X-F3-Token': S.token } };
  if (method !== 'GET') { opt.headers['Content-Type'] = 'application/json'; opt.body = JSON.stringify(body === undefined ? {} : body); }
  let r, j = null;
  try { r = await fetch(url, opt); } catch (e) { return { status: 0, ok: false, j: { ok: false, error: 'cannot reach the server: ' + e.message } }; }
  try { j = await r.json(); } catch (e) { j = { ok: false, error: 'unreadable response (' + r.status + ')' }; }
  return { status: r.status, ok: r.ok && j.ok !== false, j };
}
Ed.api = api;

// ------------------------------------------------------------------------------------------------------------------ files, undo / redo, dirty state
const isDirty = (f) => !!f && f.doc != null && JSON.stringify(f.doc) !== f.saved;
const dirtyFiles = () => Object.values(S.files).filter(isDirty);
Ed.isDirty = isDirty;

function persistDraft(f) {
  try {
    const k = 'f3ed.draft.' + location.port + '.' + fkey(f.kind, f.file);
    if (isDirty(f) && !f.isNew) localStorage.setItem(k, JSON.stringify({ etag: f.etag, doc: f.doc })); else localStorage.removeItem(k);
  } catch (e) { /* storage may be blocked: the draft then only lives in memory */ }
}

async function loadFile(kind, file, force) {
  const key = fkey(kind, file);
  if (S.files[key] && !force) return S.files[key];
  const r = await api('GET', 'get', undefined, { kind, file });
  if (!r.ok) { toast('Cannot load ' + key + ': ' + (r.j.error || r.status), 'err'); return null; }
  const j = r.j;
  const f = { kind, file, doc: j.parsed ? j.doc : null, saved: j.parsed ? JSON.stringify(j.doc) : '', etag: j.etag, generated: !!j.generated, generator: j.generator, hasComments: !!j.hasComments, parsed: j.parsed, rawText: j.text, error: j.error };
  try {   // a draft that survived a page reload (unsaved edits)
    const d = JSON.parse(localStorage.getItem('f3ed.draft.' + location.port + '.' + key) || 'null');
    if (d && d.etag === f.etag && f.doc && JSON.stringify(d.doc) !== f.saved) { f.doc = d.doc; f.restored = true; toast('Restored unsaved edits of ' + key, 'ok'); }
  } catch (e) { /* ignore */ }
  S.files[key] = f;
  return f;
}

function mutate(key, fn, opt) {
  opt = opt || {};
  const f = S.files[key];
  const before = JSON.stringify(f.doc);
  const now = Date.now();
  const last = S.undo[S.undo.length - 1];
  if (!(opt.co && S.lastCo === opt.co && now - S.lastCoT < 1500 && last && last.key === key)) {
    S.undo.push({ key, before });
    if (S.undo.length > 300) S.undo.shift();
  }
  S.lastCo = opt.co || null; S.lastCoT = now; S.redo.length = 0;
  fn(f.doc);
  afterChange(key, !!opt.full);
}
function afterChange(key, full) {
  const f = S.files[key];
  persistDraft(f);
  renderDirty(); renderList(); renderNav();
  scheduleValidate();
  if (full) renderMain(); else refreshFieldStates();
}
function undo() { stepHistory(S.undo, S.redo); }
function redo() { stepHistory(S.redo, S.undo); }
function stepHistory(from, to) {
  const e = from.pop(); if (!e) return;
  const f = S.files[e.key]; if (!f) return;
  to.push({ key: e.key, before: JSON.stringify(f.doc) });
  f.doc = JSON.parse(e.before); S.lastCo = null;
  if (S.sel && S.sel.key === e.key && S.sel.idx !== undefined && !(f.doc.records || [])[S.sel.idx]) S.sel = null;
  afterChange(e.key, true);
}
Ed.mutate = mutate;

function renderDirty() {
  const n = dirtyFiles().length;
  $('dirtyInd').textContent = n ? '● ' + n + ' file' + (n > 1 ? 's' : '') + ' with unsaved changes' : '';
  $('btnUndo').disabled = !S.undo.length; $('btnRedo').disabled = !S.redo.length;
  $('btnSaveAll').disabled = !n;
  const f = curFile(); $('btnSave').disabled = !(f && isDirty(f));
  document.title = (n ? '● ' : '') + (Ed.T ? Ed.T('F3 Content Editor') : 'F3 Content Editor');
}
window.addEventListener('beforeunload', (e) => { if (dirtyFiles().length) { e.preventDefault(); e.returnValue = ''; } });

// ------------------------------------------------------------------------------------------------------------------ records of the current kind
const curFile = () => S.sel && S.sel.key ? S.files[S.sel.key] : (S.kind !== 'locale' ? null : null);
const curRec = () => { const f = curFile(); return f && f.doc && f.doc.records ? f.doc.records[S.sel.idx] : null; };
const kindSchema = (k) => S.schema && S.schema.kinds[k || S.kind];
Object.assign(Ed, { curFile, curRec, kindSchema });

function filesOfKind(kind) { return Object.values(S.files).filter((f) => f.kind === kind && f.file[0] !== '_' && f.doc && Array.isArray(f.doc.records)); }

function clsOf(rec) {
  if (rec.weapon) return 'weapon';
  if (rec.armor || rec.head_armor !== undefined) return 'armor';
  if (rec.ammo) return 'ammo';
  const b = S.protoCls[lower(rec.id)] || S.protoCls[lower(rec.base)];
  return b || 'other';
}
function nameOf(kind, rec) {
  if (kind === 'quests') return rec.title || '';
  if (kind === 'dialogs') return rec.speaker || '';
  if (kind === 'skills') return (rec.desc || '').slice(0, 50);
  if (rec.name) return rec.name;
  const b = S.protoName && (S.protoName[lower(rec.id)] || S.protoName[lower(rec.base)]);
  return b || '';
}
function recordRows(kind) {
  const rows = [];
  for (const f of filesOfKind(kind)) {
    const savedDoc = f.saved ? JSON.parse(f.saved) : null;
    f.doc.records.forEach((r, i) => {
      if (!r || typeof r !== 'object') return;
      const sr = savedDoc && savedDoc.records ? savedDoc.records.find((x) => x && x.id === r.id) : null;
      rows.push({ key: fkey(kind, f.file), idx: i, id: String(r.id == null ? '' : r.id), name: nameOf(kind, r), file: f.file,
        origin: f.generated ? 'generated' : 'hand', cls: kind === 'items' ? clsOf(r) : kind === 'npcs' ? 'npc' : '',
        override: !!S.protoCls[lower(r.id)], modified: !sr || !eq(sr, r) || f.isNew });
    });
  }
  return rows;
}

// ------------------------------------------------------------------------------------------------------------------ navigation
const NAV = [['Content', ['items', 'npcs', 'perks', 'traits', 'skills', 'quests', 'dialogs']], ['Text', ['locale']], ['Planned', ['objects']]];
function renderNav() {
  const nav = $('nav'); nav.textContent = '';
  for (const [grp, ids] of NAV) {
    nav.append(h('div', { class: 'nav-group' }, grp));
    for (const id of ids) {
      let label, count = '', dirty = false;
      if (id === 'locale') { label = 'Locale (en / ru)'; dirty = Object.values(S.files).some((f) => f.kind.startsWith('locale/') && isDirty(f)); }
      else if (id === 'objects') label = 'Object scripts';
      else { const ki = S.info && S.info.kinds.find((k) => k.id === id); label = ki ? ki.label : id; count = ki ? String(S.loaded[id] ? filesOfKind(id).reduce((a, f) => a + f.doc.records.length, 0) : ki.records) : ''; dirty = Object.values(S.files).some((f) => f.kind === id && isDirty(f)); }
      nav.append(h('div', { class: 'nav-item' + (S.kind === id ? ' active' : '') + (id === 'objects' ? ' disabled' : ''), onclick: () => openKind(id) },
        h('span', {}, label, dirty ? h('span', { class: 'dot' }) : null), h('span', { class: 'count' }, id === 'objects' ? 'planned' : count)));
    }
  }
}

async function openKind(kind) {
  S.kind = kind; S.sel = null; S.tab = kind === 'dialogs' ? 'graph' : 'form'; S.baseRec = null;
  location.hash = kind;
  renderNav();
  if (kind === 'locale' || kind === 'objects') { renderList(); renderMain(); return; }
  $('listPane').textContent = ''; $('listPane').append(h('div', { class: 'empty' }, 'Loading ' + kind + '...'));
  const r = await api('GET', 'list', undefined, { kind });
  if (!r.ok) { $('listPane').textContent = r.j.error || 'cannot list'; return; }
  S.baseRows[kind] = r.j.rows.filter((x) => x.origin === 'base');
  S.protoName = S.protoName || {};
  for (const row of r.j.rows) if (row.cls) { S.protoCls[lower(row.id)] = row.cls; if (row.name) S.protoName[lower(row.id)] = row.name; }
  for (const f of r.j.files) await loadFile(kind, f.file);
  for (const fe of r.j.fileErrors || []) toast(kind + '/' + fe.file + ': ' + fe.error, 'err');
  S.loaded[kind] = true;
  renderNav(); renderList(); renderMain(); renderDirty(); validateNow();
}

function renderList() {
  const pane = $('listPane');
  if (S.kind === 'locale') { pane.textContent = ''; if (Ed.renderLocaleList) Ed.renderLocaleList(pane); return; }
  if (S.kind === 'objects' || !S.loaded[S.kind]) return;
  const kind = S.kind, F = S.filters, ks = kindSchema(kind);
  const own = recordRows(kind);
  const ids = new Set(own.map((r) => lower(r.id)));
  let rows = own;
  const q = lower(F.q);
  const baseWanted = (kind === 'items' || kind === 'npcs') && (F.base || q.length >= 2);
  if (baseWanted) rows = rows.concat((S.baseRows[kind] || []).filter((b) => !ids.has(lower(b.id))).map((b) => ({ baseId: b.id, id: b.id, name: b.name, origin: 'base', cls: b.cls, file: '' })));
  rows = rows.filter((r) => (!q || lower(r.id).includes(q) || lower(r.name).includes(q)) && (!F.origin || r.origin === F.origin) && (!F.cls || r.cls === F.cls) && (!F.file || r.file === F.file));
  const total = rows.length, cap = S.showAll ? 5000 : 300;
  const files = filesOfKind(kind);
  const head = h('div', { class: 'lp-head' },
    h('div', { class: 'row' }, h('input', { type: 'search', placeholder: 'Search id or name', value: F.q, id: 'q', oninput: (e) => { F.q = e.target.value; S.showAll = false; renderList(); const q2 = $('q'); q2.focus(); q2.setSelectionRange(q2.value.length, q2.value.length); } }),
      h('button', { onclick: () => newRecordDialog() }, '+ New')),
    h('div', { class: 'row' },
      h('select', { onchange: (e) => { F.origin = e.target.value; renderList(); } }, [['', 'any origin'], ['hand', 'hand-authored'], ['generated', 'generated'], ['base', 'original base']].map(([v, l]) => h('option', { value: v, selected: F.origin === v }, l))),
      h('select', { onchange: (e) => { F.file = e.target.value; renderList(); } }, [h('option', { value: '' }, 'any file'), files.map((f) => h('option', { value: f.file, selected: F.file === f.file }, f.file))])),
    kind === 'items' ? h('div', { class: 'chips' }, [['', 'all'], ['weapon', 'weapons'], ['armor', 'armor'], ['ammo', 'ammo'], ['kit', 'kits'], ['other', 'other']].map(([v, l]) => h('span', { class: 'chip' + (F.cls === v ? ' on' : ''), onclick: () => { F.cls = v; renderList(); } }, l))) : null,
    (kind === 'items' || kind === 'npcs') ? h('label', {}, h('input', { type: 'checkbox', checked: F.base, onchange: (e) => { F.base = e.target.checked; renderList(); } }), ' include original prototypes not in data/ (',
      (S.baseRows[kind] || []).length, ')') : null);
  const list = h('div', { class: 'rows' });
  rows.slice(0, cap).forEach((r) => {
    const sel = S.sel && ((r.baseId && S.sel.baseId === r.baseId) || (!r.baseId && S.sel.key === r.key && S.sel.idx === r.idx));
    list.append(h('div', { class: 'rrow' + (sel ? ' sel' : ''), onclick: () => selectRow(r) },
      h('span', { class: 'id' }, r.id || '(no id)'), h('span', { class: 'nm' }, r.name),
      r.cls && kind === 'items' ? h('span', { class: 'count' }, r.cls) : null,
      h('span', { class: 'badge ' + r.origin }, r.origin === 'base' ? 'base' : r.origin === 'generated' ? 'gen' : 'hand'),
      r.modified ? h('span', { class: 'dot', title: 'unsaved changes' }) : null));
  });
  if (!rows.length) list.append(h('div', { class: 'empty' }, kind === 'items' || kind === 'npcs' ? 'No records. Enable "include original prototypes" to see the base layer.' : 'No records yet.'));
  if (total > cap) list.append(h('div', { class: 'empty' }, 'Showing ' + cap + ' of ' + total + '. ', h('button', { class: 'linkish', onclick: () => { S.showAll = true; renderList(); } }, 'show all')));
  const foot = h('div', { class: 'lp-foot' }, h('span', { class: 'count' }, total + ' record' + (total === 1 ? '' : 's')),
    ks && ks.help ? h('span', { class: 'count', title: ks.help }, '(?)') : null, h('span', { class: 'grow' }),
    h('button', { class: 'small', title: 'Files of this kind', onclick: () => filesDialog() }, 'Files...'));
  const keepScroll = pane.querySelector('.rows') ? pane.querySelector('.rows').scrollTop : 0;
  const focused = document.activeElement && document.activeElement.id === 'q';
  const val = focused ? document.activeElement.value : null;
  pane.textContent = ''; pane.append(head, list, foot);
  list.scrollTop = keepScroll;
  if (focused) { const q2 = $('q'); q2.focus(); q2.setSelectionRange(val.length, val.length); }
}

async function selectRow(r) {
  if (r.baseId) { S.sel = { baseId: r.baseId }; S.tab = 'json'; await loadBase(r.baseId); renderList(); renderMain(); return; }
  S.sel = { key: r.key, idx: r.idx }; S.baseRec = null;
  if (S.kind === 'dialogs') S.tab = 'graph'; else if (S.tab === 'graph') S.tab = 'form';
  if (S.kind === 'items' || S.kind === 'npcs') await loadBase(r.id, curRec() && curRec().base);
  renderList(); renderMain(); applyFieldErrors();
}
async function loadBase(id, baseField) {
  S.baseRec = null;
  let r = await api('GET', 'get', undefined, { kind: S.kind, id });
  if (r.ok && r.j.base) { S.baseRec = r.j.base; return; }
  if (baseField) { r = await api('GET', 'get', undefined, { kind: S.kind, id: baseField }); if (r.ok && r.j.base) { S.baseRec = Object.assign({}, r.j.base, { mode: 'clone' }); } }
}
function selectById(kind, key, idx) {   // used by problem list / dialog editor
  return (async () => { if (S.kind !== kind) await openKind(kind); const f = S.files[key]; if (!f) return; S.sel = { key, idx }; S.tab = kind === 'dialogs' ? 'graph' : 'form'; if (kind === 'items' || kind === 'npcs') await loadBase(f.doc.records[idx].id, f.doc.records[idx].base); renderList(); renderMain(); applyFieldErrors(); })();
}

// ------------------------------------------------------------------------------------------------------------------ main pane
function renderMain() {
  const main = $('main');
  const scroll = main.scrollTop;
  main.textContent = '';
  if (S.kind === 'locale') { if (Ed.renderLocaleMain) Ed.renderLocaleMain(main); return; }
  if (S.kind === 'objects') { main.append(objectsPlaceholder()); return; }
  if (!S.sel) { main.append(h('div', { class: 'empty' }, 'Select a record on the left, or create one with + New.')); renderDirty(); return; }
  if (S.sel.baseId) { main.append(baseOnlyView()); renderDirty(); return; }
  const f = curFile(), rec = curRec();
  if (!f || !rec) { main.append(h('div', { class: 'empty' }, 'The record is gone.')); return; }
  if (!f.parsed && f.doc === null) { main.append(unparsedView(f)); return; }
  const ks = kindSchema();
  const hasBase = !!(ks && ks.hasBase);
  const head = h('div', { class: 'rec-head' }, h('h2', {}, rec.id || '(no id)'), h('span', { class: 'badge ' + (f.generated ? 'generated' : 'hand') }, f.generated ? 'generated' : 'hand-authored'),
    hasBase && S.baseRec ? h('span', { class: 'badge base' }, S.baseRec.mode === 'clone' ? 'clone of ' + S.baseRec.key : 'overrides base') : (hasBase ? h('span', { class: 'badge warn' }, 'new (no base)') : null),
    h('span', { class: 'count' }, f.kind + '/' + f.file + ' #' + S.sel.idx), h('span', { class: 'grow' }),
    h('button', { onclick: duplicateRecord }, 'Duplicate'), h('button', { class: 'danger', onclick: deleteRecord }, 'Delete'));
  main.append(head);
  if (f.generated) main.append(h('div', { class: 'banner' }, h('span', {}, 'This file is generated (' + (f.generator || 'f3_server --export-data') + '). The next export overwrites edits. Saving asks for confirmation; take it over to make it hand-authored.'),
    h('button', { onclick: () => takeOver(f) }, 'Take over (remove "generator")')));
  if (f.hasComments) main.append(h('div', { class: 'banner' }, 'This file contains // or /* */ comments. They are not kept when the editor saves it (the formatted output is identical to the exporter\'s).'));
  if (f.restored) main.append(h('div', { class: 'banner info' }, 'Unsaved edits from an earlier session were restored.'));
  const tabs = h('div', { class: 'tabs' });
  const tabList = [['form', 'Form']].concat(ks && ks.editor === 'dialog' ? [['graph', 'Dialog graph']] : [], hasBase ? [['diff', 'Diff vs base']] : [], [['json', 'JSON']]);
  if (ks && ks.editor === 'dialog') tabList.unshift(tabList.splice(1, 1)[0]);
  for (const [id, label] of tabList) tabs.append(h('div', { class: 'tab' + (S.tab === id ? ' on' : ''), onclick: () => { S.tab = id; renderMain(); applyFieldErrors(); } }, label));
  main.append(tabs);
  const body = h('div', { id: 'tabBody' });
  main.append(body);
  if (S.tab === 'form') body.append(renderRecordForm());
  else if (S.tab === 'graph' && Ed.renderDialogEditor) body.append(Ed.renderDialogEditor());
  else if (S.tab === 'diff') body.append(renderDiff());
  else body.append(renderJsonTab());
  renderDirty();
  main.scrollTop = scroll;
}
function unparsedView(f) {
  const ta = h('textarea', { class: 'json', value: f.rawText || '' });
  return h('div', {}, h('div', { class: 'banner err' }, 'This file is not valid JSON: ' + f.error), ta, h('div', { class: 'actions' }, h('button', { class: 'primary', onclick: () => {
    try { f.doc = JSON.parse(ta.value.replace(/^﻿/, '')); f.parsed = true; toast('Parsed. Review and save.', 'ok'); renderMain(); renderList(); } catch (e) { toast('Still invalid: ' + e.message, 'err'); }
  } }, 'Parse')));
}
function baseOnlyView() {
  const b = S.baseRec;
  return h('div', {}, h('div', { class: 'rec-head' }, h('h2', {}, S.sel.baseId), h('span', { class: 'badge base' }, 'original base'), h('span', { class: 'grow' }),
    h('button', { class: 'primary', onclick: () => overrideBase(S.sel.baseId) }, 'Override in data/...')),
  h('div', { class: 'banner info' }, 'This is an original prototype (read-only). Overriding writes a small record into a data file that changes only the fields you edit.'),
  b ? h('pre', { class: 'json mono' }, JSON.stringify(b.record, null, 2)) : h('div', { class: 'empty' }, 'Base record unavailable.'));
}
function objectsPlaceholder() {
  const ep = S.schema && S.schema.extensionPoints && S.schema.extensionPoints.objectScripts;
  return h('div', {}, h('h2', {}, 'Object scripts'),
    h('div', { class: 'ext-note' }, h('p', {}, h('b', {}, 'EXTENSION POINT, not implemented. '), 'Object scripts will let a designer attach behaviour to a placed map object: a computer terminal that runs a dialog, a switch that opens a door, an object that needs a skill check. The editor will get a new kind next to the others; nothing here writes files today.'),
      h('p', {}, 'Intended schema idea (served by /api/schema under extensionPoints.objectScripts, described in docs/editor.md):'),
      h('pre', { class: 'json mono' }, JSON.stringify(ep ? ep.idea : {}, null, 2)),
      h('p', {}, 'To add it: a new kind in EditorSchema.cpp and Content.cpp (same recipe as docs/editor.md, "Adding a new kind"), then register a renderer with Ed.ext.objects in tools/editor/.')));
}
Ed.ext.objects = null;   // EXTENSION POINT (object scripts): a future module sets Ed.ext.objects = (container) => {...}

// ------------------------------------------------------------------------------------------------------------------ record actions
function nextId(kind, base) {
  const used = new Set(recordRows(kind).map((r) => lower(r.id)).concat((S.baseRows[kind] || []).map((r) => lower(r.id))));
  let id = base, n = 2;
  while (used.has(lower(id))) id = base + '_' + (n++);
  return id;
}
function nextTraitIndex() { const u = new Set(); filesOfKind('traits').forEach((f) => f.doc.records.forEach((r) => u.add(r.index))); for (let i = 0; i < 16; i++) if (!u.has(i)) return i; return 15; }
function template(kind, id, base) {
  if (kind === 'items' || kind === 'npcs') return base ? { id, base } : { id };
  if (kind === 'perks') return { id, name: id };
  if (kind === 'traits') return { id, name: id, index: nextTraitIndex() };
  if (kind === 'quests') return { id, title: id };
  if (kind === 'skills') return { id };
  if (kind === 'dialogs') return { id, nodes: [{ id: 'start', text: 'Hello.', answers: [{ id: 'bye', text: 'Goodbye.' }] }] };
  return { id };
}
async function ensureNewFile(kind, name) {
  const key = fkey(kind, name);
  if (S.files[key]) return S.files[key];
  const doc = { schema: S.schema.contentSchema, kind, records: [] };
  S.files[key] = { kind, file: name, doc, saved: '', etag: null, generated: false, hasComments: false, isNew: true, parsed: true };
  return S.files[key];
}
function addRecord(kind, fileName, rec) {
  const key = fkey(kind, fileName);
  let idx = 0;
  mutate(key, (d) => { d.records.push(rec); idx = d.records.length - 1; }, { full: false });
  S.sel = { key, idx };
}
async function newRecordDialog(presetBase) {
  const kind = S.kind, ks = kindSchema();
  if (!ks) return;
  const hand = filesOfKind(kind).filter((f) => !f.generated);
  const defaultFile = hand.length ? hand[0].file : kind + '.json';
  const idIn = h('input', { type: 'text', placeholder: kind === 'skills' ? '' : 'new_id' });
  const skillSel = h('select', {}, (S.schema.enums.skills).map((s) => h('option', { value: s }, s)));
  const baseIn = (kind === 'items' || kind === 'npcs') ? h('input', { type: 'text', list: 'dl_' + (kind === 'npcs' ? 'npc' : 'item'), placeholder: 'clone this original (optional)', value: presetBase || '' }) : null;
  if (baseIn) ensureDatalist(kind === 'npcs' ? 'npc' : 'item');
  const fileSel = h('select', {}, hand.map((f) => h('option', { value: f.file }, f.file)).concat([h('option', { value: '__new' }, '(new file...)')]));
  if (!hand.length) fileSel.value = '__new';
  const newName = h('input', { type: 'text', value: defaultFile, style: { display: hand.length ? 'none' : '' } });
  fileSel.onchange = () => { newName.style.display = fileSel.value === '__new' ? '' : 'none'; };
  const body = h('div', { class: 'form' }, kind === 'skills' ? row('Skill', skillSel) : row('Id', idIn), baseIn ? row('Base', baseIn) : null, row('Save into', h('div', { class: 'line' }, fileSel, newName)));
  modal('New ' + (ks.label || kind).replace(/s$/, ''), body, [{ label: 'Cancel' }, { label: 'Create', primary: true, onclick: async () => {
    const id = (kind === 'skills' ? skillSel.value : idIn.value).trim();
    if (!id) { toast('An id is required', 'err'); return true; }
    if (recordRows(kind).some((r) => lower(r.id) === lower(id))) { toast('Id already exists', 'err'); return true; }
    let file = fileSel.value;
    if (file === '__new') { file = newName.value.trim(); if (!/^[A-Za-z0-9_][A-Za-z0-9_.-]*\.json$/.test(file)) { toast('File name: letters, digits, _ . - and .json', 'err'); return true; } await ensureNewFile(kind, file); }
    addRecord(kind, file, template(kind, id, baseIn ? baseIn.value.trim() : ''));
    S.tab = kind === 'dialogs' ? 'graph' : 'form';
    if (kind === 'items' || kind === 'npcs') await loadBase(id, baseIn && baseIn.value.trim());
    renderList(); renderMain(); validateNow();
    return false;
  } }]);
}
function row(label, ctl) { return h('div', { class: 'frow' }, h('label', {}, label), h('div', { class: 'ctl' }, ctl)); }
async function overrideBase(id) {
  const hand = filesOfKind(S.kind).filter((f) => !f.generated);
  const fileSel = h('select', {}, hand.map((f) => h('option', { value: f.file }, f.file)).concat([h('option', { value: '__new' }, '(new file...)')]));
  const newName = h('input', { type: 'text', value: S.kind + '_overrides.json', style: { display: hand.length ? 'none' : '' } });
  if (!hand.length) fileSel.value = '__new';
  fileSel.onchange = () => { newName.style.display = fileSel.value === '__new' ? '' : 'none'; };
  modal('Override ' + id, h('div', { class: 'form' }, row('Save into', h('div', { class: 'line' }, fileSel, newName))), [{ label: 'Cancel' }, { label: 'Create override', primary: true, onclick: async () => {
    let file = fileSel.value;
    if (file === '__new') { file = newName.value.trim(); if (!/^[A-Za-z0-9_][A-Za-z0-9_.-]*\.json$/.test(file)) { toast('Invalid file name', 'err'); return true; } await ensureNewFile(S.kind, file); }
    addRecord(S.kind, file, { id });
    S.tab = 'form'; await loadBase(id); renderList(); renderMain(); validateNow();
    return false;
  } }]);
}
function duplicateRecord() {
  const rec = curRec(), f = curFile(); if (!rec) return;
  const copy = clone(rec); copy.id = nextId(S.kind, (rec.id || 'record') + '_copy');
  if (S.kind === 'traits') copy.index = nextTraitIndex();
  if (S.kind === 'skills') { toast('Skills are fixed (one record per skill)', 'err'); return; }
  const idx = S.sel.idx + 1;
  mutate(S.sel.key, (d) => { d.records.splice(idx, 0, copy); }, {});
  S.sel = { key: S.sel.key, idx };
  renderList(); renderMain(); toast('Duplicated as ' + copy.id, 'ok');
}
function deleteRecord() {
  const rec = curRec(); if (!rec) return;
  modal('Delete record', h('div', {}, 'Delete ', h('b', {}, rec.id), ' from ' + S.sel.key + '? You can undo this until the file is saved; saving is refused if other records still refer to it.'), [{ label: 'Cancel' }, { label: 'Delete', danger: true, onclick: () => {
    const key = S.sel.key, idx = S.sel.idx;
    mutate(key, (d) => { d.records.splice(idx, 1); }, {});
    S.sel = null; renderList(); renderMain(); return false;
  } }]);
}
function takeOver(f) {
  mutate(fkey(f.kind, f.file), (d) => { delete d.generator; }, { full: true });
  f.generated = false; f.takingOver = true; renderList(); renderMain();
  toast('"generator" removed from the working copy; save to keep it hand-authored', 'ok');
}
Ed.takeOver = takeOver;

function filesDialog() {
  const kind = S.kind;
  const info = (S.info.kinds.find((k) => k.id === kind) || { files: [] }).files;
  const body = h('div', {}, h('p', { class: 'fieldhelp' }, 'Files of data/' + kind + '. Files starting with "_" are ignored by the server (drafts, disabled and deleted files).'),
    info.map((fi) => h('div', { class: 'rrow', style: { cursor: 'default' } }, h('span', { class: 'id' }, fi.file), h('span', { class: 'nm' }, fi.records + ' records'),
      fi.generated ? h('span', { class: 'badge generated' }, 'generated') : fi.draft ? h('span', { class: 'badge draft' }, 'draft / ignored') : h('span', { class: 'badge hand' }, 'hand'),
      !fi.draft ? h('button', { class: 'small danger', onclick: () => deleteFileDialog(fi.file) }, 'Delete') : null)),
    h('div', { class: 'actions' }, h('button', { onclick: () => newFileDialog() }, 'New file...')));
  modal('Files: ' + kind, body, [{ label: 'Close' }]);
}
function newFileDialog() {
  const name = h('input', { type: 'text', value: S.kind + '_new.json' });
  modal('New file in data/' + S.kind, row('Name', name), [{ label: 'Cancel' }, { label: 'Create', primary: true, onclick: async () => {
    if (!/^[A-Za-z0-9_][A-Za-z0-9_.-]*\.json$/.test(name.value.trim())) { toast('Invalid file name', 'err'); return true; }
    await ensureNewFile(S.kind, name.value.trim()); renderList(); toast('Created in memory; it is written when you add a record and save', 'ok'); return false;
  } }]);
}
function deleteFileDialog(file) {
  modal('Delete ' + S.kind + '/' + file, h('div', {}, 'The file is renamed to ', h('code', {}, '_deleted_' + file), ' (the server ignores it), not destroyed. Refused if other files depend on its records.'),
    [{ label: 'Cancel' }, { label: 'Delete file', danger: true, onclick: async () => {
      const r = await api('DELETE', 'delete', { kind: S.kind, file, confirm: true });
      if (!r.ok) { toast(r.j.error || 'refused', 'err'); if (r.j.errors) showSaveErrors(r.j.errors); return false; }
      delete S.files[fkey(S.kind, file)]; await refreshInfo(); await openKind(S.kind); toast('Moved to ' + r.j.movedTo, 'ok'); return false;
    } }]);
}

// ------------------------------------------------------------------------------------------------------------------ schema driven form
function ensureDatalist(ref) {
  const id = 'dl_' + ref;
  let dl = document.getElementById(id);
  if (dl) return id;
  dl = h('datalist', { id });
  const R = S.refs || {};
  let opts = [];
  if (ref === 'item') opts = (R.items || []).map((x) => [x.id, x.name]);
  else if (ref === 'npc') opts = (R.npcs || []).map((x) => [x.id, x.name]);
  else if (ref === 'proto') opts = (R.items || []).concat(R.npcs || []).map((x) => [x.id, x.name]);
  else if (ref === 'quest') opts = (R.quests || []).map((x) => [x, '']);
  else if (ref === 'perk') opts = (R.perks || []).map((x) => [x, '']);
  else if (ref === 'trait') opts = (R.traits || []).map((x) => [x, '']);
  else if (ref === 'dialog') opts = (R.dialogs || []).map((x) => [x, '']);
  else if (ref === 'map') opts = (R.maps || []).map((x) => [x, '']);
  else if (ref === 'flag') opts = (R.flags || []).map((x) => [x, '']);
  for (const [v, l] of opts) dl.append(h('option', { value: v }, l || ''));
  document.body.append(dl);
  return id;
}
Ed.ensureDatalist = ensureDatalist;
function refName(ref, v) {
  const R = S.refs || {};
  const a = ref === 'item' ? R.items : ref === 'npc' ? R.npcs : null;
  const x = a && a.find((e) => lower(e.id) === lower(v));
  return x ? x.name : '';
}
Ed.refName = refName;

// Form context: C = {key, idx, kind, base (record of the base prototype or null), generated}
function ctxNow() {
  const f = curFile();
  return { key: S.sel.key, idx: S.sel.idx, kind: S.kind, base: S.baseRec ? S.baseRec.record : null, generated: !!(f && f.generated), cls: clsOf(curRec()), ks: kindSchema() };
}
const recAt = (C) => S.files[C.key].doc.records[C.idx];

function setVal(C, path, v, full) {
  mutate(C.key, (d) => { const rec = d.records[C.idx]; if (v === undefined) delIn(rec, path); else setIn(rec, path, v); }, { co: 'f:' + C.key + C.idx + pstr(path), full: !!full });
}
function status(C, path) {
  const cur = getIn(recAt(C), path), b = C.base ? getIn(C.base, path) : undefined;
  if (!C.base || path[0] === 'id' || path[0] === 'base') return '';
  if (cur === undefined) return b === undefined ? '' : 'inh';
  if (b === undefined) return 'new';
  return eq(cur, b) ? 'same' : 'ovr';
}
function resetPath(C, path) {
  const b = C.base ? getIn(C.base, path) : undefined;
  if (b === undefined || !C.generated) setVal(C, path, undefined, true);
  else setVal(C, path, clone(b), true);
}
function fmtVal(v) { return v === undefined ? '(unset)' : typeof v === 'string' ? JSON.stringify(v) : JSON.stringify(v); }

function frow(C, f, path, ctlEls, opt) {
  opt = opt || {};
  const row = h('div', { class: 'frow', 'data-path': pstr(path) },
    h('label', { title: f.key }, f.label || f.key, f.required ? h('span', { class: 'req' }, ' *') : null),
    h('div', { class: 'ctl' }, ctlEls, f.help ? h('div', { class: 'fieldhelp' }, f.help) : null, h('div', { class: 'basehint', 'data-hint': '1' }), h('div', { class: 'fieldmsg', 'data-msg': '1' })));
  row._refresh = () => {
    const st = status(C, path);
    row.classList.toggle('ovr', st === 'ovr' || st === 'new');
    const hint = row.querySelector('[data-hint]');
    hint.textContent = '';
    if (!C.base || !st) return;
    const b = getIn(C.base, path);
    if (st === 'ovr') hint.append('base: ', h('b', {}, fmtVal(b)), ' ', h('button', { class: 'linkish', onclick: () => resetPath(C, path) }, 'reset to base'));
    else if (st === 'new') hint.append('not in base (added by this record) ', h('button', { class: 'linkish', onclick: () => resetPath(C, path) }, 'remove'));
    else if (st === 'same') hint.append('same as base', C.generated ? '' : ' (override is redundant) ', h('button', { class: 'linkish', onclick: () => resetPath(C, path) }, C.generated ? '' : 'remove'));
    else if (st === 'inh') hint.append('inherited from base: ', h('b', {}, fmtVal(b)));
  };
  row._refresh();
  return row;
}

function sampleFor(f) {
  const t = f.type;
  if (t === 'string') return f.enum ? f.enum[0] : '';
  if (t === 'int' || t === 'number') return f.default !== undefined ? f.default : f.min !== undefined ? Math.max(f.min, Math.min(f.max === undefined ? 1e9 : f.max, t === 'int' ? 1 : 1)) : 0;
  if (t === 'object') { const o = {}; for (const sf of f.fields || []) if (sf.required) o[sf.key] = sampleFor(sf); return o; }
  if (t === 'bool') return true;
  if (t === 'enum') return f.enum[0];
  if (t === 'cell') return [0, 0];
  if (t === 'list') { if (f.fixedLen) return Array.from({ length: f.fixedLen }, () => sampleFor(f.item)); return []; }
  return null;
}
Ed.sampleFor = sampleFor;

function renderFields(C, fields, path, into) {
  for (const f of fields) {
    if (f.appliesTo && C.kind === 'items') {   // weapon / armor / ammo groups only where they make sense
      const present = getIn(recAt(C), path.concat(f.key)) !== undefined;
      if (!present && C.cls !== 'other' && C.cls !== f.appliesTo && C.cls !== (f.appliesTo === 'armor' ? 'armor' : f.appliesTo)) continue;
    }
    into.append(renderField(C, f, path.concat(f.key)));
  }
  return into;
}
function renderField(C, f, path) {
  const rec = recAt(C);
  const val = getIn(rec, path);
  const t = f.type;
  const baseVal = C.base ? getIn(C.base, path) : undefined;
  if (C.ks && C.ks.editor === 'dialog' && f.key === 'nodes' && path.length === 1) {
    return frow(C, f, path, h('div', {}, (val || []).length + ' nodes. ', h('button', { class: 'linkish', onclick: () => { S.tab = 'graph'; renderMain(); } }, 'Edit them in the Dialog graph tab')));
  }
  const unset = (val !== undefined && !f.required) ? h('button', { class: 'small linkish', title: 'Remove this field from the record', onclick: () => setVal(C, path, undefined, true) }, 'unset') : null;
  if (t === 'object') return renderObject(C, f, path, val, unset);
  if (t === 'list') return renderList_(C, f, path, val, unset);
  let ctl;
  if (t === 'string' || t === 'text') {
    const dl = f.ref && f.ref !== 'stf' && f.ref !== 'node' ? ensureDatalist(f.ref) : (f.ref === 'node' && Ed.nodeDatalist ? Ed.nodeDatalist() : null);
    const inp = t === 'text' ? h('textarea', { rows: 3, value: val === undefined ? '' : val, placeholder: typeof baseVal === 'string' ? baseVal : '' })
      : f.enum ? h('select', {}, [f.required ? null : h('option', { value: '' }, ''), f.enum.map((e) => h('option', { value: e, selected: lower(val) === lower(e) }, e))])
        : h('input', { type: 'text', value: val === undefined ? '' : val, list: dl, placeholder: typeof baseVal === 'string' ? baseVal : '', maxlength: f.maxLen ? f.maxLen + 50 : null });
    if (f.enum && val !== undefined) inp.value = val;
    const upd = () => { const v = inp.value; setVal(C, path, v === '' && !f.required ? undefined : v); if (f.maxLen) cnt.textContent = f.maxLen && inp.value.length > f.maxLen ? inp.value.length + ' / ' + f.maxLen + ' too long' : ''; };
    inp.addEventListener(f.enum ? 'change' : 'input', upd);
    const cnt = h('span', { class: 'fieldmsg' });
    const nm = f.ref === 'item' || f.ref === 'npc' ? h('span', { class: 'count' }, refName(f.ref, val)) : null;
    if (nm) inp.addEventListener('input', () => { nm.textContent = refName(f.ref, inp.value); });
    ctl = h('div', { class: 'line' }, inp, nm, cnt, unset);
  } else if (t === 'int' || t === 'number') {
    const inp = h('input', { type: 'number', step: t === 'int' ? 1 : 'any', min: f.min, max: f.max, value: val === undefined ? '' : val, placeholder: baseVal !== undefined ? String(baseVal) : '' });
    inp.addEventListener('input', () => { setVal(C, path, inp.value === '' ? undefined : Number(inp.value)); });
    ctl = h('div', { class: 'line' }, inp, f.min !== undefined || f.max !== undefined ? h('span', { class: 'count' }, (f.min !== undefined ? f.min : '') + '..' + (f.max !== undefined ? f.max : '')) : null, unset);
  } else if (t === 'bool') {
    const sel = h('select', {}, h('option', { value: '' }, '(unset)'), h('option', { value: 'true' }, 'true'), h('option', { value: 'false' }, 'false'));
    sel.value = val === undefined ? '' : String(val);
    sel.addEventListener('change', () => setVal(C, path, sel.value === '' ? undefined : sel.value === 'true'));
    ctl = sel;
  } else if (t === 'enum' || t === 'intenum') {
    const labels = t === 'intenum' ? f.labels : f.enum;
    const sel = h('select', {}, h('option', { value: '' }, '(unset)'), labels.map((l, i) => h('option', { value: t === 'intenum' ? String(f.min + i) : l }, t === 'intenum' ? (f.min + i) + ' ' + l : l)));
    sel.value = val === undefined ? '' : String(val);
    sel.addEventListener('change', () => setVal(C, path, sel.value === '' ? undefined : (t === 'intenum' ? Number(sel.value) : sel.value)));
    ctl = sel;
  } else if (t === 'cell') {
    const a = Array.isArray(val) ? val : [];
    const mk = (i) => h('input', { type: 'number', min: 0, step: 1, value: a[i] === undefined ? '' : a[i], style: { width: '90px' } });
    const x = mk(0), y = mk(1);
    const upd = () => setVal(C, path, x.value === '' && y.value === '' ? undefined : [Number(x.value || 0), Number(y.value || 0)]);
    x.addEventListener('input', upd); y.addEventListener('input', upd);
    ctl = h('div', { class: 'line' }, 'x', x, 'y', y, unset);
  } else if (t === 'stringlist') {
    ctl = renderStringList(C, f, path, val, unset);
  } else if (t === 'intmap') {
    ctl = renderIntMap(C, f, path, val, unset);
  } else if (t === 'statmods') {
    ctl = renderStatMods(C, f, path, val, unset);
  } else if (t === 'condition' && Ed.renderCondition) {
    ctl = Ed.renderCondition(val, (v) => setVal(C, path, v, true), {});
  } else if (t === 'effects' && Ed.renderEffects) {
    ctl = Ed.renderEffects(val || [], (v) => setVal(C, path, v && v.length ? v : undefined, true), {});
  } else if (t === 'dlgstart' && Ed.renderDlgStart) {
    ctl = Ed.renderDlgStart(val, (v) => setVal(C, path, v, true));
  } else {
    ctl = h('div', { class: 'count' }, '(edit in the JSON tab)');
  }
  return frow(C, f, path, ctl);
}
function renderObject(C, f, path, val, unset) {
  const fs = h('fieldset', { 'data-path': pstr(path) });
  const st = status(C, path);
  fs.append(h('legend', {}, f.label || f.key, f.help ? h('span', { class: 'count', title: f.help }, '(?)') : null,
    val !== undefined ? h('button', { class: 'small linkish', onclick: () => setVal(C, path, undefined, true) }, 'remove group') : null,
    st === 'ovr' ? h('button', { class: 'small linkish', onclick: () => resetPath(C, path) }, 'reset group to base') : null,
    st === 'ovr' ? h('span', { class: 'badge warn' }, 'differs from base') : null));
  if (val === undefined || val === null) {
    fs.append(h('button', { class: 'small', onclick: () => setVal(C, path, sampleFor(f), true) }, '+ Add ' + (f.label || f.key)));
    if (C.base && getIn(C.base, path) !== undefined) fs.append(h('div', { class: 'basehint' }, 'inherited from base: ', h('b', {}, fmtVal(getIn(C.base, path)))));
    return fs;
  }
  const inner = h('div', { class: 'form' });
  renderFields(C, f.fields, path, inner);
  fs.append(inner);
  return fs;
}
function moveBtn(label, title, fn, dis) { return h('button', { class: 'small', title, disabled: dis, onclick: fn }, label); }
function renderList_(C, f, path, val, unset) {
  const fs = h('fieldset', { 'data-path': pstr(path) });
  const st = status(C, path);
  fs.append(h('legend', {}, f.label || f.key, val !== undefined ? h('span', { class: 'count' }, '(' + val.length + ')') : null,
    val !== undefined ? h('button', { class: 'small linkish', onclick: () => setVal(C, path, undefined, true) }, 'unset') : null,
    st === 'ovr' ? h('button', { class: 'small linkish', onclick: () => resetPath(C, path) }, 'reset to base') : null));
  if (val === undefined) { fs.append(h('button', { class: 'small', onclick: () => setVal(C, path, f.fixedLen ? Array.from({ length: f.fixedLen }, () => sampleFor(f.item)) : [], true) }, '+ Set ' + (f.label || f.key)));
    if (C.base && getIn(C.base, path) !== undefined) fs.append(h('div', { class: 'basehint' }, 'inherited from base: ', h('b', {}, fmtVal(getIn(C.base, path)))));
    return fs; }
  if (f.item.type === 'object') {
    val.forEach((it, i) => {
      const p = path.concat(i);
      const card = h('div', { class: 'card', 'data-path': pstr(p) }, h('div', { class: 'card-head' }, '#' + (i + 1) + (it && it.id ? ' ' + it.id : ''), h('span', { class: 'grow' }),
        moveBtn('↑', 'Move up', () => mutate(C.key, (d) => { const a = getIn(d.records[C.idx], path); if (i > 0) a.splice(i - 1, 0, a.splice(i, 1)[0]); }, { full: true }), i === 0),
        moveBtn('↓', 'Move down', () => mutate(C.key, (d) => { const a = getIn(d.records[C.idx], path); if (i < a.length - 1) a.splice(i + 1, 0, a.splice(i, 1)[0]); }, { full: true }), i === val.length - 1),
        moveBtn('⧉', 'Duplicate', () => mutate(C.key, (d) => { const a = getIn(d.records[C.idx], path); a.splice(i + 1, 0, clone(a[i])); }, { full: true }), f.maxCount && val.length >= f.maxCount),
        moveBtn('✕', 'Remove', () => mutate(C.key, (d) => { getIn(d.records[C.idx], path).splice(i, 1); }, { full: true }))));
      const inner = h('div', { class: 'form' });
      renderFields(C, f.item.fields, p, inner);
      card.append(inner); fs.append(card);
    });
    fs.append(h('button', { class: 'small', disabled: f.maxCount && val.length >= f.maxCount, onclick: () => mutate(C.key, (d) => { getIn(d.records[C.idx], path).push(sampleFor(f.item)); }, { full: true }) }, '+ Add'));
  } else {   // fixed length list of numbers (armor words)
    const line = h('div', { class: 'inline-list' });
    val.forEach((v, i) => {
      const inp = h('input', { type: 'number', step: 1, min: f.item.min, max: f.item.max, value: v === undefined ? '' : v, style: { width: '80px' } });
      inp.addEventListener('input', () => setVal(C, path.concat(i), inp.value === '' ? 0 : Number(inp.value)));
      line.append(h('div', {}, h('span', { class: 'count' }, i), inp));
    });
    fs.append(line);
  }
  if (C.base && st) { const hint = h('div', { class: 'basehint' }); fs.append(hint); if (st === 'ovr') hint.append('base: ', h('b', {}, fmtVal(getIn(C.base, path)))); }
  return fs;
}
function renderStringList(C, f, path, val, unset) {
  const dl = ensureDatalist(f.ref);
  const wrap = h('div', {});
  if (val === undefined) { wrap.append(h('button', { class: 'small', onclick: () => setVal(C, path, [], true) }, '+ Set list')); const b = C.base ? getIn(C.base, path) : undefined; if (b !== undefined) wrap.append(h('div', { class: 'basehint' }, 'inherited from base: ', h('b', {}, fmtVal(b)))); return wrap; }
  const list = h('div', {});
  val.forEach((v, i) => {
    const inp = h('input', { type: 'text', list: dl, value: v, style: { flex: 1 } });
    const nm = h('span', { class: 'count' }, refName(f.ref, v));
    inp.addEventListener('input', () => { setVal(C, path.concat(i), inp.value); nm.textContent = refName(f.ref, inp.value); });
    list.append(h('div', { class: 'line' }, inp, nm, h('button', { class: 'small', title: 'Remove', onclick: () => mutate(C.key, (d) => { getIn(d.records[C.idx], path).splice(i, 1); }, { full: true }) }, '✕')));
  });
  wrap.append(list, h('div', { class: 'line' }, h('button', { class: 'small', disabled: f.maxCount && val.length >= f.maxCount, onclick: () => mutate(C.key, (d) => { getIn(d.records[C.idx], path).push(''); }, { full: true }) }, '+ Add'), val.length ? null : h('span', { class: 'count' }, '(empty list: explicitly none)'), unset));
  return wrap;
}
function renderIntMap(C, f, path, val, unset) {
  const wrap = h('div', {});
  if (val === undefined) { wrap.append(h('button', { class: 'small', onclick: () => setVal(C, path, {}, true) }, '+ Set')); return wrap; }
  Object.keys(val).forEach((k) => {
    const sel = h('select', {}, f.keys.map((o) => h('option', { value: o, selected: o === k }, o)));
    const num = h('input', { type: 'number', step: 1, min: f.min, max: f.max, value: val[k], style: { width: '90px' } });
    sel.addEventListener('change', () => mutate(C.key, (d) => { const m = getIn(d.records[C.idx], path); const nv = {}; for (const kk of Object.keys(m)) nv[kk === k ? sel.value : kk] = m[kk]; setIn(d.records[C.idx], path, nv); }, { full: true }));
    num.addEventListener('input', () => setVal(C, path.concat(k), Number(num.value)));
    wrap.append(h('div', { class: 'line', 'data-path': pstr(path.concat(k)) }, sel, num, h('span', { class: 'count' }, f.min + '..' + f.max), h('button', { class: 'small', onclick: () => mutate(C.key, (d) => { delete getIn(d.records[C.idx], path)[k]; }, { full: true }) }, '✕')));
  });
  const free = f.keys.find((k) => !(k in val));
  wrap.append(h('div', { class: 'line' }, h('button', { class: 'small', disabled: !free, onclick: () => mutate(C.key, (d) => { getIn(d.records[C.idx], path)[free] = f.min; }, { full: true }) }, '+ Add'), unset));
  return wrap;
}
function renderStatMods(C, f, path, val, unset) {
  const wrap = h('div', {});
  const stats = S.schema.enums.stats;
  if (val === undefined) { wrap.append(h('button', { class: 'small', onclick: () => setVal(C, path, [], true) }, '+ Set effects')); return wrap; }
  val.forEach((e, i) => {
    const sel = h('select', {}, stats.map((s) => h('option', { value: s, selected: lower(s) === lower(e.stat) }, s)));
    if (!stats.some((s) => lower(s) === lower(e.stat))) sel.prepend(h('option', { value: e.stat, selected: true }, String(e.stat)));
    const num = h('input', { type: 'number', step: 1, min: -1000, max: 1000, value: e.add, style: { width: '90px' } });
    sel.addEventListener('change', () => setVal(C, path.concat(i, 'stat'), sel.value));
    num.addEventListener('input', () => setVal(C, path.concat(i, 'add'), Number(num.value)));
    wrap.append(h('div', { class: 'line', 'data-path': pstr(path.concat(i)) }, sel, 'add', num, h('button', { class: 'small', onclick: () => mutate(C.key, (d) => { getIn(d.records[C.idx], path).splice(i, 1); }, { full: true }) }, '✕')));
  });
  wrap.append(h('div', { class: 'line' }, h('button', { class: 'small', onclick: () => mutate(C.key, (d) => { getIn(d.records[C.idx], path).push({ stat: stats[0], add: 1 }); }, { full: true }) }, '+ Add effect'), unset));
  return wrap;
}

function renderRecordForm() {
  const C = ctxNow();
  const form = h('div', { class: 'form', id: 'recForm' });
  const probs = h('div', { id: 'recProblems' });
  form.append(probs);
  renderFields(C, C.ks.fields, [], form);
  return form;
}
function refreshFieldStates() {
  document.querySelectorAll('#recForm .frow').forEach((r) => { if (r._refresh) r._refresh(); });
  const jt = document.getElementById('jsonTa');
  if (jt && document.activeElement !== jt && curRec()) jt.value = JSON.stringify(curRec(), null, 2);
}

// ------------------------------------------------------------------------------------------------------------------ diff / json tabs
function flat(o, p, out) {
  out = out || {};
  if (o && typeof o === 'object' && !Array.isArray(o)) { for (const k of Object.keys(o)) flat(o[k], p.concat(k), out); if (!Object.keys(o).length) out[pstr(p)] = { path: p, v: o }; }
  else out[pstr(p)] = { path: p, v: o };
  return out;
}
function renderDiff() {
  const C = ctxNow();
  if (!C.base) return h('div', { class: 'banner info' }, 'This record has no base prototype (a new record without "base"): nothing to compare with.');
  const a = flat(recAt(C), []), b = flat(C.base, []);
  const rows = [];
  for (const k of new Set(Object.keys(a).concat(Object.keys(b)))) {
    if (k === 'id' || k === 'base') continue;
    const x = a[k], y = b[k];
    if (x && y && eq(x.v, y.v)) continue;
    if (!x) continue;   // inherited (absent in the record): not a difference
    rows.push([k, x.path, y ? y.v : undefined, x.v]);
  }
  const t = h('table', { class: 'diff-table' }, h('tr', {}, h('th', {}, 'Field'), h('th', {}, 'Base value (' + (S.baseRec.key || '') + ')'), h('th', {}, 'This record'), h('th', {})));
  rows.forEach(([k, p, from, to]) => t.append(h('tr', {}, h('td', { class: 'mono' }, k), h('td', { class: 'from mono' }, fmtVal(from)), h('td', { class: 'to mono' }, fmtVal(to)),
    h('td', {}, h('button', { class: 'small', onclick: () => resetPath(C, p) }, 'reset to base')))));
  return h('div', {}, h('p', { class: 'fieldhelp' }, rows.length ? rows.length + ' field(s) differ from the original prototype. Fields that are absent inherit the base value.' : 'No field differs from the base prototype.'),
    rows.length ? t : null, rows.length ? h('div', { class: 'actions', style: { marginTop: '8px' } }, h('button', { onclick: () => resetAll(C) }, 'Reset all to base')) : null,
    h('h4', {}, 'Base prototype record'), h('pre', { class: 'json mono' }, JSON.stringify(C.base, null, 2)));
}
function resetAll(C) {
  mutate(C.key, (d) => {
    const rec = d.records[C.idx];
    const keep = { id: rec.id }; if (rec.base) keep.base = rec.base;
    if (C.generated) { const b = clone(C.base); d.records[C.idx] = Object.assign({}, b, { id: rec.id }); if (rec.base) d.records[C.idx].base = rec.base; } else d.records[C.idx] = keep;
  }, { full: true });
}
function renderJsonTab() {
  const rec = curRec(), key = S.sel.key + '#' + S.sel.idx;
  const pending = S.rawText[key];
  const ta = h('textarea', { class: 'json' + (pending ? ' bad' : ''), id: 'jsonTa', spellcheck: 'false', value: pending !== undefined ? pending : JSON.stringify(rec, null, 2) });
  const msg = h('div', { class: 'fieldmsg' }, pending !== undefined ? 'Invalid JSON: this text is kept here and not applied until it parses.' : '');
  ta.addEventListener('input', () => {
    try {
      const v = JSON.parse(ta.value);
      if (!v || typeof v !== 'object' || Array.isArray(v)) throw new Error('a record must be a JSON object');
      delete S.rawText[key]; ta.classList.remove('bad'); msg.textContent = '';
      mutate(S.sel.key, (d) => { d.records[S.sel.idx] = v; }, { co: 'json:' + key });
      renderList();
    } catch (e) { S.rawText[key] = ta.value; ta.classList.add('bad'); msg.textContent = 'Invalid JSON, not applied: ' + e.message; }
  });
  const prev = h('pre', { class: 'json mono' }, 'loading...');
  api('POST', 'format', { doc: curFile().doc }).then((r) => { prev.textContent = r.ok ? r.j.text : (r.j.error || 'cannot format'); });
  return h('div', {}, h('p', { class: 'fieldhelp' }, 'The record as JSON. Edits apply as soon as the text is valid; invalid text is kept in the box and never written.'), ta, msg,
    h('details', { style: { marginTop: '10px' } }, h('summary', {}, 'File as it will be saved (' + curFile().kind + '/' + curFile().file + ', the exporter\'s formatting)'), prev));
}

// ------------------------------------------------------------------------------------------------------------------ validation
let valTimer = null;
function scheduleValidate() { clearTimeout(valTimer); valTimer = setTimeout(validateNow, 700); }
async function validateNow() {
  clearTimeout(valTimer);
  const f = S.sel && S.sel.key ? S.files[S.sel.key] : null;
  const inLocale = S.kind === 'locale';
  let body = {};
  if (f && f.doc && !inLocale && isDirty(f)) body = { kind: f.kind, file: f.file, doc: f.doc };
  const r = await api('POST', 'validate', body);
  if (!r.ok && !r.j.errors) { S.problems = { errors: [{ raw: r.j.error || 'validation failed', file: '', path: '', message: r.j.error || 'validation failed' }], warnings: [], own: '', key: '' }; renderBottom(); return; }
  S.problems = { errors: r.j.errors || [], warnings: r.j.warnings || [], own: r.j.own || '', key: body.file ? fkey(body.kind, body.file) : '' };
  renderBottom(); applyFieldErrors();
  if (Ed.onValidated) Ed.onValidated();
}
Ed.validateNow = validateNow;
function parseRecPath(p) { const m = /^records\[(\d+)\](.*)$/.exec(p || ''); return m ? { idx: Number(m[1]), rel: m[2].replace(/^\./, '') } : null; }
function applyFieldErrors() {
  document.querySelectorAll('#main .frow.err').forEach((e) => { e.classList.remove('err'); const m = e.querySelector('[data-msg]'); if (m) m.textContent = ''; });
  const box = document.getElementById('recProblems');
  if (box) box.textContent = '';
  if (!S.sel || !S.sel.key || S.tab !== 'form') return;
  const f = S.files[S.sel.key]; if (!f) return;
  const fileTag = 'data/' + f.kind + '/' + f.file;
  const rest = [];
  for (const m of S.problems.errors) {
    if (m.file !== fileTag) continue;
    const rp = parseRecPath(m.path);
    if (!rp || rp.idx !== S.sel.idx) continue;
    let target = null;
    const rows = Array.from(document.querySelectorAll('#recForm .frow[data-path]'));
    let p = rp.rel;
    while (p && !target) {   // the field itself, else the closest enclosing field that has a row
      target = rows.find((r) => r.getAttribute('data-path') === p) || null;
      const up = p.replace(/(\[\d+\]|\.[^.\[]+|^[^.\[]+)$/, '');
      if (up === p) break;
      p = up;
    }
    if (target) { target.classList.add('err'); const msg = target.querySelector('[data-msg]'); if (msg) msg.textContent = (msg.textContent ? msg.textContent + ' | ' : '') + m.message; }
    else rest.push(m);
  }
  if (box && rest.length) box.append(h('div', { class: 'banner err' }, rest.map((m) => h('div', {}, (m.path || '') + ': ' + m.message))));
}
function showSaveErrors(errors) { S.problems = { errors, warnings: [], own: '', key: '' }; S.bottomTab = 'problems'; renderBottom(); }
Ed.showSaveErrors = showSaveErrors;

function renderBottom() {
  const tabs = $('bottomTabs'), body = $('bottomBody');
  const e = S.problems.errors.length, w = S.problems.warnings.length;
  tabs.textContent = '';
  for (const [id, label] of [['problems', 'Problems (' + e + ' errors, ' + w + ' warnings)'], ['reload', 'Server reload report']])
    tabs.append(h('div', { class: 'tab' + (S.bottomTab === id ? ' on' : ''), onclick: () => { S.bottomTab = id; renderBottom(); } }, label));
  body.textContent = '';
  if (S.bottomTab === 'reload') { body.append(h('pre', { class: 'mono', style: { margin: 0, whiteSpace: 'pre-wrap' } }, S.reloadReport || 'Nothing applied yet. "Apply to running server" re-reads data/ and shows what changed.')); return; }
  if (!e && !w) { body.append(h('div', { class: 'ok', style: { color: 'var(--ok)' } }, 'No problems found by the server\'s validator.')); return; }
  const mk = (m, cls, kind) => h('div', { class: 'msg ' + cls, onclick: () => gotoProblem(m) }, h('span', { class: 'k' }, kind), ' ', h('span', { class: 'f' }, (m.file || '') + ' ' + (m.path || '')), ' ', m.message);
  S.problems.errors.forEach((m) => body.append(mk(m, 'e', 'error')));
  S.problems.warnings.forEach((m) => body.append(mk(m, 'w', 'warning')));
}
async function gotoProblem(m) {
  const mm = /^data\/([a-z]+)\/(.+)$/.exec(m.file || '');
  if (!mm) return;
  const kind = mm[1], file = mm[2], key = fkey(kind, file);
  if (!S.info.kinds.find((k) => k.id === kind)) return;
  if (S.kind !== kind) await openKind(kind);
  const rp = parseRecPath(m.path);
  if (rp && S.files[key]) { await selectById(kind, key, rp.idx); }
}

// ------------------------------------------------------------------------------------------------------------------ save / reload
async function saveFile(key, flags) {
  const f = S.files[key]; if (!f || !f.doc) return false;
  if (!isDirty(f) && !f.isNew) return true;
  const body = Object.assign({ kind: f.kind, file: f.file, doc: f.doc }, f.etag && !f.isNew ? { etag: f.etag } : {}, flags || {});
  const r = await api('PUT', 'put', body);
  if (r.ok) {
    if (r.j.draft) { toast('Draft saved as ' + r.j.kind + '/' + r.j.file + ' (ignored by the server; the live file is unchanged)', 'ok'); return false; }
    if (r.j.tookOver) { delete f.doc.generator; f.generated = false; f.generator = undefined; }
    f.saved = JSON.stringify(f.doc); f.etag = r.j.etag; f.isNew = false; f.restored = false; f.hasComments = false;
    persistDraft(f);
    toast('Saved ' + key + (r.j.warnings && r.j.warnings.length ? ' (' + r.j.warnings.length + ' warning(s))' : ''), 'ok');
    if (r.j.hadCommentsLost) toast('Saved. Comments in the file were dropped.', 'ok');
    await refreshInfo();
    renderDirty(); renderList(); renderNav(); if (S.kind !== 'locale') renderMain(); else if (Ed.renderLocaleMain) renderLocaleAll();
    validateNow();
    return true;
  }
  const j = r.j;
  if (r.status === 409 && j.code === 'generated') {
    modal('Generated file', h('div', {}, h('p', {}, key + ' is generated by ', h('code', {}, j.generator), '. The next ', h('code', {}, 'f3_server --export-data'), ' overwrites your edits.'),
      h('p', {}, 'Take it over to turn it into a hand-authored file (the "generator" key is removed and the exporter will skip it).')),
    [{ label: 'Cancel' }, { label: 'Save, keep generator', onclick: async () => { await saveFile(key, Object.assign({}, flags, { ackGenerated: true })); return false; } },
      { label: 'Take over and save', primary: true, onclick: async () => { await saveFile(key, Object.assign({}, flags, { takeover: true })); return false; } }]);
  } else if (r.status === 409 && j.code === 'conflict') {
    modal('File changed on disk', h('div', {}, key + ' was changed since the editor loaded it (git, the exporter, or another editor).'),
      [{ label: 'Cancel' }, { label: 'Reload from disk (lose my edits)', danger: true, onclick: async () => { delete S.files[key]; await loadFile(f.kind, f.file); renderList(); renderMain(); return false; } },
        { label: 'Overwrite the disk version', onclick: async () => { f.etag = j.etag; await saveFile(key, flags); return false; } }]);
  } else if (r.status === 422 && j.errors) {
    showSaveErrors(j.errors);
    modal('Not saved: ' + j.errors.length + ' validation error(s)', h('div', {}, h('p', {}, 'The file was NOT written. Fix the problems listed below, or keep the work as a draft file (_draft_' + f.file + ', ignored by the server).'),
      h('div', { class: 'mono' }, j.errors.slice(0, 8).map((m) => h('div', {}, (m.path || '') + ': ' + m.message)))),
    [{ label: 'Close' }, { label: 'Save as draft', onclick: async () => { await saveFile(key, Object.assign({}, flags, { draft: true })); return false; } }]);
  } else toast('Save failed: ' + (j.error || r.status), 'err');
  return false;
}
function renderLocaleAll() { renderList(); renderMain(); }
async function saveAll() { for (const f of dirtyFiles()) { const ok = await saveFile(fkey(f.kind, f.file)); if (!ok) break; } }

async function applyToServer() {
  const dirty = dirtyFiles();
  const go = async () => {
    const r = await api('POST', 'reload', {});
    S.reloadReport = (r.j.report || r.j.error || 'no answer').trim();
    S.bottomTab = 'reload'; renderBottom();
    toast(r.j.ok ? 'Server reloaded data/' : 'Reload failed, the server keeps the previous data', r.j.ok ? 'ok' : 'err');
    return false;
  };
  if (dirty.length) modal('Unsaved changes', h('div', {}, dirty.length + ' file(s) have unsaved changes; the server only sees what is saved on disk.'), [{ label: 'Cancel' }, { label: 'Apply anyway', onclick: go }]);
  else await go();
}

async function refreshInfo() {
  const r = await api('GET', 'kinds');
  if (r.ok) S.info = r.j;
  const rr = await api('GET', 'refs');
  if (rr.ok) { S.refs = rr.j; document.querySelectorAll('datalist').forEach((d) => d.remove()); }
}
Ed.refreshInfo = refreshInfo; Ed.saveFile = saveFile; Ed.openKind = openKind; Ed.renderMain = renderMain; Ed.renderList = renderList; Ed.renderNav = renderNav; Ed.loadFile = loadFile; Ed.renderDirty = renderDirty;
Ed.afterChange = afterChange; Ed.recordRows = recordRows; Ed.filesOfKind = filesOfKind; Ed.selectById = selectById; Ed.applyFieldErrors = applyFieldErrors; Ed.clsOf = clsOf; Ed.row = row; Ed.persistDraft = persistDraft;

// ------------------------------------------------------------------------------------------------------------------ boot
Ed.boot = async function () {
  const q = new URLSearchParams(location.search);
  S.token = q.get('token') || '';
  const theme = (() => { try { return localStorage.getItem('f3ed.theme'); } catch (e) { return null; } })();
  if (theme) document.documentElement.setAttribute('data-theme', theme);
  $('btnTheme').onclick = () => {
    const cur = document.documentElement.getAttribute('data-theme') || (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
    const nx = cur === 'dark' ? 'light' : 'dark';
    document.documentElement.setAttribute('data-theme', nx);
    try { localStorage.setItem('f3ed.theme', nx); } catch (e) { /* ignore */ }
  };
  $('btnUndo').onclick = undo; $('btnRedo').onclick = redo;
  $('btnSave').onclick = () => { if (S.sel && S.sel.key) saveFile(S.sel.key); };
  $('btnSaveAll').onclick = saveAll;
  $('btnValidate').onclick = async () => { await validateNow(); toast(S.problems.errors.length ? S.problems.errors.length + ' error(s)' : 'Valid', S.problems.errors.length ? 'err' : 'ok'); };
  $('btnReload').onclick = applyToServer;
  document.addEventListener('keydown', (e) => {
    const inField = /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement && document.activeElement.tagName);
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') { e.preventDefault(); if (S.sel && S.sel.key) saveFile(S.sel.key); else if (S.kind === 'locale') saveAll(); }
    else if ((e.ctrlKey || e.metaKey) && !inField && e.key.toLowerCase() === 'z') { e.preventDefault(); if (e.shiftKey) redo(); else undo(); }
    else if ((e.ctrlKey || e.metaKey) && !inField && e.key.toLowerCase() === 'y') { e.preventDefault(); redo(); }
  });
  if (!S.token) { $('main').textContent = ''; $('main').append(h('div', { class: 'empty' }, 'No token. Open the URL printed by f3_server (http://127.0.0.1:<port>/?token=...).')); return; }
  const [sch, info, refs] = await Promise.all([api('GET', 'schema'), api('GET', 'kinds'), api('GET', 'refs')]);
  if (!sch.ok || !info.ok) { $('main').textContent = ''; $('main').append(h('div', { class: 'empty' }, 'Cannot reach the editor API: ' + (sch.j.error || info.j.error || 'unknown error') + '. The token is wrong or the server restarted (use the new URL).')); return; }
  S.schema = sch.j; S.info = info.j; S.refs = refs.ok ? refs.j : {};
  $('dataDir').textContent = S.info.dataDir + (S.info.hasGamedata ? '' : '  (no game data: original prototypes unavailable)');
  $('btnReload').disabled = !S.info.canReload;
  renderBottom(); renderDirty();
  const start = (location.hash || '').replace('#', '');
  await openKind(['items', 'npcs', 'perks', 'traits', 'skills', 'quests', 'dialogs', 'locale', 'objects'].includes(start) ? start : 'items');
};
})();
