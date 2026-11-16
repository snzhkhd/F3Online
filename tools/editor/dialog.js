'use strict';
/* Dialog graph editor + condition / effect builders (loaded after app.js; see docs/editor.md). */
(function () {
const Ed = window.Ed, S = Ed.S, h = Ed.h, clone = Ed.clone, lower = Ed.lower;
const sch = () => S.schema;
const OPS = () => sch().conditions.ops;
const condKinds = () => sch().conditions.kinds;
const lst = (e) => (typeof e === 'string' ? sch().enums[e.slice(1)] : e) || [];

// ---------------------------------------------------------------------------------------------------------------- conditions
function parseCondStr(s) {
  const m = /^\s*([a-z]+)\s*:\s*(.*)$/.exec(s);
  if (!m) return null;
  const kind = m[1], rest = m[2];
  const k = condKinds().find((x) => x.id === kind);
  if (!k) return null;
  const om = /(>=|<=|==|!=|>|<|=)/.exec(rest);
  if (!om) return { kind, name: rest.trim(), op: '', value: '' };
  return { kind, name: rest.slice(0, om.index).trim(), op: om[1] === '=' ? '==' : om[1], value: rest.slice(om.index + om[1].length).trim() };
}
const buildCondStr = (c) => c.kind + ':' + c.name + (c.op ? c.op + c.value : '');

function renderCondition(val, onChange, opt) {
  opt = opt || {};
  const wrap = h('div', {});
  if (val === undefined || val === null) {
    wrap.append(h('button', { class: 'small', onclick: () => onChange('skill:' + sch().enums.skills[0] + '>=50') }, '+ Condition'));
    return wrap;
  }
  wrap.append(condNode(val, onChange, () => onChange(undefined), 0));
  return wrap;
}

function condNode(val, set, remove, depth) {
  const rm = h('button', { class: 'small', title: 'Remove', onclick: remove }, '✕');
  if (typeof val === 'string') {
    const c = parseCondStr(val);
    if (!c) {
      const inp = h('input', { type: 'text', value: val, style: { minWidth: '260px' } });
      inp.addEventListener('change', () => set(inp.value));
      return h('div', { class: 'fx' }, h('span', { class: 'count' }, 'raw'), inp, rm, h('span', { class: 'fieldmsg' }, 'not understood by the builder; the server validates it'));
    }
    const k = condKinds().find((x) => x.id === c.kind);
    const upd = () => set(buildCondStr(c));
    const kindSel = h('select', {}, condKinds().map((x) => h('option', { value: x.id, selected: x.id === c.kind }, x.id)));
    kindSel.addEventListener('change', () => {
      const nk = condKinds().find((x) => x.id === kindSel.value);
      c.kind = nk.id;
      c.name = nk.name.enum ? lst(nk.name.enum)[0] : '';
      c.op = nk.op === 'none' ? '' : nk.op === 'eq-ne' ? '==' : '>=';
      c.value = nk.value ? (nk.value.enum ? lst(nk.value.enum)[1] || lst(nk.value.enum)[0] : String(nk.value.min > 0 ? nk.value.min : 1)) : '';
      upd();
    });
    const els = [kindSel];
    if (k.name.enum) {
      const sel = h('select', {}, lst(k.name.enum).map((x) => h('option', { value: x, selected: lower(x) === lower(c.name) }, x)));
      sel.addEventListener('change', () => { c.name = sel.value; upd(); });
      els.push(sel);
    } else {
      const dl = k.name.ref ? Ed.ensureDatalist(k.name.ref) : null;
      const inp = h('input', { type: 'text', value: c.name, list: dl, placeholder: k.id === 'lua' ? 'lua_function' : k.name.ref || 'name', style: { width: '170px' } });
      inp.addEventListener('change', () => { c.name = inp.value.trim(); upd(); });
      els.push(inp);
      if (k.name.ref === 'item') els.push(h('span', { class: 'count' }, Ed.refName('item', c.name)));
    }
    if (k.op !== 'none') {
      const ops = k.op === 'eq-ne' ? ['==', '!='] : OPS().filter((o) => o !== '=');
      const opSel = h('select', {}, k.op === 'optional' ? h('option', { value: '' }, '(has any)') : null, ops.map((o) => h('option', { value: o, selected: o === c.op }, o)));
      opSel.value = c.op;
      opSel.addEventListener('change', () => { c.op = opSel.value; if (!c.op) c.value = ''; else if (c.value === '') c.value = '1'; upd(); });
      els.push(opSel);
      if (c.op) {
        if (k.value.enum) {
          const vs = h('select', {}, lst(k.value.enum).map((x) => h('option', { value: x, selected: lower(x) === lower(c.value) }, x)));
          vs.addEventListener('change', () => { c.value = vs.value; upd(); });
          els.push(vs);
        } else {
          const vi = h('input', { type: 'number', step: 1, min: k.value.min, max: k.value.max, value: c.value });
          vi.addEventListener('change', () => { c.value = vi.value; upd(); });
          els.push(vi);
        }
      }
    }
    return h('div', { class: 'fx' }, els, rm);
  }
  // groups: array = all of, {all}, {any}, {not}
  let type, kids;
  if (Array.isArray(val)) { type = 'all'; kids = val; }
  else if (val && typeof val === 'object' && val.all) { type = 'all'; kids = val.all; }
  else if (val && typeof val === 'object' && val.any) { type = 'any'; kids = val.any; }
  else if (val && typeof val === 'object' && val.not !== undefined) { type = 'not'; kids = [val.not]; }
  else return h('div', { class: 'fx' }, h('span', { class: 'fieldmsg' }, 'unsupported condition shape; use the JSON tab'), rm);
  const pack = (t, ks) => t === 'not' ? { not: ks[0] } : t === 'any' ? { any: ks } : ks;
  const comb = h('select', {}, [['all', 'all of'], ['any', 'any of'], ['not', 'not']].map(([v, l]) => h('option', { value: v, selected: v === type }, l)));
  comb.addEventListener('change', () => { const t = comb.value; set(pack(t, t === 'not' ? [kids[0] !== undefined ? kids[0] : 'flag:x'] : kids.length ? kids : ['flag:x'])); });
  const box = h('div', { class: 'cond group' }, h('div', { class: 'fx' }, comb, rm));
  kids.forEach((kid, i) => box.append(condNode(kid, (nv) => { const nk = kids.slice(); if (nv === undefined) nk.splice(i, 1); else nk[i] = nv; set(nk.length ? pack(type, nk) : undefined); },
    () => { const nk = kids.slice(); nk.splice(i, 1); set(nk.length ? pack(type, nk) : undefined); }, depth + 1)));
  if (type !== 'not' && depth < 5 && kids.length < sch().conditions.maxItems)
    box.append(h('div', { class: 'fx' }, h('button', { class: 'small', onclick: () => set(pack(type, kids.concat('flag:x'))) }, '+ condition'),
      h('button', { class: 'small', onclick: () => set(pack(type, kids.concat({ any: ['flag:x'] }))) }, '+ any of / not group')));
  return box;
}
Ed.renderCondition = renderCondition;

// ---------------------------------------------------------------------------------------------------------------- effects
function effectKind(e) { const k = sch().effects.kinds.find((x) => e && e[x.id] !== undefined); return k || null; }
function defaultEffect(id) {
  const k = sch().effects.kinds.find((x) => x.id === id), e = {};
  for (const f of k.fields) {
    if (f.type === 'const') e[f.key] = f.value;
    else if (f.type === 'cell') e[f.key] = [0, 0];
    else if (f.type === 'enum') e[f.key] = lst(f.enum)[0];
    else if (f.type === 'int') { if (f.required || f.default !== undefined) e[f.key] = f.default !== undefined ? f.default : f.min; }
    else if (f.required) e[f.key] = '';
  }
  return e;
}
function renderEffects(arr, onChange, opt) {
  const wrap = h('div', {});
  const max = sch().effects.maxItems;
  const kinds = sch().effects.kinds;
  arr.forEach((e, i) => {
    const k = effectKind(e);
    const upd = (mut) => { const na = clone(arr); mut(na[i]); onChange(na); };
    const row = h('div', { class: 'fx', 'data-path': 'fx' + i });
    const kindSel = h('select', {}, kinds.map((x) => h('option', { value: x.id, selected: k && x.id === k.id }, x.id)));
    if (!k) kindSel.prepend(h('option', { value: '', selected: true }, '(unknown)'));
    kindSel.addEventListener('change', () => { const na = clone(arr); na[i] = defaultEffect(kindSel.value); onChange(na); });
    row.append(kindSel);
    if (k) for (const f of k.fields) {
      const v = e[f.key];
      if (f.type === 'const') continue;
      if (f.type === 'cell') {
        const a = Array.isArray(v) ? v : [0, 0];
        const mk = (j) => { const n = h('input', { type: 'number', min: 0, step: 1, value: a[j], style: { width: '70px' } }); n.addEventListener('change', () => upd((x) => { const c = Array.isArray(x[f.key]) ? x[f.key] : [0, 0]; c[j] = Number(n.value || 0); x[f.key] = c; })); return n; };
        row.append('x', mk(0), 'y', mk(1));
      } else if (f.type === 'enum') {
        const s = h('select', {}, lst(f.enum).map((o) => h('option', { value: o, selected: lower(o) === lower(v) }, o)));
        s.addEventListener('change', () => upd((x) => { x[f.key] = s.value; }));
        row.append(f.key === 'state' || f.key === 'action' ? '' : f.key, s);
      } else if (f.type === 'int') {
        const n = h('input', { type: 'number', step: 1, min: f.min, max: f.max, value: v === undefined ? '' : v, placeholder: f.default !== undefined ? String(f.default) : '' });
        n.addEventListener('change', () => upd((x) => { if (n.value === '') delete x[f.key]; else x[f.key] = Number(n.value); }));
        row.append(f.key === 'count' || f.key === 'xp' ? (f.key === 'xp' ? 'xp' : 'x') : f.key, n);
      } else if (f.type === 'flagvalue') {
        const s = h('select', {}, h('option', { value: 'true' }, 'set'), h('option', { value: 'false' }, 'clear'), h('option', { value: 'n' }, 'number...'));
        const isNum = typeof v === 'number';
        s.value = isNum ? 'n' : v === false ? 'false' : 'true';
        s.addEventListener('change', () => upd((x) => { if (s.value === 'n') x[f.key] = 1; else x[f.key] = s.value === 'true'; }));
        row.append(s);
        if (isNum) { const n = h('input', { type: 'number', step: 1, value: v }); n.addEventListener('change', () => upd((x) => { x[f.key] = Number(n.value); })); row.append(n); }
      } else {
        const dl = f.ref ? Ed.ensureDatalist(f.ref) : null;
        const t = h('input', { type: 'text', value: v === undefined ? '' : v, list: dl, placeholder: f.key === 'lua' ? 'lua_function' : f.ref || f.key, style: { width: '170px' } });
        t.addEventListener('change', () => upd((x) => { x[f.key] = t.value.trim(); }));
        row.append(t);
        if (f.ref === 'item') row.append(h('span', { class: 'count' }, Ed.refName('item', v)));
      }
    }
    if (k && k.help) row.append(h('span', { class: 'count' }, k.help));
    row.append(h('button', { class: 'small', title: 'Move up', disabled: i === 0, onclick: () => { const na = clone(arr); na.splice(i - 1, 0, na.splice(i, 1)[0]); onChange(na); } }, '↑'),
      h('button', { class: 'small', title: 'Remove', onclick: () => { const na = clone(arr); na.splice(i, 1); onChange(na); } }, '✕'));
    wrap.append(row);
  });
  const add = h('select', {}, h('option', { value: '' }, '+ add effect...'), kinds.map((x) => h('option', { value: x.id }, x.id)));
  add.disabled = arr.length >= max;
  add.addEventListener('change', () => { if (!add.value) return; onChange(clone(arr).concat([defaultEffect(add.value)])); });
  wrap.append(add);
  return wrap;
}
Ed.renderEffects = renderEffects;

// ---------------------------------------------------------------------------------------------------------------- dialog analysis + graph
const isEnd = (g) => g === undefined || g === null || g === '' || lower(g) === 'end';
function routesOf(dlg) {
  if (Array.isArray(dlg.start)) return dlg.start.map((r) => ({ goto: r.goto, cond: r.if }));
  const nodes = dlg.nodes || [];
  if (typeof dlg.start === 'string') return [{ goto: dlg.start }];
  if (!nodes.length) return [];
  const st = nodes.find((n) => lower(n.id) === 'start');
  return [{ goto: (st || nodes[0]).id }];
}
function analyse(dlg) {
  const nodes = dlg.nodes || [], byId = new Map();
  nodes.forEach((n, i) => { if (n && n.id !== undefined) byId.set(lower(n.id), i); });
  const edges = [], issues = [];
  const starts = routesOf(dlg);
  starts.forEach((r) => { if (!isEnd(r.goto)) { if (byId.has(lower(r.goto))) edges.push({ from: -1, to: byId.get(lower(r.goto)), cond: !!r.cond, label: 'start' }); else issues.push({ t: 'e', msg: 'start route goes to unknown node "' + r.goto + '"' }); } });
  nodes.forEach((n, i) => {
    const ans = n.answers || [];
    if (!ans.length) issues.push({ t: 'w', node: i, msg: 'dead end: node "' + n.id + '" has no answers (the player cannot continue)' });
    ans.forEach((a, j) => {
      if (isEnd(a.goto)) edges.push({ from: i, to: -2, ans: j, cond: a.if !== undefined, label: a.id || '#' + (j + 1) });
      else if (byId.has(lower(a.goto))) edges.push({ from: i, to: byId.get(lower(a.goto)), ans: j, cond: a.if !== undefined, label: a.id || '#' + (j + 1) });
      else { edges.push({ from: i, to: -3, ans: j, bad: true, label: a.goto }); issues.push({ t: 'e', node: i, msg: 'answer ' + (a.id || '#' + (j + 1)) + ' of "' + n.id + '" goes to unknown node "' + a.goto + '"' }); }
    });
  });
  const seen = new Set(), depth = new Map();
  const q = [];
  edges.filter((e) => e.from === -1 && e.to >= 0).forEach((e) => { if (!seen.has(e.to)) { seen.add(e.to); depth.set(e.to, 0); q.push(e.to); } });
  while (q.length) { const x = q.shift(); edges.filter((e) => e.from === x && e.to >= 0).forEach((e) => { if (!seen.has(e.to)) { seen.add(e.to); depth.set(e.to, depth.get(x) + 1); q.push(e.to); } }); }
  const unreachable = new Set();
  nodes.forEach((n, i) => { if (!seen.has(i)) { unreachable.add(i); issues.push({ t: 'w', node: i, msg: 'unreachable: node "' + n.id + '" is not reachable from the start' }); } });
  const dead = new Set(issues.filter((x) => x.t === 'w' && x.msg.startsWith('dead end')).map((x) => x.node));
  // a node whose answers can never leave and never end the dialog is not possible here: every answer without goto ends it
  return { nodes, edges, issues, unreachable, dead, depth, starts };
}

function layout(a) {
  const W = 150, H = 38, GX = 70, GY = 18;
  const cols = [];
  const maxd = Math.max(0, ...Array.from(a.depth.values()));
  a.nodes.forEach((n, i) => { const d = a.depth.has(i) ? a.depth.get(i) : maxd + 1; (cols[d] = cols[d] || []).push(i); });
  const pos = new Map();
  let maxRows = 1;
  cols.forEach((c, x) => { if (!c) return; maxRows = Math.max(maxRows, c.length); c.forEach((i, r) => pos.set(i, { x: 70 + x * (W + GX), y: 20 + r * (H + GY) })); });
  const nCols = cols.length;
  const endPos = { x: 70 + nCols * (W + GX), y: 20 };
  return { W, H, pos, endPos, width: endPos.x + 90, height: Math.max(maxRows * (H + GY) + 50, 120) };
}

function drawGraph(svgHost, dlg, selIdx, onSelect) {
  const a = analyse(dlg), L = layout(a);
  const NS = 'http://www.w3.org/2000/svg';
  const el = (t, at, txt) => { const e = document.createElementNS(NS, t); for (const k in at) e.setAttribute(k, at[k]); if (txt !== undefined) e.textContent = txt; return e; };
  const svg = el('svg', { viewBox: '0 0 ' + L.width + ' ' + L.height, role: 'img', 'aria-label': 'dialog graph' });
  const defs = el('defs', {});
  for (const [id, cls] of [['arr', ''], ['arrbad', 'bad']]) {
    const m = el('marker', { id, viewBox: '0 0 8 8', refX: 7, refY: 4, markerWidth: 7, markerHeight: 7, orient: 'auto-start-reverse' });
    m.append(el('path', { d: 'M0,0 L8,4 L0,8 z', style: 'fill:' + (cls ? 'var(--err)' : 'var(--muted)') }));
    defs.append(m);
  }
  svg.append(defs);
  const startPos = { x: 8, y: 20 };
  svg.append(el('circle', { cx: 24, cy: 39, r: 9, style: 'fill:var(--ok)' }), el('text', { x: 24, y: 63, 'text-anchor': 'middle' }, 'start'));
  const anchorOut = (i) => i === -1 ? { x: 33, y: 39 } : { x: L.pos.get(i).x + L.W, y: L.pos.get(i).y + L.H / 2 };
  const anchorIn = (i) => i === -2 ? { x: L.endPos.x, y: L.endPos.y + 15 } : i === -3 ? { x: L.endPos.x, y: L.endPos.y + 70 } : { x: L.pos.get(i).x, y: L.pos.get(i).y + L.H / 2 };
  svg.append(el('rect', { class: 'endnode', x: L.endPos.x, y: L.endPos.y, width: 70, height: 30, rx: 15 }), el('text', { x: L.endPos.x + 35, y: L.endPos.y + 19, 'text-anchor': 'middle' }, 'END'));
  if (a.edges.some((e) => e.to === -3)) svg.append(el('rect', { class: 'endnode', x: L.endPos.x, y: L.endPos.y + 55, width: 70, height: 30, rx: 15, style: 'stroke:var(--err)' }), el('text', { x: L.endPos.x + 35, y: L.endPos.y + 74, 'text-anchor': 'middle' }, 'unknown'));
  const gE = el('g', {});
  svg.append(gE);
  const nodeCount = new Map();
  a.edges.forEach((e) => {
    const p = anchorOut(e.from), q = anchorIn(e.to);
    const back = e.to >= 0 && e.from >= 0 && L.pos.get(e.to).x <= L.pos.get(e.from).x;
    let d;
    if (back) { const off = 24 + (nodeCount.get(e.from) || 0) * 6; nodeCount.set(e.from, (nodeCount.get(e.from) || 0) + 1); const y = Math.max(p.y, q.y) + L.H / 2 + off; d = 'M' + p.x + ',' + p.y + ' C' + (p.x + 40) + ',' + y + ' ' + (q.x - 40) + ',' + y + ' ' + q.x + ',' + q.y; }
    else { const dx = Math.max(30, (q.x - p.x) / 2); d = 'M' + p.x + ',' + p.y + ' C' + (p.x + dx) + ',' + p.y + ' ' + (q.x - dx) + ',' + q.y + ' ' + q.x + ',' + q.y; }
    const path = el('path', { d, class: 'edge' + (e.bad ? ' bad' : '') + (e.cond ? ' cond' : ''), 'marker-end': 'url(#' + (e.bad ? 'arrbad' : 'arr') + ')' });
    path.append(el('title', {}, (e.from >= 0 ? dlg.nodes[e.from].id : 'start') + ' -> ' + (e.to >= 0 ? dlg.nodes[e.to].id : e.to === -2 ? 'END' : 'unknown "' + e.label + '"') + (e.cond ? ' (conditional)' : '') + (e.label && e.from >= 0 ? ' via ' + e.label : '')));
    gE.append(path);
  });
  a.nodes.forEach((n, i) => {
    const p = L.pos.get(i);
    const first = a.edges.some((e) => e.from === -1 && e.to === i);
    const g = el('g', { class: 'node' + (selIdx === i ? ' sel' : '') + (a.unreachable.has(i) ? ' unreach' : '') + (a.dead.has(i) ? ' dead' : '') + (first ? ' start' : ''), transform: 'translate(' + p.x + ',' + p.y + ')', tabindex: 0 });
    g.append(el('rect', { width: L.W, height: L.H }), el('text', { x: 8, y: 16 }, String(n.id).slice(0, 22)), el('text', { x: 8, y: 30, style: 'opacity:.6' }, ((n.text || (n.stf ? 'stf ' + n.stf : '')) + '').slice(0, 26)));
    g.append(el('title', {}, n.id + (a.unreachable.has(i) ? ' (unreachable)' : '') + (a.dead.has(i) ? ' (dead end)' : '')));
    g.addEventListener('click', () => onSelect(i));
    svg.append(g);
  });
  svgHost.textContent = '';
  svgHost.append(svg);
  return a;
}

// ---------------------------------------------------------------------------------------------------------------- editor
let selNode = 0, selKey = '';
const stfCache = new Map();
function stfPreview(id, into) {
  into.textContent = '';
  if (!id) return;
  if (!S.info.hasGamedata) { into.textContent = 'original text unavailable (no game data loaded)'; return; }
  const show = (t) => { into.textContent = t ? '"' + t + '"' : '(no such string)'; };
  if (stfCache.has(id)) { show(stfCache.get(id)); return; }
  Ed.api('GET', 'stf', undefined, { id: String(id) }).then((r) => { stfCache.set(id, r.ok ? r.j.text : ''); show(stfCache.get(id)); });
}
function stfField(getV, setV) {
  const inp = h('input', { type: 'number', min: 1, max: 1000000, step: 1, value: getV() === undefined ? '' : getV(), placeholder: 'original string id', style: { width: '130px' } });
  const prev = h('div', { class: 'ro', style: { display: getV() ? '' : 'none' } });
  const results = h('div', { class: 'nodelist', style: { display: 'none' } });
  const apply = () => { setV(inp.value === '' ? undefined : Number(inp.value)); prev.style.display = inp.value ? '' : 'none'; stfPreview(Number(inp.value), prev); };
  inp.addEventListener('change', apply);
  stfPreview(getV(), prev);
  const q = h('input', { type: 'search', placeholder: 'search original text...', style: { width: '170px' } });
  q.addEventListener('keydown', async (e) => {
    if (e.key !== 'Enter') return;
    const r = await Ed.api('GET', 'stf', undefined, { q: q.value });
    results.textContent = ''; results.style.display = '';
    (r.j.results || []).forEach((x) => results.append(h('div', { class: 'rrow', onclick: () => { inp.value = x.id; results.style.display = 'none'; apply(); } }, h('span', { class: 'id' }, x.id), h('span', { class: 'nm' }, x.text))));
    if (!(r.j.results || []).length) results.append(h('div', { class: 'empty' }, S.info.hasGamedata ? 'no match' : 'no game data'));
  });
  return h('div', {}, h('div', { class: 'line fx' }, inp, q, h('span', { class: 'count' }, 'Enter to search')), prev, results);
}

Ed.nodeDatalist = function () {
  let dl = document.getElementById('dl_nodes');
  if (!dl) { dl = h('datalist', { id: 'dl_nodes' }); document.body.append(dl); }
  dl.textContent = '';
  const rec = Ed.curRec();
  if (rec) (rec.nodes || []).forEach((n) => dl.append(h('option', { value: n.id })));
  dl.append(h('option', { value: 'end' }));
  return 'dl_nodes';
};

Ed.renderDlgStart = function (val, onChange) {
  const rec = Ed.curRec(), ids = ((rec && rec.nodes) || []).map((n) => n.id);
  const wrap = h('div', {});
  const mode = Array.isArray(val) ? 'routes' : 'node';
  const msel = h('select', {}, h('option', { value: 'default' }, 'default (node "start" or the first node)'), h('option', { value: 'node' }, 'a fixed node'), h('option', { value: 'routes' }, 'conditional routes'));
  msel.value = val === undefined ? 'default' : mode;
  msel.addEventListener('change', () => { if (msel.value === 'default') onChange(undefined); else if (msel.value === 'node') onChange(ids[0] || 'start'); else onChange([{ goto: ids[0] || 'start' }]); });
  wrap.append(msel);
  if (typeof val === 'string') {
    const s = h('select', {}, ids.map((i) => h('option', { value: i, selected: i === val }, i)));
    if (!ids.includes(val)) s.prepend(h('option', { value: val, selected: true }, val + ' (unknown)'));
    s.addEventListener('change', () => onChange(s.value));
    wrap.append(s);
  } else if (Array.isArray(val)) {
    val.forEach((r, i) => {
      const s = h('select', {}, ids.concat(['end']).map((x) => h('option', { value: x, selected: x === r.goto }, x)));
      if (r.goto !== undefined && !ids.concat(['end']).includes(r.goto)) s.prepend(h('option', { value: r.goto, selected: true }, r.goto + ' (unknown)'));
      s.addEventListener('change', () => { const n = clone(val); n[i].goto = s.value; onChange(n); });
      const box = h('div', { class: 'card' }, h('div', { class: 'card-head' }, 'Route ' + (i + 1) + (i === val.length - 1 ? ' (fallback when no earlier route matches)' : ''), h('span', { class: 'grow' }),
        h('button', { class: 'small', onclick: () => { const n = clone(val); n.splice(i, 1); onChange(n.length ? n : undefined); } }, '✕')),
      h('div', { class: 'fx' }, 'go to', s), h('div', {}, 'if', renderCondition(r.if, (v) => { const n = clone(val); if (v === undefined) delete n[i].if; else n[i].if = v; onChange(n); })));
      wrap.append(box);
    });
    wrap.append(h('button', { class: 'small', onclick: () => onChange(clone(val).concat([{ goto: ids[0] || 'start' }])) }, '+ Route'));
  }
  return wrap;
};

Ed.renderDialogEditor = function () {
  const key = S.sel.key, idx = S.sel.idx;
  const dlg = () => Ed.curRec();
  const tag = key + '#' + idx;
  if (selKey !== tag) { selKey = tag; selNode = 0; }
  const root = h('div', { class: 'dlg' });
  const left = h('div', {}), right = h('div', {});
  root.append(left, right);
  const rec = dlg();
  if (selNode >= (rec.nodes || []).length) selNode = Math.max(0, (rec.nodes || []).length - 1);
  const svgHost = h('div', {});
  const anaBox = h('div', { class: 'analysis' });
  const listBox = h('div', { class: 'nodelist' });
  const mut = (fn, full) => Ed.mutate(key, (d) => fn(d.records[idx]), { full: !!full });
  const mutCo = (fn, co) => Ed.mutate(key, (d) => fn(d.records[idx]), { co: 'dlg:' + tag + co });
  function select(i) { selNode = i; Ed.renderMain(); }
  function refreshLeft() {
    const r = dlg();
    const a = drawGraph(svgHost, r, selNode, select);
    anaBox.textContent = '';
    if (!a.issues.length) anaBox.append(h('div', { class: 'it ok' }, 'Checks passed: every node is reachable, no dead ends, all goto targets exist.'));
    a.issues.forEach((x) => anaBox.append(h('div', { class: 'it ' + x.t, onclick: () => { if (x.node !== undefined) select(x.node); } }, (x.t === 'e' ? 'error: ' : 'warning: ') + x.msg)));
    listBox.textContent = '';
    (r.nodes || []).forEach((n, i) => listBox.append(h('div', { class: 'rrow' + (i === selNode ? ' sel' : ''), onclick: () => select(i) },
      h('span', { class: 'id' }, n.id || '(no id)'), h('span', { class: 'nm' }, n.text || (n.stf ? 'stf ' + n.stf : '')),
      a.unreachable.has(i) ? h('span', { class: 'badge warn' }, 'unreachable') : null, a.dead.has(i) ? h('span', { class: 'badge err' }, 'dead end') : null,
      h('span', { class: 'count' }, ((n.answers || []).length) + ' ans'))));
  }
  Ed.onValidated = null;
  left.append(h('div', { class: 'fieldhelp' }, 'Graph: green = start, dashed = unreachable, red = dead end, blue edges = conditional. Click a node to edit it.'), svgHost, anaBox,
    h('div', { class: 'line fx' }, h('b', {}, 'Nodes'), h('button', { class: 'small', onclick: () => mut((r) => { r.nodes = r.nodes || []; let n = r.nodes.length + 1, id = 'node' + n; while (r.nodes.some((x) => x.id === id)) id = 'node' + (++n); r.nodes.push({ id, text: '', answers: [] }); selNode = r.nodes.length - 1; }, true) }, '+ Node')),
    listBox, h('div', { class: 'frow', style: { marginTop: '10px' } }, h('label', {}, 'Speaker'), h('div', { class: 'ctl' }, (() => {
      const i = h('input', { type: 'text', value: rec.speaker || '', list: Ed.ensureDatalist('npc') });
      i.addEventListener('change', () => mut((r) => { if (i.value.trim()) r.speaker = i.value.trim(); else delete r.speaker; }));
      return i;
    })())),
    h('div', { class: 'frow' }, h('label', {}, 'Start'), h('div', { class: 'ctl' }, Ed.renderDlgStart(rec.start, (v) => mut((r) => { if (v === undefined) delete r.start; else r.start = v; }, true)))));
  refreshLeft();

  // right: node editor
  const n = (rec.nodes || [])[selNode];
  if (!n) { right.append(h('div', { class: 'empty' }, 'No nodes. Add one with + Node.')); return root; }
  const np = (fn) => mut((r) => fn(r.nodes[selNode]), true);
  const npc = (fn, co) => mutCo((r) => fn(r.nodes[selNode]), co);
  right.append(h('div', { class: 'rec-head' }, h('h3', { style: { margin: 0 } }, 'Node: ' + n.id), h('span', { class: 'grow' }),
    h('button', { class: 'small', onclick: () => mut((r) => { r.nodes.splice(selNode + 1, 0, Object.assign(clone(r.nodes[selNode]), { id: r.nodes[selNode].id + '_copy' })); selNode++; }, true) }, 'Duplicate'),
    h('button', { class: 'small danger', onclick: () => mut((r) => { r.nodes.splice(selNode, 1); }, true) }, 'Delete')));
  const idIn = h('input', { type: 'text', value: n.id });
  idIn.addEventListener('change', () => {
    const old = n.id, nv = idIn.value.trim();
    if (!nv || nv === old) return;
    mut((r) => {   // renaming also repoints every goto / start that used the old id
      r.nodes[selNode].id = nv;
      const fix = (g) => lower(g) === lower(old) ? nv : g;
      r.nodes.forEach((x) => (x.answers || []).forEach((a) => { if (a.goto !== undefined) a.goto = fix(a.goto); }));
      if (typeof r.start === 'string') r.start = fix(r.start);
      if (Array.isArray(r.start)) r.start.forEach((x) => { x.goto = fix(x.goto); });
    }, true);
  });
  const text = h('textarea', { rows: 4, value: n.text || '', placeholder: n.stf ? '(using the original string below)' : 'What the NPC says' });
  text.addEventListener('input', () => { npc((x) => { if (text.value) x.text = text.value; else delete x.text; }, ':text'); refreshLeft(); });
  right.append(Ed.row('Node id', idIn), Ed.row('NPC text', text), Ed.row('Original text (stf)', stfField(() => n.stf, (v) => np((x) => { if (v === undefined) delete x.stf; else x.stf = v; }))),
    Ed.row('On enter', renderEffects(n.on_enter || [], (v) => np((x) => { if (v.length) x.on_enter = v; else delete x.on_enter; }))));
  right.append(h('h4', {}, 'Answers (' + (n.answers || []).length + ')'));
  (n.answers || []).forEach((a, j) => {
    const ap = (fn) => mut((r) => fn(r.nodes[selNode].answers[j]), true);
    const apc = (fn, co) => mutCo((r) => fn(r.nodes[selNode].answers[j]), ':a' + j + co);
    const at = h('textarea', { rows: 2, value: a.text || '', placeholder: a.stf ? '(using the original string below)' : 'What the player says' });
    at.addEventListener('input', () => { apc((x) => { if (at.value) x.text = at.value; else delete x.text; }, 'text'); refreshLeft(); });
    const gt = h('input', { type: 'text', list: Ed.nodeDatalist(), value: a.goto === undefined ? '' : a.goto, placeholder: '(empty = end the dialog)', style: { width: '200px' } });
    gt.addEventListener('change', () => ap((x) => { if (gt.value.trim()) x.goto = gt.value.trim(); else delete x.goto; }));
    const idA = h('input', { type: 'text', value: a.id || '', placeholder: 'optional', style: { width: '140px' } });
    idA.addEventListener('change', () => ap((x) => { if (idA.value.trim()) x.id = idA.value.trim(); else delete x.id; }));
    right.append(h('div', { class: 'card', 'data-path': 'nodes[' + selNode + '].answers[' + j + ']' },
      h('div', { class: 'card-head' }, 'Answer ' + (j + 1), h('span', { class: 'grow' }),
        h('button', { class: 'small', disabled: j === 0, title: 'Move up', onclick: () => mut((r) => { const L = r.nodes[selNode].answers; L.splice(j - 1, 0, L.splice(j, 1)[0]); }, true) }, '↑'),
        h('button', { class: 'small', disabled: j === (n.answers.length - 1), title: 'Move down', onclick: () => mut((r) => { const L = r.nodes[selNode].answers; L.splice(j + 1, 0, L.splice(j, 1)[0]); }, true) }, '↓'),
        h('button', { class: 'small', title: 'Remove', onclick: () => mut((r) => { r.nodes[selNode].answers.splice(j, 1); }, true) }, '✕')),
      h('div', { class: 'form' }, Ed.row('Id', idA), Ed.row('Player text', at), Ed.row('Original text (stf)', stfField(() => a.stf, (v) => ap((x) => { if (v === undefined) delete x.stf; else x.stf = v; }))),
        Ed.row('Shown if', renderCondition(a.if, (v) => ap((x) => { if (v === undefined) delete x.if; else x.if = v; }))),
        Ed.row('Then do', renderEffects(a.do || [], (v) => ap((x) => { if (v.length) x.do = v; else delete x.do; }))),
        Ed.row('Go to node', h('div', { class: 'line' }, gt, h('button', { class: 'small', onclick: () => { const t = (dlg().nodes || []).findIndex((x) => lower(x.id) === lower(a.goto)); if (t >= 0) select(t); } }, 'open'))))));
  });
  right.append(h('button', { class: 'small', disabled: (n.answers || []).length >= 24, onclick: () => np((x) => { x.answers = x.answers || []; x.answers.push({ text: '' }); }) }, '+ Answer'));
  return root;
};
})();
