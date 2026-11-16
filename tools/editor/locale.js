'use strict';
/* Locale tab: en and a target language side by side, missing translations (same logic as f3_server --check-locale). */
(function () {
const Ed = window.Ed, S = Ed.S, h = Ed.h, lower = Ed.lower;
const L = { st: null, lang: 'ru', q: '', group: '', filter: 'missing', page: 0, loading: false, loaded: false };
const PAGE = 120;
const kindOf = (lang) => 'locale/' + lang;

async function fetchStatus() {
  L.loading = true;
  const r = await Ed.api('GET', 'locale-status');
  L.loading = false;
  if (!r.ok) { Ed.toast('locale status: ' + (r.j.error || r.status), 'err'); return; }
  L.st = r.j;
  const others = (L.st.langs || []).filter((x) => x !== 'en');
  if (!others.includes(L.lang)) L.lang = others[0] || 'en';
}
async function loadAll() {
  const jobs = [];
  for (const lang of Object.keys(S.info.locale || {})) for (const f of S.info.locale[lang]) jobs.push(Ed.loadFile(kindOf(lang), typeof f === 'string' ? f : f.file));
  await Promise.all(jobs);
}
async function enter() {
  if (L.loaded || L.loading) return;
  await Promise.all([fetchStatus(), loadAll()]);
  L.loaded = true;
  Ed.renderList(); Ed.renderMain();
}
const filesOfLang = (lang) => Object.values(S.files).filter((f) => f.kind === kindOf(lang) && f.doc && f.file[0] !== '_');
function findFile(lang, key) { return filesOfLang(lang).find((f) => Object.prototype.hasOwnProperty.call(f.doc, key)); }
function targetFile(lang, e) {
  const have = findFile(lang, e.key);
  if (have) return have;
  const files = filesOfLang(lang);
  const pref = (e.en && e.en.file) || (e.group === 'data' ? 'content.json' : e.group === 'stf' ? 'stf.json' : '');
  return files.find((f) => f.file === pref) || files[0] || null;
}
async function ensureLangFile(lang, e) {
  let f = targetFile(lang, e);
  if (f) return f;
  const name = (e.en && e.en.file) || 'messages.json';
  const key = Ed.fkey(kindOf(lang), name);
  S.files[key] = { kind: kindOf(lang), file: name, doc: {}, saved: '', etag: null, generated: false, hasComments: false, isNew: true, parsed: true };
  return S.files[key];
}
function curText(lang, e) {
  const f = findFile(lang, e.key);
  if (f) return f.doc[e.key];
  if (lang === 'en') return e.en ? e.en.text : undefined;
  return e.tr && e.tr[lang] && e.tr[lang].text !== undefined && !findFile(lang, e.key) ? undefined : undefined;
}
function textOf(lang, e) {
  const t = curText(lang, e);
  if (t !== undefined) return t;
  if (lang === 'en') return e.en ? e.en.text : undefined;
  const c = e.tr && e.tr[lang];
  return c && c.text !== undefined ? c.text : undefined;
}
function setText(lang, e, v) {
  (async () => {
    const f = await ensureLangFile(lang, e);
    Ed.mutate(Ed.fkey(f.kind, f.file), (d) => { if (v === '') delete d[e.key]; else d[e.key] = v; }, { co: 'loc:' + lang + ':' + e.key });
    refreshCounts();
  })();
}
function effStatus(e, lang) {   // the server status, updated for edits made in this session
  const srv = (e.tr && e.tr[lang] && e.tr[lang].status) || 'ok';
  const have = textOf(lang, e);
  const inBase = !!e.en || e.dataText !== undefined;
  if (inBase && (have === undefined || have === '')) return 'missing';
  if (srv === 'missing' && have) return 'edited';
  return srv;
}
function entries() {
  if (!L.st) return [];
  const q = lower(L.q);
  return (L.st.entries || []).filter((e) => {
    if (L.group && e.group !== L.group) return false;
    if (q && !(lower(e.key).includes(q) || lower(textOf('en', e)).includes(q) || lower(textOf(L.lang, e)).includes(q) || lower(e.dataText).includes(q))) return false;
    const s = effStatus(e, L.lang);
    if (L.filter === 'missing') return s === 'missing';
    if (L.filter === 'problems') return s !== 'ok' && s !== 'edited';
    if (L.filter === 'edited') return s === 'edited' || (findFile(L.lang, e.key) && findFile(L.lang, e.key).isDirty);
    return true;
  });
}
let countEl = null, saveBtn = null;
function dirtyLoc() { return Object.values(S.files).filter((f) => f.kind.startsWith('locale/') && Ed.isDirty(f)); }
function refreshCounts() {
  if (countEl) countEl.textContent = entries().length + ' entries';
  if (saveBtn) { const n = dirtyLoc().length; saveBtn.disabled = !n; saveBtn.textContent = 'Save locale files' + (n ? ' (' + n + ')' : ''); }
}

Ed.renderLocaleList = function (pane) {
  enter();
  const langs = ((L.st && L.st.langs) || Object.keys(S.info.locale || {})).filter((x) => x !== 'en');
  const groups = L.st ? Array.from(new Set((L.st.entries || []).map((e) => e.group))).sort() : [];
  const sum = L.st && L.st.summary && L.st.summary[L.lang];
  pane.append(h('div', { class: 'lp-head' },
    h('div', { class: 'row' }, h('b', {}, 'Locale'), h('span', { class: 'grow' }), h('button', { class: 'small', onclick: async () => { await fetchStatus(); Ed.renderList(); Ed.renderMain(); } }, 'Refresh')),
    h('div', { class: 'row' }, 'Compare en with', h('select', { onchange: (e) => { L.lang = e.target.value; L.page = 0; Ed.renderList(); Ed.renderMain(); } }, langs.map((l) => h('option', { value: l, selected: l === L.lang }, l)))),
    h('div', { class: 'row' }, h('input', { type: 'search', placeholder: 'Search key or text', value: L.q, id: 'lq', oninput: (e) => { L.q = e.target.value; L.page = 0; Ed.renderLocaleMain($('main')); } })),
    h('div', { class: 'chips' }, [['missing', 'missing'], ['problems', 'any problem'], ['edited', 'edited'], ['all', 'all']].map(([v, l]) => h('span', { class: 'chip' + (L.filter === v ? ' on' : ''), onclick: () => { L.filter = v; L.page = 0; Ed.renderList(); Ed.renderLocaleMain($('main')); } }, l))),
    h('div', { class: 'row' }, h('select', { onchange: (e) => { L.group = e.target.value; L.page = 0; Ed.renderLocaleMain($('main')); } }, [h('option', { value: '' }, 'all groups'), groups.map((g) => h('option', { value: g, selected: g === L.group }, g))]))),
  h('div', { class: 'rows' }, h('div', { style: { padding: '10px' } },
    sum ? [h('div', {}, h('b', {}, L.lang), ': ' + sum.total + ' keys'),
      h('div', { style: { color: sum.missing ? 'var(--warn)' : 'var(--ok)' } }, sum.missing + ' missing'),
      h('div', {}, sum.extra + ' extra (not in en or data)'),
      h('div', { style: { color: sum.placeholders ? 'var(--err)' : 'inherit' } }, sum.placeholders + ' placeholder mismatches')] : h('div', { class: 'count' }, L.loading ? 'Loading...' : 'No status'),
    L.st && L.st.problems && L.st.problems.length ? h('div', { class: 'fieldmsg', style: { marginTop: '8px' } }, L.st.problems.slice(0, 5).map((p) => h('div', {}, p))) : null)));
  const lq = document.getElementById('lq');
  if (lq && document.activeElement === lq) lq.setSelectionRange(lq.value.length, lq.value.length);
};
const $ = (id) => document.getElementById(id);

Ed.renderLocaleMain = function (main) {
  main = main || $('main');
  if (!L.loaded) { main.textContent = ''; main.append(h('div', { class: 'empty' }, 'Loading locale...')); enter(); return; }
  const list = entries(), pages = Math.max(1, Math.ceil(list.length / PAGE));
  if (L.page >= pages) L.page = pages - 1;
  main.textContent = '';
  const dirty = Object.values(S.files).filter((f) => f.kind.startsWith('locale/') && Ed.isDirty(f));
  countEl = h('span', { class: 'count' }, list.length + ' entries');
  main.append(h('div', { class: 'rec-head' }, h('h2', {}, 'Locale: en / ' + L.lang), countEl, h('span', { class: 'grow' }),
    h('button', { onclick: addKey }, '+ New key'),
    (saveBtn = h('button', { class: 'primary', disabled: !dirty.length, onclick: async () => { for (const f of dirtyLoc()) await Ed.saveFile(Ed.fkey(f.kind, f.file)); await fetchStatus(); Ed.renderList(); Ed.renderLocaleMain($('main')); } }, 'Save locale files' + (dirty.length ? ' (' + dirty.length + ')' : '')))));
  if (L.st && L.st.report) main.append(h('details', {}, h('summary', {}, 'Report of --check-locale (same logic)'), h('pre', { class: 'json mono' }, L.st.report)));
  if (!list.length) { main.append(h('div', { class: 'empty' }, L.filter === 'missing' ? 'Nothing is missing for ' + L.lang + '.' : 'No entries match.')); return; }
  const t = h('table', { class: 'loc-table' }, h('tr', {}, h('th', {}, 'Key'), h('th', {}, 'en (source)'), h('th', {}, L.lang)));
  list.slice(L.page * PAGE, (L.page + 1) * PAGE).forEach((e) => {
    const st = effStatus(e, L.lang);
    const enHas = e.en !== undefined || findFile('en', e.key);
    const enCell = enHas ? mkArea('en', e) : h('div', { class: 'ro' }, e.dataText !== undefined ? e.dataText : '(none)');
    const tr = h('tr', { class: st }, h('td', { class: 'key' }, e.key, h('div', {}, h('span', { class: 'badge ' + (st === 'missing' ? 'warn' : st === 'placeholders' ? 'err' : 'hand') }, st), ' ', h('span', { class: 'count' }, e.group))),
      h('td', {}, enCell, enHas && e.dataText !== undefined ? h('div', { class: 'count' }, 'data text: ' + e.dataText) : null),
      h('td', { class: 'tr' }, mkArea(L.lang, e), st === 'placeholders' ? h('div', { class: 'fieldmsg' }, 'placeholders differ from en (e.g. {name})') : null));
    t.append(tr);
  });
  main.append(t);
  if (pages > 1) main.append(h('div', { class: 'line', style: { margin: '10px 0', display: 'flex', gap: '8px', alignItems: 'center' } },
    h('button', { disabled: L.page === 0, onclick: () => { L.page--; Ed.renderLocaleMain($('main')); } }, 'Prev'), 'page ' + (L.page + 1) + ' / ' + pages,
    h('button', { disabled: L.page >= pages - 1, onclick: () => { L.page++; Ed.renderLocaleMain($('main')); } }, 'Next')));
};
function mkArea(lang, e) {
  const v = textOf(lang, e);
  const ta = h('textarea', { value: v === undefined ? '' : v, spellcheck: 'false', placeholder: lang === L.lang && v === undefined ? (e.dataText !== undefined ? e.dataText : '') : '' });
  const fit = () => { ta.style.height = '28px'; ta.style.height = Math.min(220, Math.max(28, ta.scrollHeight)) + 'px'; };
  ta.addEventListener('input', () => { fit(); setText(lang, e, ta.value); });
  setTimeout(fit, 0);
  return ta;
}
function addKey() {
  const langs = (L.st && L.st.langs) || ['en'];
  const key = h('input', { type: 'text', placeholder: 'namespace.key (e.g. ui.menu.start)' });
  const areas = langs.map((l) => ({ l, ta: h('textarea', { rows: 2, placeholder: l }) }));
  Ed.modal('New locale key', h('div', { class: 'form' }, Ed.row('Key', key), areas.map((a) => Ed.row(a.l, a.ta))), [{ label: 'Cancel' }, { label: 'Add', primary: true, onclick: async () => {
    const k = key.value.trim();
    if (!k || k.length > 200) { Ed.toast('Enter a key', 'err'); return true; }
    if ((L.st.entries || []).some((e) => e.key === k)) { Ed.toast('Key exists', 'err'); return true; }
    const e = { key: k, group: k.split('.')[0], tr: {} };
    for (const a of areas) if (a.ta.value) { const f = await ensureLangFile(a.l, e); Ed.mutate(Ed.fkey(f.kind, f.file), (d) => { d[k] = a.ta.value; }, {}); }
    L.st.entries.push(Object.assign(e, { tr: Object.fromEntries(langs.filter((x) => x !== 'en').map((x) => [x, { status: 'ok' }])) }));
    L.st.entries.sort((a, b) => (a.key < b.key ? -1 : 1));
    L.q = k; Ed.renderList(); Ed.renderLocaleMain($('main'));
    return false;
  } }]);
}
})();
