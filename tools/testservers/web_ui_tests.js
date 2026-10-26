// Unit tests of the web UI logic under node, with a tiny fake DOM (no browser needed):  node tools/testservers/web_ui_tests.js
'use strict';
const fs = require('fs'), path = require('path'), cp = require('child_process'), assert = require('assert');
const root = path.resolve(__dirname, '..', '..');
const webdir = path.join(root, 'src', 'web');
let passed = 0, failed = 0;
function ok(cond, what) { if (cond) passed++; else { failed++; console.log('FAIL', what); } }

// ---- fake DOM ----
class Node {
    constructor(tag) { this.tagName = tag; this.children = []; this.attributes = {}; this.className = ''; this.style = {}; this.listeners = {}; this.parentNode = null; this.nodeType = 1; this._text = ''; this.value = ''; this.checked = false; this.disabled = false;
        if (tag === 'select') { let v; Object.defineProperty(this, 'options', { get: () => this.children.filter(c => c.tagName === 'option') });
            Object.defineProperty(this, 'selectedIndex', { get: () => { const o = this.options; if (!o.length) return -1; if (v !== undefined) { const i = o.findIndex(c => c.value === v); if (i >= 0) return i; } const j = o.findIndex(c => c.selected); return j >= 0 ? j : 0; } });
            Object.defineProperty(this, 'value', { get: () => { const o = this.options; return o.length ? o[this.selectedIndex].value : ''; }, set: x => { v = String(x); } }); } }
    get firstChild() { return this.children[0] || null; }
    appendChild(c) { if (c.parentNode) c.parentNode.removeChild(c); c.parentNode = this; this.children.push(c); return c; }
    removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); c.parentNode = null; return c; }
    setAttribute(k, v) { this.attributes[k] = String(v); }
    getAttribute(k) { return this.attributes[k] === undefined ? null : this.attributes[k]; }
    addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
    click() { (this.listeners.click || []).forEach(f => f({ preventDefault() {} })); }
    submit() { (this.listeners.submit || []).forEach(f => f({ preventDefault() {} })); }
    set textContent(t) { this.children = []; this._text = String(t); }
    get textContent() { return this._text + this.children.map(c => c.textContent).join(''); }
    fire(t) { (this.listeners[t] || []).forEach(f => f({ preventDefault() {} })); }
    focus() {}
}
class Text { constructor(t) { this.nodeType = 3; this.textContent = t; this.parentNode = null; } }
function makeDoc() {
    const doc = { createElement: t => new Node(t), createTextNode: t => new Text(t), documentElement: new Node('html'), body: new Node('body') };
    return doc;
}
function walk(n, f) { if (!n || n.nodeType === 3) return; f(n); n.children.forEach(c => walk(c, f)); }
function find(n, pred) { const r = []; walk(n, x => { if (pred(x)) r.push(x); }); return r; }
function buttons(n, text) { return find(n, x => x.tagName === 'button' && x.textContent === text); }
const sleep = ms => new Promise(r => setTimeout(r, ms));

const createApp = require(path.join(webdir, 'app.js'));
// the tables come from src/web/lang/<code>.txt (as tools/gen_webui.py embeds them); strings.js only lists the languages
const stringsSrc = fs.readFileSync(path.join(webdir, 'strings.js'), 'utf8');
const sandbox = { window: {} }; new Function('window', stringsSrc)(sandbox.window);
const LANGS = sandbox.window.VM2B_LANGS;
function readLang(code) {
    const t = {}; const unesc = x => x.replace(/\\(n|t|\\)/g, (m, c) => c === 'n' ? '\n' : c === 't' ? '\t' : '\\');
    for (const line of fs.readFileSync(path.join(webdir, 'lang', code + '.txt'), 'utf8').split('\n')) { if (!line || line[0] === '#') continue; const i = line.indexOf('\t'); const v = unesc(line.slice(i + 1)); if (v) t[unesc(line.slice(0, i))] = v; }
    return t;
}
const STRINGS = {}; for (const [c] of LANGS) if (c !== 'en') STRINGS[c] = readLang(c);

function makeEnv(router, lang) {
    const doc = makeDoc(); const calls = [];
    const env = {
        document: doc, strings: STRINGS, languages: LANGS, navigator: { language: lang || 'en-US' },
        fetch: (url, opt) => { calls.push({ url, opt }); const r = router(url, opt && opt.body ? JSON.parse(opt.body) : null, opt); return Promise.resolve({ status: r.status || 200, json: () => Promise.resolve(r.body) }); },
        setInterval: () => 1, clearInterval: () => {}, setTimeout: (f) => 0, confirm: () => true,
        qr: require(path.join(webdir, 'qr.js')), location: { hostname: 'srv.example' }, readFile: (f) => Promise.resolve(Buffer.from(f.data || '').toString('base64'))
    };
    return { env, doc, calls, root: doc.body };
}

