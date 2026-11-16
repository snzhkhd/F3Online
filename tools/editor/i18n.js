'use strict';
/* UI language of the editor (docs/editor.md, "UI language").
 *
 * The editor's own texts (labels, buttons, messages, problem prefixes) are English in the code. This file holds the ui-strings table
 * of the other languages: the KEY IS THE ENGLISH TEXT, the value its translation. A small DOM translator (text nodes, title and
 * placeholder attributes, kept up to date by a MutationObserver) swaps the texts when the language changes, so the render code stays free of lookups and a
 * text without an entry simply stays English. Content (record ids, names, JSON, the text of data files) is never translated.
 *
 *   Ed.i18n.add('de', { 'Save': 'Speichern' }, [ [/^N files$/, (m) => ...] ])   // another language: a table + optional patterns
 *   Ed.T('Cannot load {file}', { file })                                       // for strings that are not shown through the DOM (rare)
 *
 * Choice: localStorage 'f3ed.lang', default = the browser language when a table for it exists, else en.
 */
(function () {
const Ed = window.Ed;
const LS = 'f3ed.lang';
const tables = { en: {} };       // lang -> { english text: translation }
const patterns = { en: [] };     // lang -> [ [RegExp, (match, tr) => string], ... ] for texts with numbers / names inside
const names = { en: 'English' };
let lang = 'en';

function add(code, table, pats, nm) {
  const t = tables[code] = tables[code] || {};
  for (const [k, v] of Object.entries(table || {})) t[k.trim()] = v;   // keys are looked up trimmed (the app puts blanks around fragments)
  patterns[code] = (patterns[code] || []).concat(pats || []);
  if (nm) names[code] = nm;
}

function tr(s) {   // translate one string for the current language; keeps leading / trailing blanks; unknown text is returned as is
  if (lang === 'en' || typeof s !== 'string') return s;
  const m = /^(\s*)([\s\S]*?)(\s*)$/.exec(s);
  const core = m[2];
  if (!core) return s;
  const t = tables[lang] && tables[lang][core];
  if (t !== undefined) return m[1] + t + m[3];
  for (const [re, fn] of patterns[lang] || []) {
    const mm = re.exec(core);
    if (mm) return m[1] + fn(mm, tr) + m[3];
  }
  return s;
}
function T(en, params) {
  let s = tr(en);
  if (params) s = s.replace(/\{(\w+)\}/g, (a, k) => (params[k] === undefined ? a : params[k]));
  return s;
}

// ---------------------------------------------------------------------------------------------------------------- DOM translator
const SKIP_TAGS = new Set(['SCRIPT', 'STYLE', 'TEXTAREA', 'INPUT', 'PRE', 'CODE', 'svg', 'SVG']);
const SKIP_CLASS = /(^|\s)(mono|id|nm|json)(\s|$)/;
const ATTRS = ['title', 'placeholder', 'aria-label'];
const textMemo = new WeakMap();   // text node -> { en, shown }
const attrMemo = new WeakMap();   // element -> { attr: { en, shown } }

function skipEl(el, forAttr) {   // forAttr: title / placeholder of an input are UI texts, the tag itself is not a reason to skip
  for (let e = el; e && e.nodeType === 1; e = e.parentNode) {
    if ((!forAttr && SKIP_TAGS.has(e.tagName)) || e.hasAttribute('data-notr') || (e.className && typeof e.className === 'string' && SKIP_CLASS.test(e.className))) return true;
  }
  return false;
}
function doText(n) {
  const m = textMemo.get(n);
  if (m && n.data === m.shown) {                 // ours and untouched: re-derive from the English original
    const next = tr(m.en);
    if (next !== n.data) { n.data = next; m.shown = next; }
    return;
  }
  if (!n.data.trim() || !n.parentNode || skipEl(n.parentNode)) return;
  const next = tr(n.data);                       // new text (or text the app rewrote)
  textMemo.set(n, { en: n.data, shown: next });
  if (next !== n.data) n.data = next;
}
function doAttrs(el) {
  if (el.nodeType !== 1 || skipEl(el, true)) return;
  let memo = attrMemo.get(el);
  for (const a of ATTRS) {
    if (!el.hasAttribute(a)) continue;
    const cur = el.getAttribute(a);
    const m = memo && memo[a];
    let en = cur;
    if (m && cur === m.shown) en = m.en;
    const next = tr(en);
    if (!memo) { memo = {}; attrMemo.set(el, memo); }
    memo[a] = { en, shown: next };
    if (next !== cur) el.setAttribute(a, next);
  }
}
function walk(root) {
  if (!root) return;
  if (root.nodeType === 3) { doText(root); return; }
  if (root.nodeType !== 1 && root.nodeType !== 9 && root.nodeType !== 11) return;
  if (root.nodeType === 1) { doAttrs(root); if (skipEl(root, false)) return; }
  const tw = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT);
  for (let n = tw.nextNode(); n; n = tw.nextNode()) {
    if (n.nodeType === 3) doText(n); else doAttrs(n);
  }
}
let obs = null;
function startObserver() {
  if (obs) return;
  obs = new MutationObserver((list) => {
    for (const r of list) {
      if (r.type === 'attributes') { doAttrs(r.target); continue; }
      r.addedNodes.forEach((n) => walk(n));
    }
  });
  obs.observe(document.documentElement, { childList: true, subtree: true, attributes: true, attributeFilter: ATTRS });
}
function retitle() { document.title = (document.title.startsWith('● ') ? '● ' : '') + T('F3 Content Editor'); }

function set(code) {
  if (!tables[code]) code = 'en';
  lang = code;
  try { localStorage.setItem(LS, code); } catch (e) { /* blocked storage: the choice only lives for this page */ }
  document.documentElement.setAttribute('lang', code);
  walk(document.body);
  retitle();
  const b = document.getElementById('btnLang');
  if (b) {
    const nx = next();
    b.textContent = nx.toUpperCase();
    b.title = Object.keys(tables).map((c) => names[c] || c).join(' / ');
  }
  if (Ed.onLangChange) Ed.onLangChange(code);
}
const list = () => Object.keys(tables);
function next() { const l = list(); return l[(l.indexOf(lang) + 1) % l.length]; }
function initial() {
  let c = null;
  try { c = localStorage.getItem(LS); } catch (e) { /* ignore */ }
  if (!c || !tables[c]) {
    const nav = ((navigator.languages && navigator.languages[0]) || navigator.language || 'en').toLowerCase().slice(0, 2);
    c = tables[nav] ? nav : 'en';
  }
  return c;
}
function init() {
  const b = document.getElementById('btnLang');
  if (b) b.onclick = () => set(next());
  startObserver();
  set(initial());
}

Ed.i18n = { add, tr, T, set, init, list, get lang() { return lang; } };
Ed.T = T;

// ---------------------------------------------------------------------------------------------------------------- Russian
const ru = {
  // header
  'F3 Content Editor': 'Редактор контента F3',
  'Files with unsaved changes': 'Файлы с несохранёнными изменениями',
  'Undo (Ctrl+Z)': 'Отменить (Ctrl+Z)', 'Undo': 'Отменить',
  'Redo (Ctrl+Y)': 'Повторить (Ctrl+Y)', 'Redo': 'Повторить',
  "Validate the whole data folder with the server's own validator": 'Проверить всю папку data валидатором самого сервера',
  'Validate': 'Проверить',
  'Save this file (Ctrl+S)': 'Сохранить этот файл (Ctrl+S)', 'Save': 'Сохранить',
  'Save every file with changes': 'Сохранить все изменённые файлы', 'Save all': 'Сохранить всё',
  'Make the running server re-read data/ (the same logic as /reload) and show the diff report': 'Заставить запущенный сервер перечитать data/ (та же логика, что у /reload) и показать отчёт',
  'Apply to running server': 'Применить на запущенном сервере',
  'Switch light / dark': 'Светлая / тёмная тема', 'Theme': 'Тема',
  'Loading...': 'Загрузка...',
  'Interface language': 'Язык интерфейса',
  // navigation
  'Content': 'Контент', 'Text': 'Текст', 'Planned': 'В планах', 'Scripts': 'Скрипты',
  'Locale (en / ru)': 'Локализация (en / ru)', 'Object scripts': 'Скрипты объектов', 'planned': 'в планах',
  'Item': 'Предмет', 'Perk': 'Перк', 'Trait': 'Черта', 'Skill ': 'Навык', 'Quest': 'Квест', 'Dialog': 'Диалог',
  'Items': 'Предметы', 'NPCs': 'NPC', 'Perks': 'Перки', 'Traits': 'Черты', 'Skills': 'Навыки', 'Quests': 'Квесты', 'Dialogs': 'Диалоги',
  // list
  'Search id or name': 'Поиск по id или имени', '+ New': '+ Создать',
  'any origin': 'любое происхождение', 'hand-authored': 'написано вручную', 'generated': 'сгенерировано', 'original base': 'оригинальная база',
  'any file': 'любой файл', 'all': 'все', 'weapons': 'оружие', 'armor': 'броня', 'ammo': 'патроны', 'kits': 'наборы', 'other': 'прочее',
  'show all': 'показать все', 'Files...': 'Файлы...', 'Files of this kind': 'Файлы этого вида',
  'unsaved changes': 'несохранённые изменения', '(no id)': '(нет id)', 'base': 'база', 'gen': 'ген', 'hand': 'рука',
  'No records yet.': 'Записей пока нет.',
  'No records. Enable "include original prototypes" to see the base layer.': 'Записей нет. Включите «показывать оригинальные прототипы», чтобы увидеть базовый слой.',
  'Select a record on the left, or create one with + New.': 'Выберите запись слева или создайте новую кнопкой «+ Создать».',
  'The record is gone.': 'Запись исчезла.',
  // record head, banners
  'overrides base': 'переопределяет базу', 'new (no base)': 'новая (без базы)',
  'Duplicate': 'Дублировать', 'Delete': 'Удалить',
  'Take over (remove "generator")': 'Забрать себе (убрать "generator")',
  'This file contains // or /* */ comments. They are not kept when the editor saves it (the formatted output is identical to the exporter\'s).': 'В файле есть комментарии // или /* */. При сохранении редактор их не сохраняет (формат вывода совпадает с экспортёром).',
  'Unsaved edits from an earlier session were restored.': 'Восстановлены несохранённые правки из прошлого сеанса.',
  'Form': 'Форма', 'Dialog graph': 'Граф диалога', 'Diff vs base': 'Отличия от базы', 'JSON': 'JSON', 'Parse': 'Разобрать',
  'Override in data/...': 'Переопределить в data/...',
  'This is an original prototype (read-only). Overriding writes a small record into a data file that changes only the fields you edit.': 'Это оригинальный прототип (только чтение). Переопределение создаёт в файле data небольшую запись, меняющую только изменённые вами поля.',
  'Base record unavailable.': 'Базовая запись недоступна.',
  // extension placeholders (kept for old servers)
  // dialogs: new record
  'Skill': 'Навык', 'Base': 'База', 'Id': 'Id', 'Save into': 'Сохранить в', '(new file...)': '(новый файл...)',
  'Cancel': 'Отмена', 'Create': 'Создать', 'Close': 'Закрыть', 'Name': 'Имя',
  'An id is required': 'Нужен id', 'Id already exists': 'Такой id уже есть',
  'File name: letters, digits, _ . - and .json': 'Имя файла: буквы, цифры, _ . - и .json', 'Invalid file name': 'Недопустимое имя файла',
  'clone this original (optional)': 'клонировать этот оригинал (необязательно)', 'new_id': 'новый_id',
  'Create override': 'Создать переопределение', 'Delete record': 'Удалить запись',
  'Delete file': 'Удалить файл', 'New file...': 'Новый файл...', 'Files: ': 'Файлы: ',
  'Skills are fixed (one record per skill)': 'Навыки фиксированы (одна запись на навык)',
  'File changed on disk': 'Файл изменился на диске', 'Generated file': 'Сгенерированный файл',
  'Unsaved changes': 'Несохранённые изменения', 'Apply anyway': 'Всё равно применить',
  'Reload from disk (lose my edits)': 'Перечитать с диска (потерять мои правки)', 'Overwrite the disk version': 'Перезаписать версию на диске',
  'Save, keep generator': 'Сохранить, оставить generator', 'Take over and save': 'Забрать себе и сохранить', 'Save as draft': 'Сохранить как черновик',
  'Take it over to turn it into a hand-authored file (the "generator" key is removed and the exporter will skip it).': 'Заберите файл себе, чтобы он стал рукописным (ключ "generator" удаляется, экспортёр его пропустит).',
  'Parsed. Review and save.': 'Разобрано. Проверьте и сохраните.',
  // form
  'unset': 'снять', 'remove': 'убрать', 'include original prototypes not in data/ (': 'показывать оригинальные прототипы, которых нет в data/ (', 'Remove this field from the record': 'Убрать это поле из записи', 'remove group': 'убрать группу', 'reset group to base': 'сбросить группу к базе',
  'differs from base': 'отличается от базы', 'Move up': 'Вверх', 'Move down': 'Вниз', 'Remove': 'Убрать', '+ Add': '+ Добавить',
  '+ Set list': '+ Задать список', '+ Set': '+ Задать', '+ Set effects': '+ Задать эффекты', '+ Add effect': '+ Добавить эффект',
  '(unset)': '(не задано)', '(edit in the JSON tab)': '(правьте на вкладке JSON)', '(empty list: explicitly none)': '(пустой список: явно ничего)',
  'reset to base': 'сбросить к базе', 'same as base': 'как в базе', ' (override is redundant) ': ' (переопределение лишнее) ',
  'not in base (added by this record) ': 'нет в базе (добавлено этой записью) ',
  'inherited from base: ': 'унаследовано от базы: ', 'base: ': 'база: ',
  'Field': 'Поле', 'This record': 'Эта запись', 'Reset all to base': 'Сбросить всё к базе', 'Base prototype record': 'Запись базового прототипа',
  'This record has no base prototype (a new record without "base"): nothing to compare with.': 'У записи нет базового прототипа (новая запись без "base"): сравнивать не с чем.',
  'No field differs from the base prototype.': 'Ни одно поле не отличается от базового прототипа.',
  'The record as JSON. Edits apply as soon as the text is valid; invalid text is kept in the box and never written.': 'Запись в виде JSON. Правки применяются, как только текст становится корректным; некорректный текст остаётся в поле и никогда не записывается.',
  'Invalid JSON: this text is kept here and not applied until it parses.': 'Некорректный JSON: текст остаётся здесь и не применяется, пока не разберётся.',
  'a record must be a JSON object': 'запись должна быть объектом JSON',
  // bottom panel
  'Server reload report': 'Отчёт перезагрузки сервера',
  'Nothing applied yet. "Apply to running server" re-reads data/ and shows what changed.': 'Пока ничего не применено. «Применить на запущенном сервере» перечитывает data/ и показывает изменения.',
  "No problems found by the server's validator.": 'Валидатор сервера проблем не нашёл.',
  'error': 'ошибка', 'warning': 'предупреждение',
  // toasts
  'Server reloaded data/': 'Сервер перечитал data/', 'Reload failed, the server keeps the previous data': 'Перезагрузка не удалась, сервер оставил прежние данные',
  'Valid': 'Ошибок нет',
  'Created in memory; it is written when you add a record and save': 'Создан в памяти; записывается, когда вы добавите запись и сохраните',
  'Comments in the file were dropped.': 'Комментарии в файле отброшены.',
  'refused': 'отказ',
  'No token. Open the URL printed by f3_server (http://127.0.0.1:<port>/?token=...).': 'Нет токена. Откройте адрес, который напечатал f3_server (http://127.0.0.1:<порт>/?token=...).',
  // schema labels (EditorSchema.cpp)
  'AI script': 'Скрипт ИИ', 'Ammunition': 'Боеприпасы', 'Armor': 'Броня', 'Attack modes (replace the whole list)': 'Режимы атаки (заменяют весь список)',
  'Base (clone this original critter; for a new id)': 'База (клонировать этого оригинального персонажа; для нового id)',
  'Base (clone this original prototype; for a new id)': 'База (клонировать этот оригинальный прототип; для нового id)',
  'Base formula': 'Базовая формула', 'Cell [x, y]': 'Клетка [x, y]', 'Description': 'Описание',
  'Dialog (a data dialog id, or an original dialog name)': 'Диалог (id диалога из data или имя оригинального диалога)',
  'Effects': 'Эффекты', 'Effects (per rank)': 'Эффекты (за ранг)', 'Equipment (worn on spawn)': 'Экипировка (надета при появлении)',
  'Held / worn model (informational)': 'Модель в руке / на теле (справочно)', 'Id (prototype key)': 'Id (ключ прототипа)',
  'Index (creation slot / mask bit)': 'Индекс (слот создания / бит маски)', 'Inventory (carried / loot)': 'Инвентарь (носит / добыча)',
  'Item rewards': 'Награда предметами', 'Level': 'Уровень', 'Lua hook': 'Lua-хук', 'Lua hook (called as fn(critter, perk_id, rank))': 'Lua-хук (вызывается как fn(critter, perk_id, rank))',
  'Minimum SPECIAL': 'Минимум SPECIAL', 'Minimum skills': 'Минимум навыков', 'Model (informational)': 'Модель (справочно)',
  'Nodes': 'Узлы', 'Original game formula (reference only, never applied)': 'Формула оригинальной игры (только справка, не применяется)',
  'Perks that must be held': 'Требуемые перки', 'Placeholder numbers (not read from the original game)': 'Числа-заглушки (не взяты из оригинальной игры)',
  'Ranks': 'Ранги', 'Requirements': 'Требования', 'SPECIAL (omitted values keep the original)': 'SPECIAL (пропущенные значения остаются оригинальными)',
  'Spawn points (run once at startup)': 'Точки появления (однократно при запуске)', 'Speaker (critter prototype)': 'Говорящий (прототип персонажа)',
  'Start (node id, or routes { if, goto })': 'Старт (id узла или маршруты { if, goto })', 'The 7 armor words': '7 значений брони', 'Title': 'Заголовок',
  'Traits that must be chosen': 'Требуемые черты', 'Weapon': 'Оружие', 'Weight': 'Вес', 'Worn on the head': 'Надевается на голову',
  'XP for killing it': 'Опыт за убийство', 'XP reward': 'Награда опытом', 'ap (real action points, reference)': 'ap (реальные очки действий, справочно)',
  'max_picks (traits allowed at creation, 0..16; two files must not disagree)': 'max_picks (сколько черт можно выбрать при создании, 0..16; два файла не должны расходиться)',
  'stf (original English.stf string id)': 'stf (id строки оригинального English.stf)',
  'Conversation graphs run by the server (docs/dialogs.md). Use the Graph tab for nodes, answers, conditions and effects.': 'Графы разговоров, которые выполняет сервер (docs/dialogs.md). Узлы, ответы, условия и эффекты правятся на вкладке «Граф диалога».',
  'Overrides of original critter prototypes or new NPCs (`base` + a new id), with optional spawn points.': 'Переопределения оригинальных прототипов персонажей или новые NPC (`base` + новый id), с точками появления.',
  'Overrides of original prototypes (id = an existing prototype; only the written fields change) or new items (`base` + a new id).': 'Переопределения оригинальных прототипов (id = существующий прототип; меняются только записанные поля) или новые предметы (`base` + новый id).',
  'Perk definitions: requirements, declarative stat effects (applied once per rank) and an optional Lua hook.': 'Определения перков: требования, декларативные эффекты на характеристики (за каждый ранг) и необязательный Lua-хук.',
  'Quest definitions for the Pip-Boy log; rewards are paid once per character by quest_done / a dialog effect.': 'Определения квестов для журнала Pip-Boy; награда выдаётся один раз на персонажа через quest_done или эффект диалога.',
  'ends the dialog; later effects are skipped': 'завершает диалог; следующие эффекты пропускаются', 'ends the dialog; the NPC turns hostile': 'завершает диалог; NPC становится враждебным',
  'stub: there is no store yet': 'заглушка: магазина пока нет',
  // dialog editor
  'Speaker': 'Говорящий', 'Start': 'Старт', 'Node id': 'Id узла', 'NPC text': 'Текст NPC', 'Original text (stf)': 'Оригинальный текст (stf)',
  'On enter': 'При входе', 'Player text': 'Текст игрока', 'Shown if': 'Показывается, если', 'Then do': 'Затем выполнить', 'Go to node': 'Перейти в узел', 'open': 'открыть',
  '+ Node': '+ Узел', '+ Answer': '+ Ответ', '+ Route': '+ Маршрут', '+ Condition': '+ Условие', '+ add effect...': '+ добавить эффект...',
  '+ any of / not group': '+ группа «любое из» / «не»', 'All of': 'Все из', 'all of': 'все из', 'any of': 'любое из', 'not': 'не',
  'No nodes. Add one with + Node.': 'Узлов нет. Добавьте кнопкой «+ Узел».',
  'default (node "start" or the first node)': 'по умолчанию (узел "start" или первый)', 'a fixed node': 'фиксированный узел', 'conditional routes': 'маршруты по условию',
  'What the NPC says': 'Что говорит NPC', 'What the player says': 'Что говорит игрок', '(using the original string below)': '(используется оригинальная строка ниже)',
  '(empty = end the dialog)': '(пусто = завершить диалог)', 'optional': 'необязательно', 'go to': 'перейти в', 'if': 'если', 'raw': 'как есть',
  'not understood by the builder; the server validates it': 'конструктор не понимает; проверит сервер',
  'unsupported condition shape; use the JSON tab': 'неподдерживаемая форма условия; используйте вкладку JSON',
  '(has any)': '(есть любое)', 'set': 'установить', 'clear': 'сбросить', 'number...': 'число...', '(unknown)': '(неизвестно)',
  'original text unavailable (no game data loaded)': 'оригинальный текст недоступен (нет игровых данных)', '(no such string)': '(нет такой строки)',
  'original string id': 'id оригинальной строки', 'search original text...': 'поиск по оригинальному тексту...', 'Enter to search': 'Enter — искать',
  'no match': 'нет совпадений', 'no game data': 'нет игровых данных',
  'Graph: green = start, dashed = unreachable, red = dead end, blue edges = conditional. Click a node to edit it.': 'Граф: зелёный = старт, пунктир = недостижим, красный = тупик, синие стрелки = условные. Нажмите узел, чтобы править.',
  'Checks passed: every node is reachable, no dead ends, all goto targets exist.': 'Проверки пройдены: все узлы достижимы, тупиков нет, все цели goto существуют.',
  'unreachable': 'недостижим', 'dead end': 'тупик',
  // locale tab
  'Locale': 'Локализация', 'Refresh': 'Обновить', 'Compare en with': 'Сравнить en с', 'Search key or text': 'Поиск по ключу или тексту',
  'missing': 'нет перевода', 'any problem': 'любая проблема', 'edited': 'изменено', 'all groups': 'все группы',
  'No status': 'Нет статуса', '+ New key': '+ Новый ключ', 'Save locale files': 'Сохранить файлы локали', 'Loading locale...': 'Загрузка локали...',
  'Report of --check-locale (same logic)': 'Отчёт --check-locale (та же логика)', 'Key': 'Ключ', 'en (source)': 'en (оригинал)',
  'Prev': 'Назад', 'Next': 'Вперёд', 'New locale key': 'Новый ключ локали', 'Add': 'Добавить', 'Enter a key': 'Введите ключ', 'Key exists': 'Ключ уже есть',
  'namespace.key (e.g. ui.menu.start)': 'пространство.ключ (например ui.menu.start)', 'placeholders differ from en (e.g. {name})': 'плейсхолдеры отличаются от en (например {name})',
  'extra': 'лишние', '(none)': '(нет)',
};
const rp = (re, fn) => [re, fn];
const plural = (n, a, b, c) => { const m10 = n % 10, m100 = n % 100; return m10 === 1 && m100 !== 11 ? a : (m10 >= 2 && m10 <= 4 && (m100 < 10 || m100 >= 20)) ? b : c; };
const ruPatterns = [
  rp(/^● (\d+) files? with unsaved changes$/, (m) => '● ' + m[1] + ' ' + plural(+m[1], 'файл', 'файла', 'файлов') + ' с несохранёнными изменениями'),
  rp(/^Restored unsaved edits of (.+)$/, (m) => 'Восстановлены несохранённые правки: ' + m[1]),
  rp(/^Cannot load (\S+): (.*)$/, (m) => 'Не удалось загрузить ' + m[1] + ': ' + m[2]),
  rp(/^Loading ([a-z]+)\.\.\.$/, (m) => 'Загрузка: ' + m[1] + '...'),
  rp(/^Showing (\d+) of (\d+)\.$/, (m) => 'Показано ' + m[1] + ' из ' + m[2] + '.'),
  rp(/^(\d+) records?$/, (m) => m[1] + ' ' + plural(+m[1], 'запись', 'записи', 'записей')),
  rp(/^(\d+) entries$/, (m) => m[1] + ' ' + plural(+m[1], 'запись', 'записи', 'записей')),
  rp(/^(\d+) keys$/, (m) => m[1] + ' ' + plural(+m[1], 'ключ', 'ключа', 'ключей')),
  rp(/^: (\d+) keys$/, (m) => ': ' + m[1] + ' ' + plural(+m[1], 'ключ', 'ключа', 'ключей')),
  rp(/^(\d+) missing$/, (m) => 'нет перевода: ' + m[1]),
  rp(/^(\d+) extra \(not in en or data\)$/, (m) => 'лишних (нет ни в en, ни в data): ' + m[1]),
  rp(/^(\d+) placeholder mismatches$/, (m) => 'несовпадений плейсхолдеров: ' + m[1]),
  rp(/^data text: (.*)$/, (m) => 'текст в data: ' + m[1]),
  rp(/^page (\d+) \/ (\d+)$/, (m) => 'страница ' + m[1] + ' / ' + m[2]),
  rp(/^Nothing is missing for (.+)\.$/, (m) => 'Для ' + m[1] + ' всё переведено.'),
  rp(/^No entries match\.$/, () => 'Ничего не найдено.'),
  rp(/^Locale: en \/ (.+)$/, (m) => 'Локализация: en / ' + m[1]),
  rp(/^Save locale files \((\d+)\)$/, (m) => 'Сохранить файлы локали (' + m[1] + ')'),
  rp(/^locale status: (.*)$/, (m) => 'статус локали: ' + m[1]),
  rp(/^(\d+) nodes\.$/, (m) => m[1] + ' ' + plural(+m[1], 'узел', 'узла', 'узлов') + '.'),
  rp(/^Edit them in the Dialog graph tab$/, () => 'Правьте их на вкладке «Граф диалога»'),
  rp(/^clone of (.+)$/, (m) => 'клон ' + m[1]),
  rp(/^This file is generated \((.*)\)\. The next export overwrites edits\. Saving asks for confirmation; take it over to make it hand-authored\.$/, (m) => 'Файл сгенерирован (' + m[1] + '). Следующий экспорт перезапишет правки. Сохранение запросит подтверждение; заберите файл себе, чтобы он стал рукописным.'),
  rp(/^This file is not valid JSON: (.*)$/, (m) => 'В файле некорректный JSON: ' + m[1]),
  rp(/^New (.+)$/, (m, t) => 'Создать: ' + t(m[1])),
  rp(/^Override (.+)$/, (m) => 'Переопределить ' + m[1]),
  rp(/^\+ Add (.+)$/, (m, t) => '+ Добавить: ' + t(m[1])),
  rp(/^\+ Set (.+)$/, (m, t) => '+ Задать: ' + t(m[1])),
  rp(/^ from (.+)\? You can undo this until the file is saved; saving is refused if other records still refer to it\.$/, (m) => ' из ' + m[1] + '? Отменить можно до сохранения файла; сохранение будет отклонено, если на запись ещё ссылаются другие.'),
  rp(/^Files of data\/(.+)\. Files starting with "_" are ignored by the server \(drafts, disabled and deleted files\)\.$/, (m) => 'Файлы data/' + m[1] + '. Файлы с именем на «_» сервер игнорирует (черновики, отключённые и удалённые файлы).'),
  rp(/^Files: (.+)$/, (m) => 'Файлы: ' + m[1]),
  rp(/^New file in data\/(.+)$/, (m) => 'Новый файл в data/' + m[1]),
  rp(/^Delete (\S+\/\S+)$/, (m) => 'Удалить ' + m[1]),
  rp(/^Delete $/, () => 'Удалить '),
  rp(/^ \(the server ignores it\), not destroyed\. Refused if other files depend on its records\.$/, () => ' (сервер его игнорирует), файл не уничтожается. Отказ, если от его записей зависят другие файлы.'),
  rp(/^The file is renamed to$/, () => 'Файл переименован в'),
  rp(/^(\d+) records$/, (m) => m[1] + ' ' + plural(+m[1], 'запись', 'записи', 'записей')),
  rp(/^draft \/ ignored$/, () => 'черновик / игнорируется'),
  rp(/^Moved to (.+)$/, (m) => 'Перемещён в ' + m[1]),
  rp(/^Duplicated as (.+)$/, (m) => 'Дубликат: ' + m[1]),
  rp(/^"generator" removed from the working copy; save to keep it hand-authored$/, () => '"generator" убран из рабочей копии; сохраните, чтобы файл остался рукописным'),
  rp(/^(\d+) field\(s\) differ from the original prototype\. Fields that are absent inherit the base value\.$/, (m) => 'Полей, отличающихся от оригинального прототипа: ' + m[1] + '. Отсутствующие поля наследуют значение базы.'),
  rp(/^Base value \((.*)\)$/, (m) => 'Значение базы (' + m[1] + ')'),
  rp(/^File as it will be saved \((.*), the exporter's formatting\)$/, (m) => 'Файл в том виде, в каком будет сохранён (' + m[1] + ', форматирование экспортёра)'),
  rp(/^Invalid JSON, not applied: (.*)$/, (m) => 'Некорректный JSON, не применено: ' + m[1]),
  rp(/^Problems \((\d+) errors, (\d+) warnings\)$/, (m) => 'Проблемы (ошибок: ' + m[1] + ', предупреждений: ' + m[2] + ')'),
  rp(/^Draft saved as (\S+) \(ignored by the server; the live file is unchanged\)$/, (m) => 'Черновик сохранён как ' + m[1] + ' (сервер его игнорирует; рабочий файл не изменён)'),
  rp(/^Saved (\S+)( \((\d+) warning\(s\)\))?$/, (m) => 'Сохранено: ' + m[1] + (m[3] ? ' (предупреждений: ' + m[3] + ')' : '')),
  rp(/^Not saved: (\d+) validation error\(s\)$/, (m) => 'Не сохранено: ошибок проверки: ' + m[1]),
  rp(/^The file was NOT written\. Fix the problems listed below, or keep the work as a draft file \((.*), ignored by the server\)\.$/, (m) => 'Файл НЕ записан. Исправьте перечисленные ниже проблемы или сохраните работу как черновик (' + m[1] + ', сервер его игнорирует).'),
  rp(/^Save failed: (.*)$/, (m) => 'Не удалось сохранить: ' + m[1]),
  rp(/^(\S+) is generated by$/, (m) => m[1] + ' сгенерирован командой'),
  rp(/^\. The next$/, () => '. Следующий запуск'),
  rp(/^ overwrites your edits\.$/, () => ' перезапишет ваши правки.'),
  rp(/^(\S+) was changed since the editor loaded it \(git, the exporter, or another editor\)\.$/, (m) => m[1] + ' изменён с тех пор, как редактор его загрузил (git, экспортёр или другой редактор).'),
  rp(/^(\d+) file\(s\) have unsaved changes; the server only sees what is saved on disk\.$/, (m) => 'Файлов с несохранёнными изменениями: ' + m[1] + '; сервер видит только то, что сохранено на диске.'),
  rp(/^(\d+) error\(s\)$/, (m) => 'Ошибок: ' + m[1]),
  rp(/^Cannot reach the editor API: (.*)\. The token is wrong or the server restarted \(use the new URL\)\.$/, (m) => 'Редактор не достучался до API: ' + m[1] + '. Токен неверен или сервер перезапущен (используйте новый адрес).'),
  rp(/^Still invalid: (.*)$/, (m) => 'Всё ещё некорректно: ' + m[1]),
  rp(/^Node: (.+)$/, (m) => 'Узел: ' + m[1]),
  rp(/^Answers \((\d+)\)$/, (m) => 'Ответы (' + m[1] + ')'),
  rp(/^Answer (\d+)$/, (m) => 'Ответ ' + m[1]),
  rp(/^Route (\d+)( \(fallback when no earlier route matches\))?$/, (m) => 'Маршрут ' + m[1] + (m[2] ? ' (запасной, если ни один предыдущий не подошёл)' : '')),
  rp(/^(\d+) ans$/, (m) => 'отв.: ' + m[1]),
  rp(/^start route goes to unknown node "(.*)"$/, (m) => 'маршрут старта ведёт в неизвестный узел "' + m[1] + '"'),
  rp(/^(error|warning): (.*)$/, (m, t) => (m[1] === 'error' ? 'ошибка: ' : 'предупреждение: ') + t(m[2])),
  rp(/^dead end: node "(.*)" has no answers \(the player cannot continue\)$/, (m) => 'тупик: у узла "' + m[1] + '" нет ответов (игрок не может продолжить)'),
  rp(/^unreachable: node "(.*)" is not reachable from the start$/, (m) => 'недостижим: узел "' + m[1] + '" не достижим из старта'),
  rp(/^(\d+) \/ (\d+) too long$/, (m) => m[1] + ' / ' + m[2] + ' слишком длинно'),
  // server validator messages (m.message)
  rp(/^unknown (item|quest|node|map|skill|prototype|dialog) '(.*)'(.*)$/, (m) => ({ item: 'неизвестный предмет', quest: 'неизвестный квест', node: 'неизвестный узел', map: 'неизвестная карта', skill: 'неизвестный навык', prototype: 'неизвестный прототип', dialog: 'неизвестный диалог' }[m[1]]) + " '" + m[2] + "'" + m[3]),
  rp(/^duplicate id '(.*)'(.*)$/, (m) => "повторяющийся id '" + m[1] + "'" + m[2].replace(' (first defined in ', ' (впервые определён в ')),
  rp(/^missing required field$/, () => 'не хватает обязательного поля'),
  rp(/^expected (.+), got (\w+)$/, (m) => 'ожидалось: ' + m[1] + ', получено: ' + m[2]),
  rp(/^invalid JSON: (.*)$/, (m) => 'некорректный JSON: ' + m[1]),
  rp(/^must not be empty$/, () => 'не должно быть пустым'),
  rp(/^dmg_min is larger than dmg_max$/, () => 'dmg_min больше dmg_max'),
  rp(/^unknown field$/, () => 'неизвестное поле'),
  rp(/^invalid id '(.*)' \(letters, digits, _ \. - only(.*)\)$/, (m) => "недопустимый id '" + m[1] + "' (только буквы, цифры, _ . -" + m[2].replace(', max ', ', максимум ') + ')'),
];
add('ru', ru, ruPatterns, 'Русский');
add('ru', {
  'Object scripts': 'Скрипты объектов',
  'Which object': 'Какой объект',
  'Object (instance name)': 'Объект (имя экземпляра)',
  'Fallback: prototype + cell': 'Запасной вариант: прототип + клетка',
  'Pick object from the map...': 'Выбрать объект на карте...',
  'Hide object picker': 'Скрыть выбор объекта',
  'Presentation': 'Внешний вид',
  'Behaviour': 'Поведение',
  'Cursor label': 'Подпись курсора',
  'Name override': 'Своё имя',
  'Description override': 'Своё описание',
  'Usable': 'Можно использовать',
  'One shot': 'Одноразовый',
  'Cooldown (s)': 'Перезарядка (с)',
  'Range (tiles)': 'Дальность (клетки)',
  'Hidden until': 'Скрыт, пока не выполнено',
  'Lua script': 'Lua-скрипт',
  'Script': 'Скрипт',
  'Create Lua script from template': 'Создать Lua-скрипт из шаблона',
  'Declarative steps': 'Декларативные шаги',
  'On use': 'При использовании',
  'On look': 'При осмотре',
  'On skill use': 'При применении навыка',
  'on success': 'при успехе',
  'on failure': 'при провале',
  'only if': 'только если',
  '+ add step...': '+ добавить шаг...',
  '+ handle a skill...': '+ обработать навык...',
  'Move down': 'Вниз',
  'no': 'нет',
  'yes (world)': 'да (мир)',
  'world: once for everyone': 'мир: один раз для всех',
  'player: once per player': 'игрок: один раз на игрока',
  'Refresh': 'Обновить',
  'Map': 'Карта',
  'Model / type': 'Модель / тип',
  'Kind': 'Вид',
  'Position': 'Позиция',
  'Bound': 'Привязан',
  '(none)': '(нет)',
  'Loading maps...': 'Загрузка карт...',
  'loading...': 'загрузка...',
  'Search id, name, model, position': 'Поиск: id, имя, модель, позиция',
  'ring = already bound; click a dot or a row to pick': 'обводка = уже привязан; клик по точке или строке выбирает объект',
  'No script: only the declarative steps run.': 'Скрипта нет: работают только декларативные шаги.',
  '✓ file exists and loads': '✓ файл существует и загружается',
  '✗ file does not exist': '✗ файла нет',
  'open in VS Code': 'открыть в VS Code',
  'Step not understood by the builder; use the JSON tab.': 'Шаг не понятен конструктору; используйте вкладку JSON.',
  'The stable key: the instance name of the object on the map. The fallback below is used only when the object has no instance name.': 'Устойчивый ключ: имя экземпляра объекта на карте. Запасной вариант ниже нужен, только если у объекта нет имени экземпляра.',
  'Shown in the cursor hint, e.g. Use, Hack, Pull. Default: Use.': 'Показывается в подсказке курсора, например Use, Hack, Pull. По умолчанию: Use.',
  'Off: the object cannot be used (looking still works).': 'Выключено: объект нельзя использовать (осмотр работает).',
  'The used mark is stored in the world state and survives a restart.': 'Отметка об использовании хранится в состоянии мира и переживает перезапуск.',
  '0 or empty: the normal door / use range.': '0 или пусто: обычная дальность двери / использования.',
  'The object is invisible and unusable until the condition holds (flag:name, quest:id==completed, ...).': 'Объект невидим и недоступен, пока условие не выполнено (flag:имя, quest:id==completed, ...).',
  'Run top to bottom; a dialog step hands over to the dialog; end stops the list.': 'Выполняются сверху вниз; шаг dialog передаёт управление диалогу; end останавливает список.',
  'Steps run when the player uses that skill on the object (Science to hack a computer, Lockpick, Repair...). With a Lua on_skill hook that returns true these steps are skipped.': 'Шаги выполняются, когда игрок применяет этот навык к объекту (Science для взлома компьютера, Lockpick, Repair...). Если Lua-хук on_skill вернул true, эти шаги пропускаются.',
  'A file below scripts/ (usually objects/<name>.lua) with on_use / on_skill / on_look hooks. It runs before the steps below; a hook that returns true skips them.': 'Файл внутри scripts/ (обычно objects/<имя>.lua) с хуками on_use / on_skill / on_look. Он выполняется раньше шагов ниже; хук, вернувший true, их пропускает.',
}, [
  rp(/^The object list needs the running server: (.*)$/, (m) => 'Список объектов требует запущенного сервера: ' + m[1]),
  rp(/^(\d+) object\(s\)$/, (m) => 'объектов: ' + m[1]),
  rp(/^hooks: (.*)$/, (m) => 'хуки: ' + m[1]),
  rp(/^warning: (.*)$/, (m) => 'предупреждение: ' + m[1]),
  rp(/^✗ does not load: (.*)$/, (m) => '✗ не загружается: ' + m[1]),
  rp(/^Bindings of this map that match no object: (.*)$/, (m) => 'Привязки этой карты, которым не соответствует ни один объект: ' + m[1]),
  rp(/^Showing 300 of (\d+) in the table \(the plot shows all\)\. Narrow the search\.$/, (m) => 'В таблице 300 из ' + m[1] + ' (график показывает все). Сузьте поиск.'),
  rp(/^Picked (.*)$/, (m) => 'Выбрано: ' + m[1]),
  rp(/^Created scripts\/(.*) from the template$/, (m) => 'Создан scripts/' + m[1] + ' из шаблона'),
  rp(/^scripts\/(.*) already exists \(not overwritten\)$/, (m) => 'scripts/' + m[1] + ' уже существует (не перезаписан)'),
  rp(/^Cannot create: (.*)$/, (m) => 'Не удалось создать: ' + m[1]),
]);
})();
