// VM2Backup administration UI: one page, plain JavaScript, no libraries, nothing from the internet.
// All text is built with textContent / text nodes (never parsed as markup), so data from the service cannot inject HTML.
// createApp(env) takes everything it needs from env (document, fetch, ...) so that the logic runs under node with a fake DOM.
function createApp(env) {
    'use strict';
    var doc = env.document;
    var strings = env.strings || {};
    var LANG = 'en';
    var S = { user: '', role: '', csrf: '', view: 'datacenter', state: null, servers: [], tasks: [], history: [], sel: {}, timer: null, tick: 0 };
    var root = null;

    // ---------- language: the English text is the key, like TR("English") in the program ----------
    function T(s) {
        var t = strings[LANG] && strings[LANG][s];
        var out = t !== undefined ? t : s;
        for (var i = 1; i < arguments.length; i++) out = out.split('{' + (i - 1) + '}').join(String(arguments[i]));
        return out;
    }
    // the languages: env.languages = [[code, native name], ...] (all 17); a table is loaded on demand (env.loadLang(code) -> Promise) and may be missing or partly empty: English is shown then
    function langList() {
        if (env.languages && env.languages.length) return env.languages;
        var r = [['en', 'English']]; for (var k in strings) if (Object.prototype.hasOwnProperty.call(strings, k)) r.push([k, k.toUpperCase()]); return r;
    }
    function knownLang(l) { var a = langList(); for (var i = 0; i < a.length; i++) if (a[i][0] === l) return true; return false; }
    function pickLang(nav) {
        var l = ((nav && (nav.language || (nav.languages && nav.languages[0]))) || 'en').toLowerCase().split('-')[0];
        return knownLang(l) ? l : 'en';
    }
    function setLang(l) { LANG = (l === 'en' || strings[l] || knownLang(l)) ? l : 'en'; try { doc.documentElement.lang = LANG; } catch (e) { /* fake DOM */ } }
    function langs() { return langList().map(function (x) { return x[0]; }); }
    function ensureLang(code) {
        if (code === 'en' || strings[code] || !env.loadLang) return Promise.resolve();
        return Promise.resolve(env.loadLang(code)).then(function () {}, function () {});
    }
    // a language chosen by the user: loaded, remembered, the page redrawn by again
    function chooseLang(code, again) {
        return ensureLang(code).then(function () { setLang(code); if (env.saveLang) env.saveLang(code); again(); });
    }
    function langSelect(again) {
        var sel2 = h('select', { title: T('Language'), on: { change: function () { chooseLang(sel2.value, again); } } });
        langList().forEach(function (l) { sel2.appendChild(h('option', { value: l[0], selected: l[0] === LANG, text: l[1] })); });
        return sel2;
    }

    // ---------- permissions (the service enforces them; this only hides what would be refused) ----------
    var VIEWER = ['state', 'servers.list', 'tasks.list', 'tasks.next', 'jobs.list', 'history.list', 'history.get', 'backups.list'];
    var OPERATOR = VIEWER.concat(['tasks.run', 'jobs.abort', 'backup.run', 'backups.verify']);
    var ADMIN = OPERATOR.concat(['scheduler.set', 'settings.get', 'settings.set', 'servers.save', 'servers.delete', 'servers.unblock', 'servers.test', 'servers.vms', 'tasks.save', 'tasks.copy', 'tasks.delete', 'history.clear', 'backups.restore', 'backups.restore_esxi', 'esxi.restore_options', 'net.drives', 'net.browse', 'fs.browse', 'backups.delete', 'resumable.discard',
        'web.status', 'web.settings.get', 'web.settings.set', 'web.interfaces.list', 'web.cert.generate', 'web.cert.import', 'web.users.list', 'web.users.save', 'web.users.delete', 'web.users.totp_enroll', 'web.users.totp_confirm', 'web.users.totp_disable', 'web.audit.tail']);
    function can(cmd) {
        if (S.role === 'admin') return ADMIN.indexOf(cmd) >= 0;
        if (S.role === 'operator') return OPERATOR.indexOf(cmd) >= 0;
        if (S.role === 'viewer') return VIEWER.indexOf(cmd) >= 0;
        return false;
    }

    // ---------- small helpers ----------
    function h(tag, attrs) {
        var el = doc.createElement(tag);
        if (attrs) for (var k in attrs) {
            if (!Object.prototype.hasOwnProperty.call(attrs, k)) continue;
            var v = attrs[k];
            if (v === undefined || v === null || v === false) continue;
            if (k === 'class') el.className = v;
            else if (k === 'text') el.textContent = v;
            else if (k === 'on') { for (var ev in v) el.addEventListener(ev, v[ev]); }
            else if (k === 'value' || k === 'checked' || k === 'disabled' || k === 'selected' || k === 'min' || k === 'max') el[k] = v;
            else el.setAttribute(k, v === true ? '' : String(v));
        }
        for (var i = 2; i < arguments.length; i++) add(el, arguments[i]);
        return el;
    }
    function add(el, c) {
        if (c === null || c === undefined || c === false) return;
        if (Array.isArray(c)) { for (var i = 0; i < c.length; i++) add(el, c[i]); return; }
        el.appendChild(typeof c === 'string' || typeof c === 'number' ? doc.createTextNode(String(c)) : c);
    }
    function clear(el) { while (el.firstChild) el.removeChild(el.firstChild); }
    function fmtSize(n) {
        n = Number(n) || 0; var u = ['B', 'KB', 'MB', 'GB', 'TB'], i = 0;
        while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
        return (i === 0 ? String(n) : n.toFixed(n >= 100 ? 0 : 1)) + ' ' + u[i];
    }
    function fmtSpeed(b) { return fmtSize(b) + '/s'; }
    function pad(n) { return (n < 10 ? '0' : '') + n; }
    function fmtTime(sec) {
        if (!sec) return '';
        var d = new Date(sec * 1000);
        return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes());
    }
    function fmtDur(sec) {
        sec = Math.max(0, Math.round(sec)); var hh = Math.floor(sec / 3600), mm = Math.floor(sec % 3600 / 60), ss = sec % 60;
        return hh ? hh + 'h ' + pad(mm) + 'm ' + pad(ss) + 's' : mm ? mm + 'm ' + pad(ss) + 's' : ss + 's';
    }
    function toast(msg) {
        var t = h('div', { class: 'toast', text: msg }); doc.body.appendChild(t);
        if (env.setTimeout) env.setTimeout(function () { if (t.parentNode) t.parentNode.removeChild(t); }, 4000);
    }

    // ---------- the API ----------
    function api(cmd, args) {
        return env.fetch('/api/' + cmd, {
            method: 'POST', credentials: 'same-origin',
            headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': S.csrf },
            body: JSON.stringify(args || {})
        }).then(function (r) {
            return r.json().then(function (j) { return { status: r.status, json: j }; }, function () { return { status: r.status, json: { ok: false, error: 'bad answer' } }; });
        }).then(function (x) {
            if (x.status === 401) { showLogin(T('Your session has ended. Sign in again.')); throw new Error('signed out'); }
            if (!x.json.ok) throw new Error(x.json.error || ('HTTP ' + x.status));
            return x.json.result;
        });
    }
    // an error from an action outside a dialog stays until it is dismissed (and can be selected / copied)
    var errBanner = null;
    function showError(msg) {
        if (errBanner && errBanner.parentNode) errBanner.parentNode.removeChild(errBanner);
        var b = h('div', { class: 'errbanner' }, h('span', { class: 'msgtext', text: msg }), h('button', { text: T('Close'), on: { click: function () { if (b.parentNode) b.parentNode.removeChild(b); errBanner = null; } } }));
        errBanner = b; doc.body.appendChild(b);
    }
    function fail(e) { if (e && e.message !== 'signed out') showError(e.message || String(e)); }

    // ---------- login ----------
    function showLogin(note, needCode, noUsers) {
        stopPolling();
        S.user = ''; S.role = '';
        clear(root);
        var user = h('input', { type: 'text', id: 'user', autocomplete: 'username' });
        var pass = h('input', { type: 'password', id: 'pass', autocomplete: 'current-password' });
        var code = h('input', { type: 'text', id: 'code', autocomplete: 'one-time-code', inputmode: 'numeric' });
        var useRec = false;
        var err = h('div', { class: note ? 'msg ' + (noUsers ? 'warn' : 'err') : '', text: note || '' });
        var codeRow = h('div', { class: 'f' }, h('label', { text: T('Code') }), code);
        var recLink = h('a', { href: '#', text: T('Use a recovery code') });
        codeRow.style.display = needCode ? '' : 'none';
        var btn = h('button', { class: 'primary', type: 'submit', text: T('Sign in') });
        if (noUsers) btn.disabled = true;
        recLink.addEventListener('click', function (e) { e.preventDefault(); useRec = !useRec; recLink.textContent = useRec ? T('Use the authenticator code') : T('Use a recovery code'); codeRow.firstChild.textContent = useRec ? T('Recovery code') : T('Code'); });
        var recRow = h('div', { class: 'note' }, recLink); recRow.style.display = needCode ? '' : 'none';
        var form = h('form', { class: 'login-form', on: { submit: function (e) {
            e.preventDefault(); btn.disabled = true;
            var body = { user: user.value, password: pass.value };
            if (needCode) { if (useRec) body.recovery = code.value; else body.code = code.value; }
            env.fetch('/api/login', { method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })
                .then(function (r) { return r.json(); })
                .then(function (j) {
                    btn.disabled = false;
                    if (j.ok) { S.user = j.user; S.role = j.role; S.csrf = j.csrf; pass.value = ''; showApp(); return; }
                    if (j.need_2fa) { needCode = true; codeRow.style.display = ''; recRow.style.display = ''; err.className = 'msg info'; err.textContent = T('Enter the code from your authenticator app, or a recovery code.'); return; }
                    err.className = 'msg err'; err.textContent = j.error || T('Sign-in failed.');
                    if (j.no_users) btn.disabled = true;
                    pass.value = '';
                }, function () { btn.disabled = false; err.className = 'msg err'; err.textContent = T('Cannot reach the server.'); });
        } } },
            err,
            h('div', { class: 'f' }, h('label', { text: T('User name') }), user),
            h('div', { class: 'f' }, h('label', { text: T('Password') }), pass),
            codeRow, recRow,
            h('div', { class: 'df' }, btn));
        var again = function () { showLogin(note, needCode, noUsers); };
        root.appendChild(h('div', { class: 'login' }, h('div', { class: 'dt', text: 'VM2Backup' }), h('div', { class: 'db' }, form, h('div', { class: 'f' }, h('label', { text: T('Language') }), langSelect(again)))));
    }
    function checkSession() {
        return env.fetch('/api/me', { credentials: 'same-origin' }).then(function (r) { return r.json(); }).then(function (j) {
            if (j.authenticated) { S.user = j.user; S.role = j.role; S.csrf = j.csrf; showApp(); }
            else if (j.no_users) showLogin(T('No users are configured. On the server, run VM2BackupService.exe /reset-web as an administrator.'), false, true);
            else showLogin('');
        }, function () { showLogin(T('Cannot reach the server.')); });
    }

    // ---------- main frame ----------
    var NAV = [['datacenter', 'Datacenter'], ['explorer', 'Backup Explorer'], ['tasks', 'Scheduled Tasks'], ['history', 'Task History'], ['settings', 'Settings'], ['webaccess', 'Web access']];
    var pageEl = null, jobsEl = null, navEl = null;
    function showApp() {
        clear(root);
        var langSel = langSelect(showApp);
        var band = h('div', { class: 'titleband' }, h('span', { class: 'app', text: 'VM2Backup' }), h('span', { class: 'grow' }),
            h('span', { class: 'who', text: S.user + ' (' + T(S.role) + ')' }), langSel,
            h('button', { text: T('Password'), on: { click: passwordDialog } }),
            h('button', { text: T('Sign out'), on: { click: function () { api('logout').then(function () { S.csrf = ''; showLogin(''); }, function () { showLogin(''); }); } } }));
        navEl = h('div', { class: 'nav' });
        NAV.filter(function (n) { return n[0] !== 'webaccess' || can('web.status'); }).forEach(function (n) { navEl.appendChild(h('a', { 'data-view': n[0], text: T(n[1]), on: { click: function () { go(n[0]); } } })); });
        pageEl = h('div', { class: 'page' });
        jobsEl = h('div', { class: 'jobs' });
        root.appendChild(band);
        root.appendChild(h('div', { class: 'main' }, navEl, h('div', { class: 'content' }, pageEl, jobsEl)));
        go(S.view);
        startPolling();
    }
    function go(view) {
        S.view = view; S.sel = {};
        for (var i = 0; i < navEl.children.length; i++) { var a = navEl.children[i]; a.className = a.getAttribute('data-view') === view ? 'on' : ''; }
        refreshView();
    }
    function refreshView() {
        var v = { datacenter: viewDatacenter, explorer: viewExplorer, tasks: viewTasks, history: viewHistory, settings: viewSettings, webaccess: viewWebAccess }[S.view];
        clear(pageEl); if (v) v();
    }

    // ---------- polling: running jobs every second ----------
    function startPolling() { stopPolling(); pollOnce(); S.timer = env.setInterval(pollOnce, 1000); }
    function stopPolling() { if (S.timer) { env.clearInterval(S.timer); S.timer = null; } }
    function pollOnce() {
        if (!S.csrf) return;
        S.tick++;
        api('state').then(function (st) {
            var prev = S.state && S.state.jobs ? S.state.jobs.length : 0, before = S.jobStates || {}, now = {}, fin = [];
            (st.jobs || []).forEach(function (j) { now[j.id] = j.status; if (before[j.id] && (before[j.id] === 'Running' || before[j.id] === 'Waiting') && j.status !== 'Running' && j.status !== 'Waiting') fin.push(j); });
            Object.keys(before).forEach(function (id) { if (!(id in now) && (before[id] === 'Running' || before[id] === 'Waiting')) fin.push({ id: id, name: (S.jobNames || {})[id], status: 'Success' }); });
            S.jobStates = now; S.jobNames = S.jobNames || {}; (st.jobs || []).forEach(function (j) { S.jobNames[j.id] = j.name || j.kind; });
            S.state = st; renderJobs(st.jobs || []);
            fin.forEach(function (j) { toast(T('Job "{0}" finished: {1}', j.name || (S.jobNames || {})[j.id] || j.id, T(j.status))); });
            if ((prev !== (st.jobs || []).length || fin.length) && (S.view === 'history' || S.view === 'tasks' || S.view === 'explorer')) refreshView();     // a finished job changes these lists
        }, fail);
    }
    function renderJobs(jobs) {
        if (!jobsEl) return;
        clear(jobsEl);
        jobsEl.style.display = jobs.length ? '' : 'none';
        if (!jobs.length) return;
        jobsEl.appendChild(h('h3', { text: T('Running jobs') + ' (' + jobs.length + ')' }));
        jobs.forEach(function (j) {
            var running = j.status === 'Running';
            var info = [];
            if (j.kind) info.push(T(j.kind));
            if (j.vm) info.push(j.vm);
            if (j.restored_vm) info.push(T('Restored as') + ' ' + j.restored_vm);
            if (j.phase) info.push(T(j.phase));
            info.push(T(j.status));
            if (j.raw_bytes) info.push(T('read') + ' ' + fmtSize(j.raw_bytes));
            if (j.archive_bytes) info.push(T('transferred') + ' ' + fmtSize(j.archive_bytes));
            if (j.file) info.push(j.file);
            if (j.volumes) info.push(T('{0} volume(s)', j.volumes));
            if (running && j.rate > 0) info.push(fmtSpeed(j.rate));
            if (j.limit_mbit > 0) info.push(T('limit {0} Mbit/s', Number(j.limit_mbit).toFixed(0)));
            if (j.retry) info.push(j.retry);
            if (j.started) info.push(T('running {0}', fmtDur((S.state ? S.state.now : 0) - j.started)));
            jobsEl.appendChild(h('div', { class: 'job' },
                h('div', { class: 'nm', text: j.name || j.kind, title: j.name || '' }),
                h('div', { class: 'info' }, running ? h('div', { class: 'bar' }, h('i')) : null, h('span', { text: info.join('  |  ') + (j.message ? '  |  ' + j.message : '') })),
                can('jobs.abort') && (j.status === 'Running' || j.status === 'Waiting') ? h('button', { class: 'danger', text: T('Abort'), on: { click: function () { api('jobs.abort', { id: j.id }).then(pollOnce, fail); } } }) : null));
        });
    }

    // ---------- generic table with a selection ----------
    function grid(cols, rows, key, onSelect, onOpen) {
        if (!rows.length) return h('div', { class: 'empty', text: T('Nothing here yet.') });
        var tb = h('tbody');
        rows.forEach(function (r, i) {
            var k = key ? key(r, i) : String(i);
            var tr = h('tr', { class: 'row' + (S.sel.k === k ? ' sel' : ''), on: { click: function () {
                S.sel.k = k; S.sel.row = r; for (var q = 0; q < tb.children.length; q++) tb.children[q].className = 'row'; tr.className = 'row sel'; if (onSelect) onSelect(r);
            }, dblclick: function () { if (onOpen) onOpen(r); } } });
            cols.forEach(function (c) {
                var v = c.get(r), td = h('td');
                if (c.cls) td.className = c.cls(r);
                if (v && v.nodeType !== undefined) td.appendChild(v); else td.textContent = v === undefined || v === null ? '' : String(v);
                tr.appendChild(td);
            });
            tb.appendChild(tr);
        });
        return h('table', { class: 'grid' }, h('thead', null, h('tr', null, cols.map(function (c) { return h('th', { text: T(c.title) }); }))), tb);
    }
    function btn(label, cmd, fn, cls) {
        if (cmd && !can(cmd)) return null;
        return h('button', { class: cls || '', text: T(label), on: { click: fn } });
    }
    function needSel(fn) { return function () { if (!S.sel.row) { toast(T('Select a row first.')); return; } fn(S.sel.row); }; }

    // ---------- Datacenter ----------
    function vmTable(vms) { return grid([{ title: 'Virtual machine', get: function (v) { return v.name; } }, { title: 'State', get: function (v) { return v.state || ''; } }, { title: 'Size', get: function (v) { return v.size ? fmtSize(v.size) : ''; } }], vms, function (v) { return v.name; }, null, null); }
    function testResultDialog(srv, x) {
        var keep = { k: S.sel.k, row: S.sel.row };
        var body = h('div', null, h('div', { class: 'msg info', text: x.message || T('The connection works.') }));
        if ((x.vms || []).length) body.appendChild(vmTable(x.vms));
        S.sel.k = keep.k; S.sel.row = keep.row;
        return dialog({ title: 'Test connection', width: 560, body: h('div', null, h('div', { class: 'note', text: srv.name }), body), buttons: [{ label: 'Close', primary: true }] });
    }
    function loadServers() { return api('servers.list').then(function (l) { S.servers = l; return l; }); }
    function editServer(r) { if (r.type === 'local' || r.id === 'local') { toast(T('Local Computer cannot be changed.')); return; } serverWizard(r); }
    function viewDatacenter() {
        var box = h('div');
        pageEl.appendChild(h('h2', { text: T('Datacenter') }));
        pageEl.appendChild(h('div', { class: 'toolbar' },
            btn('Add Server', 'servers.save', function () { serverWizard(null); }, 'primary'),
            btn('Edit', 'servers.save', needSel(function (r) { editServer(r); })),
            btn('Try again', 'servers.unblock', needSel(function (r) { if (!r.auth_blocked) { toast(T('The credentials of this server were not rejected.')); return; } api('servers.unblock', { id: r.id }).then(function () { toast(T('The next run will contact the server again.')); refreshView(); }, fail); })),
            btn('Test connection', 'servers.test', needSel(function (r) { api('servers.test', { id: r.id }).then(function (x) { testResultDialog(r, x); }, fail); })),
            btn('Virtual machines...', 'servers.vms', needSel(function (r) { if (!isSourceType(r.type)) { toast(T('This server holds backups, not virtual machines.')); return; } api('servers.vms', { id: r.id }).then(function (x) { testResultDialog(r, { message: T('{0} virtual machine(s)', (x.vms || []).length), vms: x.vms || [] }); }, fail); })),
            btn('Create schedule...', 'tasks.save', needSel(function (r) { if (!isSourceType(r.type)) { toast(T('Choose a server with virtual machines.')); return; } vmBackupDialog({ server: r.id, onOk: function (e) { taskDialog({ name: T('Backup of {0}', (e.source.vms || []).join(', ')), enabled: true, schedule: { enabled: true, mode: 'weekly', start_time: '22:00', weekdays: [0, 1, 2, 3, 4], week_interval: 1 }, windows: [], elements: [e] }, true); } }); })),
            btn('Backup virtual machines...', 'backup.run', needSel(function (r) { vmBackupDialog({ server: r.id, run: true }); })),
            btn('Delete', 'servers.delete', needSel(function (r) { if (r.id === 'local') { toast(T('Local Computer cannot be deleted.')); return; } if (env.confirm(T('Delete server "{0}"? Backups already made stay where they are.', r.name))) api('servers.delete', { id: r.id }).then(refreshView, fail); }), 'danger'),
            h('button', { text: T('Refresh'), on: { click: refreshView } })));
        pageEl.appendChild(box);
        loadServers().then(function (l) {
            clear(box);
            box.appendChild(grid([
                { title: 'Name', get: function (r) { return r.name; } }, { title: 'Type', get: function (r) { return serverTypeName(r.type); } },
                { title: 'Address', get: function (r) { return r.host ? r.host + (r.port ? ':' + r.port : '') : (r.path || r.directory || ''); } },
                { title: 'User', get: function (r) { return r.user || ''; } },
                { title: 'Password', get: function (r) { return r.has_password ? T('stored') : ''; } },
                { title: 'Problem', get: function (r) { return r.auth_blocked ? h('span', { class: 'bad', text: T('Credentials rejected since {0}: {1}', fmtTime(r.auth_blocked.time), r.auth_blocked.text || '') }) : ''; } }
            ], l, function (r) { return r.id; }, null, can('servers.save') ? editServer : null));
        }, fail);
    }
    var TYPES = [['directory', 'VM folder (VMware Workstation / Player)'], ['esxi', 'VMware ESXi host'], ['vcenter', 'VMware vCenter'], ['ftp', 'FTP'], ['ftpes', 'FTP over explicit TLS (FTPES)'], ['ftps', 'FTP over implicit TLS (FTPS)'], ['scp', 'SCP (SSH)'], ['smb', 'Network folder (SMB)'], ['local', 'Local Computer']];
    function serverTypeName(t) { for (var i = 0; i < TYPES.length; i++) if (TYPES[i][0] === t) return T(TYPES[i][1]); return t === 'sftp' ? T('SFTP (no longer supported)') : t; }
    function isSourceType(t) { return t === 'directory' || t === 'esxi' || t === 'vcenter'; }
    function isTargetType(t) { return t === 'local' || t === 'ftp' || t === 'ftpes' || t === 'ftps' || t === 'scp' || t === 'smb'; }

    // ---------- dialogs ----------
    function dialog(o) {
        var tabEls = [], panes = [];
        var errEl = h('div', { class: 'err' });
        var body = h('div', { class: 'db' });
        var tabs = o.tabs && o.tabs.length > 1 ? h('div', { class: 'tabs' }) : null;
        (o.tabs || [{ name: '', el: o.body }]).forEach(function (t, i) {
            var pane = h('div', null, t.el); pane.style.display = i === 0 ? '' : 'none'; panes.push(pane); body.appendChild(pane);
            if (tabs) { var a = h('a', { text: T(t.name), class: i === 0 ? 'on' : '', on: { click: function () { panes.forEach(function (p, k) { p.style.display = k === i ? '' : 'none'; tabEls[k].className = k === i ? 'on' : ''; }); } } }); tabEls.push(a); tabs.appendChild(a); }
        });
        var foot = h('div', { class: 'df' }, errEl);
        var ov = h('div', { class: 'overlay' });
        var d = { el: ov, close: function () { if (ov.parentNode) ov.parentNode.removeChild(ov); }, error: function (m) { errEl.textContent = m || ''; }, buttons: {} };
        (o.buttons || []).forEach(function (b) {
            var bt = h('button', { class: b.primary ? 'primary' : '', text: T(b.label), on: { click: function () { d.error(''); if (!b.fn) { d.close(); return; } var r = b.fn(d); if (b.close !== false && r !== false) d.close(); } } });
            d.buttons[b.label] = bt; foot.appendChild(bt);
        });
        var box = h('div', { class: 'dialog' }, h('div', { class: 'dt', text: T(o.title) }), tabs, body, foot);
        if (o.width) box.style.width = o.width + 'px';
        // Esc = Cancel / Close, Enter = the main button (not inside a text area or on another button)
        ov.addEventListener('keydown', function (e) {
            if (e.key === 'Escape') { var last = (o.buttons || [])[(o.buttons || []).length - 1]; if (last && d.buttons[last.label]) d.buttons[last.label].click(); else d.close(); }
            else if (e.key === 'Enter' && e.target && e.target.tagName === 'INPUT' && e.target.type !== 'checkbox' && e.target.type !== 'radio' && e.target.type !== 'file') {
                var prim = (o.buttons || []).filter(function (b) { return b.primary; })[0]; if (prim && d.buttons[prim.label] && !d.buttons[prim.label].disabled) { e.preventDefault(); d.buttons[prim.label].click(); }
            }
        });
        ov.appendChild(box); doc.body.appendChild(ov);
        return d;
    }
    function field(label, input, note) { return h('div', { class: 'f' }, h('label', { text: T(label) }), input, note ? h('span', { class: 'note', text: T(note) }) : null); }
    function sel(options, value) {
        var s = h('select');
        options.forEach(function (o) { s.appendChild(h('option', { value: String(o[0]), selected: String(o[0]) === String(value), text: o[1] })); });
        return s;
    }
    // The one compressor is xz, levels 1..9; the figures are the approximate ones of docs/bench/xz-levels.md (one CPU core, typical data).
    var LV_SPEED = ['6', '5', '4', '2.5', '1.7', '1.4', '1.4', '1.3', '1.3'], LV_SIZE = [40, 30, 25, 13, 0, -2, -8, -12, -12], LV_MEM = ['', '', '', '', '', '', '0.5', '0.8', '0.8'];
    function levelName(l) { return l === 1 ? T('Fastest') : l === 5 ? T('Balanced (default)') : l === 9 ? T('Maximum') : l < 5 ? T('Fast') : T('Strong'); }
    function levelHint(l) {
        if (!(l >= 1 && l <= 9)) l = 5;
        var i = l - 1, d = LV_SIZE[i], nm = levelName(l);
        var r = d > 0 ? T('{0}: about {1} MB/s per CPU core; backups are about {2}% larger than at level 5.', nm, LV_SPEED[i], d) : d < 0 ? T('{0}: about {1} MB/s per CPU core; backups are about {2}% smaller than at level 5.', nm, LV_SPEED[i], -d) : T('{0}: about {1} MB/s per CPU core; the baseline size (levels 1-4 are larger, 6-9 smaller).', nm, LV_SPEED[i]);
        return LV_MEM[i] ? r + ' ' + T('Needs about {0} GiB of memory per CPU core.', LV_MEM[i]) : r;
    }
    function levelSel(v) { return sel([1, 2, 3, 4, 5, 6, 7, 8, 9].map(function (l) { return [l, l + ' - ' + levelName(l)]; }), v); }
    function threadsSel(v, cpus) {
        var n = Math.max(1, Math.min(64, cpus || 1, 64)), o = [[0, T('Auto')]]; if (v > n) n = Math.min(64, v);
        for (var t = 1; t <= n; t++) o.push([t, String(t)]);
        return sel(o, v);
    }
    function num(v, min) { return h('input', { type: 'number', value: v === undefined ? '' : String(v), min: min }); }
    function txt(v, type) { return h('input', { type: type || 'text', value: v === undefined || v === null ? '' : String(v) }); }
    function chk(label, v) { var c = h('input', { type: 'checkbox', checked: !!v }); var l = h('label', null, c, ' ' + T(label)); l.input = c; return l; }

    function passwordDialog() {
        var cur = txt('', 'password'), n1 = txt('', 'password'), n2 = txt('', 'password');
        return dialog({ title: 'Change password', body: h('div', null, field('Current password', cur), field('New password', n1, 'At least 10 characters'), field('Repeat', n2)),
            buttons: [{ label: 'Change', primary: true, fn: function (dd) {
                if (n1.value !== n2.value) { dd.error(T('The passwords differ.')); return false; }
                api('password', { current: cur.value, next: n1.value }).then(function () { dd.close(); showLogin(T('Password changed. Sign in again.')); }, function (e) { dd.error(e.message); });
                return false;
            } }, { label: 'Cancel' }] });
    }

    // ---------- Add Server wizard ----------
    // ---------- the folder picker of the SERVICE computer (admin: fs.browse) ----------
    // field + a "Browse..." button that opens the picker for the folders (or files) of the computer the service runs on
    function fieldBrowse(label, input, note, o) {
        var kids = [h('label', { text: T(label) }), input];
        if (can('fs.browse')) kids.push(h('button', { text: T('Browse...'), on: { click: function () { folderDialog({ title: o && o.title, files: o && o.files, start: input.value.trim(), onPick: function (p) { input.value = p; if (o && o.onPick) o.onPick(p); } }); } } }));
        if (note) kids.push(h('span', { class: 'note', text: T(note) }));
        return h('div', { class: 'f' }, kids);
    }
    function folderDialog(o) {
        var tok = 0, cur = null, selected = null;
        var crumbs = h('div', { class: 'crumbs' }), listBox = h('div'), status = h('div', { class: 'note' }), upBtn = h('button', { text: T('Up') }), openBtn = h('button', { text: T('Open') });
        function sepText() { return doc.createTextNode(' › '); }
        function drawCrumbs(path) {
            clear(crumbs); crumbs.appendChild(h('a', { class: 'crumb', text: T('This computer'), on: { click: function () { load(''); } } }));
            var m = /^([A-Za-z]:)\\(.*)$/.exec(path || ''); if (!m) return;
            var acc = m[1] + '\\'; crumbs.appendChild(sepText()); crumbs.appendChild(h('a', { class: 'crumb', text: m[1], on: { click: (function (p) { return function () { load(p); }; })(acc) } }));
            m[2].split('\\').filter(function (x) { return x; }).forEach(function (part) { acc += part + '\\'; crumbs.appendChild(sepText()); crumbs.appendChild(h('a', { class: 'crumb', text: part, on: { click: (function (p) { return function () { load(p); }; })(acc) } })); });
        }
        function openEntry(x) { if (x && (x.kind === 'dir' || x.kind === 'drive')) load(x.path); }
        function load(path) {
            var my = ++tok; selected = null; clear(listBox); status.className = 'note'; status.textContent = T('Loading...');
            var a = { path: path }; if (o.files) a.files = true;
            api('fs.browse', a).then(function (r) {
                if (my !== tok) return; status.textContent = ''; cur = r; drawCrumbs(r.path); upBtn.disabled = !r.path;
                if (r.truncated) status.textContent = T('The list is cut: only the first entries are shown.');
                var es = r.entries || [];
                if (!es.length) { listBox.appendChild(h('div', { class: 'empty', text: T('Nothing here.') })); return; }
                var keep = { k: S.sel.k, row: S.sel.row };
                listBox.appendChild(grid([{ title: 'Name', get: function (x) { return x.name; } }, { title: 'Type', get: function (x) { return x.kind === 'drive' ? T('Drive') + (x.drive_type ? ' (' + T(x.drive_type) + ')' : '') : x.kind === 'dir' ? T('Folder') : T('File'); } }],
                    es, function (x) { return x.path; }, function (x) { selected = x; }, openEntry));
                S.sel.k = keep.k; S.sel.row = keep.row;
            }, function (e) { if (my !== tok) return; status.className = 'msg err'; status.textContent = e.message || ''; });
        }
        upBtn.addEventListener('click', function () { if (cur && cur.path) load(cur.parent || ''); });
        openBtn.addEventListener('click', function () { openEntry(selected); });
        var dd = dialog({ title: o.title || (o.files ? 'Choose a file' : 'Choose a folder'), width: 640, body: h('div', null, crumbs, h('div', { class: 'toolbar' }, upBtn, openBtn), listBox, status),
            buttons: [{ label: 'Select', primary: true, close: false, fn: function (d2) {
                var pick = null;
                if (selected && (selected.kind === 'dir' || (o.files && selected.kind === 'file'))) pick = selected.path.replace(/(.)\\$/, '$1');
                else if (selected && selected.kind === 'drive') pick = selected.path;
                else if (!o.files && cur && cur.path) pick = cur.path.replace(/(.)\\$/, '$1');
                if (!pick) { d2.error(o.files ? T('Choose a file.') : T('Choose a folder.')); return false; }
                tok++; d2.close(); o.onPick(pick); return false;
            } }, { label: 'Cancel', fn: function () { tok++; } }] });
        var st = o.start || ''; var m = /^([A-Za-z]:\\.*?)\\?$/.exec(st);
        load(m ? (o.files ? st.replace(/\\[^\\]*$/, '\\') : m[1] + '\\') : '');
        return dd;
    }

    // ---------- network drives and network browse (admin: net.drives / net.browse) ----------
    var DRIVE_RE = /^[A-Za-z]:([\\\/]|$)/;
    function uncJoin(unc, rest) { return unc.replace(/[\\\/]+$/, '') + (rest ? '\\' + rest.replace(/^[\\\/]+/, '').replace(/\//g, '\\') : ''); }
    function netButtons(f) {
        var row = h('div', { class: 'toolbar' });
        if (can('net.drives')) row.appendChild(h('button', { text: T('Network drive...'), on: { click: function () { networkDriveDialog(function (dr) {
            f.path.value = uncJoin(dr.unc, ''); if (dr.user && !f.user.value) f.user.value = dr.user;
            f.password.value = ''; if (f.password.focus) { if (f.user.value) f.password.focus(); else f.user.focus(); }
        }); } } }));
        if (can('net.browse')) row.appendChild(h('button', { text: T('Browse network...'), on: { click: function () { networkBrowseDialog({
            path: function () { return f.path.value.trim(); }, user: function () { return f.user.value.trim(); }, password: function () { var v = f.password.value; return v === '********' ? '' : v; },
            onPick: function (p) { f.path.value = p; } }); } } }));
        return row;
    }
    function driveNote(letter, unc) { return T('The service cannot see drive letters. {0} is the network drive {1}: its UNC path is stored instead. Enter the user name and password that give access to it.', letter, unc); }
    function networkDriveDialog(onPick) {
        var chosen = null, box = h('div', null, h('div', { class: 'note', text: T('Loading...') }));
        var keep = { k: S.sel.k, row: S.sel.row };
        var dd = dialog({ title: 'Network drive', width: 640, body: h('div', null, h('div', { class: 'note', text: T('Windows maps drives per user. The service sees only the drives mapped for users who are logged on and its own. The drive letter itself cannot be used by the service: its UNC path is stored, and the credentials must be entered.') }), box),
            buttons: [{ label: 'Use', primary: true, close: false, fn: function (d2) { if (!chosen) { d2.error(T('Select a drive first.')); return false; } d2.close(); S.sel.k = keep.k; S.sel.row = keep.row; onPick(chosen); return false; } }, { label: 'Cancel' }] });
        api('net.drives').then(function (r) {
            clear(box); var l = r.drives || [];
            if (!l.length) { box.appendChild(h('div', { class: 'empty', text: T('No mapped network drives were found.') })); return; }
            box.appendChild(grid([{ title: 'Drive', get: function (x) { return x.letter; } }, { title: 'Network path', get: function (x) { return x.unc; } }, { title: 'Owner', get: function (x) { return x.owner || (x.source === 'service' ? T('the service') : ''); } }, { title: 'User', get: function (x) { return x.user || ''; } }],
                l, function (x) { return x.letter + '|' + x.owner; }, function (x) { chosen = x; dd.error(''); }, function (x) { chosen = x; dd.buttons['Use'].click(); }));
            S.sel.k = keep.k; S.sel.row = keep.row;
        }, function (e) { clear(box); box.appendChild(h('div', { class: 'msg err', text: e.message })); });
        return dd;
    }
    function convertDrive(f, mustConvert, d, driveMsg, proceed) {
        var typed = f.path.value.trim(), letter = typed.substr(0, 2).toUpperCase(), rest = typed.substr(2);
        api('net.drives').then(function (r) {
            var hit = null; (r.drives || []).forEach(function (x) { if (!hit && x.letter.toUpperCase() === letter) hit = x; });
            if (!hit) {
                if (!mustConvert) { go2(); return; }          // a local folder of the VM-folder server: fine as it is
                d.error(T('Drive {0} is not a network drive that the service can see. Enter the UNC path of the share or choose "Network drive...".', letter)); return;
            }
            var unc = uncJoin(hit.unc, rest); f.path.value = unc; if (hit.user && !f.user.value) f.user.value = hit.user;
            driveMsg.textContent = driveNote(letter, unc); driveMsg.style.display = '';
            if (f.password.focus) { if (f.user.value) f.password.focus(); else f.user.focus(); }
        }, function (e) { d.error(e.message); });
        function go2() { proceed(); }
    }
    // o: { path(), user(), password(), onPick(unc) }. The credentials are read from the form for every request, sent in the body only, never kept here.
    function networkBrowseDialog(o) {
        var tok = 0, elapsed = 0, timer = null, cur = null, selected = null;
        var crumbs = h('div', { class: 'crumbs' }), listBox = h('div'), status = h('div', { class: 'note' }), hintBox = h('div', { class: 'note' }), srvIn = txt('');
        var upBtn = h('button', { text: T('Up') }), openBtn = h('button', { text: T('Open') }), stopBtn = h('button', { text: T('Stop') }); stopBtn.disabled = true;
        var srvRow = h('div', { class: 'toolbar' }, h('label', { text: T('Server name') + ' ' }), srvIn, h('button', { text: T('Go'), on: { click: function () { var v = srvIn.value.trim().replace(/^[\\\/]+/, ''); if (!v) return; load('\\\\' + v); } } }));
        function stopTimer() { if (timer && env.clearInterval) env.clearInterval(timer); timer = null; }
        function busy(on) { stopBtn.disabled = !on; upBtn.disabled = on; openBtn.disabled = on; }
        function explain(msg) {
            if (/\(1326\)/.test(msg)) return msg + ' ' + T('Check the user name and password in the form.');
            if (/\(1460\)|\(121\)/.test(msg)) return msg + ' ' + T('The server did not answer in time; try again.');
            if (/\(53\)|\(67\)/.test(msg)) return msg + ' ' + T('The server or share was not found.');
            return msg;
        }
        function drawCrumbs(path) {
            clear(crumbs); crumbs.appendChild(h('a', { class: 'crumb', text: T('Network'), on: { click: function () { load(''); } } }));
            var m = /^\\\\([^\\]+)(?:\\([^\\]+)(.*))?$/.exec(path || ''); if (!m) return;
            var acc = '\\\\' + m[1]; crumbs.appendChild(document_text(' › ')); crumbs.appendChild(h('a', { class: 'crumb', text: m[1], on: { click: (function (p) { return function () { load(p); }; })(acc) } }));
            if (m[2]) { acc += '\\' + m[2]; crumbs.appendChild(document_text(' › ')); crumbs.appendChild(h('a', { class: 'crumb', text: m[2], on: { click: (function (p) { return function () { load(p); }; })(acc) } }));
                (m[3] || '').split('\\').filter(function (x) { return x; }).forEach(function (part) { acc += '\\' + part; crumbs.appendChild(document_text(' › ')); crumbs.appendChild(h('a', { class: 'crumb', text: part, on: { click: (function (p) { return function () { load(p); }; })(acc) } })); }); }
        }
        function document_text(t) { return doc.createTextNode(t); }
        function openEntry(x) { if (x && (x.kind === 'server' || x.kind === 'share' || x.kind === 'folder')) load(x.path); }
        function load(path) {
            var my = ++tok; elapsed = 0; stopTimer(); selected = null; clear(listBox); hintBox.textContent = ''; status.className = 'note'; status.textContent = T('Loading...'); busy(true);
            if (env.setInterval) timer = env.setInterval(function () { if (my !== tok) return; elapsed++; status.textContent = T('Loading... {0} s', elapsed); }, 1000);
            var body = { path: path }, u = o.user(), pw = o.password(); if (u) body.user = u; if (pw) body.password = pw;
            var pr = api('net.browse', body); body.password = ''; pw = '';
            pr.then(function (r) {
                if (my !== tok) return; stopTimer(); busy(false); status.textContent = ''; cur = r; drawCrumbs(r.path);
                srvRow.style.display = r.kind === 'root' ? '' : 'none'; upBtn.disabled = r.kind === 'root';
                if (r.hint) hintBox.textContent = r.hint;
                if (r.truncated) status.textContent = T('The list is cut: only the first entries are shown.');
                var es = r.entries || [];
                if (!es.length) { listBox.appendChild(h('div', { class: 'empty', text: r.kind === 'root' ? T('No computers were found. Type a server name below.') : T('Nothing here.') })); return; }
                listBox.appendChild(grid([{ title: 'Name', get: function (x) { return x.name; } }, { title: 'Type', get: function (x) { return T(x.kind); } }, { title: 'Comment', get: function (x) { return x.comment || ''; } }],
                    es, function (x) { return x.path; }, function (x) { selected = x; }, openEntry));
            }, function (e) {
                if (my !== tok) return; stopTimer(); busy(false); status.className = 'msg err'; status.textContent = explain(e.message || '');
            });
        }
        upBtn.addEventListener('click', function () { if (cur && cur.kind !== 'root') load(cur.parent || ''); });
        openBtn.addEventListener('click', function () { openEntry(selected); });
        stopBtn.addEventListener('click', function () { tok++; stopTimer(); busy(false); status.className = 'note'; status.textContent = T('Stopped.'); });
        var dd = dialog({ title: 'Browse network', width: 680, body: h('div', null, crumbs, h('div', { class: 'toolbar' }, upBtn, openBtn, stopBtn), listBox, srvRow, hintBox, status),
            buttons: [{ label: 'Select', primary: true, close: false, fn: function (d2) {
                var pick = null;
                if (selected && (selected.kind === 'share' || selected.kind === 'folder')) pick = selected.path;
                else if (!selected && cur && (cur.kind === 'share' || cur.kind === 'folder')) pick = cur.path;
                if (!pick) { d2.error(T('Choose a share or a folder.')); return false; }
                tok++; stopTimer(); d2.close(); o.onPick(pick); return false;
            } }, { label: 'Cancel', fn: function () { tok++; stopTimer(); } }] });
        var start = o.path(); load(/^\\\\/.test(start) ? start.replace(/^(\\\\[^\\]+\\[^\\]+).*$/, '$1') : '');
        return dd;
    }

    function serverWizard(existing) {
        var s = existing ? JSON.parse(JSON.stringify(existing)) : { type: 'directory', name: '' };
        var step = existing ? 1 : 0;
        var body = h('div'); var d, f = null; var driveMsg = h('div', { class: 'msg info' }); driveMsg.style.display = 'none';
        function defaultPort(t) { return { esxi: 443, vcenter: 443, ftp: 21, ftpes: 21, ftps: 990, scp: 22 }[t] || ''; }
        function render() {
            clear(body); f = null;
            if (step === 0) {
                body.appendChild(h('p', { text: T('Choose the kind of server to add.') }));
                var list = h('div');
                TYPES.filter(function (t) { return t[0] !== 'local'; }).forEach(function (t) {
                    var r = h('input', { type: 'radio', name: 'stype', value: t[0], checked: s.type === t[0], on: { change: function () { s.type = t[0]; } } });
                    list.appendChild(h('div', null, h('label', null, r, ' ' + T(t[1]))));
                });
                body.appendChild(list);
            } else if (step === 1) {
                f = {}; var t = s.type;
                f.name = txt(s.name); body.appendChild(field('Name', f.name));
                if (t === 'directory') {
                    f.path = txt(s.path); body.appendChild(fieldBrowse('Folder with VMs', f.path, 'A path on the server computer', { title: 'Folder with VMs' }));
                    f.user = txt(s.user); f.password = txt(s.has_password ? '********' : (s.password || ''), 'password');
                    body.appendChild(netButtons(f)); body.appendChild(driveMsg);
                    body.appendChild(field('User', f.user, 'Only for a network folder that asks for a user name')); body.appendChild(field('Password', f.password));
                }
                else if (t === 'smb') {
                    f.path = txt(s.path); f.path.placeholder = '\\\\server\\share\\folder'; f.user = txt(s.user); f.password = txt(s.has_password ? '********' : (s.password || ''), 'password');
                    body.appendChild(field('Network folder', f.path)); body.appendChild(netButtons(f)); body.appendChild(driveMsg);
                    body.appendChild(field('User', f.user, 'Domain user name or user@domain; empty = the account of the service')); body.appendChild(field('Password', f.password));
                    body.appendChild(h('div', { class: 'msg info', text: T('A mapped drive letter (Z:) cannot be used: the service does not see it. Choose "Network drive..." to use its UNC path, or enter the UNC path.') }));
                }
                else if (t === 'local') { f.directory = txt(s.directory); body.appendChild(fieldBrowse('Folder', f.directory, '', { title: 'Folder' })); }
                else {
                    f.host = txt(s.host); f.port = num(s.port || defaultPort(t), 1); f.user = txt(s.user); f.password = txt(s.has_password ? '********' : (s.password || ''), 'password');
                    body.appendChild(field('Host', f.host)); body.appendChild(field('Port', f.port)); body.appendChild(field('User', f.user)); body.appendChild(field('Password', f.password));
                    if (isTargetType(t)) { f.directory = txt(s.directory); body.appendChild(field('Folder on the server', f.directory)); }
                    if (t === 'vcenter') { f.datacenter = txt(s.datacenter); body.appendChild(field('Datacenter', f.datacenter, 'optional: the datacenter used when a backup does not name one')); }
                    if (t === 'ftp' || t === 'ftpes' || t === 'ftps') { f.passive = chk('Passive mode', s.passive !== false); body.appendChild(h('div', { class: 'f' }, f.passive)); }
                    if (t === 'esxi' || t === 'vcenter' || t === 'ftpes' || t === 'ftps') {
                        f.verify = chk('Verify the certificate', s.verify !== false); f.fingerprint = txt(s.fingerprint);
                        body.appendChild(h('div', { class: 'f' }, f.verify)); body.appendChild(field('Pinned fingerprint', f.fingerprint, 'sha256:... (optional)'));
                        f.tls_min = sel([['1.2', 'TLS 1.2'], ['1.3', 'TLS 1.3'], ['1.1', T('TLS 1.1 (weak)')], ['1.0', T('TLS 1.0 (weak)')]], s.tls_min || '1.2');
                        var tlsWarn = h('div', { class: 'msg warn', text: T('TLS 1.0 and 1.1 are weak. Use them only for old hosts (ESXi 6.0 / 6.5) that offer nothing newer.') });
                        var showTls = function () { tlsWarn.style.display = f.tls_min.value === '1.0' || f.tls_min.value === '1.1' ? '' : 'none'; };
                        f.tls_min.addEventListener('change', showTls); showTls();
                        body.appendChild(field('Minimum TLS version', f.tls_min)); body.appendChild(tlsWarn);
                    }
                    if (t === 'scp') {
                        f.host_key = txt(s.host_key); body.appendChild(field('Host key', f.host_key, 'optional'));
                        f.key_file = txt(s.key_file); body.appendChild(fieldBrowse('Private key file', f.key_file, 'A path on the server computer (the service reads it)', { files: true, title: 'Private key file' }));
                        f.key_text = h('textarea', { rows: 4 }); f.key_text.value = s.has_key_text ? '********' : (s.key_text || '');
                        body.appendChild(field('Private key text', f.key_text, 'Paste the key instead of a file; it is stored protected and never shown again'));
                        f.key_passphrase = txt(s.has_key_passphrase ? '********' : (s.key_passphrase || ''), 'password'); body.appendChild(field('Key passphrase', f.key_passphrase));
                    }
                }
            } else {
                body.appendChild(h('div', { class: 'note', text: T('The connection is tested now. "Test connection" tests again; "Finish" saves the server.') }));
                body.appendChild(testBox); runTest();
            }
        }
        // ---- the connection test: a persistent result with the VM list, and the offer to trust an unknown identity ----
        var testBox = h('div', { class: 'testbox' }), lastTest = null;
        var FP_RE = /(sha256:[0-9a-fA-F:]{64,95}|SHA256:[A-Za-z0-9+\/]{43})/;
        function runTest() {
            lastTest = null; clear(testBox); testBox.appendChild(h('div', { class: 'note', text: T('Testing...') }));
            api('servers.test', { server: s }).then(function (x) {
                lastTest = { ok: true }; clear(testBox);
                testBox.appendChild(h('div', { class: 'msg info', text: x.message || T('The connection works.') }));
                var vms = x.vms || []; if (vms.length) testBox.appendChild(grid([{ title: 'Virtual machine', get: function (v) { return v.name; } }, { title: 'State', get: function (v) { return v.state || ''; } }, { title: 'Size', get: function (v) { return v.size ? fmtSize(v.size) : ''; } }], vms, function (v) { return v.name; }, null, null));
            }, function (e) {
                lastTest = { ok: false, error: e.message }; clear(testBox);
                testBox.appendChild(h('div', { class: 'msg err', text: e.message }));
                var m = FP_RE.exec(e.message || '');
                if (m) {
                    var key = /^SHA256:/.test(m[1]) ? 'host_key' : 'fingerprint';
                    testBox.appendChild(h('div', { class: 'note', text: T('The server presented an identity that is not known yet: {0}. If you recognise it, you can trust it.', m[1]) }));
                    testBox.appendChild(h('button', { text: T('Trust this identity and test again'), on: { click: function () { s[key] = m[1]; if (key === 'fingerprint') s.verify = true; runTest(); } } }));
                }
            });
        }
        function collect() {
            if (!f) return s;
            var o = { type: s.type, name: f.name.value.trim() }; if (s.id) o.id = s.id;
            ['path', 'directory', 'host', 'user', 'fingerprint', 'host_key'].forEach(function (k) { if (f[k]) o[k] = f[k].value.trim(); });
            if (f.port) o.port = parseInt(f.port.value, 10) || 0;
            if (f.password) o.password = f.password.value;
            if (f.verify) o.verify = f.verify.input.checked;
            if (f.tls_min) o.tls_min = f.tls_min.value;
            if (f.passive) o.passive = f.passive.input.checked;
            if (f.datacenter) o.datacenter = f.datacenter.value.trim();
            if (f.key_file) o.key_file = f.key_file.value.trim();
            if (f.key_text) o.key_text = f.key_text.value.trim() === '********' ? '********' : f.key_text.value.trim();
            if (f.key_passphrase) o.key_passphrase = f.key_passphrase.value;
            return o;
        }
        function nav() {
            d.buttons['Back'].style.display = step > 0 && !existing ? '' : 'none'; d.buttons['Next'].style.display = step < 2 ? '' : 'none';
            d.buttons['Test connection'].style.display = step === 2 ? '' : 'none'; d.buttons['Finish'].style.display = step === 2 ? '' : 'none';
        }
        d = dialog({ title: existing ? 'Edit Server' : 'Add Server', width: 520, body: body, buttons: [
            { label: 'Back', close: false, fn: function () { if (step === 1) s = collect(); step--; render(); nav(); return false; } },
            { label: 'Next', primary: true, close: false, fn: function () {
                if (step === 1) {
                    s = collect(); if (!s.name) { d.error(T('Give the server a name.')); return false; }
                    if (s.type === 'scp' && !s.password && !s.key_file && !s.key_text) { d.error(T('Enter a password, a private key file or the private key text.')); return false; }
                    if (s.host !== undefined && !s.host && s.type !== 'directory' && s.type !== 'smb' && s.type !== 'local') { d.error(T('Enter the address of the server.')); return false; }
                    if (s.port !== undefined && s.host !== undefined && (s.port < 1 || s.port > 65535)) { d.error(T('The port must be 1..65535.')); return false; }
                    if ((s.type === 'smb' || s.type === 'directory') && DRIVE_RE.test(s.path || '')) {         // a typed drive letter: the service cannot see it, convert through the drives it knows
                        if (!can('net.drives')) { if (s.type === 'smb') { d.error(T('The service cannot see drive letters. Enter the UNC path of the share.')); return false; } }
                        else { convertDrive(f, s.type === 'smb', d, driveMsg, function () { step++; render(); nav(); }); return false; }
                    }
                }
                step++; render(); nav(); return false; } },
            { label: 'Test connection', close: false, fn: function () { runTest(); return false; } },
            { label: 'Finish', primary: true, close: false, fn: function () {
                if ((!lastTest || !lastTest.ok) && !env.confirm(T('The connection test failed or has not finished. Save the server anyway?'))) return false;
                api('servers.save', { server: s }).then(function () { d.close(); refreshView(); }, function (e) { d.error(e.message); }); return false; } },
            { label: 'Cancel' }] });
        render(); nav();
        return d;
    }

    // ---------- Virtual Machine Backup dialog (General / Connection / Advanced / Verify Backup) ----------
    // o: { server (source id), element (to edit), run (start now), onOk(element) }
    function vmBackupDialog(o) {
        var el = o.element ? JSON.parse(JSON.stringify(o.element)) : { type: 'backup', enabled: true, source: { server: o.server || '', vms: [] }, target: { server: 'local', directory: '' }, compress: { codec: 'xz', level: 5 }, encrypt: { enabled: false }, keep: 0, verify: true, segment_mib: 256, speed_mbit: 0, on_error_next: true };
        el.source = el.source || { vms: [] }; el.target = el.target || { server: 'local' }; el.compress = el.compress || { codec: 'xz', level: 5 }; el.encrypt = el.encrypt || {};
        var srcSel = h('select'), tgtSel = h('select');
        var vmBox = h('div', { class: 'vmlist' }), manual = h('textarea', { rows: 3 }); manual.style.display = 'none';
        var checks = [];
        var dir = txt(el.target.directory), keep = sel([[0, T('All')], [1, 1], [2, 2], [3, 3], [5, 5], [7, 7], [10, 10], [14, 14], [30, 30], [60, 60], [90, 90]], el.keep || 0);
        var level = levelSel(el.compress.level || (S.state && S.state.settings && S.state.settings.level) || 5), levelNote = h('div', { class: 'note' });
        function updateLevel() { levelNote.textContent = levelHint(parseInt(level.value, 10)); }
        level.addEventListener('change', updateLevel); updateLevel();
        var enc = chk('Encrypt the backup (AES-256)', el.encrypt.enabled); var pw = txt(el.encrypt.has_password ? '********' : '', 'password');
        var speed = num(el.speed_mbit || 0, 0), seg = sel([[64, '64 MiB'], [128, '128 MiB'], [256, '256 MiB'], [512, '512 MiB'], [1024, '1 GiB'], [4096, '4 GiB'], [0, T('One volume (no resuming)')]], el.segment_mib === undefined ? 256 : el.segment_mib);
        var resume = chk('Continue an interrupted backup of this VM when there is one', el.resume !== false), live = chk('Allow a copy of a running VM (the copy is consistent only at the crash level)', el.allow_live), nextTask = chk('On error, go on with the next VM', el.on_error_next !== false);
        var verify = chk('Verify the backup after it is made', el.verify !== false);
        var transport = h('div', { class: 'note' });
        var snap = el.snapshot || {};
        var quiesce = chk('Quiesce the guest file system (needs VMware Tools in the VM)', snap.quiesce !== false), memory = chk('Include the memory of the virtual machine in the snapshot', !!snap.memory), snapDc = txt(el.source.datacenter || '');
        var snapNote = h('div', { class: 'note' }), liveRow = h('div', { class: 'f' }, live);
        var snapBox = h('div', null, h('fieldset', null, h('legend', { text: T('Snapshot') }), h('div', { class: 'f' }, quiesce), h('div', { class: 'f' }, memory),
            h('div', { class: 'note', text: T('A running ESXi / vCenter VM is copied from a temporary snapshot, which is removed afterwards. Without VMware Tools the quiesced snapshot fails: switch quiescing off for such VMs.') })), field('Datacenter', snapDc, 'vCenter only; empty = the one of the server'));
        function srvOf(id) { for (var i = 0; i < S.servers.length; i++) if (S.servers[i].id === id) return S.servers[i]; return null; }
        function fillServers() {
            S.servers.forEach(function (sv) {
                if (isSourceType(sv.type)) srcSel.appendChild(h('option', { value: sv.id, text: sv.name, selected: sv.id === el.source.server }));
                if (isTargetType(sv.type)) tgtSel.appendChild(h('option', { value: sv.id, text: sv.name, selected: sv.id === (el.target.server || 'local') }));
            });
            updateTarget(); updateSource(); loadVms();
        }
        function updateSource() {
            var sv = srvOf(srcSel.value), ty = sv ? sv.type : '', vm = ty === 'esxi' || ty === 'vcenter';
            quiesce.input.disabled = !vm; memory.input.disabled = !vm; snapDc.disabled = ty !== 'vcenter'; liveRow.style.display = vm ? 'none' : '';
            snapNote.textContent = vm ? '' : T('The snapshot settings apply to ESXi and vCenter sources only.');
        }
        function updateTarget() {
            var t = srvOf(tgtSel.value); var ty = t ? t.type : 'local';
            transport.textContent = ty === 'ftpes' || ty === 'ftps' ? T('Transfer: TLS encrypted (FTP over TLS)') : ty === 'scp' ? T('Transfer: SSH encrypted') : ty === 'ftp' ? T('Transfer: plain FTP (not encrypted - enable encryption of the backup!)') : ty === 'smb' ? T('Transfer: network folder (SMB; not encrypted unless the share requires it)') : T('Transfer: local / network folder');
        }
        function loadVms() {
            clear(vmBox); checks = []; if (!srcSel.value) return;
            var wanted = el.source.vms || [];
            if (!can('servers.vms')) { vmBox.style.display = 'none'; manual.style.display = ''; manual.value = wanted.join('\n'); return; }
            vmBox.appendChild(h('div', { class: 'note', text: T('Loading...') }));
            api('servers.vms', { id: srcSel.value }).then(function (r) {
                clear(vmBox); var vms = r.vms || [];
                if (!vms.length) vmBox.appendChild(h('div', { class: 'note', text: T('No virtual machines found.') }));
                vms.forEach(function (v) {
                    var c = h('input', { type: 'checkbox', checked: wanted.indexOf(v.name) >= 0 }); checks.push({ name: v.name, c: c });
                    vmBox.appendChild(h('label', null, c, ' ' + v.name + (v.size ? '  (' + fmtSize(v.size) + ')' : '') + (v.state ? '  - ' + v.state : '')));
                });
            }, function (e) { clear(vmBox); vmBox.appendChild(h('div', { class: 'msg err', text: e.message })); });
        }
        srcSel.addEventListener('change', function () { el.source.vms = []; updateSource(); loadVms(); });
        tgtSel.addEventListener('change', updateTarget);
        var general = h('div', null, field('Source server', srcSel), h('div', { class: 'f' }, h('label', { text: T('Virtual machines') }), vmBox, manual),
            field('Target server', tgtSel), fieldBrowse('Folder', dir, 'Use {VM} for the VM name', { title: 'Folder' }), field('Keep last backups', keep), field('Compression level', level), levelNote, h('div', { class: 'note', text: T('The figures are approximate (typical data). More CPU cores make compression faster.') }),
            h('div', { class: 'f' }, enc), field('Password', pw), h('div', { class: 'note', text: T('Warning: the password is stored encrypted and is not retrievable. Be sure to remember it or write it down in a safe place in order to be able to decrypt the backup.') }));
        var connection = h('div', null, h('fieldset', null, h('legend', { text: T('Network') }), field('Speed limit, Mbit/s (0 = unlimited)', speed), h('div', { class: 'note', text: T('The schedule windows of a task can set another limit for its hours; the lowest one applies.') })), transport);
        var advanced = h('div', null, h('fieldset', null, h('legend', { text: T('Resumable transfer') }), field('Volume size', seg), h('div', { class: 'f' }, resume), h('div', { class: 'note', text: T('After a lost connection the program reconnects and goes on; nothing finished is sent again.') })),
            h('fieldset', null, h('legend', { text: T('Virtual machine') }), liveRow, h('div', { class: 'f' }, nextTask)));
        var snapTab = h('div', null, snapNote, snapBox);
        var verifyTab = h('div', null, h('fieldset', null, h('legend', { text: T('Verify Backup') }), h('div', { class: 'f' }, verify), h('div', { class: 'note', text: T('Every volume is read back from the target: its size and SHA-256 are compared with the ones recorded while it was written, and the data is decrypted and decompressed to prove that the backup can be restored.') })));
        function collect() {
            var vms = checks.filter(function (c) { return c.c.checked; }).map(function (c) { return c.name; });
            if (manual.style.display !== 'none') vms = manual.value.split(/\r?\n/).map(function (x) { return x.trim(); }).filter(Boolean);
            var out = JSON.parse(JSON.stringify(el));                                  // keeps the element id and every key the form does not show
            out.type = 'backup'; out.enabled = el.enabled !== false;
            out.source = Object.assign({}, el.source, { server: srcSel.value, vms: vms }); out.target = Object.assign({}, el.target, { server: tgtSel.value, directory: dir.value.trim() });
            out.compress = { codec: 'xz', level: parseInt(level.value, 10) }; out.encrypt = { enabled: enc.input.checked }; out.keep = parseInt(keep.value, 10) || 0;
            out.verify = verify.input.checked; out.allow_live = live.input.checked; out.resume = resume.input.checked; out.on_error_next = nextTask.input.checked;
            out.segment_mib = parseInt(seg.value, 10); out.speed_mbit = parseFloat(speed.value) || 0;
            var sv = srvOf(srcSel.value), ty = sv ? sv.type : '';
            if (ty === 'esxi' || ty === 'vcenter') { out.snapshot = { quiesce: quiesce.input.checked, memory: memory.input.checked }; out.allow_live = true; } else delete out.snapshot;
            if (ty === 'vcenter' && snapDc.value.trim()) out.source.datacenter = snapDc.value.trim(); else delete out.source.datacenter;
            if (enc.input.checked) out.encrypt.password = pw.value;
            return out;
        }
        function check(e) {
            if (!e.source.server) return T('Choose a source server.');
            if (!e.source.vms.length) return T('Choose at least one virtual machine.');
            if (e.source.vms.length > 1 && e.target.directory && e.target.directory.indexOf('{VM}') < 0) return T('For several virtual machines the folder must contain the {VM} tag.');
            if (e.encrypt.enabled && !e.encrypt.password) return T('Enter the password for the encryption.');
            if (e.speed_mbit < 0) return T('The speed limit cannot be negative.');
            return '';
        }
        var d = dialog({ title: 'Virtual Machine Backup', width: 640, tabs: [{ name: 'General', el: general }, { name: 'Connection', el: connection }, { name: 'Snapshot', el: snapTab }, { name: 'Advanced', el: advanced }, { name: 'Verify Backup', el: verifyTab }], buttons: [
            { label: o.run ? 'Start' : 'OK', primary: true, fn: function (dd) {
                var e = collect(), m = check(e); if (m) { dd.error(m); return false; }
                if (o.run) { api('backup.run', { element: e, name: T('Backup of {0}', e.source.vms[0]) + (e.source.vms.length > 1 ? ' (+' + (e.source.vms.length - 1) + ')' : '') }).then(function () { dd.close(); toast(T('Backup started.')); pollOnce(); }, function (er) { dd.error(er.message); }); return false; }
                if (o.onOk) o.onOk(e);
            } }, { label: 'Cancel' }] });
        (S.servers.length ? Promise.resolve() : loadServers()).then(fillServers, fail);
        return d;
    }

    // ---------- Scheduled Tasks ----------
    function describeSchedule(sp) {
        if (!sp || !sp.enabled) return T('Not scheduled');
        var dn = [T('Mon'), T('Tue'), T('Wed'), T('Thu'), T('Fri'), T('Sat'), T('Sun')];
        if (sp.mode === 'every') return T('every {0} {1}', sp.every.value, T(sp.every.unit)) + ' ' + T('from {0}', String(sp.first_time || '').replace('T', ' '));
        if (sp.mode === 'weekly') return (sp.week_interval > 1 ? T('every {0} weeks', sp.week_interval) : T('every week')) + ' ' + T('on {0} at {1}', (sp.weekdays || []).map(function (d) { return dn[d]; }).join(','), sp.start_time);
        if (sp.mode === 'monthly') return (sp.month_interval > 1 ? T('every {0} months', sp.month_interval) : T('every month')) + ' ' + T('on day {0} at {1}', sp.day === 'last' ? T('last') : sp.day, sp.start_time);
        if (sp.mode === 'once') return T('once at {0}', String(sp.at || '').replace('T', ' '));
        return '';
    }
    function viewTasks() {
        var box = h('div');
        pageEl.appendChild(h('h2', { text: T('Scheduled Tasks') }));
        pageEl.appendChild(h('div', { class: 'toolbar' },
            btn('Add Scheduled Task', 'tasks.save', function () { taskDialog(null); }, 'primary'),
            btn('Edit', 'tasks.save', needSel(function (r) { taskDialog(r); })),
            btn('Copy', 'tasks.copy', needSel(function (r) { api('tasks.copy', { id: r.id }).then(function () { toast(T('The task was copied.')); refreshView(); }, fail); })),
            btn('Run now', 'tasks.run', needSel(function (r) { api('tasks.run', { id: r.id }).then(function () { toast(T('Task started.')); pollOnce(); }, fail); })),
            btn('Delete', 'tasks.delete', needSel(function (r) { if (env.confirm(T('Delete task "{0}"?', r.name))) api('tasks.delete', { id: r.id }).then(refreshView, fail); }), 'danger'),
            h('button', { text: T('Refresh'), on: { click: refreshView } })));
        var schedNote = h('div', { class: 'msg warn' }); schedNote.style.display = 'none'; pageEl.appendChild(schedNote);
        api('state').then(function (st) {
            if (st.scheduler) return;
            schedNote.style.display = ''; schedNote.appendChild(h('span', { text: T('The scheduler is off: scheduled tasks do not start by themselves.') + ' ' }));
            if (can('scheduler.set')) schedNote.appendChild(h('button', { text: T('Enable scheduler'), on: { click: function () { api('scheduler.set', { enabled: true }).then(refreshView, fail); } } }));
        }, function () {});
        pageEl.appendChild(box);
        api('tasks.list').then(function (l) {
            S.tasks = l; clear(box);
            var nexts = {};
            var jobs = l.map(function (t) { return t.schedule && t.schedule.enabled && t.enabled ? api('tasks.next', { id: t.id }).then(function (n) { nexts[t.id] = n.text || ''; }, function () {}) : Promise.resolve(); });
            Promise.all(jobs).then(function () {
                clear(box);
                box.appendChild(grid([
                    { title: 'Title', get: function (r) { return r.name; } }, { title: 'Enabled', get: function (r) { return r.enabled ? T('Yes') : T('No'); } },
                    { title: 'Schedule', get: function (r) { return describeSchedule(r.schedule); } }, { title: 'Next run', get: function (r) { return nexts[r.id] || ''; } },
                    { title: 'Elements', get: function (r) { return (r.elements || []).map(function (e) { return (e.source && e.source.vms || []).join(', '); }).join('; '); } },
                    { title: 'Last result', get: function (r) { return r.state && r.state.last_status ? T(r.state.last_status) : ''; }, cls: function (r) { return r.state ? 'st-' + r.state.last_status : ''; } },
                    { title: 'Last run', get: function (r) { return r.state ? fmtTime(r.state.last_start) : ''; } }
                ], l, function (r) { return r.id; }, null, can('tasks.save') ? taskDialog : null));
            });
        }, fail);
    }

    var DAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
    function scheduleFromForm(f) {
        var m = f.mode.value;
        if (m === 'none') return { enabled: false };
        if (m === 'every') {
            var v = parseInt(f.everyValue.value, 10);
            if (!(v >= 1)) return { error: T('Enter the interval (a number of 1 or more).') };
            if (!f.first.value) return { error: T('Choose the first start time.') };
            return { enabled: true, mode: 'every', every: { value: v, unit: f.everyUnit.value }, first_time: f.first.value };
        }
        if (m === 'weekly') {
            var days = []; f.days.forEach(function (c, i) { if (c.input ? c.input.checked : c.checked) days.push(i); });
            if (!days.length) return { error: T('Choose at least one day of the week.') };
            if (!f.time.value) return { error: T('Choose the start time.') };
            return { enabled: true, mode: 'weekly', start_time: f.time.value, weekdays: days, week_interval: Math.max(1, parseInt(f.weekInt.value, 10) || 1), anchor: f.anchor || undefined };
        }
        if (m === 'monthly') {
            if (!f.time.value) return { error: T('Choose the start time.') };
            var dd = f.day.value === 'last' ? 'last' : parseInt(f.day.value, 10);
            return { enabled: true, mode: 'monthly', start_time: f.time.value, day: dd, month_interval: Math.max(1, parseInt(f.monthInt.value, 10) || 1) };
        }
        if (m === 'once') { if (!f.at.value) return { error: T('Choose the date and time.') }; return { enabled: true, mode: 'once', at: f.at.value }; }
        return { error: 'mode' };
    }
    function windowRow(w, onDel) {
        var days = DAYS.map(function (n, i) { return chk(n, (w.days || [0, 1, 2, 3, 4]).indexOf(i) >= 0); });
        var from = txt(w.from || '22:00', 'time'), to = txt(w.to || '06:00', 'time'), mbit = num(w.mbit || 0, 0);
        var row = h('fieldset', null, h('div', { class: 'chips' }, days), h('div', { class: 'f' }, h('label', { text: T('From') }), from, h('label', { text: T('To') }), to),
            h('div', { class: 'f' }, h('label', { text: T('Speed limit, Mbit/s (0 = unlimited)') }), mbit), h('button', { text: T('Remove window'), on: { click: function () { onDel(row); } } }));
        row.get = function () { var ds = []; days.forEach(function (c, i) { if (c.input.checked) ds.push(i); }); return { days: ds, from: from.value, to: to.value, mbit: parseFloat(mbit.value) || 0 }; };
        return row;
    }
    function taskDialog(existing, asNew) {
        var t = existing ? JSON.parse(JSON.stringify(existing)) : { name: '', enabled: true, schedule: { enabled: true, mode: 'weekly', start_time: '22:00', weekdays: [0, 1, 2, 3, 4], week_interval: 1 }, windows: [], elements: [] };
        var sp = t.schedule && t.schedule.mode ? t.schedule : { enabled: false, mode: 'weekly' };
        var name = txt(t.name), enabled = chk('Enabled', t.enabled !== false);
        var mode = sel([['none', T('Not scheduled (run manually)')], ['every', T('Every N minutes / hours / days')], ['weekly', T('Weekly')], ['monthly', T('Monthly')], ['once', T('Once')]], t.schedule && t.schedule.enabled ? sp.mode : 'none');
        var dayOpts = []; for (var di = 1; di <= 31; di++) dayOpts.push([String(di), String(di)]); dayOpts.push(['last', T('last')]);
        var f = { mode: mode, everyValue: num(sp.every ? sp.every.value : 1, 1), everyUnit: sel([['minutes', T('minutes')], ['hours', T('hours')], ['days', T('days')]], sp.every ? sp.every.unit : 'hours'),
            first: txt(sp.first_time || '', 'datetime-local'), time: txt(sp.start_time || '22:00', 'time'), weekInt: num(sp.week_interval || 1, 1), monthInt: num(sp.month_interval || 1, 1),
            day: sel(dayOpts, sp.day === undefined ? '1' : sp.day), at: txt(sp.at || '', 'datetime-local'), anchor: sp.anchor, days: DAYS.map(function (n, i) { return chk(n, (sp.weekdays || [0, 1, 2, 3, 4]).indexOf(i) >= 0); }) };
        var secEvery = h('div', null, field('Repeat every', f.everyValue), field('Unit', f.everyUnit), field('First start', f.first));
        var secWeekly = h('div', null, h('div', { class: 'f chips' }, f.days), field('Start time', f.time), field('Every N weeks', f.weekInt));
        var secMonthly = h('div', null, field('Day of the month', f.day), field('Every N months', f.monthInt));
        var secOnce = h('div', null, field('Date and time', f.at));
        function showMode() { secEvery.style.display = mode.value === 'every' ? '' : 'none'; secWeekly.style.display = mode.value === 'weekly' ? '' : 'none'; secMonthly.style.display = mode.value === 'monthly' ? '' : 'none'; secOnce.style.display = mode.value === 'once' ? '' : 'none'; }
        mode.addEventListener('change', showMode); showMode();
        var winBox = h('div'); var wins = [];
        function addWin(w) { var r = windowRow(w || {}, function (row) { wins.splice(wins.indexOf(row), 1); winBox.removeChild(row); }); wins.push(r); winBox.appendChild(r); }
        (t.windows || []).forEach(addWin);
        var elBox = h('div'); var elements = (t.elements || []).slice();
        function srvName(id) { for (var i = 0; i < S.servers.length; i++) if (S.servers[i].id === id) return S.servers[i].name; return id || ''; }
        function drawEls() {
            clear(elBox);
            if (!elements.length) elBox.appendChild(h('div', { class: 'empty', text: T('No elements yet. Add a backup element.') }));
            elements.forEach(function (e, i) {
                var on = chk('Enabled', e.enabled !== false); on.input.addEventListener('change', function () { e.enabled = on.input.checked; });
                var up = h('button', { text: T('Up'), on: { click: function () { if (i > 0) { var t = elements[i - 1]; elements[i - 1] = elements[i]; elements[i] = t; drawEls(); } } } }); up.disabled = i === 0;
                var down = h('button', { text: T('Down'), on: { click: function () { if (i < elements.length - 1) { var t = elements[i + 1]; elements[i + 1] = elements[i]; elements[i] = t; drawEls(); } } } }); down.disabled = i === elements.length - 1;
                elBox.appendChild(h('div', { class: 'job' }, on, h('div', { class: 'nm', text: (e.source && e.source.vms || []).join(', ') }),
                    h('div', { class: 'info', text: srvName(e.source && e.source.server) + ' -> ' + srvName(e.target && e.target.server) + (e.target && e.target.directory ? ' ' + e.target.directory : '') + (e.encrypt && e.encrypt.enabled ? '  |  ' + T('encrypted') : '') }),
                    up, down,
                    h('button', { text: T('Edit'), on: { click: function () { vmBackupDialog({ element: e, onOk: function (ne) { elements[i] = ne; drawEls(); } }); } } }),
                    h('button', { text: T('Copy'), on: { click: function () {          // a copy is a new element: no id, and an encrypted one needs its own password
                        var c = JSON.parse(JSON.stringify(e)); delete c.id; if (c.encrypt) { delete c.encrypt.has_password; delete c.encrypt.password; }
                        vmBackupDialog({ element: c, onOk: function (ne) { elements.splice(i + 1, 0, ne); drawEls(); } }); } } }),
                    h('button', { class: 'danger', text: T('Remove'), on: { click: function () { elements.splice(i, 1); drawEls(); } } })));
            });
        }
        drawEls();
        var elTab = h('div', null, elBox, h('button', { text: T('Add element...'), on: { click: function () { vmBackupDialog({ onOk: function (ne) { elements.push(ne); drawEls(); } }); } } }));
        var winTab = h('div', null, h('div', { class: 'note', text: T('The task may run only inside its windows. A window that ends before it starts goes over midnight.') }), winBox, h('button', { text: T('Add window'), on: { click: function () { addWin(); } } }));
        var nextHint = h('div', { class: 'note' }), hintTok = 0;
        function updateHint() {
            var sc = scheduleFromForm(f); var my = ++hintTok;
            if (sc.error) { nextHint.className = 'note bad'; nextHint.textContent = sc.error; return; }
            if (!sc.enabled) { nextHint.className = 'note'; nextHint.textContent = T('Not scheduled'); return; }
            api('tasks.next', { schedule: sc }).then(function (n) { if (my !== hintTok) return; nextHint.className = 'note'; nextHint.textContent = n.text ? T('Next run: {0}', n.text) : T('Never'); }, function (e) { if (my !== hintTok) return; nextHint.className = 'note bad'; nextHint.textContent = e.message; });
        }
        var general = h('div', null, field('Title', name), h('div', { class: 'f' }, enabled), field('Schedule', mode), secEvery, secWeekly, secMonthly, secOnce, nextHint);
        ['change', 'input'].forEach(function (ev) { general.addEventListener(ev, updateHint); }); mode.addEventListener('change', updateHint);
        var d = dialog({ title: existing && !asNew ? 'Edit Scheduled Task' : 'Add Scheduled Task', width: 660, tabs: [{ name: 'Schedule', el: general }, { name: 'Time windows', el: winTab }, { name: 'Elements', el: elTab }], buttons: [
            { label: 'OK', primary: true, fn: function (dd) {
                var s = scheduleFromForm(f); if (s.error) { dd.error(s.error); return false; }
                if (!name.value.trim()) { dd.error(T('Give the task a title.')); return false; }
                if (!elements.length) { dd.error(T('Add at least one backup element.')); return false; }
                for (var wi = 0; wi < wins.length; wi++) { var wv = wins[wi].get(); if (!wv.days.length) { dd.error(T('Choose at least one day in every time window.')); return false; } if (wv.mbit < 0) { dd.error(T('The speed limit cannot be negative.')); return false; } }
                var task = { name: name.value.trim(), enabled: enabled.input.checked, schedule: s, windows: wins.map(function (w) { return w.get(); }), elements: elements }; if (existing && !asNew) task.id = existing.id;
                api('tasks.save', { task: task }).then(function () { dd.close(); refreshView(); }, function (e) { dd.error(e.message); }); return false;
            } }, { label: 'Cancel' }] });
        (S.servers.length ? Promise.resolve() : loadServers()).then(drawEls, fail); updateHint();
        return d;
    }

    // ---------- Task History ----------
    var HSTAT = ['Success', 'Warning', 'Failed', 'Abort', 'Running'];
    function viewHistory() {
        var box = h('div'), filter = sel([['', T('All results')]].concat(HSTAT.map(function (x) { return [x, T(x)]; })), S.historyFilter || ''), all = [];
        pageEl.appendChild(h('h2', { text: T('Task History') }));
        pageEl.appendChild(h('div', { class: 'toolbar' }, btn('Details', 'history.get', needSel(function (r) { historyDetails(r.id); })),
            btn('HTML report...', 'history.get', needSel(function (r) { historyReport(r.id); })),
            btn('Clear history', 'history.clear', function () { if (env.confirm(T('Clear the whole history?'))) api('history.clear').then(refreshView, fail); }, 'danger'), h('button', { text: T('Refresh'), on: { click: refreshView } }),
            h('label', { text: ' ' + T('Show') + ' ' }), filter));
        pageEl.appendChild(box);
        function draw() {
            S.historyFilter = filter.value; clear(box); S.sel = {};
            var l = all.filter(function (r) { return !filter.value || r.status === filter.value; });
            box.appendChild(grid([
                { title: 'Task', get: function (r) { return r.name || r.kind; } }, { title: 'Kind', get: function (r) { return T(r.kind || ''); } }, { title: 'Started', get: function (r) { return fmtTime(r.started); } },
                { title: 'Duration', get: function (r) { return r.ended && r.started ? fmtDur(r.ended - r.started) : ''; } },
                { title: 'Result', get: function (r) { return T(r.status) + (r.summary && r.summary.skipped ? ' (' + T('{0} skipped: credentials rejected', r.summary.skipped) + ')' : ''); }, cls: function (r) { return 'st-' + r.status; } },
                { title: 'Virtual machines', get: function (r) { return r.summary && r.summary.count !== undefined ? r.summary.count : ''; } },
                { title: 'Source', get: function (r) { return fmtSize(r.raw_bytes); } }, { title: 'Stored', get: function (r) { return fmtSize(r.archive_bytes); } }, { title: 'Trigger', get: function (r) { return T(r.trigger || ''); } }
            ], l, function (r) { return r.id; }, null, can('history.get') ? function (r) { historyDetails(r.id); } : null));
        }
        filter.addEventListener('change', draw);
        api('history.list', { limit: 500 }).then(function (l) { S.history = l; all = l; draw(); }, fail);
    }
    function recordRows(rec) { return rec.results || []; }
    function historyDetails(id) {
        api('history.get', { id: id }).then(function (rec) {
            var rows = recordRows(rec), shown = rows, box = h('div'), fsel = sel([['', T('All results')]].concat(HSTAT.concat(['Disabled']).map(function (x) { return [x, T(x)]; })), '');
            var cols = [{ title: 'Virtual machine', get: function (r) { return r.vm || ''; } }, { title: 'Result', get: function (r) { return T(r.status || '') + (r.skipped ? ' (' + T('Skipped: credentials rejected') + ')' : ''); }, cls: function (r) { return 'st-' + r.status; } },
                { title: 'Duration', get: function (r) { return r.duration ? fmtDur(r.duration) : ''; } }, { title: 'Speed', get: function (r) { return r.rate ? fmtSpeed(r.rate) : ''; } },
                { title: 'Source', get: function (r) { return r.raw ? fmtSize(r.raw) : ''; } }, { title: 'Stored', get: function (r) { return r.size ? fmtSize(r.size) : ''; } }, { title: 'Volumes', get: function (r) { return r.volumes || ''; } },
                { title: 'Encrypted', get: function (r) { return r.encrypted ? T('Yes') : ''; } }, { title: 'Archive', get: function (r) { return r.archive || ''; } }]
                .concat(rows.some(function (r) { return r.restored_vm; }) ? [{ title: 'Restored as', get: function (r) { return r.restored_vm || ''; } }, { title: 'Machine file', get: function (r) { return r.vmx || ''; } }] : [], [{ title: 'Message', get: function (r) { return r.message || ''; } }]);
            function draw() { clear(box); var keep = { k: S.sel.k, row: S.sel.row }; shown = rows.filter(function (r) { return !fsel.value || r.status === fsel.value; }); box.appendChild(grid(cols, shown, null, null, null)); S.sel.k = keep.k; S.sel.row = keep.row; }
            fsel.addEventListener('change', draw); draw();
            var sum = rec.summary || {};
            var body = h('div', null,
                h('div', { class: 'kv' }, h('b', { text: T('Task') }), h('span', { text: rec.name || '' }), h('b', { text: T('Kind') }), h('span', { text: T(rec.kind || '') }), h('b', { text: T('Started') }), h('span', { text: fmtTime(rec.started) }), h('b', { text: T('Finished') }), h('span', { text: fmtTime(rec.ended) }),
                    h('b', { text: T('Duration') }), h('span', { text: rec.ended && rec.started ? fmtDur(rec.ended - rec.started) : '' }), h('b', { text: T('Version') }), h('span', { text: rec.version || '' }),
                    h('b', { text: T('Result') }), h('span', { class: 'st-' + rec.status, text: T(rec.status) }), h('b', { text: T('Virtual machines') }), h('span', { text: sum.count !== undefined ? String(sum.count) : String(rows.length) }),
                    h('b', { text: T('Source') }), h('span', { text: rec.raw_bytes ? fmtSize(rec.raw_bytes) : '' }), h('b', { text: T('Stored') }), h('span', { text: rec.archive_bytes ? fmtSize(rec.archive_bytes) : '' })),
                h('div', { class: 'toolbar' }, h('label', { text: T('Show') + ' ' }), fsel), box);
            dialog({ title: 'Scheduled Task Details', width: 900, body: body, buttons: [{ label: 'HTML report...', close: false, fn: function () { saveReport(rec); return false; } }, { label: 'Close', primary: true }] });
        }, fail);
    }
    // ---- the HTML report of one history entry (built in the browser from history.get; every value is escaped) ----
    function esc(x) { return String(x === undefined || x === null ? '' : x).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;'); }
    var STCOLOR = { Success: '#1e8e3e', Warning: '#b26a00', Failed: '#c62828', Abort: '#c62828', Running: '#005fb8' };
    function reportHtml(rec) {
        var rows = recordRows(rec), out = [];
        out.push('<!doctype html><html><head><meta charset="utf-8"><title>' + esc(T('Report') + ': ' + (rec.name || rec.kind || '')) + '</title>');
        out.push('<style>body{font:14px Segoe UI,Arial,sans-serif;margin:24px;color:#1b2733}h1{font-size:20px;color:#005fb8}table{border-collapse:collapse;margin-top:12px}th,td{border:1px solid #d4dbe3;padding:5px 9px;text-align:left;vertical-align:top}th{background:#f0f3f7}td.n{text-align:right}.kv td:first-child{font-weight:600;border:0;padding-left:0}.kv td{border:0}</style></head><body>');
        out.push('<h1>' + esc(rec.name || rec.kind || '') + '</h1><table class="kv">');
        var kv = [[T('Kind'), T(rec.kind || '')], [T('Started'), fmtTime(rec.started)], [T('Finished'), fmtTime(rec.ended)], [T('Duration'), rec.ended && rec.started ? fmtDur(rec.ended - rec.started) : ''], [T('Version'), rec.version || ''], [T('Trigger'), T(rec.trigger || '')]];
        kv.forEach(function (r) { out.push('<tr><td>' + esc(r[0]) + '</td><td>' + esc(r[1]) + '</td></tr>'); });
        out.push('<tr><td>' + esc(T('Result')) + '</td><td style="color:' + (STCOLOR[rec.status] || '#1b2733') + ';font-weight:600">' + esc(T(rec.status || '')) + '</td></tr></table>');
        out.push('<table><tr>' + [T('Virtual machine'), T('Result'), T('Archive'), T('Source'), T('Stored'), T('Duration'), T('Speed'), T('Message')].map(function (x) { return '<th>' + esc(x) + '</th>'; }).join('') + '</tr>');
        rows.forEach(function (r) {
            out.push('<tr><td>' + esc(r.vm) + (r.restored_vm ? '<br>' + esc(T('Restored as') + ': ' + r.restored_vm) : '') + '</td><td style="color:' + (STCOLOR[r.status] || '#1b2733') + ';font-weight:600">' + esc(T(r.status || '') + (r.skipped ? ' (' + T('Skipped: credentials rejected') + ')' : '')) + '</td><td>' + esc(r.archive) + '</td><td class="n">' + esc(r.raw ? fmtSize(r.raw) : '') +
                '</td><td class="n">' + esc(r.size ? fmtSize(r.size) : '') + '</td><td class="n">' + esc(r.duration ? fmtDur(r.duration) : '') + '</td><td class="n">' + esc(r.rate ? fmtSpeed(r.rate) : '') + '</td><td>' + esc(r.message) + '</td></tr>');
        });
        out.push('</table></body></html>');
        return out.join('\n');
    }
    function saveReport(rec) {
        var name = String(rec.name || 'report').replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, 60) + '-' + fmtTime(rec.started).replace(/[^0-9]/g, '') + '.html';
        if (!env.saveFile) { showError(T('This browser cannot save files.')); return; }
        env.saveFile(name, 'text/html;charset=utf-8', reportHtml(rec));
    }
    function historyReport(id) { api('history.get', { id: id }).then(saveReport, fail); }

    // ---------- Backup Explorer ----------
    function viewExplorer() {
        var srv = sel([], ''), dirIn = txt(S.explorerDir || '');
        var box = h('div');
        pageEl.appendChild(h('h2', { text: T('Backup Explorer') }));
        var tools = h('div', { class: 'toolbar' }, h('label', { text: T('Target') + ' ' }), srv, h('label', { text: ' ' + T('Folder') + ' ' }), dirIn, h('button', { text: T('Show'), on: { click: load } }));
        var actions = h('div', { class: 'toolbar' },
            btn('Verify', 'backups.verify', needSel(function (r) { if (r.resumable) { toast(T('This is an unfinished backup.')); return; } passwordThen(r, function (pw) { api('backups.verify', { server: r.server, directory: r.directory, archive: r.archive, password: pw }).then(function () { toast(T('Verification started.')); pollOnce(); }, fail); }); })),
            btn('Restore...', 'backups.restore', needSel(function (r) { restoreDialog(r); })),
            btn('Delete backup', 'backups.delete', needSel(function (r) { if (env.confirm(r.resumable ? T('Discard the unfinished backup "{0}"? What was already copied is deleted.', r.archive) : T('Delete the backup "{0}"?', r.archive))) api(r.resumable ? 'resumable.discard' : 'backups.delete', { server: r.server, directory: r.directory, archive: r.archive }).then(load, fail); }), 'danger'));
        pageEl.appendChild(tools); pageEl.appendChild(actions); pageEl.appendChild(box);
        function load() {
            S.explorerDir = dirIn.value; S.explorerServer = srv.value; clear(box); box.appendChild(h('div', { class: 'note', text: T('Loading...') }));
            var a = { server: srv.value }; if (dirIn.value.trim()) a.directory = dirIn.value.trim();
            api('backups.list', a).then(function (r) {
                clear(box); var rows = (r.backups || []).slice().concat((r.resumable || []).map(function (b) { var c = JSON.parse(JSON.stringify(b)); c.resumable = true; return c; }));
                rows.sort(function (x, y) { return (y.created || 0) - (x.created || 0); });
                box.appendChild(grid([
                    { title: 'Virtual machine', get: function (r) { return r.vm; } }, { title: 'Created', get: function (r) { return fmtTime(r.created); } },
                    { title: 'Source', get: function (r) { return fmtSize(r.raw_size || r.raw_bytes); } }, { title: 'Stored', get: function (r) { return fmtSize(r.size || r.stored_bytes); } },
                    { title: 'Compression', get: function (r) { return r.compression === 'xz' ? 'xz ' + (r.level || '') : (r.compression || '?'); } }, { title: 'Encrypted', get: function (r) { return r.encrypted ? T('Yes') : T('No'); } },
                    { title: 'Volumes', get: function (r) { return r.volumes || r.volumes_done || ''; } }, { title: 'Status', get: function (r) { return r.resumable ? T('Unfinished (resumable)') : T('Complete'); } }, { title: 'Folder', get: function (r) { return r.directory; } }
                ], rows, function (r) { return r.archive + '|' + r.directory; }, null, can('backups.restore') ? function (r) { if (r.resumable) { toast(T('This is an unfinished backup.')); return; } restoreDialog(r); } : null));
            }, function (e) { clear(box); box.appendChild(h('div', { class: 'msg err', text: e.message })); });
        }
        loadServers().then(function (l) {
            l.filter(function (s) { return isTargetType(s.type); }).forEach(function (s) { srv.appendChild(h('option', { value: s.id, text: s.name, selected: s.id === (S.explorerServer || 'local') })); });
            load();
        }, fail);
    }
    function passwordThen(r, fn) {
        if (!r.encrypted) { fn(''); return; }
        var pw = txt('', 'password');
        dialog({ title: 'Password', body: field('Password', pw), buttons: [{ label: 'OK', primary: true, fn: function () { fn(pw.value); } }, { label: 'Cancel' }] });
    }
    function restoreDialog(r) {
        var dest = txt(''), pw = txt('', 'password'), dd;
        var canEsxi = can('backups.restore_esxi') && can('esxi.restore_options');
        var modeSel = sel([['folder', T('Folder')], ['esxi', T('ESXi host or vCenter')]], 'folder');
        var srvSel = sel([], ''), loadBtn = h('button', { text: T('Load') }), status = h('div', { class: 'note' });
        var dcSel = sel([], ''), dsSel = sel([], ''), folderSel = sel([], ''), poolSel = sel([], ''), hostSel = sel([], ''), nameIn = txt(''), power = chk('Power on after restore', false);
        var noSrv = h('div', { class: 'note', text: T('There is no ESXi or vCenter server yet. Add one on the Datacenter page first.') });
        var dcRow = field('Datacenter', dcSel), hostRow = field('Host', hostSel);
        var srvRow = h('div', { class: 'f' }, h('label', { text: T('Server') }), srvSel, loadBtn);
        var esxiBox = h('div', null, srvRow, dcRow, field('Datastore', dsSel), field('VM folder', folderSel), field('Resource pool', poolSel), hostRow, field('New VM name', nameIn), power, status);
        var folderBox = h('div', null, fieldBrowse('Restore to folder', dest, 'A path on the server computer', { title: 'Restore to folder' }));
        var servers = [], opts = null, busy = false, autoName = '', serial = 0, serversLoaded = false;
        function curServer() { for (var k = 0; k < servers.length; k++) if (servers[k].id === srvSel.value) return servers[k]; return null; }
        function isVc() { var c = curServer(); return !!c && c.type === 'vcenter'; }
        function showMode() {
            var e = modeSel.value === 'esxi';
            folderBox.style.display = e ? 'none' : ''; esxiBox.style.display = e ? '' : 'none';
            if (e && !serversLoaded) loadEsxiServers();
            if (e) showHaveSrv();
        }
        function showHaveSrv() {
            var have = servers.length > 0;
            srvRow.style.display = have ? '' : 'none'; noSrv.style.display = have ? 'none' : '';
            var vc = isVc(); dcRow.style.display = have && vc ? '' : 'none'; hostRow.style.display = have && vc ? '' : 'none';
            [dcSel, dsSel, folderSel, poolSel, hostSel].forEach(function (x) { x.disabled = !opts; });
        }
        function loadEsxiServers() {
            serversLoaded = true;
            loadServers().then(function (l) {
                servers = l.filter(function (x) { return x.type === 'esxi' || x.type === 'vcenter'; });
                clear(srvSel); servers.forEach(function (x) { srvSel.appendChild(h('option', { value: x.id, text: x.name + ' (' + serverTypeName(x.type) + ')' })); });
                showHaveSrv();
            }, function (e) { serversLoaded = false; fail(e); });
        }
        function fillSel(el, items) { clear(el); items.forEach(function (it) { el.appendChild(h('option', { value: String(it[0]), text: it[1] })); }); }
        function exists(vms, n) { for (var k = 0; k < (vms || []).length; k++) if (vms[k] === n) return true; return false; }
        function fill(o) {
            opts = o;
            var dcs = o.datacenters || [], cur = o.datacenter || '';
            var dl = dcs.map(function (x, k) { return [k, x]; }); if (!dl.length && cur) dl = [[0, cur]];
            fillSel(dcSel, dl); for (var k = 0; k < dl.length; k++) if (dl[k][1] === cur) dcSel.value = String(dl[k][0]);
            var ds = [[-1, T('As in the backup (every file back to its original datastore)')]];
            (o.datastores || []).forEach(function (x, k) { if (x.accessible === false) return; ds.push([k, x.name + '  -  ' + T('{0} free of {1}', fmtSize(x.free), fmtSize(x.capacity))]); });
            fillSel(dsSel, ds);
            var fo = [[-1, T('(the root VM folder)')]]; (o.folders || []).forEach(function (x, k) { if (x.path) fo.push([k, x.path]); }); fillSel(folderSel, fo);
            var po = [[-1, T('(the default resource pool)')]]; (o.pools || []).forEach(function (x, k) { po.push([k, x.path || x.name || '']); }); fillSel(poolSel, po);
            var ho = [[-1, T('(chosen by the server)')]]; (o.hosts || []).forEach(function (x, k) { ho.push([k, x.name]); }); fillSel(hostSel, ho);
            var want = r.vm || '', nm = want;
            for (var q = 0; q < 100 && exists(o.vms, nm); q++) nm = q === 0 ? want + '-restored' : want + '-restored-' + (q + 1);
            if (!nameIn.value || nameIn.value === autoName) nameIn.value = nm;
            autoName = nm;
            status.textContent = T('{0} datastore(s), {1} folder(s), {2} resource pool(s) found.', (o.datastores || []).length, (o.folders || []).length, (o.pools || []).length);
            showHaveSrv();
        }
        function load(dc) {
            var c = curServer(); if (!c) { dd.error(T('Choose the ESXi or vCenter server to restore to.')); return; }
            var my = ++serial; busy = true; loadBtn.disabled = true; status.textContent = T('Loading...');
            var a = { server: c.id }; if (dc) a.datacenter = dc;
            api('esxi.restore_options', a).then(function (o) { if (my !== serial) return; busy = false; loadBtn.disabled = false; fill(o); },
                function (e) { if (my !== serial) return; busy = false; loadBtn.disabled = false; opts = null; status.textContent = T('Cannot read: ') + e.message; showHaveSrv(); });
        }
        loadBtn.addEventListener('click', function () { load(''); });
        srvSel.addEventListener('change', function () { serial++; busy = false; loadBtn.disabled = false; opts = null; status.textContent = ''; showHaveSrv(); });
        dcSel.addEventListener('change', function () { var o = dcSel.options[dcSel.selectedIndex]; if (o) load(o.textContent); });
        modeSel.addEventListener('change', showMode);
        function esxiRequest() {
            var c = curServer();
            if (!c) { dd.error(T('Choose the ESXi or vCenter server to restore to.')); return null; }
            if (busy) { dd.error(T('Wait until the choices of the server are loaded.')); return null; }
            if (!opts) { dd.error(T('Press Load to read the datacenters, datastores, folders and resource pools of the server.')); return null; }
            var dc = opts.datacenter || ''; if (c.type === 'vcenter' && dcSel.options.length && dcSel.selectedIndex >= 0) dc = dcSel.options[dcSel.selectedIndex].textContent;
            if (!dc) { dd.error(T('Choose the datacenter.')); return null; }
            if (dsSel.selectedIndex < 0) { dd.error(T('Choose the datastore.')); return null; }
            var name = nameIn.value.trim();
            if (!name) { dd.error(T('Enter a name for the new virtual machine.')); return null; }
            if (name.length > 80 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf('%') >= 0) { dd.error(T('The name of the virtual machine must be at most 80 characters and cannot contain slashes or %.')); return null; }
            if (exists(opts.vms, name)) { dd.error(T('A virtual machine with this name already exists on the server. Enter another name; an existing machine is never overwritten.')); return null; }
            var ds = '', folder = '', pool = '', host = '', di = parseInt(dsSel.value, 10);
            if (di >= 0) {
                var x = opts.datastores[di]; ds = x.name; var need = r.raw_size || r.raw_bytes || 0;
                if (x.free > 0 && need > x.free && !env.confirm(T('The datastore "{0}" has {1} free, the backup holds up to {2}. Restore anyway?', ds, fmtSize(x.free), fmtSize(need)))) return null;
            }
            var fi = parseInt(folderSel.value, 10); if (fi >= 0) folder = opts.folders[fi].id;
            var pi = parseInt(poolSel.value, 10); if (pi >= 0) pool = opts.pools[pi].id;
            if (c.type === 'vcenter') { var hi = parseInt(hostSel.value, 10); if (hi >= 0) host = opts.hosts[hi].id; }
            return { server: r.server, directory: r.directory, archive: r.archive, password: pw.value, esxi_server: c.id,
                     options: { vm_name: name, datacenter: dc, datastore: ds, folder: folder, pool: pool, host: host, power_on: !!power.input.checked } };
        }
        dd = dialog({ title: 'Restore', width: 560, body: h('div', null, h('div', { class: 'note', text: T('Restoring "{0}" of {1}.', r.archive, r.vm) }),
            canEsxi ? field('Restore to', modeSel) : null, r.encrypted ? field('Password', pw) : null, folderBox, canEsxi ? esxiBox : null, canEsxi ? noSrv : null),
            buttons: [{ label: 'Restore', primary: true, fn: function (d) {
                if (r.encrypted && !pw.value) { d.error(T('This backup is encrypted: enter its password.')); return false; }
                var cmd, body;
                if (canEsxi && modeSel.value === 'esxi') { body = esxiRequest(); if (!body) return false; cmd = 'backups.restore_esxi'; }
                else {
                    if (!dest.value.trim()) { d.error(T('Enter the destination folder.')); return false; }
                    body = { server: r.server, directory: r.directory, archive: r.archive, dest: dest.value.trim(), password: pw.value }; cmd = 'backups.restore';
                }
                api(cmd, body).then(function () { d.close(); toast(T('Restore started.')); pollOnce(); }, function (e) { d.error(e.message); }); return false;
            } }, { label: 'Cancel' }] });
        showMode();
        return dd;
    }

    // ---------- Web access (admin): the site administers itself ----------
    function pwProblem(user, pw) {
        if (pw.length < 10) return T('The password must be at least 10 characters long.');
        var lp = pw.toLowerCase(), lu = (user || '').toLowerCase();
        if (lu && (lp === lu || (lu.length >= 4 && lp.indexOf(lu) >= 0))) return T('The password must not contain the user name.');
        var same = true; for (var i = 1; i < pw.length; i++) if (pw[i] !== pw[0]) same = false;
        if (same) return T('The password is too simple.');
        return '';
    }
    function isLoopbackEntry(e) { return e === 'localhost' || e === '::1' || /^127\./.test(e); }
    function siteUrl(port) { var hn = (env.location && env.location.hostname) || '127.0.0.1'; return 'https:' + '//' + (hn.indexOf(':') >= 0 ? '[' + hn + ']' : hn) + ':' + port + '/'; }
    function qrTable(text) {
        var q = env.qr ? env.qr.encode(text) : null;
        if (!q) return h('div', { class: 'note', text: T('The QR code is not available.') });
        var tb = h('table', { class: 'qr' });
        for (var y = 0; y < q.size; y++) { var tr = h('tr'); for (var x = 0; x < q.size; x++) tr.appendChild(h('td', { class: q.modules[y][x] ? 'b' : '' })); tb.appendChild(tr); }
        return h('div', { class: 'qrbox' }, tb);
    }
    function viewWebAccess() {
        pageEl.appendChild(h('h2', { text: T('Web access') }));
        var box = h('div'); pageEl.appendChild(box);
        Promise.all([api('web.status'), api('web.settings.get'), api('web.interfaces.list'), api('web.users.list')]).then(function (r) {
            renderWebAccess(box, { status: r[0], settings: r[1].settings || {}, warnings: r[1].warnings || [], ifaces: r[2].interfaces || [], users: r[3].users || [] });
        }, fail);
    }
    function renderWebAccess(box, W) {
        clear(box);
        var st = W.settings, bind = (st.bind || []).slice();
        // -- listening interfaces --
        var loEntry = 'localhost'; for (var i = 0; i < bind.length; i++) if (isLoopbackEntry(bind[i])) { loEntry = bind[i]; break; }
        var rows = [];      // { entry, input, note }
        var ifBox = h('div', { class: 'ifs' });
        function statusOf(entry) {
            var ents = (W.status.entries || []);
            for (var k = 0; k < ents.length; k++) if (ents[k].entry === entry) return ents[k].listening ? T('listening') : (ents[k].error || T('not listening'));
            return '';
        }
        function addRow(label, entry, checked, disabled) {
            var c = h('input', { type: 'checkbox', checked: checked, disabled: disabled });
            var stt = checked ? statusOf(entry) : '';
            var note = h('span', { class: 'note', text: stt }); if (stt && stt !== T('listening')) note.className = 'note bad';
            rows.push({ entry: entry, input: c });
            ifBox.appendChild(h('div', { class: 'ifrow' }, h('label', null, c, ' ' + label), ' ', note));
        }
        addRow(T('This computer only (localhost)') + ' - ' + loEntry, loEntry, true, true);
        addRow(T('All interfaces'), 'all', bind.indexOf('all') >= 0, false);
        var known = { all: 1 }; known[loEntry] = 1;
        W.ifaces.forEach(function (a) {
            if (a.loopback) return;
            var addrs = (a.addresses || []).map(function (x) { return x.address; }).join(', ');
            var e = 'adapter:' + a.name; known[e] = 1;
            addRow((a.friendly || a.name) + (a.description ? ' - ' + a.description : '') + (addrs ? ' (' + addrs + ')' : '') + (a.up ? '' : ' - ' + T('down')), e, bind.indexOf(e) >= 0, false);
        });
        bind.forEach(function (e) { if (!known[e]) { known[e] = 1; addRow(e, e, true, false); } });
        var addIn = txt(''), port = num(st.port || 8443, 1), webMsg = h('div', { class: 'msg' });
        var addBtn = h('button', { text: T('Add address'), on: { click: function () { var v = addIn.value.trim(); if (!v) return; if (known[v]) { addIn.value = ''; return; } known[v] = 1; addRow(v, v, true, false); addIn.value = ''; } } });
        var applyBtn = h('button', { class: 'primary', text: T('Apply'), on: { click: function () {
            var list = []; rows.forEach(function (r) { if (r.input.checked) list.push(r.entry); });
            var np = parseInt(port.value, 10);
            api('web.settings.set', { bind: list, port: np }).then(function (res) {
                if (np !== st.port) { webMsg.className = 'msg info'; webMsg.textContent = T('The site is moving to port {0}. Open {1} and sign in again.', np, siteUrl(np)); }
                else { toast(T('Saved.')); if (env.setTimeout) env.setTimeout(refreshView, 1500); else refreshView(); }
            }, function (e) { webMsg.className = 'msg err'; webMsg.textContent = e.message; });
        } } });
        box.appendChild(h('h3', { text: T('Listening interfaces') }));
        box.appendChild(h('div', { class: 'note', text: T('The site always stays reachable on this computer (localhost): that is how a mistake here can be repaired.') }));
        box.appendChild(ifBox);
        box.appendChild(h('div', { class: 'toolbar' }, h('label', { text: T('Port') + ' ' }), port, h('label', { text: ' ' + T('Address') + ' ' }), addIn, addBtn, applyBtn));
        box.appendChild(webMsg);
        (W.warnings || []).forEach(function (w) { box.appendChild(h('div', { class: 'msg warn', text: w })); });
        // -- security --
        var tfa = chk('Require two-factor authentication for administrators', st.require_2fa_admins), sess = num(st.session_minutes || 20, 1), proxy = chk('Trust X-Forwarded-For from a reverse proxy on this computer', st.trust_proxy);
        var secMsg = h('div', { class: 'msg' });
        box.appendChild(h('h3', { text: T('Security') }));
        box.appendChild(h('div', { class: 'f' }, tfa)); box.appendChild(field('Session idle timeout, minutes', sess)); box.appendChild(h('div', { class: 'f' }, proxy));
        box.appendChild(h('div', { class: 'note', text: T('Enable this only behind a reverse proxy that you control and that sets X-Forwarded-For itself; otherwise anybody can fake the address that is shown in the audit log and used for the login limits.') }));
        box.appendChild(h('div', { class: 'toolbar' }, h('button', { class: 'primary', text: T('Apply'), on: { click: function () {
            api('web.settings.set', { require_2fa_admins: tfa.input.checked, session_minutes: parseInt(sess.value, 10), trust_proxy: proxy.input.checked }).then(function () { toast(T('Saved.')); }, function (e) { secMsg.className = 'msg err'; secMsg.textContent = e.message; });
        } } })));
        box.appendChild(secMsg);
        // -- certificate --
        var c = W.status.cert || {};
        box.appendChild(h('h3', { text: T('Certificate') }));
        box.appendChild(h('div', { class: 'kv' }, h('b', { text: T('Subject') }), h('span', { text: c.subject || '' }), h('b', { text: T('Valid from') }), h('span', { text: fmtTime(c.not_before) }), h('b', { text: T('Valid until') }), h('span', { text: fmtTime(c.not_after) }),
            h('b', { text: T('Fingerprint') }), h('span', { class: 'mono', text: c.fingerprint || '' }), h('b', { text: T('Names') }), h('span', { text: (c.names || []).join(', ') }), h('b', { text: T('Kind') }), h('span', { text: c.self_signed ? T('Self-signed') : T('Imported') })));
        box.appendChild(h('div', { class: 'toolbar' }, h('button', { text: T('Create new self-signed...'), on: { click: function () { certGenerateDialog(); } } }), h('button', { text: T('Upload PFX...'), on: { click: function () { certUploadDialog(); } } })));
        // -- users --
        box.appendChild(h('h3', { text: T('Users') }));
        S.sel = {};
        var sw = function (v) { return v; };
        box.appendChild(h('div', { class: 'toolbar' },
            h('button', { text: T('Add user...'), on: { click: function () { userDialog(null); } } }),
            h('button', { text: T('Edit'), on: { click: needSel(function (r) { userDialog(r); }) } }),
            h('button', { class: 'danger', text: T('Delete'), on: { click: needSel(function (r) { if (env.confirm(T('Delete user "{0}"?', r.name))) api('web.users.delete', { name: r.name }).then(refreshView, fail); }) } }),
            h('button', { text: T('Set up two-factor...'), on: { click: needSel(function (r) { totpDialog(r); }) } }),
            h('button', { text: T('Disable two-factor'), on: { click: needSel(function (r) { if (env.confirm(T('Disable two-factor authentication of "{0}"?', r.name))) api('web.users.totp_disable', { name: r.name }).then(refreshView, fail); }) } })));
        box.appendChild(grid([
            { title: 'Name', get: function (r) { return r.name; } }, { title: 'Role', get: function (r) { return T(r.role); } },
            { title: 'Two-factor', get: function (r) { return r.totp_enabled ? T('on') + ' (' + T(r.twofa) + ')' : r.totp_pending ? T('not confirmed') : T('off') + ' (' + T(r.twofa) + ')'; } },
            { title: 'Recovery codes left', get: function (r) { return r.totp_enabled ? r.recovery_left : ''; } }, { title: 'Created', get: function (r) { return fmtTime(r.created); } }
        ], W.users, function (r) { return r.name; }, null, userDialog));
        // -- audit --
        var auditPre = h('pre', { class: 'audit' }), lines = sel([[50, '50'], [200, '200'], [1000, '1000']], 200);
        function loadAudit() { api('web.audit.tail', { lines: parseInt(lines.value, 10) }).then(function (r) { auditPre.textContent = r.text || ''; }, fail); }
        box.appendChild(h('h3', { text: T('Audit log') }));
        box.appendChild(h('div', { class: 'toolbar' }, h('label', { text: T('Lines') + ' ' }), lines, h('button', { text: T('Refresh'), on: { click: loadAudit } })));
        box.appendChild(auditPre); loadAudit();
        void sw;
    }
    function certGenerateDialog() {
        var names = h('textarea', { rows: 4 });
        dialog({ title: 'Create a self-signed certificate', width: 520, body: h('div', null, h('div', { class: 'note', text: T('The certificate always includes this computer name, localhost, 127.0.0.1 and ::1. Add the DNS names and IP addresses under which the site is reached, one per line.') }), field('Additional names', names)),
            buttons: [{ label: 'Create', primary: true, fn: function (dd) {
                var list = names.value.split(/[\s,;]+/).filter(function (x) { return x; });
                api('web.cert.generate', { names: list }).then(function () { dd.close(); toast(T('A new certificate was created. Your browser will ask you to trust it again.')); if (env.setTimeout) env.setTimeout(refreshView, 1500); }, function (e) { dd.error(e.message); }); return false;
            } }, { label: 'Cancel' }] });
    }
    function certUploadDialog() {
        var file = h('input', { type: 'file' }), pw = txt('', 'password');
        dialog({ title: 'Upload a certificate (PFX)', width: 520, body: h('div', null, h('div', { class: 'note', text: T('A PKCS#12 file (.pfx) with the certificate and its private key, 1 MB at most. Only the certificate itself is served, not the chain.') }), field('File', file), field('Password of the file', pw)),
            buttons: [{ label: 'Upload', primary: true, fn: function (dd) {
                var f = file.files && file.files[0];
                if (!f) { dd.error(T('Choose the file.')); return false; }
                if (f.size > 1048576) { dd.error(T('The file is larger than 1 MB.')); return false; }
                if (!env.readFile) { dd.error(T('This browser cannot read files.')); return false; }
                env.readFile(f).then(function (b64) {
                    var body = { pfx_base64: b64, password: pw.value }; pw.value = '';
                    return api('web.cert.import', body);
                }).then(function () { dd.close(); toast(T('The certificate was installed.')); if (env.setTimeout) env.setTimeout(refreshView, 1500); }, function (e) { dd.error(e.message); }); return false;
            } }, { label: 'Cancel' }] });
    }
    function userDialog(r) {
        var edit = !!(r && r.name);
        var name = txt(edit ? r.name : ''), role = sel([['admin', T('admin')], ['operator', T('operator')], ['viewer', T('viewer')]], edit ? r.role : 'operator'), pw = txt('', 'password');
        var tf = sel([['off', T('off')], ['optional', T('optional')], ['required', T('required')]], edit ? r.twofa : 'optional'), hint = h('div', { class: 'note' });
        if (edit) name.disabled = true;
        function upd() { hint.textContent = pw.value ? pwProblem(name.value.trim(), pw.value) : (edit ? T('Leave empty to keep the password.') : T('At least 10 characters, not containing the user name.')); }
        pw.addEventListener('input', upd); name.addEventListener('input', upd); upd();
        dialog({ title: edit ? 'Edit user' : 'Add user', width: 520, body: h('div', null, field('User', name), field('Role', role), field('Password', pw), hint, field('Two-factor', tf),
            h('div', { class: 'note', text: T('An administrator needs two-factor authentication when it is required in the security settings.') })),
            buttons: [{ label: 'OK', primary: true, fn: function (dd) {
                var nm = name.value.trim(); if (!nm) { dd.error(T('Enter the user name.')); return false; }
                if (!edit && !pw.value) { dd.error(T('A new user needs a password.')); return false; }
                if (pw.value) { var pp = pwProblem(nm, pw.value); if (pp) { dd.error(pp); return false; } }
                var body = { name: nm, role: role.value, twofa: tf.value }; if (pw.value) body.password = pw.value;
                api('web.users.save', body).then(function () { pw.value = ''; dd.close(); refreshView(); }, function (e) { dd.error(e.message); }); return false;
            } }, { label: 'Cancel' }] });
    }
    function totpDialog(u) {
        api('web.users.totp_enroll', { name: u.name }).then(function (en) {
            var code = txt('');
            dialog({ title: 'Set up two-factor authentication', width: 560, body: h('div', null,
                h('div', { class: 'note', text: T('Scan the code with an authenticator app (or type the secret), then enter the 6-digit code it shows.') }),
                qrTable(en.uri || ''), h('div', { class: 'kv' }, h('b', { text: T('Secret') }), h('span', { class: 'mono', text: en.secret || '' }), h('b', { text: T('Link') }), h('span', { class: 'mono', text: en.uri || '' })), field('Code', code)),
                buttons: [{ label: 'Confirm', primary: true, fn: function (dd) {
                    api('web.users.totp_confirm', { name: u.name, code: code.value.trim() }).then(function (cf) {
                        dd.close();
                        dialog({ title: 'Recovery codes', width: 460, body: h('div', null, h('div', { class: 'msg warn', text: T('Write these one-time codes down now: they are shown only once.') }), h('pre', { class: 'audit', text: (cf.recovery_codes || []).join('\n') })),
                            buttons: [{ label: 'Close', primary: true, fn: function () { refreshView(); } }] });
                    }, function (e) { dd.error(e.message); }); return false;
                } }, { label: 'Cancel' }] });
        }, fail);
    }

    // ---------- Settings ----------
    function viewSettings() {
        pageEl.appendChild(h('h2', { text: T('Settings') }));
        var box = h('div'); pageEl.appendChild(box);
        api('state').then(function (st) {
            S.state = st;
            var s = st.settings || {};
            var kv = h('div', { class: 'kv' });
            function row(k, v) { kv.appendChild(h('b', { text: T(k) })); kv.appendChild(h('span', { text: String(v) })); }
            row('Version', st.version); row('Data folder', st.data_dir); row('Scheduler', st.scheduler ? T('on') : T('off'));
            row('Parallel jobs', s.max_parallel); row('History entries kept', s.max_history); row('Speed limit, Mbit/s (0 = unlimited)', s.speed_limit_mbit);
            row('Compression level', (s.level || 5) + ' - ' + levelName(s.level || 5)); row('Compression threads', s.threads ? s.threads : T('Auto')); row('Retries after a lost connection', s.retry_attempts); row('First wait before a reconnect, s', s.retry_base_sec); row('Longest wait before a reconnect, s', s.retry_max_sec); row('Volume size, MiB (0 = one volume)', s.segment_mib); row('Keep the computer awake while the scheduler is on', s.prevent_sleep ? T('Yes') : T('No')); row('Default local folder', s.default_local_path);
            box.appendChild(kv);
            box.appendChild(h('div', { class: 'toolbar' },
                btn(st.scheduler ? 'Switch the scheduler off' : 'Switch the scheduler on', 'scheduler.set', function () { api('scheduler.set', { enabled: !st.scheduler }).then(refreshView, fail); }),
                btn('Change settings...', 'settings.set', function () { settingsDialog(s); })));
        }, fail);
    }
    function settingsDialog(s) {
        var par = num(s.max_parallel, 1), hist = num(s.max_history, 10), speed = num(s.speed_limit_mbit, 0), retries = num(s.retry_attempts, 0), rbase = num(s.retry_base_sec, 1), rmax = num(s.retry_max_sec, 1), segm = num(s.segment_mib, 0), awake = chk('Keep the computer awake while the scheduler is on', s.prevent_sleep), lvl = levelSel(s.level || 5), lvlNote = h('div', { class: 'note' }), thr = threadsSel(s.threads || 0, (S.state && S.state.cpus)), local = txt(s.default_local_path);
        lvl.addEventListener('change', function () { lvlNote.textContent = levelHint(parseInt(lvl.value, 10)); }); lvlNote.textContent = levelHint(parseInt(lvl.value, 10));
        dialog({ title: 'Settings', width: 520, body: h('div', null, field('Parallel jobs', par), field('History entries kept', hist), field('Speed limit, Mbit/s (0 = unlimited)', speed), field('Retries after a lost connection', retries), field('First wait before a reconnect, s', rbase), field('Longest wait before a reconnect, s', rmax), field('Volume size, MiB (0 = one volume)', segm), h('div', { class: 'f' }, awake), field('Compression level', lvl), lvlNote, field('Compression threads', thr), h('div', { class: 'note', text: T('The figures are approximate (typical data). More CPU cores make compression faster.') }), fieldBrowse('Default local folder', local, '', { title: 'Default local folder' })),
            buttons: [{ label: 'OK', primary: true, fn: function (dd) {
                var nums = [par, hist, retries, rbase, rmax, segm].map(function (x) { return parseInt(x.value, 10); });
                if (nums.some(function (x) { return isNaN(x); })) { dd.error(T('Enter a whole number in every numeric field.')); return false; }
                if (nums[0] < 1 || nums[0] > 16) { dd.error(T('Parallel jobs: 1 to 16.')); return false; }
                if (nums[3] < 1 || nums[4] < 1 || nums[3] > 86400 || nums[4] > 86400) { dd.error(T('The waits must be between 1 and 86400 seconds.')); return false; }
                if (nums[4] < nums[3]) { dd.error(T('The longest wait cannot be shorter than the first one.')); return false; }
                if (nums[5] < 0) { dd.error(T('The volume size cannot be negative.')); return false; }
                if (parseFloat(speed.value) < 0) { dd.error(T('The speed limit cannot be negative.')); return false; }
                api('settings.set', { max_parallel: parseInt(par.value, 10), max_history: parseInt(hist.value, 10), speed_limit_mbit: parseFloat(speed.value) || 0, retry_attempts: parseInt(retries.value, 10), retry_base_sec: parseInt(rbase.value, 10), retry_max_sec: parseInt(rmax.value, 10), segment_mib: parseInt(segm.value, 10), prevent_sleep: awake.input.checked, level: parseInt(lvl.value, 10), threads: parseInt(thr.value, 10), default_local_path: local.value.trim() })
                    .then(function () { dd.close(); refreshView(); }, function (e) { dd.error(e.message); }); return false;
            } }, { label: 'Cancel' }] });
    }

    function start(rootEl) {
        root = rootEl;
        var want = (env.savedLang && env.savedLang()) || pickLang(env.navigator); if (!knownLang(want)) want = 'en';
        return ensureLang(want).then(function () { setLang(want); return checkSession(); });
    }

    return { start: start, T: T, pollOnce: pollOnce, reportHtml: reportHtml, restoreDialog: restoreDialog, userDialog: userDialog, totpDialog: totpDialog, pwProblem: pwProblem, certUploadDialog: certUploadDialog, certGenerateDialog: certGenerateDialog, h: h, can: can, fmtSize: fmtSize, fmtDur: fmtDur, fmtTime: fmtTime, describeSchedule: describeSchedule, scheduleFromForm: scheduleFromForm,
             setLang: setLang, pickLang: pickLang, state: S, api: api, showLogin: showLogin, showApp: showApp, go: go, vmBackupDialog: vmBackupDialog, taskDialog: taskDialog, serverWizard: serverWizard, renderJobs: renderJobs };
}
if (typeof module !== 'undefined') module.exports = createApp;