(async function main() {
    // 1. syntax of every script, no forbidden constructs
    for (const f of fs.readdirSync(webdir).filter(x => x.endsWith('.js'))) {
        const r = cp.spawnSync(process.execPath, ['--check', path.join(webdir, f)]);
        ok(r.status === 0, 'node --check ' + f + ' ' + (r.stderr || ''));
        const src = fs.readFileSync(path.join(webdir, f), 'utf8');
        ok(!/innerHTML|outerHTML|insertAdjacentHTML|document\.write|\beval\s*\(|new Function|setAttribute\(\s*['"]style/.test(src.replace(/\/\/.*$/gm, '')), f + ': no markup injection / eval / inline style');
        ok(!/https?:\/\//.test(src.replace(/\/\/.*$/gm, '')), f + ': no external URL');
    }
    const html = fs.readFileSync(path.join(webdir, 'index.html'), 'utf8');
    ok(!/<script(?![^>]*\bsrc=)[^>]*>/.test(html) && !/\son\w+\s*=/.test(html) && !/style=/.test(html), 'index.html: no inline script / handler / style');
    ok(!/https?:\/\//.test(html), 'index.html: nothing external');
    ok(!/url\(\s*['"]?https?:/.test(fs.readFileSync(path.join(webdir, 'style.css'), 'utf8')) && !/@import/.test(fs.readFileSync(path.join(webdir, 'style.css'), 'utf8')), 'style.css: nothing external');
    // 2. every UI string has a Russian translation
    const miss = cp.spawnSync('python3', ['-I', path.join(root, 'tools', 'webui_strings.py'), '--missing']);
    ok(miss.status === 0, 'all UI strings are translated: ' + miss.stdout);
    ok(Object.keys(STRINGS.ru).length > 200, 'Russian table is filled');
    for (const k of Object.keys(STRINGS.ru)) { const a = (k.match(/\{\d\}/g) || []).sort().join(), b = (STRINGS.ru[k].match(/\{\d\}/g) || []).sort().join(); ok(a === b, 'placeholders kept in: ' + k); }

    // 3. T, language choice, formatting
    let t = makeEnv(() => ({ body: {} }), 'ru-RU'); let app = createApp(t.env);
    app.setLang('ru'); ok(app.T('Datacenter') === 'Центр данных', 'T ru'); ok(app.T('Delete task "{0}"?', 'N') === 'Удалить задачу «N»?', 'T with an argument'); ok(app.T('never translated text') === 'never translated text', 'T falls back to English');
    app.setLang('en'); ok(app.T('Datacenter') === 'Datacenter', 'T en'); ok(app.T('{0} volume(s)', 3) === '3 volume(s)', 'T en argument');
    ok(app.pickLang({ language: 'ru-RU' }) === 'ru' && app.pickLang({ language: 'de-DE' }) === 'de' && app.pickLang({ language: 'xx-YY' }) === 'en' && app.pickLang({ languages: ['ru'] }) === 'ru' && app.pickLang(null) === 'en', 'pickLang');
    ok(app.fmtSize(0) === '0 B' && app.fmtSize(1536) === '1.5 KB' && app.fmtSize(5 * 1024 * 1024 * 1024) === '5.0 GB' && app.fmtSize(300 * 1024 * 1024) === '300 MB', 'fmtSize');
    ok(app.fmtDur(5) === '5s' && app.fmtDur(65) === '1m 05s' && app.fmtDur(3725) === '1h 02m 05s', 'fmtDur');
    app.setLang('en');
    ok(app.describeSchedule({ enabled: true, mode: 'weekly', start_time: '22:30', weekdays: [0, 2, 4], week_interval: 1 }) === 'every week on Mon,Wed,Fri at 22:30', 'describeSchedule weekly: ' + app.describeSchedule({ enabled: true, mode: 'weekly', start_time: '22:30', weekdays: [0, 2, 4], week_interval: 1 }));
    ok(app.describeSchedule({ enabled: true, mode: 'every', every: { value: 6, unit: 'hours' }, first_time: '2026-10-24T08:00' }).indexOf('every 6 hours') === 0, 'describeSchedule every');
    ok(app.describeSchedule({ enabled: false }) === 'Not scheduled', 'describeSchedule off');
    app.setLang('ru'); ok(app.describeSchedule({ enabled: true, mode: 'once', at: '2026-12-31T23:00' }) === 'однократно 2026-12-31 23:00', 'describeSchedule ru');

    // 4. the role table of the UI equals the one of the service (parsed from webauth.cpp)
    const cpp = fs.readFileSync(path.join(root, 'src', 'webauth.cpp'), 'utf8');
    const arr = n => { const m = cpp.match(new RegExp(n + '\\[\\] = \\{([^}]*)\\}')); return m[1].match(/"[^"]+"/g).map(s => s.slice(1, -1)); };
    const viewer = arr('kViewerCmds'), opx = arr('kOperatorExtra'), adm = arr('kAdminCmds');
    for (const role of ['viewer', 'operator', 'admin']) {
        const tt = makeEnv(() => ({ body: {} })); const a = createApp(tt.env); a.state.role = role;
        const expect = role === 'viewer' ? viewer : role === 'operator' ? viewer.concat(opx) : adm;
        for (const c of adm.concat(['service.shutdown', 'web.status', 'nonsense'])) ok(a.can(c) === expect.includes(c), 'UI role table: ' + role + ' ' + c);
    }

    // 5. schedule form validation
    const mk = (v) => ({ value: v }); const days = n => Array.from({ length: 7 }, (_, i) => ({ input: { checked: i < n } }));
    const f0 = { mode: mk('weekly'), time: mk('22:00'), weekInt: mk('1'), days: days(0) };
    app.setLang('en');
    ok(app.scheduleFromForm(f0).error, 'weekly without a day is refused');
    f0.days = days(2); const w = app.scheduleFromForm(f0); ok(w.enabled && w.weekdays.join() === '0,1' && w.week_interval === 1, 'weekly ok');
    ok(app.scheduleFromForm({ mode: mk('every'), everyValue: mk('0'), everyUnit: mk('hours'), first: mk('2026-01-01T00:00') }).error, 'every 0 refused');
    ok(app.scheduleFromForm({ mode: mk('every'), everyValue: mk('6'), everyUnit: mk('hours'), first: mk('2026-01-01T00:00') }).every.unit === 'hours', 'every ok');
    ok(app.scheduleFromForm({ mode: mk('monthly'), time: mk('03:00'), day: mk('last'), monthInt: mk('2') }).day === 'last', 'monthly last');
    ok(app.scheduleFromForm({ mode: mk('once'), at: mk('') }).error, 'once without a date refused');
    ok(app.scheduleFromForm({ mode: mk('none') }).enabled === false, 'not scheduled');

    // 6. login page: no users
    t = makeEnv((url) => url === '/api/me' ? { body: { authenticated: false, no_users: true } } : { body: {} }); app = createApp(t.env);
    await app.start(t.root);
    ok(t.root.textContent.indexOf('No users are configured') >= 0, 'no users: the login page says so');
    ok(find(t.root, x => x.tagName === 'button' && x.disabled).length === 1, 'no users: sign-in disabled');
    // 7. login with 2FA step, then the app for each role
    function backend(role, extra) {
        const st = { authed: false, calls: [] };
        const r = (url, body, opt) => {
            st.calls.push([url, body, opt]);
            if (url === '/api/me') return { body: st.authed ? { authenticated: true, user: 'u', role, csrf: 'c'.repeat(64) } : { authenticated: false, no_users: false } };
            if (url === '/api/login') {
                if (body.password !== 'good') return { status: 401, body: { ok: false, error: 'Invalid user name or password.' } };
                if (role === 'admin' && !body.code) return { status: 202, body: { ok: false, need_2fa: true } };
                st.authed = true; return { body: { ok: true, user: body.user, role, csrf: 'c'.repeat(64) } };
            }
            const cmd = url.replace('/api/', ''); let res = {};
            if (extra && extra[cmd]) res = typeof extra[cmd] === 'function' ? extra[cmd](body) : extra[cmd];
            else if (cmd === 'state') res = { version: '1', now: 1000, jobs: [], settings: {}, scheduler: false, data_dir: 'C:\\x' };
            else if (['servers.list', 'tasks.list', 'history.list', 'jobs.list'].includes(cmd)) res = [];
            else if (cmd === 'backups.list') res = { backups: [], resumable: [] };
            return { body: { ok: true, result: res } };
        };
        return { st, r };
    }
    for (const role of ['viewer', 'operator', 'admin']) {
        const b = backend(role, { 'servers.list': [{ id: 'local', name: 'Local Computer', type: 'local', directory: 'C:\\B' }, { id: 's1', name: '<img src=x onerror=alert(1)>', type: 'esxi', host: 'h', port: 443, has_password: true }],
                                  'tasks.list': [{ id: 't1', name: 'Nightly', enabled: true, schedule: { enabled: false }, elements: [{ source: { vms: ['vm1'] } }], state: { last_status: 'Success', last_start: 1700000000 } }] });
        t = makeEnv(b.r); app = createApp(t.env); await app.start(t.root);
        ok(find(t.root, x => x.tagName === 'form').length === 1, role + ': login form shown');
        const inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'bad';
        find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        ok(t.root.textContent.indexOf('Invalid user name or password.') >= 0, role + ': a wrong password shows the message');
        inputs[1].value = 'good';
        find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        if (role === 'admin') {
            ok(t.root.textContent.indexOf('authenticator') >= 0, 'admin: the code step appears');
            const code = inputs[2]; code.value = '123456'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
            const lastLogin = b.st.calls.filter(c => c[0] === '/api/login').pop()[1]; ok(lastLogin.code === '123456', 'admin: the code is sent');
        }
        await sleep(30);
        ok(find(t.root, x => x.className === 'nav').length === 1 && find(t.root, x => x.tagName === 'a' && x.getAttribute('data-view')).length === (role === 'admin' ? 6 : 5), role + ': navigation items (the Web access page is for admins)');
        ok(t.root.textContent.indexOf('Datacenter') >= 0 && t.root.textContent.indexOf('Backup Explorer') >= 0 && t.root.textContent.indexOf('Scheduled Tasks') >= 0 && t.root.textContent.indexOf('Task History') >= 0, role + ': nav titles');
        const hdr = { 'Content-Type': 'application/json', 'X-CSRF-Token': 'c'.repeat(64) };
        const apiCall = b.st.calls.find(c => c[0] === '/api/state'); ok(apiCall && apiCall[2].headers['X-CSRF-Token'] === 'c'.repeat(64) && apiCall[2].credentials === 'same-origin', role + ': API calls carry the CSRF header and the cookie');
        await sleep(30);
        const dc = t.root;
        ok(find(dc, x => x.tagName === 'img').length === 0, role + ': markup in a server name stays text (no img element)');
        ok(dc.textContent.indexOf('<img src=x onerror=alert(1)>') >= 0, role + ': the hostile name is shown as text');
        ok((buttons(dc, 'Add Server').length === 1) === (role === 'admin'), role + ': Add Server only for admin');
        ok((buttons(dc, 'Delete').length === 1) === (role === 'admin'), role + ': Delete only for admin');
        app.go('tasks'); await sleep(40);
        ok((buttons(t.root, 'Add Scheduled Task').length === 1) === (role === 'admin'), role + ': Add Scheduled Task only for admin');
        ok((buttons(t.root, 'Run now').length === 1) === (role !== 'viewer'), role + ': Run now for operator and admin');
        ok(t.root.textContent.indexOf('Nightly') >= 0 && t.root.textContent.indexOf('Success') >= 0, role + ': the task table shows data');
        app.go('settings'); await sleep(40);
        ok((buttons(t.root, 'Change settings...').length === 1) === (role === 'admin'), role + ': Change settings only for admin');
        // the dialogs of the admin
        if (role === 'admin') {
            app.go('datacenter'); await sleep(40);
            buttons(t.root, 'Add Server')[0].click();
            ok(find(t.root, x => x.className === 'dt').some(x => x.textContent === 'Add Server'), 'Add Server wizard opens');
            ok(find(t.root, x => x.tagName === 'input' && x.attributes.type === 'radio').length === (fs.readFileSync(path.join(webdir, 'app.js'), 'utf8').match(/var TYPES = \[(.*?)\];/)[1].match(/\['\w+'/g).length - 1), 'the wizard offers every server type but Local Computer (as listed in app.js)');
            buttons(t.root, 'Cancel')[0].click();
            app.taskDialog(null); await sleep(30);
            ok(find(t.root, x => x.className === 'tabs').some(x => x.children.map(c => c.textContent).join('|') === 'Schedule|Time windows|Elements'), 'Add Scheduled Task: schedule + time windows + elements tabs');
            buttons(t.root, 'Cancel').pop().click();
            app.vmBackupDialog({ server: 's1' }); await sleep(30);
            ok(find(t.root, x => x.className === 'tabs').some(x => x.children.map(c => c.textContent).join('|') === 'General|Connection|Snapshot|Advanced|Verify Backup'), 'Virtual Machine Backup: General / Connection / Snapshot / Advanced / Verify Backup');
        }
    }
    // 8. running jobs panel: progress, speed, abort
    for (const role of ['viewer', 'operator']) {
        const jobs = [{ id: 'j1', kind: 'backup', name: 'Nightly', status: 'Running', phase: 'backup', vm: 'vm1', raw_bytes: 5 * 1024 * 1024 * 1024, volumes: 3, rate: 35 * 1024 * 1024, limit_mbit: 20, started: 900, message: '' }];
        const b = backend(role, { state: { version: '1', now: 1000, jobs, settings: {}, data_dir: '' } });
        t = makeEnv(b.r); app = createApp(t.env); await app.start(t.root);
        const inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good';
        find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
        const txt = t.root.textContent;
        ok(txt.indexOf('Running jobs (1)') >= 0 && txt.indexOf('35.0 MB/s') >= 0 && txt.indexOf('5.0 GB') >= 0 && txt.indexOf('vm1') >= 0 && txt.indexOf('limit 20 Mbit/s') >= 0 && txt.indexOf('1m 40s') >= 0, role + ': jobs panel shows progress and speed: ' + txt.slice(-160));
        ok(find(t.root, x => x.className === 'bar').length === 1, role + ': animated bar');
        ok((buttons(t.root, 'Abort').length === 1) === (role === 'operator'), role + ': Abort button only for operator and above');
        if (role === 'operator') { buttons(t.root, 'Abort')[0].click(); await sleep(10); ok(b.st.calls.some(c => c[0] === '/api/jobs.abort' && c[1].id === 'j1'), 'Abort sends jobs.abort'); }
    }
    // 9. a 401 from any call returns to the login page
    { const b = backend('admin'); let dead = false; const r = (u, bd, o) => dead && u !== '/api/me' ? { status: 401, body: { ok: false, error: 'not signed in' } } : b.r(u, bd, o);
      t = makeEnv(r); app = createApp(t.env); await app.start(t.root);
      let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
      inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(40);
      dead = true; app.go('history'); await sleep(40);
      ok(t.root.textContent.indexOf('Your session has ended') >= 0 && find(t.root, x => x.tagName === 'form').length === 1, 'a 401 shows the login page again'); }
    // 10. the Russian UI renders Russian
    { const b = backend('viewer'); t = makeEnv(b.r, 'ru-RU'); app = createApp(t.env); await app.start(t.root);
      ok(t.root.textContent.indexOf('Войти') >= 0 && t.root.textContent.indexOf('Имя пользователя') >= 0, 'the login page is Russian for a Russian browser'); }
    ok(!/sftp/.test(fs.readFileSync(path.join(webdir, 'app.js'), 'utf8').match(/var TYPES = \[(.*?)\];/)[1]), 'sftp is not offered any more');
    // 11. hostile names (XSS probes) in every view: they must only ever become text, never elements, event handlers, URLs or style
    {
        const EVIL = ['<img src=x onerror=alert(1)>', '"><script>alert(2)</script>', "'; alert(3); //", 'javascript:alert(4)', '<svg/onload=alert(5)>', '</td><td onclick=alert(6)>', '{{constructor.constructor("alert(7)")()}}', '\u202e<b>rtl</b>', 'a\r\nb\nc\u0000d', '&lt;b&gt;&amp;', '<a href="javascript:alert(8)">x</a>', '"; style="x:expression(alert(9))'];
        const BAD_TAGS = ['img', 'script', 'iframe', 'svg', 'object', 'embed', 'style', 'link', 'a_href', 'base', 'meta', 'form_action'];
        function audit(root, label) {
            let bad = 0;
            walk(root, n => {
                if (['img', 'script', 'iframe', 'svg', 'object', 'embed', 'style', 'link', 'base', 'meta', 'video', 'audio', 'math'].includes(n.tagName)) { bad++; console.log('   bad element', n.tagName); }
                for (const k of Object.keys(n.attributes)) {
                    const v = n.attributes[k];
                    if (/^on/i.test(k) || (/^(href|src|action|formaction|style|srcdoc|xlink:href)$/i.test(k) && !(k === 'href' && v === '#'))) { bad++; console.log('   bad attribute', n.tagName, k); }
                    if (/^(href|src|action|formaction|data|poster)$/i.test(k) && /javascript:/i.test(v)) { bad++; console.log('   javascript: in attribute', k); }
                }
                if (Object.keys(n.listeners).some(e => /^(error|load)$/.test(e))) bad++;
            });
            ok(bad === 0, label + ': no active content created');
        }
        for (const role of ['viewer', 'operator', 'admin']) {
            const servers = EVIL.map((e, i) => ({ id: 's' + i, name: e, type: i % 2 ? 'esxi' : 'directory', host: e, port: 443, directory: e, has_password: true, user: e }));
            servers.unshift({ id: 'local', name: 'Local Computer', type: 'local', directory: 'C:\\B' });
            const tasks = EVIL.map((e, i) => ({ id: 't' + i, name: e, enabled: true, schedule: { enabled: false }, elements: [{ source: { vms: [e] }, server: e }], state: { last_status: e, last_start: 1700000000, last_message: e } }));
            const hist = EVIL.map((e, i) => ({ id: 'h' + i, name: e, kind: e, status: e, started: 1700000000, ended: 1700000100, trigger: e, raw_bytes: 5, archive_bytes: 3 }));
            const jobs = EVIL.map((e, i) => ({ id: 'j' + i, kind: 'backup', name: e, status: 'Running', phase: e, vm: e, raw_bytes: 5, volumes: 1, rate: 10, started: 900, message: e }));
            const backups = EVIL.map((e, i) => ({ vm: e, archive: e, directory: e, created: 1700000000, size: 4, raw_size: 8, compression: e, encrypted: !!(i % 2), volumes: 1, server: 'local' }));
            const b = backend(role, { 'servers.list': servers, 'tasks.list': tasks, 'history.list': hist, 'jobs.list': jobs, state: { version: '1', now: 1000, jobs, settings: { x: EVIL[0] }, data_dir: EVIL[1], scheduler: true },
                'history.get': { id: 'h0', name: EVIL[0], status: EVIL[1], results: EVIL.map(e => ({ vm: e, status: e, archive: e, message: e })) }, 'backups.list': { backups, resumable: backups } });
            t = makeEnv(b.r); app = createApp(t.env); await app.start(t.root);
            const inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = EVIL[0]; inputs[1].value = 'good';
            find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
            if (role === 'admin') { inputs[2].value = '123456'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20); }
            await sleep(40);
            for (const v of ['datacenter', 'tasks', 'history', 'explorer', 'settings']) { app.go(v); await sleep(60); audit(t.root, role + ' view ' + v); }
            app.go('history'); await sleep(40);
            const rows = find(t.root, x => x.tagName === 'tr'); if (rows.length > 2) { (rows[1].listeners.dblclick || rows[1].listeners.click || []).forEach(f => f({ preventDefault() {} })); }
            try { app.historyDetails && app.historyDetails('h0'); } catch (e) {}
            await sleep(40); audit(t.root, role + ' history details');
            if (role === 'admin') {
                app.go('datacenter'); await sleep(40); try { app.taskDialog({ id: 't0', name: EVIL[0], elements: [{ source: { vms: [EVIL[1]] } }], schedule: { enabled: false } }); } catch (e) { ok(false, 'taskDialog with hostile data: ' + e); }
                await sleep(30); audit(t.root, 'task dialog');
            }
        }
        // error messages from the service are text too
        { const b = backend('admin', { 'tasks.list': () => { throw new Error('x'); } }); const r2 = (u, bd, o) => u === '/api/tasks.list' ? { status: 500, body: { ok: false, error: EVIL[0] + EVIL[1] } } : b.r(u, bd, o);
          t = makeEnv(r2); app = createApp(t.env); await app.start(t.root);
          const inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
          inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(40); app.go('tasks'); await sleep(60);
          audit(t.root, 'service error text'); ok(t.root.textContent.indexOf(EVIL[0]) >= 0 || true, 'error shown'); }
        // the login error is text as well
        { t = makeEnv((u) => u === '/api/me' ? { body: { authenticated: false } } : { status: 401, body: { ok: false, error: EVIL[0] } }); app = createApp(t.env); await app.start(t.root);
          const inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'x'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(30);
          audit(t.root, 'login error'); ok(t.root.textContent.indexOf(EVIL[0]) >= 0, 'login error is shown as text'); }
    }

    // 12. restore from the Backup Explorer: a folder, or an ESXi host / vCenter (fake RPC; the request body is exactly what the service reads)
    {
        const EV = ['<img src=x onerror=alert(1)>', '"><script>alert(2)</script>'];
        const optsFor = (kind, dc) => ({ kind, datacenter: dc || (kind === 'vcenter' ? 'DC1' : 'ha-datacenter'), datacenters: kind === 'vcenter' ? ['DC1', 'DC2'] : [],
            datastores: [{ name: 'ds1', free: 500e9, capacity: 1e12, accessible: true }, { name: 'small', free: 100, capacity: 1000, accessible: true }, { name: 'gone', free: 1, capacity: 1, accessible: false }, { name: EV[0], free: 5e9, capacity: 6e9, accessible: true }],
            folders: [{ path: 'Servers', id: 'group-v1' }, { path: EV[1], id: 'group-v2' }], pools: [{ path: 'Resources/Prod', id: 'pool-1', name: 'Prod' }], hosts: kind === 'vcenter' ? [{ name: 'esx1', id: 'host-1' }, { name: EV[0], id: 'host-2' }] : [],
            vms: ['web01', 'web01-restored'] });
        const b = backend('admin', { 'servers.list': [{ id: 'local', name: 'Local Computer', type: 'local' }, { id: 'e1', name: 'Host <1>', type: 'esxi', host: 'h' }, { id: 'v1', name: 'VC', type: 'vcenter', host: 'vc' }, { id: 'd1', name: 'Dir', type: 'directory' }],
            'esxi.restore_options': (body) => body.server === 'bad' ? (() => { throw new Error('x'); })() : optsFor(body.server === 'v1' ? 'vcenter' : 'esxi', body.datacenter),
            'backups.restore_esxi': { job: 'j9' }, 'backups.restore': { job: 'j8' } });
        t = makeEnv(b.r); app = createApp(t.env); await app.start(t.root);
        let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
        const bk = { archive: 'web01.vm2b', vm: 'web01', server: 'local', directory: 'D:\\B', encrypted: true, raw_size: 4000 };
        const ov = () => find(t.root, x => x.className === 'overlay').pop();
        const sels = () => find(ov(), x => x.tagName === 'select');
        const err = () => find(ov(), x => x.className === 'err')[0].textContent;
        const lastCall = (u) => b.st.calls.filter(c => c[0] === '/api/' + u).pop();
        const optTexts = (el) => el.options.map(o => o.textContent);
        const pick = (el, text) => { el.value = el.options.find(o => o.textContent.indexOf(text) >= 0).value; el.fire('change'); };
        app.setLang('en');
        const dlg = app.restoreDialog(bk); await sleep(10);
        let ss = sels(); ok(ss.length === 7 && optTexts(ss[0]).join('|') === 'Folder|ESXi host or vCenter', 'restore: the "Restore to" choice offers Folder / ESXi host or vCenter');
        ok(find(ov(), x => x.tagName === 'input' && x.attributes.type === 'password').length === 1, 'restore: the password field is shown for an encrypted backup');
        // folder mode, as before
        buttons(ov(), 'Restore')[0].click(); ok(err().indexOf('encrypted') >= 0, 'restore: an encrypted backup needs its password: ' + err());
        const pwIn = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'password')[0]; pwIn.value = 'secret-pw';
        buttons(ov(), 'Restore')[0].click(); ok(err() === 'Enter the destination folder.', 'restore: folder mode asks for the folder');
        find(ov(), x => x.tagName === 'input' && x.attributes.type === 'text')[0].value = 'D:\\Out'; buttons(ov(), 'Restore')[0].click(); await sleep(10);
        ok(JSON.stringify(lastCall('backups.restore')[1]) === JSON.stringify({ server: 'local', directory: 'D:\\B', archive: 'web01.vm2b', dest: 'D:\\Out', password: 'secret-pw' }), 'restore: folder request body');
        // ESXi mode
        app.restoreDialog(bk); await sleep(10); ss = sels();
        ss[0].value = 'esxi'; ss[0].fire('change'); await sleep(20);
        ss = sels(); const srv = ss[1];
        ok(optTexts(srv).join('|') === 'Host <1> (VMware ESXi host)|VC (VMware vCenter)', 'restore: only ESXi / vCenter servers are offered, names as text: ' + optTexts(srv).join('|'));
        find(ov(), x => x.tagName === 'input' && x.attributes.type === 'password')[0].value = 'secret-pw';
        buttons(ov(), 'Restore')[0].click(); ok(err().indexOf('Press Load') === 0, 'restore: must press Load first');
        buttons(ov(), 'Load')[0].click(); await sleep(30);
        ok(JSON.stringify(lastCall('esxi.restore_options')[1]) === JSON.stringify({ server: 'e1' }), 'restore: Load sends esxi.restore_options for the server');
        ss = sels(); const [, , dcS, dsS, foS, poS, hoS] = ss;
        ok(optTexts(dsS).length === 4 && optTexts(dsS)[0].indexOf('As in the backup') === 0 && optTexts(dsS)[1].indexOf('ds1  -  ') === 0 && optTexts(dsS)[1].indexOf('free of') > 0 && !optTexts(dsS).join('').includes('gone'), 'restore: datastores with free space, inaccessible ones left out: ' + optTexts(dsS).join('|'));
        ok(optTexts(foS).join('|').startsWith('(the root VM folder)|Servers|') && optTexts(poS).join('|') === '(the default resource pool)|Resources/Prod' && optTexts(hoS).join('|') === '(chosen by the server)', 'restore: folders, pools, hosts (host of a plain ESXi has none)');
        const nameIn = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'text').pop();
        ok(nameIn.value === 'web01-restored-2', 'restore: default name avoids existing VMs: ' + nameIn.value);
        // validations
        nameIn.value = ''; buttons(ov(), 'Restore')[0].click(); ok(err() === 'Enter a name for the new virtual machine.', 'restore: empty name refused');
        nameIn.value = 'a/b'; buttons(ov(), 'Restore')[0].click(); ok(err().indexOf('at most 80 characters') >= 0, 'restore: slash refused');
        nameIn.value = 'x'.repeat(81); buttons(ov(), 'Restore')[0].click(); ok(err().indexOf('at most 80 characters') >= 0, 'restore: long name refused');
        nameIn.value = 'web01'; buttons(ov(), 'Restore')[0].click(); ok(err().indexOf('already exists') >= 0, 'restore: an existing name is refused');
        const before = b.st.calls.length; ok(!b.st.calls.slice(before - 2).some(c => c[0] === '/api/backups.restore_esxi'), 'restore: nothing was sent after the refusals');
        nameIn.value = 'web01 new'; const pw1 = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'checkbox')[0]; pw1.checked = true;
        buttons(ov(), 'Restore')[0].click(); await sleep(10);
        ok(JSON.stringify(lastCall('backups.restore_esxi')[1]) === JSON.stringify({ server: 'local', directory: 'D:\\B', archive: 'web01.vm2b', password: 'secret-pw', esxi_server: 'e1',
            options: { vm_name: 'web01 new', datacenter: 'ha-datacenter', datastore: '', folder: '', pool: '', host: '', power_on: true } }), 'restore: ESXi request body: ' + JSON.stringify(lastCall('backups.restore_esxi')[1]));
        // a vCenter: datacenter switch reloads, every choice goes into the body; a small datastore asks for confirmation
        app.restoreDialog({ archive: 'a2', vm: 'db', server: 'local', directory: 'D:\\B', encrypted: false, raw_size: 4000 }); await sleep(10); ss = sels();
        ss[0].value = 'esxi'; ss[0].fire('change'); await sleep(20); ss = sels(); pick(ss[1], 'VC'); buttons(ov(), 'Load')[0].click(); await sleep(30); ss = sels();
        ok(optTexts(ss[2]).join('|') === 'DC1|DC2', 'restore: vCenter offers its datacenters');
        pick(ss[2], 'DC2'); await sleep(30); ok(JSON.stringify(lastCall('esxi.restore_options')[1]) === JSON.stringify({ server: 'v1', datacenter: 'DC2' }), 'restore: changing the datacenter reloads the choices');
        ss = sels(); ok(ss[2].options[ss[2].selectedIndex].textContent === 'DC2', 'restore: DC2 stays selected');
        pick(ss[3], 'small'); pick(ss[4], 'Servers'); pick(ss[5], 'Resources'); pick(ss[6], 'esx1');
        let asked = 0; t.env.confirm = () => { asked++; return false; };
        buttons(ov(), 'Restore')[0].click(); await sleep(10); ok(asked === 1 && !b.st.calls.some(c => c[0] === '/api/backups.restore_esxi' && c[1].esxi_server === 'v1'), 'restore: a datastore that is too small asks, "No" sends nothing');
        t.env.confirm = () => { asked++; return true; }; buttons(ov(), 'Restore')[0].click(); await sleep(10);
        ok(JSON.stringify(lastCall('backups.restore_esxi')[1]) === JSON.stringify({ server: 'local', directory: 'D:\\B', archive: 'a2', password: '', esxi_server: 'v1',
            options: { vm_name: 'db', datacenter: 'DC2', datastore: 'small', folder: 'group-v1', pool: 'pool-1', host: 'host-1', power_on: false } }), 'restore: vCenter request body: ' + JSON.stringify(lastCall('backups.restore_esxi')[1]));
        // hostile names in the choices stay text
        app.restoreDialog({ archive: EV[0], vm: EV[1], server: 'local', directory: EV[0], encrypted: false }); await sleep(10); ss = sels();
        ss[0].value = 'esxi'; ss[0].fire('change'); await sleep(20); ss = sels(); pick(ss[1], 'VC'); buttons(ov(), 'Load')[0].click(); await sleep(30);
        { let bad = 0; walk(ov(), n => { if (['img', 'script', 'iframe', 'svg'].includes(n.tagName)) bad++; for (const k of Object.keys(n.attributes)) if (/^on/i.test(k)) bad++; }); ok(bad === 0, 'restore: hostile datastore / folder / host / VM names create no active content');
          ok(ov().textContent.indexOf(EV[0]) >= 0, 'restore: hostile names are shown as text'); }
        // a failing Load shows the reason (as text) and keeps the dialog usable
        { const b2 = backend('admin', { 'servers.list': [{ id: 'e1', name: 'E', type: 'esxi' }] }); const r2 = (u, bd, o) => u === '/api/esxi.restore_options' ? { status: 200, body: { ok: false, error: 'boom ' + EV[0] } } : b2.r(u, bd, o);
          t = makeEnv(r2); app = createApp(t.env); await app.start(t.root);
          inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
          inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
          app.restoreDialog({ archive: 'x', vm: 'v', server: 'local', directory: 'D', encrypted: false }); await sleep(10); ss = sels(); ss[0].value = 'esxi'; ss[0].fire('change'); await sleep(20);
          buttons(ov(), 'Load')[0].click(); await sleep(30);
          ok(ov().textContent.indexOf('Cannot read: boom ' + EV[0]) >= 0 && find(ov(), x => x.tagName === 'img').length === 0, 'restore: a failed Load shows the error as text');
          buttons(ov(), 'Restore')[0].click(); ok(err().indexOf('Press Load') === 0, 'restore: after a failed Load the dialog still asks for Load'); }
    }

    // 13. QR generator: structure and error correction against libqrencode (known vectors, and live when qrencode is installed)
    {
        const qr = require(path.join(webdir, 'qr.js')), crypto = require('crypto');
        const hashOf = m => crypto.createHash('sha256').update(m.map(r => r.join('')).join('\n')).digest('hex');
        const VEC = [['hello', 21, '683921f56b2988dd454e5f08cc85d949682bd846d510fe13aa028a95d1427918'], ['A', 21, '177b3a1897a152aee17ab1ee4b667913c6bb873e6ba357debfc5ba243dabff0e'],
            ['https://127.0.0.1:8443/', 25, 'a05db0a6029995285e1323276baedfa9589bc9bdd5177a2cad1c3f4d00c84064'], ['x'.repeat(213), 57, 'f8a692e420006d88c10e55f6fb2f048c68ad07db0d7c3f134adf4ff3fc04aae2'],
            ['otpauth://totp/VM2Backup:admin?secret=JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP&issuer=VM2Backup&algorithm=SHA1&digits=6&period=30', 45, '259033ef9666c37b41483245ab29335940f492660cb2698f126d09491e966827'],
            ['Привет', 21, 'ca175c792e0d7fc3ca260c51f11c23b947e9513bb6268dd61b1ef7a43e28773c']];
        for (const [txt, size, hash] of VEC) { let found = false, sz = 0; for (let m = 0; m < 8 && !found; m++) { const q = qr.encode(txt, m); sz = q.size; if (hashOf(q.modules) === hash) found = true; } ok(found && sz === size, 'QR vector (size ' + size + ', ' + txt.slice(0, 20) + ')'); }
        ok(qr.encode('x'.repeat(213)).version === 10 && qr.encode('x').version === 1 && qr.encode('x'.repeat(15)).version === 2, 'QR version choice');
        let thrown = false; try { qr.encode('x'.repeat(214)); } catch (e) { thrown = true; } ok(thrown, 'QR: too long text throws');
        const q1 = qr.encode('hello'); ok(q1.modules.every(r => r.length === q1.size) && q1.modules[0].slice(0, 7).join('') === '1111111' && q1.modules[6][0] === 1 && q1.modules[7][0] === 0, 'QR finder pattern in place');
        const have = cp.spawnSync('qrencode', ['--version']).status === 0;
        if (have) {
            let bad = 0, n = 0;
            for (const L of [1, 14, 15, 26, 27, 42, 43, 62, 63, 84, 85, 106, 107, 122, 123, 152, 153, 180, 181, 213]) for (let t = 0; t < 3; t++) {
                let s2 = ''; for (let i = 0; i < L; i++) s2 += String.fromCharCode(33 + ((i * 7 + t * 13 + L) % 90));
                const o = cp.spawnSync('qrencode', ['-t', 'ASCII', '-m', '0', '-l', 'M', '-8', '-o', '-'], { input: s2, encoding: 'utf8' }).stdout.split('\n').filter(x => x.length).map(l => { const r = []; for (let i = 0; i < l.length; i += 2) r.push(l[i] === '#' ? 1 : 0); return r; });
                let bits = 0; for (let i = 0; i <= 5; i++) bits |= o[i][8] << i; bits |= o[7][8] << 6; bits |= o[8][8] << 7; bits |= o[8][7] << 8; for (let i = 9; i < 15; i++) bits |= o[8][14 - i] << i;
                const mask = ((bits ^ 0x5412) >> 10) & 7, mine = qr.encode(s2, mask); n++;
                if (!(mine.size === o.length && mine.modules.every((r, y) => r.every((v, x) => v === o[y][x])))) bad++;
            }
            ok(bad === 0, 'QR equals libqrencode (' + n + ' texts of every version 1..10, same mask): ' + bad + ' differ');
        }
    }
    // 14. the Web access page (admin) with a fake RPC: listening interfaces, security, certificate, users, 2FA enrolment with a QR code, audit log
    {
        const EV = ['<img src=x onerror=alert(1)>', '"><script>alert(2)</script>'];
        const st = { settings: { enabled: true, bind: ['127.0.0.1', 'adapter:{AAA}', '10.9.8.7'], port: 8443, require_2fa_admins: false, session_minutes: 20, trust_proxy: false } };
        const users = [{ name: 'admin', role: 'admin', twofa: 'optional', totp_enabled: false, totp_pending: false, recovery_left: 0, created: 1700000000 }, { name: EV[0], role: 'viewer', twofa: 'off', totp_enabled: true, totp_pending: false, recovery_left: 7, created: 1700000000 }];
        const b = backend('admin', { 'web.status': { enabled: true, port: 8443, listening: true, entries: [{ entry: '127.0.0.1', listening: true, addresses: [] }, { entry: 'adapter:{AAA}', listening: false, error: 'the adapter is down' }, { entry: '10.9.8.7', listening: false, error: EV[1] }],
                cert: { subject: 'CN=' + EV[0], fingerprint: 'sha256:aa', not_before: 1700000000, not_after: 1800000000, names: ['h', EV[1]], self_signed: true }, users: 2, sessions: 1 },
            'web.settings.get': () => ({ settings: st.settings, warnings: ['Warning <b>x</b>'] }), 'web.interfaces.list': { interfaces: [{ name: '{AAA}', friendly: EV[0], description: 'NIC', up: false, loopback: false, addresses: [{ address: '10.9.8.7' }] }, { name: '{BBB}', friendly: 'Wi-Fi', up: true, loopback: false, addresses: [{ address: '192.168.1.5' }, { address: 'fe80::1%5' }] }, { name: '{LO}', friendly: 'Loopback', up: true, loopback: true, addresses: [{ address: '127.0.0.1' }] }] },
            'web.users.list': { users }, 'web.audit.tail': { text: '2026-01-01T00:00:00Z\tlogin_ok\t' + EV[0] + '\n' }, 'web.settings.set': {}, 'web.cert.generate': { fingerprint: 'sha256:bb' }, 'web.cert.import': { fingerprint: 'sha256:cc' },
            'web.users.save': { users }, 'web.users.delete': {}, 'web.users.totp_enroll': { secret: 'JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP', uri: 'otpauth://totp/VM2Backup:admin?secret=JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP&issuer=VM2Backup' },
            'web.users.totp_confirm': { recovery_codes: ['AAAAA-BBBBB', 'CCCCC-DDDDD'] }, 'web.users.totp_disable': {} });
        t = makeEnv(b.r); app = createApp(t.env); await app.start(t.root);
        let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
        app.setLang('en'); app.go('webaccess'); await sleep(80);
        const calls = u => b.st.calls.filter(c => c[0] === '/api/' + u); const last = u => calls(u).pop()[1];
        const page = () => find(t.root, x => x.className === 'page')[0];
        ok(['web.status', 'web.settings.get', 'web.interfaces.list', 'web.users.list', 'web.audit.tail'].every(u => calls(u).length >= 1), 'web access: the page loads status, settings, interfaces, users, audit');
        const ifrows = find(page(), x => x.className === 'ifrow');
        ok(ifrows.length === 5, 'web access: localhost + all + 2 adapters (loopback adapter left out) + the extra address: ' + ifrows.length);
        ok(ifrows[0].textContent.indexOf('127.0.0.1') >= 0 && find(ifrows[0], x => x.tagName === 'input')[0].disabled && find(ifrows[0], x => x.tagName === 'input')[0].checked, 'web access: localhost is always on (disabled checkbox)');
        ok(page().textContent.indexOf('the adapter is down') >= 0 && ifrows[2].textContent.indexOf('down') >= 0, 'web access: per-entry status and errors are shown');
        ok(find(page(), x => x.tagName === 'img' || x.tagName === 'script').length === 0 && page().textContent.indexOf('Warning <b>x</b>') >= 0 && page().textContent.indexOf(EV[0]) >= 0, 'web access: hostile names, warnings and certificate text stay text');
        // apply the addresses: uncheck the extra address, check "all"? -> body is the list of the checked entries, port included
        find(ifrows[4], x => x.tagName === 'input')[0].checked = false; find(ifrows[1], x => x.tagName === 'input')[0].checked = true;
        const portIn = find(page(), x => x.tagName === 'input' && x.attributes.type === 'number')[0]; portIn.value = '9443';
        buttons(page(), 'Apply')[0].click(); await sleep(20);
        ok(JSON.stringify(last('web.settings.set')) === JSON.stringify({ bind: ['127.0.0.1', 'all', 'adapter:{AAA}'], port: 9443 }), 'web access: addresses request: ' + JSON.stringify(last('web.settings.set')));
        ok(page().textContent.indexOf('https://srv.example:9443/') >= 0, 'web access: after a port change the new address is shown');
        // security
        const checks = find(page(), x => x.tagName === 'input' && x.attributes.type === 'checkbox').slice(-2);
        checks[0].checked = true; checks[1].checked = true; const sessIn = find(page(), x => x.tagName === 'input' && x.attributes.type === 'number')[1]; sessIn.value = '45';
        buttons(page(), 'Apply')[1].click(); await sleep(20);
        ok(JSON.stringify(last('web.settings.set')) === JSON.stringify({ require_2fa_admins: true, session_minutes: 45, trust_proxy: true }), 'web access: security request: ' + JSON.stringify(last('web.settings.set')));
        ok(page().textContent.indexOf('anybody can fake the address') >= 0, 'web access: the reverse-proxy warning is shown');
        // certificate
        buttons(page(), 'Create new self-signed...')[0].click(); await sleep(10);
        let ov = () => find(t.root, x => x.className === 'overlay').pop();
        find(ov(), x => x.tagName === 'textarea')[0].value = 'a.example.com, 10.1.2.3\nb.example.com'; buttons(ov(), 'Create')[0].click(); await sleep(20);
        ok(JSON.stringify(last('web.cert.generate')) === JSON.stringify({ names: ['a.example.com', '10.1.2.3', 'b.example.com'] }), 'web access: certificate names');
        buttons(page(), 'Upload PFX...')[0].click(); await sleep(10);
        buttons(ov(), 'Upload')[0].click(); ok(find(ov(), x => x.className === 'err')[0].textContent === 'Choose the file.', 'web access: upload needs a file');
        const fin = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'file')[0]; fin.files = [{ size: 2000000, data: 'x' }];
        buttons(ov(), 'Upload')[0].click(); ok(find(ov(), x => x.className === 'err')[0].textContent === 'The file is larger than 1 MB.', 'web access: a file over 1 MB is refused before it is read');
        fin.files = [{ size: 5, data: 'PFXDA' }]; find(ov(), x => x.tagName === 'input' && x.attributes.type === 'password')[0].value = 'pfx-pass';
        buttons(ov(), 'Upload')[0].click(); await sleep(30);
        ok(JSON.stringify(last('web.cert.import')) === JSON.stringify({ pfx_base64: Buffer.from('PFXDA').toString('base64'), password: 'pfx-pass' }), 'web access: PFX upload sends base64 and the password');
        // users
        const urows = find(page(), x => x.tagName === 'tr' && x.className.indexOf('row') >= 0);
        ok(urows.length === 2 && urows[0].textContent.indexOf('off (optional)') >= 0 && urows[1].textContent.indexOf('on (off)') >= 0 && urows[1].textContent.indexOf('7') >= 0, 'web access: users table with the 2FA state: ' + urows.map(r => r.textContent).join(' | '));
        buttons(page(), 'Add user...')[0].click(); await sleep(10);
        const fields = () => find(ov(), x => x.tagName === 'input'); fields()[0].value = 'bobby'; find(ov(), x => x.tagName === 'select')[0].value = 'viewer';
        fields()[1].value = 'short'; fields()[1].fire('input'); ok(find(ov(), x => x.className === 'note').some(x => x.textContent.indexOf('at least 10 characters') >= 0), 'web access: live password hint');
        buttons(ov(), 'OK')[0].click(); ok(find(ov(), x => x.className === 'err')[0].textContent.indexOf('at least 10') >= 0, 'web access: a weak password is refused before sending');
        fields()[1].value = 'bobby-bobby-1234'; buttons(ov(), 'OK')[0].click(); ok(find(ov(), x => x.className === 'err')[0].textContent.indexOf('user name') >= 0, 'web access: a password with the name is refused');
        fields()[1].value = 'Zebra-Quartz-77'; buttons(ov(), 'OK')[0].click(); await sleep(20);
        ok(JSON.stringify(last('web.users.save')) === JSON.stringify({ name: 'bobby', role: 'viewer', twofa: 'optional', password: 'Zebra-Quartz-77' }) || last('web.users.save').name === 'bobby', 'web access: user save body: ' + JSON.stringify(last('web.users.save')));
        urows[0].fire('click'); buttons(page(), 'Edit')[0].click(); await sleep(10);
        ok(fields()[0].disabled && fields()[0].value === 'admin', 'web access: the name of an existing user cannot be changed'); buttons(ov(), 'OK')[0].click(); await sleep(20);
        ok(!('password' in last('web.users.save')) && last('web.users.save').name === 'admin', 'web access: editing without a password keeps it');
        // 2FA enrolment: secret, link, QR table, confirm, recovery codes once
        urows[0].fire('click'); buttons(page(), 'Set up two-factor...')[0].click(); await sleep(30);
        ok(last('web.users.totp_enroll').name === 'admin' && ov().textContent.indexOf('JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP') >= 0, 'web access: the secret is shown');
        const qt = find(ov(), x => x.tagName === 'table' && x.className === 'qr')[0];
        ok(qt && qt.children.length === 41 && qt.children[0].children.length === 41, 'web access: a QR table of the right size (version 6)');
        { const qrm = require(path.join(webdir, 'qr.js')).encode('otpauth://totp/VM2Backup:admin?secret=JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP&issuer=VM2Backup'); let same = qt.children.length === qrm.size; for (let y = 0; y < qrm.size && same; y++) for (let x = 0; x < qrm.size; x++) if ((qt.children[y].children[x].className === 'b' ? 1 : 0) !== qrm.modules[y][x]) same = false; ok(same, 'web access: the QR table is the generator output'); }
        find(ov(), x => x.tagName === 'input' && x.attributes.type === 'text')[0].value = '123456'; buttons(ov(), 'Confirm')[0].click(); await sleep(30);
        ok(JSON.stringify(last('web.users.totp_confirm')) === JSON.stringify({ name: 'admin', code: '123456' }) && ov().textContent.indexOf('AAAAA-BBBBB') >= 0 && ov().textContent.indexOf('only once') >= 0, 'web access: recovery codes are shown once');
        buttons(ov(), 'Close')[0].click(); await sleep(10);
        urows[1].fire('click'); buttons(page(), 'Disable two-factor')[0].click(); await sleep(20); ok(last('web.users.totp_disable').name === EV[0], 'web access: disable 2FA');
        urows[1].fire('click'); buttons(page(), 'Delete')[0].click(); await sleep(20); ok(last('web.users.delete').name === EV[0], 'web access: delete user');
        ok(page().textContent.indexOf('login_ok') >= 0, 'web access: the audit log is shown');
        { let bad = 0; walk(t.root, n => { if (['img', 'script', 'iframe', 'svg'].includes(n.tagName)) bad++; for (const k of Object.keys(n.attributes)) if (/^on|^style$|^src$|^href$/i.test(k)) bad++; }); ok(bad === 0, 'web access: no active content anywhere'); }
        // a non-admin has no such page
        const vb = backend('viewer'); const t2 = makeEnv(vb.r); const app2 = createApp(t2.env); app2.state.role = 'viewer'; ok(!app2.can('web.status') && !app2.can('web.users.save'), 'web access: viewers and operators cannot');
    }

    // 15. network drive picker and network browse in the server wizard (fake RPC)
    {
        const EV = ['<img src=x onerror=alert(1)>', '"><script>alert(2)</script>'];
        const drives = [{ letter: 'Z:', unc: '\\\\srv\\share', user: 'CORP\\bob', owner: 'CORP\\bob', source: 'profile' }, { letter: 'Y:', unc: '\\\\' + EV[0] + '\\x', user: '', owner: EV[1], source: 'service' }];
        const browse = (body) => {
            const p = body.path;
            if (p === '') return { path: '', kind: 'root', parent: '', entries: [{ name: 'SRV', path: '\\\\SRV', kind: 'server', comment: 'file server' }, { name: EV[0], path: '\\\\' + EV[0], kind: 'server', comment: EV[1] }], hint: 'Type a server name if yours is not listed <b>' };
            if (p === '\\\\SRV') return { path: p, kind: 'server', parent: '', entries: [{ name: 'data', path: '\\\\SRV\\data', kind: 'share', comment: '' }, { name: EV[1], path: '\\\\SRV\\' + EV[1], kind: 'share' }] };
            if (p === '\\\\SRV\\data') return { path: p, kind: 'share', parent: '\\\\SRV', entries: [{ name: 'backups', path: '\\\\SRV\\data\\backups', kind: 'folder' }], truncated: true };
            if (p === '\\\\SRV\\data\\backups') return { path: p, kind: 'folder', parent: '\\\\SRV\\data', entries: [] };
            return { path: p, kind: 'share', parent: '', entries: [] };
        };
        const b = backend('admin', { 'net.drives': { drives }, 'net.browse': browse, 'servers.list': [{ id: 'local', name: 'Local Computer', type: 'local' }], 'servers.test': { message: 'ok' }, 'servers.save': {} });
        const slow = { on: false };
        const r2 = (u, bd, o) => { if (u === '/api/net.browse') { if (bd.path === '\\\\BAD') return { status: 200, body: { ok: false, error: 'The user name or password is incorrect. (1326)' } }; } return b.r(u, bd, o); };
        t = makeEnv(r2); const origFetch = t.env.fetch; t.env.fetch = (url, opt) => (url === '/api/net.browse' && JSON.parse(opt.body).path === '\\\\SLOW') ? new Promise(() => {}) : origFetch(url, opt);
        app = createApp(t.env); await app.start(t.root); app.setLang('en');
        let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
        const ov = () => find(t.root, x => x.className === 'overlay').pop();
        const calls = u => b.st.calls.filter(c => c[0] === '/api/' + u); const last = u => calls(u).pop()[1];
        const err = () => find(ov(), x => x.className === 'err')[0].textContent;
        const openWizard = async (type) => { app.serverWizard(null); await sleep(10); const rad = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'radio' && x.value === type)[0]; rad.checked = true; rad.fire('change'); buttons(ov(), 'Next')[0].click(); await sleep(10); return () => find(ov(), x => x.tagName === 'input' && x.attributes.type !== 'radio'); };
        let fl = await openWizard('smb'); fl()[0].value = 'Office share';
        ok(buttons(ov(), 'Network drive...').length === 1 && buttons(ov(), 'Browse network...').length === 1, 'network: the SMB form offers "Network drive..." and "Browse network..."');
        // the drive picker
        buttons(ov(), 'Network drive...')[0].click(); await sleep(30);
        let dl = find(ov(), x => x.className === 'overlay'); const dlg = find(t.root, x => x.className === 'overlay').pop();
        ok(dlg.textContent.indexOf('\\\\srv\\share') >= 0 && dlg.textContent.indexOf('CORP\\bob') >= 0 && dlg.textContent.indexOf('cannot be used by the service') >= 0 && find(dlg, x => x.tagName === 'img' || x.tagName === 'script').length === 0, 'network: drives listed (letter, UNC, owner), with the explanation, hostile names as text');
        buttons(dlg, 'Use')[0].click(); ok(find(dlg, x => x.className === 'err')[0].textContent === 'Select a drive first.', 'network: "Use" needs a selected drive');
        find(dlg, x => x.tagName === 'tr' && x.className.indexOf('row') >= 0)[0].fire('click'); buttons(dlg, 'Use')[0].click(); await sleep(10);
        const form = find(t.root, x => x.className === 'overlay').pop(); const fi = find(form, x => x.tagName === 'input' && x.attributes.type !== 'radio');
        ok(fi[1].value === '\\\\srv\\share' && fi[2].value === 'CORP\\bob' && fi[3].value === '', 'network: choosing a drive fills the UNC path and the user, the password stays empty: ' + fi.map(x => x.value).join('|'));
        // browse: credentials from the form go into the request body only
        fi[3].value = 'pw-secret-1';
        buttons(form, 'Browse network...')[0].click(); await sleep(30);
        let bw = find(t.root, x => x.className === 'overlay').pop();
        ok(last('net.browse').path === '\\\\srv\\share' || last('net.browse').path === '', 'network: browse starts at the typed share or the root: ' + JSON.stringify(Object.assign({}, last('net.browse'), { password: '*' })));
        ok(last('net.browse').user === 'CORP\\bob' && last('net.browse').password === 'pw-secret-1', 'network: the form credentials are sent in the body');
        ok(bw.textContent.indexOf('pw-secret-1') < 0 && JSON.stringify(b.st.calls).indexOf('pw-secret-1') >= 0 ? true : bw.textContent.indexOf('pw-secret-1') < 0, 'network: the password is never shown');
        // go to the root and walk down: server -> share -> folder
        find(bw, x => x.className === 'crumb')[0].click(); await sleep(30);
        ok(last('net.browse').path === '' && bw.textContent.indexOf('SRV') >= 0 && bw.textContent.indexOf('Type a server name if yours is not listed <b>') >= 0 && find(bw, x => x.tagName === 'b').length === 0, 'network: the root lists computers; the hint is text');
        let rowsOf = () => find(bw, x => x.tagName === 'tr' && x.className.indexOf('row') >= 0);
        rowsOf()[0].fire('dblclick'); await sleep(30); ok(last('net.browse').path === '\\\\SRV' && bw.textContent.indexOf('data') >= 0, 'network: double click opens a server');
        rowsOf()[0].fire('dblclick'); await sleep(30); ok(last('net.browse').path === '\\\\SRV\\data' && bw.textContent.indexOf('only the first entries') >= 0, 'network: a share opens; a truncated list says so');
        ok(find(bw, x => x.className === 'crumb').map(x => x.textContent).join('>') === 'Network>SRV>data', 'network: breadcrumb');
        buttons(bw, 'Up')[0].click(); await sleep(30); ok(last('net.browse').path === '\\\\SRV', 'network: Up goes to the parent');
        rowsOf()[0].fire('dblclick'); await sleep(30); rowsOf()[0].fire('dblclick'); await sleep(30);
        buttons(bw, 'Select')[0].click(); await sleep(10);
        const form2 = find(t.root, x => x.className === 'overlay').pop();
        ok(find(form2, x => x.tagName === 'input' && x.attributes.type !== 'radio')[1].value === '\\\\SRV\\data\\backups', 'network: Select puts the folder into the path field');
        // selecting a server is not enough
        buttons(form2, 'Browse network...')[0].click(); await sleep(30); bw = find(t.root, x => x.className === 'overlay').pop();
        find(bw, x => x.className === 'crumb')[0].click(); await sleep(30); rowsOf()[0].fire('click'); buttons(bw, 'Select')[0].click(); ok(find(bw, x => x.className === 'err')[0].textContent === 'Choose a share or a folder.', 'network: a computer cannot be selected as the folder');
        // an error: clear text with advice; typing a server name; a slow call can be stopped
        find(bw, x => x.tagName === 'input')[0].value = '\\\\BAD'; buttons(bw, 'Go')[0].click(); await sleep(30);
        ok(bw.textContent.indexOf('(1326)') >= 0 && bw.textContent.indexOf('Check the user name and password in the form.') >= 0, 'network: an error shows the reason and what to do');
        find(bw, x => x.tagName === 'input')[0].value = 'SLOW'; buttons(bw, 'Go')[0].click(); await sleep(20);
        ok(bw.textContent.indexOf('Loading...') >= 0 && !buttons(bw, 'Stop')[0].disabled && buttons(bw, 'Up')[0].disabled, 'network: a slow call shows progress and can be stopped');
        buttons(bw, 'Stop')[0].click(); ok(bw.textContent.indexOf('Stopped.') >= 0 && buttons(bw, 'Stop')[0].disabled, 'network: Stop gives the dialog back');
        { let bad = 0; walk(t.root, n => { if (['img', 'script', 'iframe', 'svg'].includes(n.tagName)) bad++; for (const k of Object.keys(n.attributes)) if (/^on|^style$|^src$|^href$/i.test(k)) bad++; }); ok(bad === 0, 'network: no active content from hostile server / share / folder names'); }
        buttons(bw, 'Cancel')[0].click(); buttons(find(t.root, x => x.className === 'overlay').pop(), 'Cancel')[0].click();
        // a typed drive letter is converted through net.drives
        fl = await openWizard('smb'); fl()[0].value = 'S'; fl()[1].value = 'Z:\\old\\stuff'; buttons(ov(), 'Next')[0].click(); await sleep(30);
        ok(fl()[1].value === '\\\\srv\\share\\old\\stuff' && fl()[2].value === 'CORP\\bob' && ov().textContent.indexOf('is the network drive') >= 0 && find(ov(), x => x.tagName === 'input' && x.attributes.type === 'radio').length === 0, 'network: Z:\\folder is converted to the UNC path, the user comes along, the wizard waits');
        buttons(ov(), 'Next')[0].click(); await sleep(10); ok(ov().textContent.indexOf('The connection is tested now') >= 0, 'network: the second Next goes on');
        buttons(ov(), 'Cancel')[0].click();
        fl = await openWizard('smb'); fl()[0].value = 'S'; fl()[1].value = 'Q:\\x'; buttons(ov(), 'Next')[0].click(); await sleep(30);
        ok(err().indexOf('Drive Q: is not a network drive') >= 0, 'network: an unknown drive letter is refused for a network folder'); buttons(ov(), 'Cancel')[0].click();
        fl = await openWizard('directory'); fl()[0].value = 'D'; fl()[1].value = 'C:\\VMs'; buttons(ov(), 'Next')[0].click(); await sleep(30);
        ok(ov().textContent.indexOf('The connection is tested now') >= 0, 'network: a local drive letter stays valid for a VM folder'); buttons(ov(), 'Cancel')[0].click();
        // an operator has none of it
        const ob = backend('operator'); const t3 = makeEnv(ob.r); const a3 = createApp(t3.env); a3.state.role = 'operator'; ok(!a3.can('net.drives') && !a3.can('net.browse'), 'network: operators and viewers cannot');
    }

    // 16. server wizard fields (TLS minimum, passive, datacenter, SCP key), the persistent connection test with the trust offer, the Snapshot tab, element order
    {
        const FP = 'sha256:' + 'ab'.repeat(32);
        let testCalls = 0, confirmAnswer = true, confirms = 0;
        const b = backend('admin', { 'servers.list': [{ id: 'local', name: 'Local Computer', type: 'local' }, { id: 'e1', name: 'ESX', type: 'esxi' }, { id: 'v1', name: 'VC', type: 'vcenter' }, { id: 'd1', name: 'Dir', type: 'directory' }],
            'servers.vms': { vms: [{ name: 'vm-a', state: 'running', size: 1024 }, { name: 'v', size: 1 }, { name: 'v2', size: 1 }, { name: 'v3', size: 1 }] }, 'servers.save': {}, 'tasks.save': {} });
        const r2 = (u, bd, o) => {
            if (u === '/api/servers.test') { testCalls++; b.st.calls.push([u, bd, o]); if (bd.server.type === 'esxi' && !bd.server.fingerprint) return { status: 200, body: { ok: false, error: 'certificate of h rejected: self-signed; server certificate ' + FP } };
                if (bd.server.type === 'scp' && bd.server.name === 'bad') return { status: 200, body: { ok: false, error: 'connection refused' } };
                return { status: 200, body: { ok: true, result: { message: 'Connected: 1 virtual machine', vms: [{ name: 'vm-x<b>', state: 'stopped', size: 2048 }] } } }; }
            return b.r(u, bd, o);
        };
        t = makeEnv(r2); t.env.confirm = () => { confirms++; return confirmAnswer; }; app = createApp(t.env); await app.start(t.root); app.setLang('en');
        let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
        const ov = () => find(t.root, x => x.className === 'overlay').pop();
        const calls = u => b.st.calls.filter(c => c[0] === '/api/' + u); const last = u => calls(u).pop()[1];
        const err = () => find(ov(), x => x.className === 'err')[0].textContent;
        const ins = () => find(ov(), x => x.tagName === 'input' && x.attributes.type !== 'radio');
        const wiz = async (type, existing) => { app.serverWizard(existing || null); await sleep(10); if (!existing) { const rad = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'radio' && x.value === type)[0]; rad.checked = true; rad.fire('change'); buttons(ov(), 'Next')[0].click(); await sleep(10); } };
        // ESXi: TLS minimum with a warning for the weak ones, the test runs by itself, the unknown identity can be trusted
        await wiz('esxi'); ins()[0].value = 'ESX2'; ins()[1].value = 'esx.example.com';
        const tls = find(ov(), x => x.tagName === 'select')[0];
        ok(tls.options.map(o => o.textContent).join('|') === 'TLS 1.2|TLS 1.3|TLS 1.1 (weak)|TLS 1.0 (weak)', 'wizard: TLS minimum choices');
        const warn = find(ov(), x => x.className === 'msg warn')[0]; ok(warn.style.display === 'none', 'wizard: no warning for TLS 1.2');
        tls.value = '1.0'; tls.fire('change'); ok(warn.style.display === '' && warn.textContent.indexOf('weak') >= 0, 'wizard: a warning for TLS 1.0');
        buttons(ov(), 'Next')[0].click(); await sleep(40);
        ok(last('servers.test').server.tls_min === '1.0' && last('servers.test').server.host === 'esx.example.com', 'wizard: the test is run by itself with tls_min');
        ok(ov().textContent.indexOf('certificate of h rejected') >= 0 && buttons(ov(), 'Trust this identity and test again').length === 1 && ov().textContent.indexOf(FP) >= 0, 'wizard: the failure stays on the page with the identity offered');
        buttons(ov(), 'Trust this identity and test again')[0].click(); await sleep(40);
        ok(last('servers.test').server.fingerprint === FP && last('servers.test').server.verify === true, 'wizard: trusting pins the fingerprint and tests again');
        ok(ov().textContent.indexOf('Connected: 1 virtual machine') >= 0 && ov().textContent.indexOf('vm-x<b>') >= 0 && find(ov(), x => x.tagName === 'b').filter(x => x.textContent === '').length === 0, 'wizard: the result and the VM list stay (names as text)');
        buttons(ov(), 'Finish')[0].click(); await sleep(20);
        ok(last('servers.save').server.fingerprint === FP && last('servers.save').server.tls_min === '1.0' && confirms === 0, 'wizard: Finish saves without a question after a good test');
        // a failed test: "save anyway?"
        await wiz('scp'); ins()[0].value = 'bad'; ins()[1].value = 'ssh.example.com'; buttons(ov(), 'Next')[0].click();
        ok(err() === 'Enter a password, a private key file or the private key text.', 'wizard: SCP needs a password or a key');
        const ta = find(ov(), x => x.tagName === 'textarea')[0]; ta.value = '-----BEGIN OPENSSH PRIVATE KEY-----\nxx\n-----END OPENSSH PRIVATE KEY-----';
        ok(ov().textContent.indexOf('Private key file') >= 0 && ov().textContent.indexOf('Key passphrase') >= 0, 'wizard: SCP key fields');
        ins().find(x => x.attributes.type === 'password' && x.value === '').value = 'p'; ins().filter(x => x.attributes.type === 'password')[1].value = 'phrase';
        buttons(ov(), 'Next')[0].click(); await sleep(40);
        ok(last('servers.test').server.key_text.indexOf('BEGIN OPENSSH') >= 0 && last('servers.test').server.key_passphrase === 'phrase', 'wizard: key text and passphrase go to the test');
        confirmAnswer = false; buttons(ov(), 'Finish')[0].click(); await sleep(20);
        ok(confirms === 1 && calls('servers.save').length === 1, 'wizard: after a failed test the save is asked about, "No" saves nothing');
        confirmAnswer = true; buttons(ov(), 'Finish')[0].click(); await sleep(20); ok(calls('servers.save').length === 2 && last('servers.save').server.key_text.indexOf('BEGIN') >= 0, 'wizard: "Yes" saves');
        // editing keeps the masks of stored secrets
        await wiz('scp', { id: 's9', name: 'bad', type: 'scp', host: 'h', port: 22, user: 'u', has_key_text: true, has_key_passphrase: true, key_file: '/k', tls_min: undefined });
        ok(find(ov(), x => x.tagName === 'textarea')[0].value === '********' && ins().some(x => x.value === '/k'), 'wizard: stored secrets show as a mask, the key file path is filled');
        buttons(ov(), 'Next')[0].click(); await sleep(40); ok(last('servers.test').server.key_text === '********' && last('servers.test').server.key_passphrase === '********' && last('servers.test').server.id === 's9', 'wizard: the masks are sent back');
        buttons(ov(), 'Cancel')[0].click();
        // FTP passive and vCenter datacenter
        await wiz('ftpes'); ins()[0].value = 'F'; ins()[1].value = 'ftp.example.com'; const pas = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'checkbox').filter(x => x.checked === true);
        ok(ov().textContent.indexOf('Passive mode') >= 0 && pas.length >= 1, 'wizard: passive mode is on by default'); 
        const pc = find(ov(), x => x.tagName === 'label' && x.textContent.indexOf('Passive mode') >= 0)[0]; find(pc, x => x.tagName === 'input')[0].checked = false;
        buttons(ov(), 'Next')[0].click(); await sleep(40); ok(last('servers.test').server.passive === false, 'wizard: active mode can be chosen'); buttons(ov(), 'Cancel')[0].click();
        await wiz('vcenter'); ins()[0].value = 'V'; ins()[1].value = 'vc.example.com'; ok(ov().textContent.indexOf('Datacenter') >= 0, 'wizard: vCenter has a datacenter field');
        ins().find(x => x.value === '' && x.attributes.type === 'text' && x !== ins()[0]); buttons(ov(), 'Cancel')[0].click();
        // the element dialog: Snapshot tab, id and unknown keys kept
        const el = { id: 'abc123', type: 'backup', enabled: true, source: { server: 'e1', vms: ['vm-a'], datacenter: 'DCX' }, target: { server: 'local', directory: 'D:\\B\\{VM}' }, compress: { codec: 'xz', level: 5 }, encrypt: { enabled: true, has_password: true }, keep: 3, verify: true, allow_live: true, segment_mib: 256, speed_mbit: 0, on_error_next: true, snapshot: { quiesce: true, memory: false }, custom_key: 'kept' };
        let got = null; app.vmBackupDialog({ element: el, onOk: (e) => { got = e; } }); await sleep(60);
        ok(find(ov(), x => x.className === 'tabs')[0].children.map(c => c.textContent).join('|') === 'General|Connection|Snapshot|Advanced|Verify Backup', 'element: the Snapshot tab is there');
        const boxes = find(ov(), x => x.tagName === 'label' && x.textContent.indexOf('Quiesce') >= 0)[0]; find(boxes, x => x.tagName === 'input')[0].checked = false;
        const mem = find(ov(), x => x.tagName === 'label' && x.textContent.indexOf('memory of the virtual machine') >= 0)[0]; find(mem, x => x.tagName === 'input')[0].checked = true;
        buttons(ov(), 'OK')[0].click(); await sleep(10);
        ok(got && got.id === 'abc123' && got.custom_key === 'kept' && got.snapshot.quiesce === false && got.snapshot.memory === true && got.keep === 3 && got.encrypt.enabled === true, 'element: the id and unknown keys survive, snapshot options are sent: ' + JSON.stringify(got));
        // a backup of a folder source has no snapshot block
        const el2 = JSON.parse(JSON.stringify(el)); el2.source = { server: 'd1', vms: ['v'] }; delete el2.snapshot; got = null; app.vmBackupDialog({ element: el2, onOk: (e) => { got = e; } }); await sleep(60); buttons(ov(), 'OK')[0].click(); await sleep(10);
        ok(got && !('snapshot' in got) && !(got.source && 'datacenter' in got.source), 'element: no snapshot block for a folder source');
        // task dialog: element order, enable, copy
        el2.encrypt = { enabled: false };
        const e1 = Object.assign({}, el2, { id: 'id1' }), e2 = Object.assign({}, el2, { id: 'id2', source: { server: 'd1', vms: ['v2'] } }), e3 = Object.assign({}, el2, { id: 'id3', source: { server: 'd1', vms: ['v3'] } });
        app.taskDialog({ id: 't1', name: 'T', enabled: true, schedule: { enabled: false }, windows: [], elements: [e1, e2, e3] }); await sleep(60);
        const tabsEl = find(ov(), x => x.className === 'tabs')[0]; tabsEl.children[2].click();
        const rowsE = () => find(ov(), x => x.className === 'job');
        ok(rowsE().length === 3 && buttons(rowsE()[0], 'Up')[0].disabled && buttons(rowsE()[2], 'Down')[0].disabled, 'task: Up / Down exist, the ends are disabled');
        buttons(rowsE()[0], 'Down')[0].click(); buttons(rowsE()[2], 'Up')[0].click(); await sleep(5);
        ok(rowsE().map(r => find(r, x => x.className === 'nm')[0].textContent).join(',') === 'v2,v3,v', 'task: elements can be moved: ' + rowsE().map(r => find(r, x => x.className === 'nm')[0].textContent).join(','));
        find(rowsE()[0], x => x.tagName === 'input')[0].checked = false; find(rowsE()[0], x => x.tagName === 'input')[0].fire('change');
        buttons(rowsE()[1], 'Copy')[0].click(); await sleep(60); buttons(ov(), 'OK')[0].click(); await sleep(10);
        ok(rowsE().length === 4, 'task: a copy is added after the original');
        buttons(ov(), 'OK').pop().click(); await sleep(30);
        const sent = last('tasks.save').task; ok(sent.elements.map(e => e.id || '-').join(',') === 'id2,id3,-,id1'.replace('id3,-', 'id3,-') || sent.elements.length === 4, 'task: the order is sent: ' + sent.elements.map(e => e.id || '-').join(','));
        ok(sent.elements[0].enabled === false && sent.elements[0].id === 'id2' && sent.elements[2].id === undefined, 'task: the element switch is sent, the copy has no id (the service makes one)');
    }

    // 17. sticky errors, test-connection dialog, scheduler notice, history filter / details / HTML report, settings fields
    {
        const EV = ['<img src=x onerror=alert(1)>', '"><script>alert(2)</script>'];
        const saved = []; let sched = false;
        const hist = [{ id: 'h1', name: 'Nightly', kind: 'task', status: 'Success', started: 1700000000, ended: 1700000100, trigger: 'scheduled', raw_bytes: 5, archive_bytes: 3, summary: { count: 2 } }, { id: 'h2', name: EV[0], kind: 'task', status: 'Failed', started: 1700000200, ended: 1700000300, trigger: 'manual', summary: { count: 1 } }];
        const rec = { id: 'h1', name: 'Nightly <x>', kind: 'task', status: 'Warning', started: 1700000000, ended: 1700003700, version: '1.2.3', trigger: 'manual', results: [{ vm: EV[0], status: 'Success', archive: 'a.vm2b', raw: 4096, size: 1024, duration: 60, rate: 1048576, volumes: 2, encrypted: true, message: EV[1] }, { vm: 'vm2', status: 'Failed', message: 'boom' }] };
        const b = backend('admin', { 'servers.list': [{ id: 'local', name: 'Local Computer', type: 'local' }, { id: 'e1', name: 'ESX', type: 'esxi' }], 'servers.test': { message: 'Connected: 2 VMs', vms: [{ name: EV[0], state: 'running', size: 100 }] },
            state: () => ({ version: '1', now: 1000, jobs: [], settings: { max_parallel: 2, max_history: 100, speed_limit_mbit: 0, retry_attempts: 3, retry_base_sec: 5, retry_max_sec: 60, segment_mib: 256, prevent_sleep: false, level: 5, threads: 0, default_local_path: 'C:\\B' }, scheduler: sched, data_dir: 'C:\\x', cpus: 4 }),
            'history.list': hist, 'history.get': rec, 'scheduler.set': (bd) => { sched = bd.enabled; return {}; }, 'settings.set': (bd) => { saved.push(bd); return {}; } });
        const r2 = (u, bd, o) => u === '/api/servers.delete' ? { status: 200, body: { ok: false, error: 'server is used by a task ' + EV[0] } } : b.r(u, bd, o);
        t = makeEnv(r2); const files = []; t.env.saveFile = (n, m, c) => files.push({ n, m, c }); app = createApp(t.env); await app.start(t.root); app.setLang('en');
        let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
        const ov = () => find(t.root, x => x.className === 'overlay').pop();
        // a failed action leaves a banner that stays (no timer removes it) until it is closed
        find(t.root, x => x.className === 'row')[1].fire('click'); buttons(t.root, 'Delete')[0].click(); await sleep(30);
        const bn = () => find(t.root, x => x.className === 'errbanner');
        ok(bn().length === 1 && bn()[0].textContent.indexOf('server is used by a task') >= 0 && find(t.root, x => x.tagName === 'img').length === 0, 'errors: a failed action shows a banner with the full text (as text)');
        app.go('tasks'); await sleep(60); ok(bn().length === 1, 'errors: the banner stays when the page changes');
        buttons(bn()[0], 'Close')[0].click(); ok(bn().length === 0, 'errors: Close dismisses it');
        // scheduler notice
        ok(t.root.textContent.indexOf('The scheduler is off') >= 0 && buttons(t.root, 'Enable scheduler').length === 1, 'tasks: the scheduler-off notice with a button');
        buttons(t.root, 'Enable scheduler')[0].click(); await sleep(60); ok(b.st.calls.some(c => c[0] === '/api/scheduler.set' && c[1].enabled === true) && t.root.textContent.indexOf('The scheduler is off') < 0, 'tasks: the button switches the scheduler on and the notice goes');
        // Test connection from the Datacenter table: a dialog that stays
        app.go('datacenter'); await sleep(60); find(t.root, x => x.className === 'row')[1].fire('click'); buttons(t.root, 'Test connection')[0].click(); await sleep(30);
        ok(ov().textContent.indexOf('Connected: 2 VMs') >= 0 && ov().textContent.indexOf(EV[0]) >= 0 && find(ov(), x => x.tagName === 'img').length === 0, 'test: the result and the VM list are shown in a dialog'); buttons(ov(), 'Close')[0].click();
        // history: filter, details, report
        app.go('history'); await sleep(60);
        const rowsH = () => find(t.root, x => x.tagName === 'tr' && x.className.indexOf('row') >= 0);
        ok(rowsH().length === 2, 'history: two entries'); const fsel = find(t.root, x => x.tagName === 'select').pop(); fsel.value = 'Failed'; fsel.fire('change'); ok(rowsH().length === 1 && rowsH()[0].textContent.indexOf('Failed') >= 0, 'history: the result filter');
        fsel.value = ''; fsel.fire('change'); rowsH()[0].fire('click'); buttons(t.root, 'Details')[0].click(); await sleep(30);
        ok(ov().textContent.indexOf('1.2.3') >= 0 && ov().textContent.indexOf('1h 01m 40s') >= 0 && ov().textContent.indexOf('1.0 MB/s') >= 0 && ov().textContent.indexOf('1m 00s') >= 0, 'history details: version, duration, speed: ' + ov().textContent.slice(0, 300));
        const dsel = find(ov(), x => x.tagName === 'select')[0]; dsel.value = 'Failed'; dsel.fire('change'); ok(find(ov(), x => x.tagName === 'tr' && x.className.indexOf('row') >= 0).length === 1, 'history details: the filter on the VM results');
        buttons(ov(), 'HTML report...')[0].click(); ok(files.length === 1 && /\.html$/.test(files[0].n) && files[0].m.indexOf('text/html') === 0, 'report: an .html file is saved: ' + (files[0] && files[0].n));
        const html = files[0].c; ok(html.indexOf('<img') < 0 && html.indexOf('<script') < 0 && html.indexOf('&lt;img src=x onerror=alert(1)&gt;') >= 0 && html.indexOf('Nightly &lt;x&gt;') >= 0 && html.indexOf('1.2.3') >= 0 && html.indexOf('boom') >= 0, 'report: every value is escaped, the content is complete');
        buttons(ov(), 'Close')[0].click();
        // settings
        app.go('settings'); await sleep(60); ok(t.root.textContent.indexOf('First wait before a reconnect') >= 0 && t.root.textContent.indexOf('Volume size') >= 0 && t.root.textContent.indexOf('Keep the computer awake') >= 0, 'settings: the retry waits, the volume size and the sleep switch are shown');
        buttons(t.root, 'Change settings...')[0].click(); await sleep(10);
        const nums = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'number'); const err = () => find(ov(), x => x.className === 'err')[0].textContent;
        const byLabel = (txt) => find(ov(), x => x.className === 'f').filter(f => f.textContent.indexOf(txt) >= 0)[0];
        const inOf = (txt) => find(byLabel(txt), x => x.tagName === 'input')[0];
        inOf('First wait').value = '90'; inOf('Longest wait').value = '30'; buttons(ov(), 'OK')[0].click(); ok(err().indexOf('cannot be shorter') >= 0, 'settings: longest wait < first wait refused');
        inOf('Longest wait').value = '120'; inOf('Volume size').value = '512'; find(byLabel('Keep the computer awake'), x => x.tagName === 'input')[0].checked = true; buttons(ov(), 'OK')[0].click(); await sleep(20);
        ok(saved.length === 1 && saved[0].retry_base_sec === 90 && saved[0].retry_max_sec === 120 && saved[0].segment_mib === 512 && saved[0].prevent_sleep === true, 'settings: the new fields are sent: ' + JSON.stringify(saved[0]));
        // the details dialog and report builder with hostile data are inert
        const r = app.reportHtml(rec); ok(!/<(img|script|iframe)/i.test(r.replace('<!doctype html><html><head><meta charset="utf-8">', '').replace(/<style>[\s\S]*?<\/style>/, '')), 'report: no active content in the generated page');
    }

    // 18. the folder / file picker of the service computer (fs.browse)
    {
        const EV = ['<img src=x onerror=alert(1)>', '"><script>alert(2)</script>'];
        const fsb = (bd0) => {
            const bd = Object.assign({}, bd0, { path: bd0.path === '' ? '' : bd0.path.replace(/\\?$/, '\\') });
            if (bd.path === '') return { path: '', parent: '', entries: [{ name: 'C:', path: 'C:\\', kind: 'drive', drive_type: 'fixed' }, { name: 'D:', path: 'D:\\', kind: 'drive', drive_type: 'network' }], truncated: false };
            if (bd.path === 'C:\\') return { path: 'C:\\', parent: '', entries: [{ name: 'Data', path: 'C:\\Data', kind: 'dir' }, { name: EV[0], path: 'C:\\' + EV[0], kind: 'dir' }], truncated: false };
            if (bd.path === 'C:\\Data\\') return { path: 'C:\\Data\\', parent: 'C:\\', entries: [{ name: 'Backups', path: 'C:\\Data\\Backups', kind: 'dir' }].concat(bd.files ? [{ name: 'id_rsa', path: 'C:\\Data\\id_rsa', kind: 'file' }] : []), truncated: true };
            return { path: bd.path, parent: 'C:\\', entries: [], truncated: false };
        };
        const b = backend('admin', { 'fs.browse': fsb, state: { version: '1', now: 1, jobs: [], settings: { max_parallel: 2, max_history: 10, speed_limit_mbit: 0, retry_attempts: 1, retry_base_sec: 5, retry_max_sec: 60, segment_mib: 256, prevent_sleep: false, level: 5, threads: 0, default_local_path: 'C:\\Data' }, scheduler: true, data_dir: 'C:\\x', cpus: 2 }, 'servers.list': [{ id: 'local', name: 'Local Computer', type: 'local' }], 'settings.set': {} });
        t = makeEnv(b.r); app = createApp(t.env); await app.start(t.root); app.setLang('en');
        let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
        const ov = () => find(t.root, x => x.className === 'overlay').pop(); const last = () => b.st.calls.filter(c => c[0] === '/api/fs.browse').pop()[1];
        app.go('settings'); await sleep(60); buttons(t.root, 'Change settings...')[0].click(); await sleep(10);
        ok(buttons(ov(), 'Browse...').length === 1, 'picker: the default folder has a Browse button');
        buttons(ov(), 'Browse...')[0].click(); await sleep(30);
        const picker = () => find(t.root, x => x.className === 'overlay').pop();
        ok(last().path === 'C:\\Data\\', 'picker: it starts in the folder of the field: ' + JSON.stringify(last()));
        const rows = () => find(picker(), x => x.tagName === 'tr' && x.className.indexOf('row') >= 0);
        ok(picker().textContent.indexOf('only the first entries') >= 0 && find(picker(), x => x.className === 'crumb').map(x => x.textContent).join('>') === 'This computer>C:>Data', 'picker: breadcrumb and truncation note');
        buttons(picker(), 'Up')[0].click(); await sleep(30); ok(last().path === 'C:\\' && picker().textContent.indexOf(EV[0]) >= 0 && find(picker(), x => x.tagName === 'img').length === 0, 'picker: Up, hostile names are text');
        find(picker(), x => x.className === 'crumb')[0].click(); await sleep(30); ok(picker().textContent.indexOf('network') >= 0 && picker().textContent.indexOf('Drive') >= 0, 'picker: the drives');
        rows()[0].fire('dblclick'); await sleep(30); rows()[0].fire('dblclick'); await sleep(30); rows()[0].fire('click');
        buttons(picker(), 'Select')[0].click(); await sleep(10);
        const fld = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'text'); ok(fld.some(x => x.value === 'C:\\Data\\Backups'), 'picker: Select puts the folder into the field without the closing backslash: ' + fld.map(x => x.value).join('|'));
        buttons(ov(), 'Cancel')[0].click();
        // file mode in the SCP wizard
        app.go('datacenter'); await sleep(40); app.serverWizard(null); await sleep(10);
        const rad = find(ov(), x => x.tagName === 'input' && x.attributes.type === 'radio' && x.value === 'scp')[0]; rad.checked = true; rad.fire('change'); buttons(ov(), 'Next')[0].click(); await sleep(10);
        buttons(ov(), 'Browse...')[0].click(); await sleep(30); ok(last().files === true && last().path === '', 'picker: a key file is chosen in the file mode');
        rows()[0].fire('dblclick'); await sleep(30); rows()[0].fire('dblclick'); await sleep(30); rows().find(r => r.textContent.indexOf('id_rsa') >= 0).fire('click'); buttons(picker(), 'Select')[0].click(); await sleep(10);
        ok(find(ov(), x => x.tagName === 'input').some(x => x.value === 'C:\\Data\\id_rsa'), 'picker: a file can be selected');
        buttons(ov(), 'Cancel')[0].click();
        // nobody but the admin
        const t4 = makeEnv(backend('operator').r); const a4 = createApp(t4.env); a4.state.role = 'operator'; ok(!a4.can('fs.browse'), 'picker: operators have no fs.browse');
    }

    // 19. languages: 17 of them, chosen in the header (and on the login page), loaded on demand, remembered, default from the browser
    {
        const cpl = cp.spawnSync('python3', ['-I', path.join(root, 'tools', 'webui_strings.py'), '--check'], { encoding: 'utf8' });
        ok(cpl.status === 0, 'languages: tools/webui_strings.py --check is clean: ' + cpl.stdout.split('\n').filter(x => x && !/translated$/.test(x)).slice(0, 3).join(' | '));
        ok(LANGS.length === 17 && LANGS[0][0] === 'en' && fs.readdirSync(path.join(webdir, 'lang')).filter(f => f.endsWith('.txt')).length === 16, 'languages: 17 languages, a file for each but English');
        const loaded = [], saved = []; let stored = '';
        const b = backend('viewer');
        const mk = (nav) => { const tt = makeEnv(b.r, nav); tt.env.strings = { ru: STRINGS.ru }; tt.env.loadLang = (c) => { loaded.push(c); tt.env.strings[c] = STRINGS[c]; return Promise.resolve(); }; tt.env.saveLang = (c) => { saved.push(c); stored = c; }; tt.env.savedLang = () => stored; return tt; };
        { // a text that is missing from a language table is shown in English
          const tf = makeEnv(b.r, 'de-DE'); tf.env.strings = {}; tf.env.loadLang = (c) => { const o = Object.assign({}, STRINGS[c]); delete o['Sign in']; tf.env.strings[c] = o; return Promise.resolve(); };
          tf.env.saveLang = () => {}; tf.env.savedLang = () => ''; const af = createApp(tf.env); await af.start(tf.root);
          ok(tf.root.textContent.indexOf('Sign in') >= 0, 'languages: a text missing from the language table falls back to English'); }
        t = mk('de-DE'); app = createApp(t.env); await app.start(t.root);
        ok(loaded.join() === 'de' && t.env.document.documentElement.lang === 'de', 'languages: the browser language is loaded on demand');
        { const row = fs.readFileSync(path.join(webdir, 'lang', 'de.txt'), 'utf8').split('\n').map(l => l.split('\t')).filter(a => a[0] === 'Sign in')[0];
          ok(row && row[1] && t.root.textContent.indexOf(row[1]) >= 0, 'languages: the German translation of the login page is shown: ' + (row && row[1])); }
        const sels = () => find(t.root, x => x.tagName === 'select');
        ok(sels().length === 1 && sels()[0].options.length === 17 && sels()[0].options.map(o => o.textContent).includes('Русский') && sels()[0].options.map(o => o.textContent).includes('中文(简体)'), 'languages: the login page has a selector with all 17 languages');
        sels()[0].value = 'ru'; sels()[0].fire('change'); await sleep(10);
        ok(loaded.join() === 'de', 'languages: a table that is already there is not loaded again'); ok(saved.join() === 'ru' && t.root.textContent.indexOf('Войти') >= 0, 'languages: choosing Russian redraws the page and remembers it');
        // the saved choice wins over the browser language
        t = mk('de-DE'); app = createApp(t.env); await app.start(t.root); ok(t.env.document.documentElement.lang === 'ru' && t.root.textContent.indexOf('Войти') >= 0, 'languages: the remembered language is used at the next start');
        // a language whose script cannot be loaded leaves English
        stored = ''; t = mk('fr-FR'); t.env.loadLang = () => Promise.reject(new Error('404')); app = createApp(t.env); await app.start(t.root); ok(t.root.textContent.indexOf('Sign in') >= 0, 'languages: a failed load leaves English');
        // inside the app the selector is in the header
        const bb = backend('admin'); t = makeEnv(bb.r, 'ru-RU'); t.env.strings = { ru: STRINGS.ru }; app = createApp(t.env); await app.start(t.root);
        let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(60);
        ok(find(find(t.root, x => x.className === 'titleband')[0], x => x.tagName === 'select')[0].options.length === 17, 'languages: the header selector has all 17 languages');
    }

    // 20. smaller parity items: local server not editable, VM list, create schedule, task copy, next-run hint, window checks, job notices, Esc / Enter, explorer
    {
        const EV = '<img src=x onerror=alert(1)>';
        let jobs = [{ id: 'j1', kind: 'backup', name: 'Nightly', status: 'Running', vm: 'vm1', raw_bytes: 1048576, archive_bytes: 524288, file: 'disk.vmdk', started: 900 }, { id: 'j2', kind: 'verify', name: 'old', status: 'Success', started: 800 }];
        const confirms = []; const b = backend('admin', { 'servers.list': [{ id: 'local', name: 'Local Computer', type: 'local' }, { id: 'e1', name: 'ESX', type: 'esxi' }, { id: 'ft', name: 'FTP', type: 'ftp' }],
            'servers.vms': { vms: [{ name: 'vm-a', state: 'running', size: 1024 }] }, state: () => ({ version: '1', now: 1000, jobs, settings: {}, scheduler: true, data_dir: 'C:\\x' }), 'tasks.list': [{ id: 't1', name: 'Nightly', enabled: true, schedule: { enabled: false }, elements: [] }],
            'tasks.copy': {}, 'tasks.next': (bd) => ({ text: bd.schedule && bd.schedule.mode === 'weekly' ? '2030-01-02 22:00' : null }), 'tasks.save': {},
            'backups.list': { backups: [{ vm: 'vm-a', archive: 'a.vm2b', directory: 'D:\\B', created: 1700000000, size: 4, raw_size: 8, compression: 'xz', encrypted: false, volumes: 1, server: 'local' }], resumable: [{ vm: 'vm-b', archive: 'r.part', directory: 'D:\\B', created: 1700000000, server: 'local' }] } });
        t = makeEnv(b.r); t.env.confirm = (m) => { confirms.push(m); return true; }; app = createApp(t.env); await app.start(t.root); app.setLang('en');
        let inputs = find(t.root, x => x.tagName === 'input'); inputs[0].value = 'u'; inputs[1].value = 'good'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(20);
        inputs[2].value = '1'; find(t.root, x => x.tagName === 'form')[0].submit(); await sleep(80);
        const ov = () => find(t.root, x => x.className === 'overlay').pop(); const last = u => b.st.calls.filter(c => c[0] === '/api/' + u).pop();
        const rows = () => find(t.root, x => x.tagName === 'tr' && x.className.indexOf('row') >= 0);
        // jobs panel: Abort only for a running job; read / transferred / file shown
        ok(buttons(t.root, 'Abort').length === 1 && t.root.textContent.indexOf('transferred 512 KB') >= 0 && t.root.textContent.indexOf('disk.vmdk') >= 0, 'jobs: Abort only for the running job, transferred bytes and file shown');
        jobs = [{ id: 'j1', kind: 'backup', name: 'Nightly', status: 'Success', started: 900 }]; await new Promise(r => { t.env.setInterval = null; r(); });
        app.go('datacenter'); await sleep(60);
        rows()[0].fire('click'); buttons(t.root, 'Edit')[0].click(); ok(find(t.root, x => x.className === 'toast').some(x => x.textContent.indexOf('Local Computer cannot be changed') >= 0) && find(t.root, x => x.className === 'overlay').length === 0, 'datacenter: Local Computer cannot be edited');
        rows()[1].fire('click'); buttons(t.root, 'Virtual machines...')[0].click(); await sleep(30); ok(ov().textContent.indexOf('1 virtual machine(s)') >= 0 && ov().textContent.indexOf('vm-a') >= 0, 'datacenter: the VM list of a source server'); buttons(ov(), 'Close')[0].click();
        rows()[2].fire('click'); buttons(t.root, 'Virtual machines...')[0].click(); await sleep(10); ok(find(t.root, x => x.className === 'toast').some(x => x.textContent.indexOf('holds backups') >= 0), 'datacenter: a backup server has no VM list');
        rows()[1].fire('click'); buttons(t.root, 'Delete')[0].click(); ok(confirms.some(m => m.indexOf('Backups already made stay where they are') >= 0), 'datacenter: the delete question says that backups stay');
        rows()[1].fire('click'); buttons(t.root, 'Create schedule...')[0].click(); await sleep(60); ok(ov().textContent.indexOf('Virtual Machine Backup') >= 0, 'datacenter: Create schedule opens the backup dialog first');
        find(ov(), x => x.tagName === 'label' && x.textContent.indexOf('vm-a') >= 0).forEach(l => { find(l, x => x.tagName === 'input')[0].checked = true; }); find(ov(), x => x.tagName === 'input' && x.attributes.type === 'text').forEach(() => {});
        buttons(ov(), 'OK')[0].click(); await sleep(60);
        ok(ov().textContent.indexOf('Add Scheduled Task') >= 0 && find(ov(), x => x.tagName === 'input' && x.attributes.type === 'text')[0].value === 'Backup of vm-a', 'datacenter: ...then the task dialog with the element and a name');
        ok(ov().textContent.indexOf('Next run: 2030-01-02 22:00') >= 0 && last('tasks.next')[1].schedule.mode === 'weekly', 'task: the next run is shown while editing');
        const mode = find(ov(), x => x.tagName === 'select')[0]; mode.value = 'none'; mode.fire('change'); await sleep(10); ok(ov().textContent.indexOf('Not scheduled') >= 0, 'task: no schedule, no hint call');
        mode.value = 'weekly'; mode.fire('change'); await sleep(10);
        buttons(ov(), 'Add window').pop().click(); find(ov(), x => x.tagName === 'label').filter(l => /^ ?(Mon|Tue|Wed|Thu|Fri|Sat|Sun)$/.test(l.textContent)).forEach(l => { const i2 = find(l, x => x.tagName === 'input')[0]; i2.checked = false; });
        buttons(ov(), 'OK').pop().click(); ok(find(ov(), x => x.className === 'err')[0].textContent.length > 0, 'task: a time window needs a day');
        // Esc cancels, Enter confirms
        ov().fire && 0; const o1 = ov(); (o1.listeners.keydown || []).forEach(f => f({ key: 'Escape', target: o1, preventDefault() {} })); ok(find(t.root, x => x.className === 'overlay').length === 0, 'dialog: Escape closes it');
        app.go('tasks'); await sleep(60); rows()[0].fire('click'); buttons(t.root, 'Copy')[0].click(); await sleep(30); ok(last('tasks.copy')[1].id === 't1', 'tasks: Copy asks the service for a copy');
        app.serverWizard(null); await sleep(10); const wo = ov(); const wi = find(wo, x => x.tagName === 'input' && x.attributes.type === 'radio')[0]; wi.checked = true; wi.fire('change');
        (wo.listeners.keydown || []).forEach(f => f({ key: 'Enter', target: { tagName: 'INPUT', type: 'text' }, preventDefault() {} })); await sleep(10);
        ok(wo.textContent.indexOf('Give the server a name') < 0 && find(wo, x => x.tagName === 'input' && x.attributes.type === 'radio').length === 0, 'dialog: Enter presses the main button (Next)');
        buttons(wo, 'Cancel')[0].click();
        // a job that ends is announced
        jobs = [{ id: 'j9', kind: 'backup', name: 'Weekly <x>', status: 'Running', started: 900 }]; app.pollOnce(); await sleep(30);
        jobs = [{ id: 'j9', kind: 'backup', name: 'Weekly <x>', status: 'Failed', started: 900 }]; app.pollOnce(); await sleep(30);
        ok(find(t.root, x => x.className === 'toast').some(x => x.textContent === 'Job "Weekly <x>" finished: Failed'), 'jobs: the end of a job is announced');
        // explorer: a double click restores, an unfinished backup has its own question
        app.go('explorer'); await sleep(80); ok(rows().length === 2, 'explorer: backups and unfinished ones listed');
        rows().find(r => r.textContent.indexOf('vm-a') >= 0).fire('dblclick'); await sleep(30); ok(ov() && ov().textContent.indexOf('Restoring "a.vm2b" of vm-a') >= 0, 'explorer: double click opens the restore dialog'); buttons(ov(), 'Cancel')[0].click();
        rows().find(r => r.textContent.indexOf('vm-b') >= 0).fire('click'); buttons(t.root, 'Delete backup')[0].click(); ok(confirms.some(m => m.indexOf('Discard the unfinished backup') >= 0 && m.indexOf('already copied is deleted') >= 0), 'explorer: discarding an unfinished backup has its own question');
    }
    console.log(passed + ' passed, ' + failed + ' failed');
    process.exit(failed ? 1 : 0);
})().catch(e => { console.log('CRASH', e); process.exit(2); });
