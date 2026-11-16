'use strict';
/* "Object scripts" editor (loaded after dialog.js; see docs/object-scripts.md and docs/editor.md section 7).
 *
 * Replaces the generic record form for the kind "objects": identity (map + object picker), presentation, flags, the Lua script
 * (status + create from template), and the declarative step lists (on_use / on_look / per skill). Conditions reuse the dialog
 * condition builder (Ed.renderCondition). The object picker is fed by GET /api/objects (the running server's loaded maps).
 */
(function () {
const Ed = window.Ed, S = Ed.S, h = Ed.h, clone = Ed.clone, lower = Ed.lower;
const sch = () => S.schema;
const stepsSch = () => (sch() && sch().steps) || { kinds: [], maxItems: 24 };
const lst = (e) => (typeof e === 'string' ? sch().enums[e.slice(1)] : e) || [];
const NS = 'http://www.w3.org/2000/svg';

// ------------------------------------------------------------------------------------------------------------------ record access
const curRec = () => Ed.curRec();
function setField(key, val, full) {   // val undefined removes the field
  const sel = S.sel; if (!sel) return;
  Ed.mutate(sel.key, (d) => {
    const r = d.records[sel.idx];
    if (val === undefined || val === '' || val === null) delete r[key]; else r[key] = val;
  }, { co: 'obj.' + key, full: !!full });
}
function frow(key, label, ctl, help, req) {
  return h('div', { class: 'frow', 'data-path': key },
    h('label', { title: key }, label, req ? h('span', { class: 'req' }, ' *') : null),
    h('div', { class: 'ctl' }, ctl, help ? h('div', { class: 'fieldhelp' }, help) : null, h('div', { class: 'fieldmsg', 'data-msg': '1' })));
}
function textInput(key, rec, opt) {
  opt = opt || {};
  const i = h('input', { type: 'text', value: rec[key] === undefined ? '' : String(rec[key]), list: opt.list, placeholder: opt.ph || '', style: { width: opt.w || '260px' } });
  i.addEventListener('change', () => { setField(key, i.value.trim()); if (opt.after) opt.after(); });
  return i;
}

// ------------------------------------------------------------------------------------------------------------------ object picker (server endpoint)
const pk = { open: false, maps: null, map: '', q: '', data: null, busy: false, err: '', seq: 0 };
const mapCache = {};   // map name -> object list (for door targets)
async function loadMaps() {
  const r = await Ed.api('GET', 'objects');
  pk.err = '';
  if (!r.ok) { pk.err = r.j.error || ('HTTP ' + r.status); pk.maps = []; return; }
  pk.maps = r.j.maps || [];
}
async function loadObjects() {
  if (!pk.map) { pk.data = null; return; }
  const my = ++pk.seq;
  pk.busy = true;
  const r = await Ed.api('GET', 'objects', undefined, { map: pk.map, q: pk.q, limit: 1500 });
  if (my !== pk.seq) return;
  pk.busy = false; pk.err = '';
  if (!r.ok) { pk.err = r.j.error || ('HTTP ' + r.status); pk.data = null; return; }
  pk.data = r.j;
  if (!pk.q) mapCache[pk.map] = r.j.objects || [];
}
async function fetchMapObjects(map) {   // for the door datalist, independent of the picker's search
  if (!map || mapCache[map]) return mapCache[map] || [];
  const r = await Ed.api('GET', 'objects', undefined, { map, q: '', limit: 1500 });
  mapCache[map] = r.ok ? (r.j.objects || []) : [];
  return mapCache[map];
}
function pick(o) {
  const sel = S.sel; if (!sel) return;
  Ed.mutate(sel.key, (d) => {
    const r = d.records[sel.idx];
    r.map = pk.map;
    if (o.name) { r.object = o.name; delete r.proto; delete r.cell; }
    else { delete r.object; r.proto = o.proto; r.cell = [o.x, o.y]; }
  }, { full: true });
  Ed.toast('Picked ' + (o.name || (o.proto + ' @ ' + o.x + ',' + o.y)), 'ok');
}
const KIND_COLOR = { door: 'var(--base)', container: 'var(--warn)', usable: 'var(--accent)', other: 'var(--muted)' };
function isCurrent(o, rec) {
  if (rec.map !== pk.map) return false;
  if (rec.object) return lower(rec.object) === lower(o.name);
  if (rec.proto && Array.isArray(rec.cell)) return lower(rec.proto) === lower(o.proto) && rec.cell[0] === o.x && rec.cell[1] === o.y;
  return false;
}
function scatter(data, rec) {
  const W = data.width || 200, H = data.height || 200;
  const el = (t, at, txt) => { const e = document.createElementNS(NS, t); for (const k in at) e.setAttribute(k, at[k]); if (txt !== undefined) e.textContent = txt; return e; };
  const svg = el('svg', { viewBox: '0 0 ' + W + ' ' + H, class: 'scatter', role: 'img', 'aria-label': 'map objects' });
  svg.append(el('rect', { x: 0, y: 0, width: W, height: H, class: 'sc-bg' }));
  for (let g = 50; g < W; g += 50) svg.append(el('line', { x1: g, y1: 0, x2: g, y2: H, class: 'sc-grid' }));
  for (let g = 50; g < H; g += 50) svg.append(el('line', { x1: 0, y1: g, x2: W, y2: g, class: 'sc-grid' }));
  const r = Math.max(W, H) / 110;
  const objs = data.objects || [];
  objs.forEach((o) => {
    const cur = isCurrent(o, rec);
    const c = el('circle', { cx: o.x, cy: o.y, r: cur ? r * 1.9 : r * 1.25, class: 'sc-dot' + (cur ? ' cur' : '') + (o.binding ? ' bound' : ''), style: 'fill:' + (KIND_COLOR[o.kind] || KIND_COLOR.other) });
    c.append(el('title', {}, (o.name || '(no instance name)') + ' | ' + (o.display || o.proto) + ' | ' + o.kind + ' | ' + o.x + ',' + o.y + (o.binding ? ' | bound: ' + o.binding : '')));
    c.addEventListener('click', () => pick(o));
    svg.append(c);
  });
  return svg;
}
function pickerView(host, rec) {
  host.textContent = '';
  if (pk.maps === null) { host.append(h('div', { class: 'empty' }, 'Loading maps...')); return; }
  if (pk.err && !pk.maps.length) { host.append(h('div', { class: 'banner err' }, 'The object list needs the running server: ' + pk.err)); return; }
  const mapSel = h('select', { id: 'pkMap' }, pk.maps.map((m) => h('option', { value: m.name, selected: m.name === pk.map }, m.name + ' (' + m.objects + ')')));
  mapSel.addEventListener('change', async () => { pk.map = mapSel.value; pk.data = null; pickerView(host, rec); await loadObjects(); pickerView(host, curRec() || rec); });
  const q = h('input', { type: 'search', id: 'pkQ', placeholder: 'Search id, name, model, position', value: pk.q, style: { width: '260px' } });
  let timer = null;
  q.addEventListener('input', () => { pk.q = q.value; clearTimeout(timer); timer = setTimeout(async () => { await loadObjects(); const keep = document.getElementById('pkQ'); pickerView(host, curRec() || rec); const nq = document.getElementById('pkQ'); if (nq && keep) { nq.focus(); nq.setSelectionRange(nq.value.length, nq.value.length); } }, 250); });
  host.append(h('div', { class: 'fx' }, 'Map', mapSel, q, h('span', { class: 'count' }, pk.busy ? 'loading...' : pk.data ? pk.data.total + ' object(s)' : ''),
    h('button', { class: 'small', onclick: async () => { pk.maps = null; pickerView(host, rec); await loadMaps(); await loadObjects(); pickerView(host, curRec() || rec); } }, 'Refresh')));
  if (pk.err) host.append(h('div', { class: 'banner err' }, pk.err));
  if (!pk.data) return;
  const d = pk.data;
  const legend = h('div', { class: 'fx legend' }, Object.keys(KIND_COLOR).map((k) => h('span', { class: 'count' }, h('span', { class: 'sw', style: { background: KIND_COLOR[k] } }), k)),
    h('span', { class: 'count' }, 'ring = already bound; click a dot or a row to pick'));
  const table = h('table', { class: 'diff-table pk-table' }, h('tr', {}, ['Id', 'Name', 'Model / type', 'Kind', 'Position', 'Bound'].map((t) => h('th', {}, t))));
  (d.objects || []).slice(0, 300).forEach((o) => {
    const cur = isCurrent(o, rec);
    table.append(h('tr', { class: 'pk-row' + (cur ? ' sel' : ''), onclick: () => pick(o) },
      h('td', { class: 'mono' }, String(o.id)), h('td', { class: 'mono' }, o.name || '(none)', o.display && o.display !== o.name ? h('div', { class: 'count' }, o.display) : null),
      h('td', { class: 'mono' }, o.model || o.proto || ''), h('td', {}, o.kind), h('td', { class: 'mono' }, o.x + ', ' + o.y), h('td', {}, o.binding || '')));
  });
  const more = (d.objects || []).length > 300 ? h('div', { class: 'count' }, 'Showing 300 of ' + d.objects.length + ' in the table (the plot shows all). Narrow the search.') : null;
  const unres = (d.unresolved || []).length ? h('div', { class: 'banner err' }, 'Bindings of this map that match no object: ' + d.unresolved.join(', ')) : null;
  host.append(legend, h('div', { class: 'pk-body' }, h('div', { class: 'pk-plot' }, scatter(d, rec)), h('div', { class: 'pk-list' }, table, more)), unres || '');
}
function renderPicker(rec) {
  const wrap = h('div', { class: 'picker' });
  const body = h('div', { id: 'pkBody' });
  wrap.append(h('button', { onclick: () => { pk.open = !pk.open; Ed.renderMain(); } }, pk.open ? 'Hide object picker' : 'Pick object from the map...'));
  if (!pk.open) return wrap;
  wrap.append(body);
  const refresh = () => { const b = document.getElementById('pkBody'); if (b) pickerView(b, curRec() || rec); };
  const has = (m) => (pk.maps || []).some((x) => x.name === m);
  (async () => {
    if (pk.maps === null) { await loadMaps(); }
    if (!pk.map || (rec.map && has(rec.map) && rec.map !== pk.map && !pk.data)) pk.map = rec.map && has(rec.map) ? rec.map : (pk.maps[0] ? pk.maps[0].name : '');
    if (!pk.data && pk.map) await loadObjects();
    refresh();
  })();
  pickerView(body, rec);
  return wrap;
}

// ------------------------------------------------------------------------------------------------------------------ Lua script status
let scriptTimer = null;
function renderScript(rec) {
  const box = h('div', { class: 'scriptbox' });
  const status = h('div', { class: 'script-status', id: 'scriptStatus' });
  const inp = h('input', { type: 'text', value: rec.script || '', placeholder: 'objects/' + (rec.id || 'name') + '.lua', style: { width: '300px' }, id: 'scriptPath' });
  inp.addEventListener('change', () => { setField('script', inp.value.trim()); check(); });
  async function check() {
    const p = (curRec() || rec).script;
    status.textContent = '';
    if (!p) { status.append(h('span', { class: 'count' }, 'No script: only the declarative steps run.')); return; }
    const r = await Ed.api('GET', 'objscript', undefined, { path: p });
    status.textContent = '';
    if (!r.ok) { status.append(h('span', { class: 'bad' }, '✗ ' + (r.j.error || 'cannot check')), ' '); return; }
    const j = r.j;
    if (!j.exists) {
      status.append(h('span', { class: 'bad' }, '✗ file does not exist'), ' ', h('span', { class: 'count' }, 'scripts/' + p));
      return;
    }
    if (!j.valid) status.append(h('span', { class: 'bad' }, '✗ does not load: ' + (j.error || '')));
    else status.append(h('span', { class: 'good' }, '✓ file exists and loads'), ' ', h('span', { class: 'count' }, 'hooks: ' + ((j.hooks || []).join(', ') || 'none')));
    if (j.warning) status.append(h('div', { class: 'fieldmsg' }, 'warning: ' + j.warning));
    if (j.abs) status.append(h('div', { class: 'count' }, j.abs, ' ', j.vscode ? h('a', { href: j.vscode, title: 'Open in VS Code' }, 'open in VS Code') : null));
  }
  const create = h('button', { onclick: async () => {
    const r0 = curRec() || rec;
    const p = (r0.script || '').trim() || ('objects/' + r0.id + '.lua');
    if (!r0.script) setField('script', p);
    const r = await Ed.api('POST', 'objscript/create', { path: p });
    if (!r.ok) { Ed.toast('Cannot create: ' + (r.j.error || r.status), 'err'); return; }
    Ed.toast(r.j.created ? 'Created scripts/' + p + ' from the template' : 'scripts/' + p + ' already exists (not overwritten)', 'ok');
    Ed.renderMain();
  } }, 'Create Lua script from template');
  box.append(h('div', { class: 'fx' }, inp, create), status);
  clearTimeout(scriptTimer); scriptTimer = setTimeout(check, 0);
  return box;
}

// ------------------------------------------------------------------------------------------------------------------ steps
function stepKind(step) { return stepsSch().kinds.find((k) => step && step[k.id] !== undefined) || null; }
function defaultStep(id) {
  const k = stepsSch().kinds.find((x) => x.id === id), s = {};
  for (const f of k.fields) {
    if (f.type === 'const') s[f.key] = f.value;
    else if (f.key === id) {
      if (f.type === 'enum') s[f.key] = lst(f.enum)[0];
      else if (f.type === 'doortarget' || f.type === 'string' || f.type === 'text') s[f.key] = f.ref === 'dialog' ? ((S.refs && S.refs.dialogs && S.refs.dialogs[0]) || '') : f.ref === 'map' ? ((S.refs && S.refs.maps && S.refs.maps[0]) || '') : '';
      else if (f.type === 'int') s[f.key] = f.min || 1;
    } else if (f.type === 'enum' && f.required) s[f.key] = lst(f.enum)[0];
    else if (f.type === 'steps') { /* optional */ }
  }
  if (id === 'flag') { s.flag = 'flag_name'; s.set = true; }
  if (id === 'skill') { s.difficulty = 0; s.success = []; s.failure = []; }
  if (id === 'message') s.message = 'Text';
  if (id === 'call') s.call = 'lua_function';
  if (id === 'play') s.play = 'effect';
  return s;
}
function renderSteps(arr, onChange, depth, mapName) {
  const wrap = h('div', { class: 'steps depth' + depth });
  const kinds = stepsSch().kinds;
  arr.forEach((step, i) => {
    const k = stepKind(step);
    const card = h('div', { class: 'stepcard', 'data-step': i });
    const set = (mut) => { const na = clone(arr); mut(na[i]); onChange(na); };
    const kindSel = h('select', {}, kinds.map((x) => h('option', { value: x.id, selected: k && x.id === k.id }, x.id)));
    if (!k) kindSel.prepend(h('option', { value: '', selected: true }, '(unknown)'));
    kindSel.addEventListener('change', () => { const na = clone(arr); const nd = defaultStep(kindSel.value); if (step['if'] !== undefined) nd['if'] = step['if']; na[i] = nd; onChange(na); });
    const head = h('div', { class: 'fx' }, h('span', { class: 'count' }, '#' + (i + 1)), kindSel,
      k && k.help ? h('span', { class: 'count' }, k.help) : null, h('span', { class: 'grow' }),
      h('button', { class: 'small', title: 'Move up', disabled: i === 0, onclick: () => { const na = clone(arr); na.splice(i - 1, 0, na.splice(i, 1)[0]); onChange(na); } }, '↑'),
      h('button', { class: 'small', title: 'Move down', disabled: i === arr.length - 1, onclick: () => { const na = clone(arr); na.splice(i + 1, 0, na.splice(i, 1)[0]); onChange(na); } }, '↓'),
      h('button', { class: 'small', title: 'Remove', onclick: () => { const na = clone(arr); na.splice(i, 1); onChange(na); } }, '✕'));
    card.append(head);
    if (!k) card.append(h('div', { class: 'fieldmsg' }, 'Step not understood by the builder; use the JSON tab.'));
    else {
      const line = h('div', { class: 'fx' });
      const nested = [];
      for (const f of k.fields) {
        const v = step[f.key];
        if (f.type === 'const') continue;
        if (f.type === 'steps') { nested.push(f); continue; }
        const lab = f.label || f.key;
        if (f.type === 'enum') {
          const s = h('select', {}, lst(f.enum).map((o) => h('option', { value: o, selected: lower(o) === lower(v) }, o)));
          s.addEventListener('change', () => set((x) => { x[f.key] = s.value; }));
          line.append(f.key === k.id ? '' : lab, s);
        } else if (f.type === 'int') {
          const n = h('input', { type: 'number', step: 1, min: f.min, max: f.max, value: v === undefined ? '' : v, placeholder: f.default !== undefined ? String(f.default) : '' });
          n.addEventListener('change', () => set((x) => { if (n.value === '') delete x[f.key]; else x[f.key] = Number(n.value); }));
          line.append(lab, n);
        } else if (f.type === 'bool') {
          const c = h('input', { type: 'checkbox', checked: !!v });
          c.addEventListener('change', () => set((x) => { if (c.checked) x[f.key] = true; else delete x[f.key]; }));
          line.append(h('label', { class: 'inl' }, c, ' ', lab));
        } else if (f.type === 'cell') {
          const a = Array.isArray(v) ? v : [0, 0];
          const mk = (j) => { const n = h('input', { type: 'number', min: 0, step: 1, value: a[j], style: { width: '70px' } }); n.addEventListener('change', () => set((x) => { const c = Array.isArray(x[f.key]) ? x[f.key] : [0, 0]; c[j] = Number(n.value || 0); x[f.key] = c; })); return n; };
          line.append(f.key === 'cell' ? 'cell' : f.key, 'x', mk(0), 'y', mk(1));
          if (!f.required) line.append(h('button', { class: 'small', title: 'Remove', onclick: () => set((x) => { delete x[f.key]; }) }, '✕'));
        } else if (f.type === 'flagvalue') {
          const s = h('select', {}, h('option', { value: 'true' }, 'set'), h('option', { value: 'false' }, 'clear'), h('option', { value: 'n' }, 'number...'));
          const isNum = typeof v === 'number';
          s.value = isNum ? 'n' : v === false ? 'false' : 'true';
          s.addEventListener('change', () => set((x) => { if (s.value === 'n') x[f.key] = 1; else x[f.key] = s.value === 'true'; }));
          line.append(s);
          if (isNum) { const n = h('input', { type: 'number', step: 1, value: v }); n.addEventListener('change', () => set((x) => { x[f.key] = Number(n.value); })); line.append(n); }
        } else if (f.type === 'doortarget') {
          const dlid = 'dl_doors_' + (mapName || '');
          const t = h('input', { type: 'text', value: Array.isArray(v) ? v.join(', ') : (v === undefined ? '' : String(v)), list: dlid, placeholder: 'instance name or x, y', style: { width: '220px' } });
          t.addEventListener('change', () => set((x) => { const m = /^\s*(\d+)\s*[, ]\s*(\d+)\s*$/.exec(t.value); x[f.key] = m ? [Number(m[1]), Number(m[2])] : t.value.trim(); }));
          line.append('door', t);
          ensureDoorList(dlid, mapName);
        } else {
          const dl = f.ref ? Ed.ensureDatalist(f.ref) : null;
          const t = h('input', { type: 'text', value: v === undefined ? '' : v, list: dl, placeholder: f.ref || f.key, style: { width: f.type === 'text' ? '320px' : '180px' } });
          t.addEventListener('change', () => set((x) => { if (t.value.trim() === '' && !f.required) delete x[f.key]; else x[f.key] = t.value.trim(); }));
          line.append(f.key === k.id ? '' : lab, t);
        }
      }
      card.append(line);
      if (depth < 4) card.append(h('div', { class: 'fx' }, h('span', { class: 'count' }, 'only if'), Ed.renderCondition(step['if'], (nv) => set((x) => { if (nv === undefined) delete x['if']; else x['if'] = nv; }))));
      for (const f of nested) {
        card.append(h('div', { class: 'nest' }, h('div', { class: 'count' }, f.key === 'success' ? 'on success' : f.key === 'failure' ? 'on failure' : f.key),
          renderSteps(Array.isArray(step[f.key]) ? step[f.key] : [], (na) => set((x) => { if (na.length) x[f.key] = na; else delete x[f.key]; }), depth + 1, mapName)));
      }
    }
    wrap.append(card);
  });
  const add = h('select', {}, h('option', { value: '' }, '+ add step...'), kinds.map((x) => h('option', { value: x.id }, x.id)));
  add.disabled = arr.length >= stepsSch().maxItems;
  add.addEventListener('change', () => { if (add.value) onChange(clone(arr).concat([defaultStep(add.value)])); });
  wrap.append(add);
  return wrap;
}
function ensureDoorList(id, map) {
  let dl = document.getElementById(id);
  if (!dl) { dl = h('datalist', { id }); document.body.append(dl); }
  fetchMapObjects(map).then((objs) => {
    dl.textContent = '';
    for (const o of objs) if (o.name && (o.kind === 'door' || o.kind === 'container')) dl.append(h('option', { value: o.name }, (o.display || '') + ' @ ' + o.x + ',' + o.y));
  });
}
function stepsField(key, label, rec, help) {
  const arr = Array.isArray(rec[key]) ? rec[key] : [];
  return frow(key, label, renderSteps(arr, (na) => setField(key, na.length ? na : undefined, true), 0, rec.map), help);
}
function skillsField(rec) {
  const m = rec.skills && typeof rec.skills === 'object' ? rec.skills : {};
  const wrap = h('div', {});
  const used = Object.keys(m);
  used.forEach((sk) => {
    const k = h('div', { class: 'nest' }, h('div', { class: 'fx' }, h('b', {}, sk), h('span', { class: 'grow' }), h('button', { class: 'small', title: 'Remove', onclick: () => {
      const n = clone(m); delete n[sk]; setField('skills', Object.keys(n).length ? n : undefined, true);
    } }, '✕')),
    renderSteps(Array.isArray(m[sk]) ? m[sk] : [], (na) => { const n = clone(m); n[sk] = na; setField('skills', n, true); }, 0, rec.map));
    wrap.append(k);
  });
  const sel = h('select', {}, h('option', { value: '' }, '+ handle a skill...'), lst('$skills').filter((s) => !used.includes(s)).map((s) => h('option', { value: s }, s)));
  sel.addEventListener('change', () => { if (!sel.value) return; const n = clone(m); n[sel.value] = []; setField('skills', n, true); });
  wrap.append(sel);
  return frow('skills', 'On skill use', wrap, 'Steps run when the player uses that skill on the object (Science to hack a computer, Lockpick, Repair...). With a Lua on_skill hook that returns true these steps are skipped.');
}

// ------------------------------------------------------------------------------------------------------------------ the form
function renderObjectForm() {
  const rec = curRec();
  const form = h('div', { class: 'form', id: 'recForm' });
  form.append(h('div', { id: 'recProblems' }));
  if (!rec) return form;
  const mapDl = Ed.ensureDatalist('map');
  const cellIn = (j) => {
    const a = Array.isArray(rec.cell) ? rec.cell : [];
    const n = h('input', { type: 'number', min: 0, step: 1, value: a[j] === undefined ? '' : a[j], style: { width: '80px' } });
    n.addEventListener('change', () => {
      const c = Array.isArray(rec.cell) ? clone(rec.cell) : [0, 0];
      const other = Array.isArray(rec.cell) ? rec.cell[1 - j] : undefined;
      if (n.value === '' && other === undefined) { setField('cell', undefined); return; }
      c[j] = Number(n.value || 0); if (c[1 - j] === undefined) c[1 - j] = 0; setField('cell', c);
    });
    return n;
  };
  form.append(
    h('h4', {}, 'Which object'),
    frow('id', 'Id', textInput('id', rec), null, true),
    frow('map', 'Map', textInput('map', rec, { list: mapDl, ph: 'map name' }), null, true),
    frow('object', 'Object (instance name)', textInput('object', rec, { ph: 'e.g. Computer_000' }), 'The stable key: the instance name of the object on the map. The fallback below is used only when the object has no instance name.'),
    frow('proto', 'Fallback: prototype + cell', h('div', { class: 'fx' }, textInput('proto', rec, { w: '240px', ph: 'prototype name' }), 'x', cellIn(0), 'y', cellIn(1))),
    h('div', { class: 'frow' }, h('label', {}), h('div', { class: 'ctl' }, renderPicker(rec))),
    h('h4', {}, 'Presentation'),
    frow('label', 'Cursor label', textInput('label', rec, { w: '160px', ph: 'Use' }), 'Shown in the cursor hint, e.g. Use, Hack, Pull. Default: Use.'),
    frow('name', 'Name override', textInput('name', rec, { w: '320px' })),
    frow('desc', 'Description override', (() => { const t = h('textarea', { rows: 2, style: { width: '420px' } }); t.value = rec.desc || ''; t.addEventListener('change', () => setField('desc', t.value)); return t; })()),
    h('h4', {}, 'Behaviour'),
    frow('usable', 'Usable', (() => { const c = h('input', { type: 'checkbox', checked: rec.usable !== false }); c.addEventListener('change', () => setField('usable', c.checked ? undefined : false)); return c; })(), 'Off: the object cannot be used (looking still works).'),
    frow('one_shot', 'One shot', (() => {
      const s = h('select', {}, [['', 'no'], ['true', 'yes (world)'], ['world', 'world: once for everyone'], ['player', 'player: once per player']].map(([v, l]) => h('option', { value: v }, l)));
      s.value = rec.one_shot === true ? 'true' : rec.one_shot === undefined || rec.one_shot === false ? '' : String(rec.one_shot);
      s.addEventListener('change', () => setField('one_shot', s.value === '' ? undefined : s.value === 'true' ? true : s.value));
      return s;
    })(), 'The used mark is stored in the world state and survives a restart.'),
    frow('cooldown', 'Cooldown (s)', (() => { const n = h('input', { type: 'number', min: 0, max: 86400, step: 1, value: rec.cooldown === undefined ? '' : rec.cooldown, style: { width: '100px' } }); n.addEventListener('change', () => setField('cooldown', n.value === '' ? undefined : Number(n.value))); return n; })()),
    frow('range', 'Range (tiles)', (() => { const n = h('input', { type: 'number', min: 0, max: 50, step: 1, value: rec.range === undefined ? '' : rec.range, placeholder: '0', style: { width: '100px' } }); n.addEventListener('change', () => setField('range', n.value === '' ? undefined : Number(n.value))); return n; })(), '0 or empty: the normal door / use range.'),
    frow('hidden_until', 'Hidden until', Ed.renderCondition(rec.hidden_until, (nv) => setField('hidden_until', nv, true)), 'The object is invisible and unusable until the condition holds (flag:name, quest:id==completed, ...).'),
    h('h4', {}, 'Lua script'),
    frow('script', 'Script', renderScript(rec), 'A file below scripts/ (usually objects/<name>.lua) with on_use / on_skill / on_look hooks. It runs before the steps below; a hook that returns true skips them.'),
    h('h4', {}, 'Declarative steps'),
    stepsField('on_use', 'On use', rec, 'Run top to bottom; a dialog step hands over to the dialog; end stops the list.'),
    skillsField(rec),
    stepsField('on_look', 'On look', rec));
  return form;
}
Ed.renderObjectForm = renderObjectForm;
})();
