/**
 * ESP-NODE web UI: one page, no libraries.
 *
 * Node mode:   Overview, Ports, Network, RDM, Scenes, Tools, System.
 * Tester mode: Tester (per port: send / receive / RDM, plus the cable test),
 *              Network, System. Chosen by the firmware at start-up
 *              (/api/system/info -> tester.enabled); switching restarts it.
 *
 * Pages and port sections are built from /api/system/info: the number of
 * ports and the features the firmware was built with (features.*).
 * Levels arrive over the WebSocket as binary frames; everything else is
 * polled from the JSON API.
 */
'use strict';

// ============================================================================
// Helpers
// ============================================================================

/** Escape text from the device (scene names, RDM labels, sACN source names)
 *  before it goes into innerHTML. */
function esc(v) {
    return String(v ?? '').replace(/[&<>"']/g, c => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
    }[c]));
}

/** 15-bit Art-Net Port-Address -> "Net:Sub:Uni" */
function netSubUni(addr) {
    addr = parseInt(addr) || 0;
    return `${(addr >> 8) & 0x7F}:${(addr >> 4) & 0xF}:${addr & 0xF}`;
}

/** Dotted IPv4 -> unsigned 32-bit number, null when invalid */
function ipv4(s) {
    const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(String(s ?? '').trim());
    if (!m) return null;
    const p = m.slice(1).map(Number);
    if (p.some(x => x > 255)) return null;
    return ((p[0] << 24) >>> 0) + (p[1] << 16) + (p[2] << 8) + p[3];
}

const $ = id => document.getElementById(id);
const sleep = ms => new Promise(r => setTimeout(r, ms));
function setText(id, t) {
    const e = typeof id === 'string' ? $(id) : id;
    if (e && e.textContent !== String(t)) e.textContent = t;
}
function plural(n, one, many) { return `${n} ${n === 1 ? one : (many || one + 's')}`; }
function clampInt(v, lo, hi, d) {
    v = parseInt(v);
    return Number.isFinite(v) ? Math.min(hi, Math.max(lo, v)) : d;
}
function ms(us, digits) {
    if (!us) return '--';
    const v = us / 1000;
    return v.toFixed(digits ?? (v < 10 ? 2 : 1)) + ' ms';
}
function hex2(n) { return '0x' + (n || 0).toString(16).toUpperCase().padStart(2, '0'); }
function ago(s) {
    if (s < 0) return 'never';
    if (s < 60) return `${s} s ago`;
    if (s < 3600) return `${Math.floor(s / 60)} min ago`;
    return `${Math.floor(s / 3600)} h ago`;
}

const ICONS = {
    overview: '<rect x="3" y="3" width="7" height="9" rx="1.5"/><rect x="14" y="3" width="7" height="5" rx="1.5"/><rect x="14" y="12" width="7" height="9" rx="1.5"/><rect x="3" y="16" width="7" height="5" rx="1.5"/>',
    tester: '<path d="M9 3h6M10 3v6L4.5 18.5A1.7 1.7 0 0 0 6 21h12a1.7 1.7 0 0 0 1.5-2.5L14 9V3"/><path d="M7 15h10"/>',
    ports: '<circle cx="12" cy="12" r="9"/><circle cx="12" cy="7.5" r="1.2"/><circle cx="7.6" cy="10.8" r="1.2"/><circle cx="16.4" cy="10.8" r="1.2"/><circle cx="9.3" cy="16" r="1.2"/><circle cx="14.7" cy="16" r="1.2"/>',
    network: '<path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M2 9a14.5 14.5 0 0 1 20 0"/><circle cx="12" cy="19.5" r="1"/>',
    rdm: '<path d="M21 16V8a2 2 0 0 0-1-1.7l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.7l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><path d="M3.3 7 12 12l8.7-5M12 22V12"/>',
    scenes: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 9h18M8 4v5M16 4v5"/>',
    tools: '<path d="M14.7 6.3a4 4 0 0 0-5.4 5.2L3 17.8V21h3.2l6.3-6.3a4 4 0 0 0 5.2-5.4l-2.6 2.6-2.4-.6-.6-2.4z"/>',
    system: '<circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M4.9 4.9 7 7M17 17l2.1 2.1M2 12h3M19 12h3M4.9 19.1 7 17M17 7l2.1-2.1"/>',
};
const icon = name => `<svg class="i" viewBox="0 0 24 24" aria-hidden="true">${ICONS[name] || ''}</svg>`;

/* Pages: node = shown in node mode, tester = shown in tester mode */
const PAGES = [
    { id: 'overview', label: 'Overview', node: true },
    { id: 'tester', label: 'Tester', tester: true },
    { id: 'ports', label: 'Ports', node: true },
    { id: 'network', label: 'Network', node: true, tester: true },
    { id: 'rdm', label: 'RDM', node: true, feature: 'rdm' },
    { id: 'scenes', label: 'Scenes', node: true, feature: 'scenes' },
    { id: 'tools', label: 'Tools', node: true, feature: 'dmx_tools' },
    { id: 'system', label: 'System', node: true, tester: true, sep: true },
];

const MODE_NAMES = { 0: 'Disabled', 1: 'DMX output', 2: 'DMX input', 3: 'RDM controller', 4: 'RDM responder' };
const PROTOCOLS = [
    [0, 'Art-Net only'], [1, 'sACN only'], [2, 'Both, Art-Net wins'], [3, 'Both, sACN wins'], [4, 'Both, merged'],
];
const MERGES = [
    [0, 'HTP: highest level wins'], [1, 'LTP: latest change wins'], [2, 'Last source to send wins'],
    [3, 'Backup: primary, then backup address'], [4, 'No merge: first source only'],
];
const RULE_TYPES = [['patch', 'Patch'], ['limit', 'Limit'], ['park', 'Park']];
const RULES_MAX = 8;
const SEND_LIST_MAX = 4;   /* input port unicast addresses (config CONFIG_SEND_LIST_MAX) */

/** Why a channel rule is not valid (as the firmware checks it), or '' */
function ruleProblem(r) {
    const ok = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
    if (!RULE_TYPES.some(([t]) => t === r.type)) return 'Pick patch, limit or park';
    if (!ok(r.channel, 1, 512)) return 'The channel must be 1–512';
    if (!ok(r.count, 1, 512) || r.channel + r.count - 1 > 512) return 'The channels must stay within 1–512';
    if (r.type === 'patch') {
        if (!ok(r.from, 1, 512) || r.from + r.count - 1 > 512) return 'The channels it copies must stay within 1–512';
    } else if (!ok(r.level, 0, 255)) {
        return 'The level must be 0–255';
    }
    return '';
}

function chRange(first, count) { return count > 1 ? `${first}–${first + count - 1}` : `${first}`; }

/** A rule in words */
function ruleText(r) {
    if (ruleProblem(r)) return ruleProblem(r);
    const many = r.count > 1;
    const ch = `Channel${many ? 's' : ''} ${chRange(r.channel, r.count)}`;
    const pct = v => `${v} (${Math.round(v / 2.55)}%)`;
    if (r.type === 'patch') return `${ch} ${many ? 'output' : 'outputs'} what arrives on ${chRange(r.from, r.count)}.`;
    if (r.type === 'limit') return `${ch} never ${many ? 'go' : 'goes'} above ${pct(r.level)}.`;
    return `${ch} ${many ? 'stay' : 'stays'} at ${pct(r.level)}, whatever arrives.`;
}

/** Dotted IPv4 strings in the same subnet? */
function sameSubnet(a, b, mask) {
    const x = ipv4(a), y = ipv4(b), m = ipv4(mask);
    return x !== null && y !== null && m !== null && ((x & m) >>> 0) === ((y & m) >>> 0);
}

const ROLES = {
    off: { label: 'Off', chip: '', help: 'The port is switched off. Pick a job for it.' },
    send: { label: 'Send', chip: 'out', help: 'Outputs DMX. Set levels with the faders, run test patterns and change the output timing.' },
    receive: { label: 'Receive', chip: 'in', help: 'Listens to a DMX line: live levels, refresh rate, slots, break / MAB, line errors, frame jitter, flicker and signal loss.' },
    rdm: { label: 'RDM', chip: 'info', help: 'RDM controller: finds fixtures, sets their DMX address and makes them identify. It also outputs the levels set here.' },
};
const TIMING_PRESETS = [
    { name: 'Standard', rate_hz: 44, break_us: 176, mab_us: 12, slots: 512, help: '44 Hz, 176 / 12 µs, 512 slots' },
    { name: 'Spec minimum', rate_hz: 44, break_us: 92, mab_us: 12, slots: 512, help: 'shortest break and MAB E1.11 allows' },
    { name: 'Slow and long', rate_hz: 20, break_us: 400, mab_us: 100, slots: 512, help: 'for old or fussy fixtures' },
    { name: 'Short frames', rate_hz: 44, break_us: 176, mab_us: 12, slots: 24, help: '24 slots' },
];

// ============================================================================
// DMX level grid: 512 cells, only changed cells are touched
// ============================================================================

class LevelGrid {
    constructor(el, opts = {}) {
        this.el = el;
        this.cells = [];
        this.cols = 0;
        this.vals = new Uint8Array(512);
        this.drawn = null;
        this.marks = {};
        el.classList.add('levels');
        if (opts.onClick) {
            el.classList.add('click');
            el.addEventListener('click', e => {
                const c = e.target.closest('.lv');
                if (c) opts.onClick(parseInt(c.dataset.ch));
            });
        }
        el.addEventListener('mouseover', e => {
            const c = e.target.closest('.lv');
            if (!c) return;
            const ch = parseInt(c.dataset.ch), v = this.vals[ch - 1] || 0;
            c.title = `Channel ${ch}: ${v} (${Math.round(v / 2.55)}%)`;
        });
        if (window.ResizeObserver) new ResizeObserver(() => this.layout()).observe(el);
        this.layout();
    }

    layout() {
        const w = this.el.clientWidth;
        if (!w) return;   /* hidden: done when it is shown */
        const cols = w >= 640 ? 32 : 16;
        const cell = (w - 36) / cols;
        this.el.classList.toggle('dense', cell < 24);
        this.el.classList.toggle('tight', cell < 15);
        if (cols === this.cols && this.cells.length) return;
        this.cols = cols;
        this.el.style.setProperty('--cols', cols);
        const frag = document.createDocumentFragment();
        const cells = [];
        for (let i = 0; i < 512; i++) {
            if (i % cols === 0) {
                const l = document.createElement('div');
                l.className = 'lv-row';
                l.textContent = i + 1;
                frag.appendChild(l);
            }
            const c = document.createElement('div');
            c.className = 'lv z';
            c.dataset.ch = i + 1;
            c.appendChild(document.createElement('b'));
            frag.appendChild(c);
            cells.push(c);
        }
        this.el.replaceChildren(frag);
        this.cells = cells;
        this.drawn = null;
        this.paint();
        for (const cls in this.marks) this._applyMark(cls);
    }

    set(levels, live = true, input = false) {
        this.vals = levels;
        this.el.classList.toggle('in', !!input);
        this.el.classList.toggle('stale', !live);
        this.paint();
    }

    paint() {
        if (!this.cells.length) return;
        const prev = this.drawn, v = this.vals;
        for (let i = 0; i < 512; i++) {
            const x = v[i] || 0;
            if (prev && prev[i] === x) continue;
            const c = this.cells[i];
            c.firstChild.textContent = app.fmtLevel(x);
            c.style.setProperty('--p', (x / 2.55).toFixed(1) + '%');
            c.classList.toggle('z', x === 0);
        }
        this.drawn = Uint8Array.from(v);
    }

    redraw() { this.drawn = null; this.paint(); }

    /** Give the channels in @p set (1-based numbers) the class @p cls */
    mark(cls, set) {
        this.marks[cls] = set instanceof Set ? set : new Set(set || []);
        this._applyMark(cls);
    }

    _applyMark(cls) {
        const set = this.marks[cls];
        if (!this.cells.length || !set) return;
        for (let i = 0; i < 512; i++) {
            const on = set.has(i + 1);
            if (this.cells[i].classList.contains(cls) !== on) this.cells[i].classList.toggle(cls, on);
        }
    }
}

// ============================================================================
// Application
// ============================================================================

const app = {
    info: null,
    features: {},
    nports: 2,
    tester: false,
    started: false,
    wsToken: null,
    authReloading: false,
    ws: null,
    wsTimer: null,
    lastFrameAt: 0,
    timer: null,
    tickN: 0,
    busy: {},
    page: '',
    fmt: 'dec',

    levels: {},          // port -> Uint8Array(512): what it sends / receives
    live: {},
    input: {},
    grids: {},           // 'ov1' / 'ts1' -> LevelGrid
    config: null,
    scenes: [],
    lastFrames: {},
    lastFramesAt: 0,
    analysisHtml: {},
    rdmDevices: [],
    rdmIdent: new Set(),

    netCfg: null,
    netStatus: null,
    netWatch: null,
    netPollTimer: null,
    wifiProfiles: [],
    wifiEditIndex: -1,
    scanList: [],

    log: { pos: 0, timer: null, paused: false, partial: '', lines: 0, busy: false },
    st: { timer: null, resultId: 'stResult', btnId: 'stRun' },
    identifyLeft: 0,
    identifyTimer: null,

    ts: {
        data: null,          // last GET /api/tester
        tab: 1,              // port number, or 'cable'
        bank: {},            // port -> first channel of the fader bank
        roles: {},           // port -> role shown in the UI
        signal: {},          // port -> last signal state (for the loss alarm)
        local: {},           // port -> Map(channel -> {v, t}) set here, not echoed yet
        pending: {},         // port -> Map(channel -> value) waiting for a POST (no WebSocket)
        flushTimer: null,
        highlight: true,
        beep: false,
        audio: null,
    },

    // ------------------------------------------------------------------------
    // Start-up and sign-in
    // ------------------------------------------------------------------------

    async init() {
        this.loadPrefs();
        this.setupNetworkForms();
        window.addEventListener('hashchange', () => this.go(location.hash.slice(1), false));
        const auth = await this.fetchAuthStatus();
        this.drawLogin();
        if (auth && auth.setup_required) { this.showSetup(auth); return; }
        if (auth && auth.locked) {
            this.setConn(false, 'Locked');
            this.toast('Too many failed sign-ins. Wait a minute, then reload.', 'error');
            return;
        }
        if (auth && !auth.authenticated) { this.handleUnauthorized(); return; }
        this.start();
    },

    async start() {
        if (this.started) return;
        this.started = true;
        let info = null;
        for (let i = 0; i < 3 && !info; i++) {
            try {
                info = await this.api('/api/system/info', { quiet: i < 2 });
            } catch (e) {
                if (e.silent) return;
                await sleep(1500);
            }
        }
        this.applyInfo(info || {});
        this.build();
        this.go(location.hash.slice(1), false);
        this.loadConfig();
        this.refreshNet();
        if (this.tester) {
            this.loadTester();
        } else {
            this.loadPortsStatus();
            if (this.has('scenes')) this.loadSceneOptions();
        }
        this.timer = setInterval(() => this.tick(), 1000);
        this.connectWs();
    },

    loadPrefs() {
        try {
            const f = localStorage.getItem('dmxFormat');
            if (f === 'dec' || f === 'pct') this.fmt = f;
            const t = localStorage.getItem('theme');
            if (t === 'light' || t === 'dark') document.documentElement.dataset.theme = t;
            this.ts.highlight = localStorage.getItem('tsHighlight') !== '0';
            this.ts.beep = localStorage.getItem('tsBeep') === '1';
        } catch (_) { /* storage blocked */ }
        this.markFormat();
    },

    savePref(k, v) {
        try { localStorage.setItem(k, v); } catch (_) { /* storage blocked */ }
    },

    toggleTheme() {
        const root = document.documentElement;
        const dark = root.dataset.theme ? root.dataset.theme === 'dark'
            : !window.matchMedia('(prefers-color-scheme: light)').matches;
        root.dataset.theme = dark ? 'light' : 'dark';
        this.savePref('theme', root.dataset.theme);
    },

    /** GET /api/auth/status: never triggers a sign-in prompt. */
    async fetchAuthStatus() {
        try {
            const r = await fetch('/api/auth/status', { cache: 'no-store' });
            if (!r.ok) return null;
            const data = await r.json();
            this.wsToken = data.ws_token || null;
            if (typeof data.login_required === 'boolean') this.loginRequired = data.login_required;
            return data;
        } catch (_) {
            return null;
        }
    },

    /** Credentials missing or stale: reloading makes the browser ask again. */
    handleUnauthorized() {
        if (this.authReloading) return;
        this.authReloading = true;
        this.toast('Signed out. Reloading to sign in again…', 'warning');
        setTimeout(() => window.location.reload(), 1500);
    },

    stopActivity() {
        clearInterval(this.timer);
        this.timer = null;
        clearTimeout(this.wsTimer);
        if (this.ws) { this.ws.onclose = null; this.ws.close(); this.ws = null; }
        this.stopLog();
    },

    showSetup(auth) {
        this.stopActivity();
        if (auth && auth.ap_ssid) setText('setupApSsid', auth.ap_ssid);
        $('setupOverlay').hidden = false;
        this.showSetupChoices();
        const first = document.querySelector('#setupForm input[name="password"]');
        if (first) first.focus();
    },

    /** Setup form: hide the password fields the "no password" boxes replace. */
    showSetupChoices() {
        const f = $('setupForm');
        const noPw = f.elements.no_password.checked;
        const open = f.elements.ap_open.checked;
        $('setupPwFields').hidden = noPw;
        f.elements.password.disabled = noPw;
        f.elements.password_confirm.disabled = noPw;
        $('setupApField').hidden = open;
        f.elements.ap_password.disabled = open;
    },

    async submitSetup(event) {
        event.preventDefault();
        const f = new FormData(event.target);
        const noPw = f.get('no_password') === 'on';
        const open = f.get('ap_open') === 'on';
        const pw = f.get('password') || '';
        const ap = open ? '' : (f.get('ap_password') || '');
        const err = $('setupError');
        let problem = '';
        if (!noPw && (pw.length < 8 || pw.length > 63)) problem = 'The admin password must be 8–63 characters.';
        else if (!noPw && pw !== f.get('password_confirm')) problem = 'The admin passwords do not match.';
        else if (!open) problem = this.apPasswordProblem(ap);
        err.textContent = problem;
        if (problem) return false;

        const btn = $('setupSubmit');
        btn.disabled = true;
        try {
            const r = await fetch('/api/setup', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
                body: JSON.stringify(noPw ? { no_password: true, ap_password: ap } : { password: pw, ap_password: ap })
            });
            const body = await r.json().catch(() => ({}));
            if (!r.ok) throw new Error(body.message || `HTTP ${r.status}`);
            err.textContent = '';
            event.target.reset();
            event.target.hidden = true;
            setText('setupText', 'Saved. The node is restarting and this page reloads in 10 seconds.' +
                (noPw ? '' : ' Sign in as "admin" with your new password.'));
            setTimeout(() => window.location.reload(), 10000);
        } catch (e) {
            err.textContent = 'Setup failed: ' + e.message;
            btn.disabled = false;
        }
        return false;
    },

    /** Same rule as the firmware: WPA2 length, not the factory default.
     *  (An empty password, an open network, is chosen with its own box.) */
    apPasswordProblem(ap) {
        if (ap.length < 8 || ap.length > 63) return 'The access point password must be 8–63 characters.';
        if (ap === '12345678') return 'Choose an access point password other than the factory default.';
        return '';
    },

    /** System page password card: change it, or turn sign-in on or off. */
    drawLogin() {
        const f = $('passwordForm');
        if (!f) return;
        const on = this.loginRequired !== false;
        $('loginOffNote').hidden = on;
        $('curPwField').hidden = !on;
        f.elements.current_password.disabled = !on;
        $('loginOffBtn').hidden = !on;
        setText('pwSubmit', on ? 'Change password' : 'Turn on sign-in');
        setText('pwAgain', on ? 'again' : 'with it');
    },

    async changePassword(event) {
        event.preventDefault();
        const form = event.target;
        const f = new FormData(form);
        const on = this.loginRequired !== false;
        const pw = f.get('new_password') || '';
        if (pw.length < 8 || pw.length > 63) { this.toast('The new password must be 8–63 characters', 'error'); return false; }
        if (pw !== f.get('confirm_password')) { this.toast('The new passwords do not match', 'error'); return false; }
        const body = { new_password: pw };
        if (on) body.current_password = f.get('current_password') || '';
        try {
            await this.post('/api/auth/password', body);
            form.reset();
            this.toast(on ? 'Password changed. Sign in again with the new one.'
                          : 'Sign-in is on. Sign in as "admin" with the new password.', 'success');
            this.authReloading = true;
            setTimeout(() => window.location.reload(), 2000);
        } catch (_) { /* toast already shown */ }
        return false;
    },

    async turnLoginOff() {
        const f = $('passwordForm');
        const cur = f.elements.current_password.value;
        if (!cur) { this.toast('Enter the current password first', 'error'); f.elements.current_password.focus(); return; }
        if (!confirm('Turn off sign-in?\n\nAnyone who can reach this node on the network will be able to change its settings.')) return;
        try {
            await this.post('/api/auth/password', { current_password: cur, no_password: true });
            f.reset();
            this.loginRequired = false;
            this.drawLogin();
            this.toast('Sign-in is off', 'success');
        } catch (_) { /* toast already shown */ }
    },

    // ------------------------------------------------------------------------
    // API
    // ------------------------------------------------------------------------

    /** fetch + JSON. options.quiet: no error toast (background polling). */
    async api(url, options = {}) {
        const { quiet, ...opts } = options;
        try {
            const response = await fetch(url, {
                ...opts,
                headers: { 'Content-Type': 'application/json', 'X-Requested-With': 'XMLHttpRequest', ...opts.headers }
            });
            if (response.status === 401) {
                this.handleUnauthorized();
                const e = new Error('Sign-in required');
                e.silent = true;
                throw e;
            }
            if (response.status === 403) {
                const body = await response.clone().json().catch(() => ({}));
                if (body.error === 'setup_required') {
                    this.showSetup(null);
                    const e = new Error('Setup required');
                    e.silent = true;
                    throw e;
                }
            }
            if (!response.ok) {
                let msg = `HTTP ${response.status}`;
                try {
                    const body = await response.json();
                    if (body && body.message) msg = body.message;
                } catch (_) { /* not JSON */ }
                throw new Error(msg);
            }
            return await response.json();
        } catch (error) {
            if (!error.silent && !quiet) this.toast(`Request failed: ${error.message}`, 'error');
            throw error;
        }
    },

    post(url, body, options = {}) {
        return this.api(url, { method: 'POST', body: body === undefined ? undefined : JSON.stringify(body), ...options });
    },

    /** Run @p fn unless the previous call with the same @p key is still busy */
    async once(key, fn) {
        if (this.busy[key]) return;
        this.busy[key] = true;
        try { await fn(); } catch (_) { /* polling: try again next time */ } finally { this.busy[key] = false; }
    },

    setConn(ok, text) {
        const c = $('conn');
        c.classList.toggle('ok', !!ok);
        c.classList.toggle('bad', !ok);
        setText('connText', text || (ok ? 'Connected' : 'Offline'));
    },

    // ------------------------------------------------------------------------
    // Layout: pages, features, ports
    // ------------------------------------------------------------------------

    has(f) { return !!this.features[f]; },

    applyInfo(info) {
        this.info = info;
        const f = info.features || {};
        const dflt = (k, d) => (k in f ? !!f[k] : d);
        this.features = {
            ethernet: dflt('ethernet', true), sacn: dflt('sacn', true), rdm: dflt('rdm', true),
            scenes: dflt('scenes', true), dmx_tools: dflt('dmx_tools', true), web_log: dflt('web_log', true),
            mdns: dflt('mdns', true), tester: dflt('tester', false),
        };
        this.nports = Math.min(4, Math.max(1, parseInt(info.ports) || 2));
        this.tester = !!(info.tester && info.tester.enabled);
        this.renderSystemInfo(info);
    },

    pages() {
        return PAGES.filter(p => (this.tester ? p.tester : p.node) && (!p.feature || this.has(p.feature)));
    },

    build() {
        const nav = $('nav');
        nav.innerHTML = this.pages().map(p =>
            (p.sep ? '<div class="nav-sep"></div>' : '') +
            `<button type="button" class="nav-btn" data-page="${p.id}" onclick="app.go('${p.id}')">${icon(p.id)}<span>${p.label}</span></button>`
        ).join('') + `<div class="nav-foot" id="navFoot"></div>`;

        document.body.classList.toggle('tester-mode', this.tester);
        $('testerBanner').hidden = !this.tester;
        const chip = $('modeChip');
        chip.hidden = !this.tester;
        chip.className = 'chip accent';
        setText('brandSub', this.tester ? 'DMX tester' : 'Art-Net / sACN node');
        if (!this.has('ethernet')) {
            setText('modeAutoHelp', 'Your saved Wi-Fi networks, otherwise the node\'s own access point.');
            setText('modeWifiHelp', 'Joins your saved Wi-Fi networks. The access point only comes on as a rescue.');
        }

        // Port lists
        document.querySelectorAll('select[data-port-select]').forEach(sel => {
            sel.innerHTML = this.portList().map(p => `<option value="${p}">Port ${p}</option>`).join('');
            if (sel.dataset.second !== undefined && this.nports > 1) sel.value = '2';
        });

        if (this.tester) {
            this.buildTester();
        } else {
            this.buildOverview();
            this.buildPortForms();
        }
        this.applyFeatures(document);
    },

    portList() { return Array.from({ length: this.nports }, (_, i) => i + 1); },

    applyFeatures(root) {
        root.querySelectorAll('[data-feature]').forEach(el => {
            const on = this.has(el.dataset.feature);
            if (el.tagName === 'OPTION') { if (!on) el.remove(); } else if (!on) el.hidden = true;
        });
        root.querySelectorAll('[data-node-only]').forEach(el => { if (this.tester) el.hidden = true; });
    },

    go(page, push = true) {
        const pages = this.pages();
        if (!pages.length) return;
        if (!pages.some(p => p.id === page)) { page = pages[0].id; push = true; }
        const prev = this.page;
        this.page = page;
        document.querySelectorAll('.page').forEach(s => s.classList.toggle('active', s.id === 'page-' + page));
        document.querySelectorAll('.nav-btn').forEach(b => b.classList.toggle('active', b.dataset.page === page));
        if (push && location.hash.slice(1) !== page) history.replaceState(null, '', '#' + page);
        if (prev !== page) window.scrollTo(0, 0);
        this.onPageShow(page);
    },

    onPageShow(page) {
        if (page === 'system') { this.loadCrash(); this.loadHealth(); if (this.has('web_log')) this.startLog(); } else { this.stopLog(); }
        if (page === 'scenes') this.loadScenes();
        if (page === 'rdm') this.loadRdmDevices();
        if (page === 'tools') { this.loadTests(); this.loadSelfTest(); }
        if (page === 'ports' && this.has('scenes')) this.loadSceneOptions();
        if (page === 'tester') {
            this.loadTester();
            this.loadTests();
            this.loadChannels();
            if (this.has('rdm')) this.loadRdmDevices();
        }
        for (const k in this.grids) this.grids[k].layout();
        this.renderAllLevels();
    },

    tick() {
        const n = ++this.tickN;
        if (this.tester) {
            if (this.page === 'tester' || n % 3 === 0) this.loadTester();
            if (this.page === 'tester') this.loadChannels();
        }
        if (n % 2 === 0) this.pollStatus();
        if (n % 2 === 1 && Date.now() - this.lastFrameAt > 3000) this.pollDmx();
        if (n % 2 === 1 && !this.tester && this.page === 'overview') this.loadSources();
        if (n % 5 === 1 && !this.tester && this.page === 'overview') this.loadNodes();
    },

    // ------------------------------------------------------------------------
    // System info, status polling
    // ------------------------------------------------------------------------

    renderSystemInfo(d) {
        const notice = d.settings_notice || '';
        setText('settingsNotice', notice);
        $('settingsBanner').hidden = !notice;
        if (!d) return;
        setText('sysFw', d.firmware_version || '--');
        setText('sysBuilt', d.build_date ? `${d.build_date} ${d.build_time || ''}`.trim() : '--');
        setText('sysBoard', d.board || d.hardware || '--');
        setText('sysChip', d.chip || '--');
        setText('sysPorts', d.ports || '--');
        setText('sysIdf', d.idf_version || '--');
        setText('sysPart', d.partition || '--');
        setText('sysHeap', d.free_heap ? `${this.formatBytes(d.free_heap)} (lowest ${this.formatBytes(d.min_free_heap)})` : '--');
        setText('sysUptime', this.formatUptime(d.uptime_sec));
        const t = d.tester || {};
        let mode = t.enabled ? 'DMX tester' : 'Art-Net / sACN node';
        if (t.available && t.next_boot !== undefined && !!t.next_boot !== !!t.enabled) {
            mode += t.next_boot ? ' (tester after the next restart)' : ' (node after the next restart)';
        }
        setText('sysMode', mode);
        setText('ovFw', d.firmware_version || '--');
        setText('ovHw', d.hardware || '--');
        setText('ovUptime', this.formatUptime(d.uptime_sec));
        setText('ovHeap', d.free_heap ? `${this.formatBytes(d.free_heap)} free` : '--');
        const foot = $('navFoot');
        if (foot) foot.textContent = [d.board || d.hardware, d.firmware_version].filter(Boolean).join(' · ');
        const fe = $('sysFeatures');
        if (fe && d.features) {
            const names = { ethernet: 'Ethernet', sacn: 'sACN', rdm: 'RDM', scenes: 'Scenes', dmx_tools: 'DMX tools', tester: 'Tester', web_log: 'Web log', mdns: '.local name' };
            const html = Object.keys(names).map(k =>
                `<span class="chip ${d.features[k] ? 'ok' : ''}" title="${d.features[k] ? 'built in' : 'not built in'}">${d.features[k] ? '' : '<s>'}${names[k]}${d.features[k] ? '' : '</s>'}</span>`).join('');
            this._setHtml(fe, html);
        }
    },

    async dismissSettingsNotice() {
        $('settingsBanner').hidden = true;
        try { await this.post('/api/system/notice/clear'); } catch (_) { /* shows again next poll */ }
    },

    async pollStatus() {
        this.once('status', async () => {
            try {
                const info = await this.api('/api/system/info', { quiet: true });
                this.renderSystemInfo(info);
                this.setConn(true);
            } catch (e) {
                if (!e.silent) this.setConn(false);
                return;
            }
            if (!this.netWatch || this.netWatch.done) {
                try { this.renderNetwork(await this.api('/api/network/status', { quiet: true })); } catch (_) { /* next time */ }
            }
            if (this.tester) return;
            try {
                const s = await this.api('/api/system/stats', { quiet: true });
                if (s.artnet) {
                    setText('ovArtnet', (s.artnet.packets || 0).toLocaleString());
                    setText('ovArtnetDmx', `${(s.artnet.dmx_packets || 0).toLocaleString()} ArtDmx`);
                }
                if (s.sacn) {
                    setText('ovSacn', (s.sacn.packets || 0).toLocaleString());
                    setText('ovSacnData', `${(s.sacn.data_packets || 0).toLocaleString()} data · ${plural(s.sacn.subscriptions || 0, 'universe')}`);
                }
            } catch (_) { /* next time */ }
            await this.loadPortsStatus(true);
        });
    },

    async loadConfig() {
        try {
            const data = await this.api('/api/config');
            this.applyConfig(data);
        } catch (_) { /* toast shown */ }
    },

    applyConfig(data) {
        this.config = data;
        if (data.node_info) {
            const n = data.node_info;
            setText('brandName', n.short_name || 'ESP-NODE');
            document.title = (n.short_name || 'ESP-NODE') + (this.tester ? ' · DMX tester' : '');
            const f = $('identityForm');
            if (f) {
                f.elements.short_name.value = n.short_name || '';
                f.elements.long_name.value = n.long_name || '';
                f.elements.allow_ip_prog.checked = n.allow_ip_prog !== false;
            }
        }
        const mf = $('mergeForm');
        if (mf) {
            if (data.merge) {
                mf.elements.timeout_seconds.value = data.merge.timeout_seconds ?? 3;
                mf.elements.loss_behavior.value = String(data.merge.loss_behavior ?? 0);
            }
            if (data.node_info) mf.elements.sacn_priority.value = data.node_info.sacn_priority ?? 100;
        }
        if (data.network) this.applyNetworkConfig(data.network);
        if (!this.tester) {
            for (const n of this.portList()) {
                const pc = data[`port${n}`];
                if (pc) this.fillPortForm(n, pc);
            }
            this.fillSceneSelects();
        }
    },

    // ------------------------------------------------------------------------
    // WebSocket and levels
    // ------------------------------------------------------------------------

    async connectWs() {
        try {
            // The WebSocket needs a token from a signed-in request (tokens
            // change when the password changes or the node restarts).
            const auth = await this.fetchAuthStatus();
            if (!auth || !auth.ws_token) {
                if (auth && auth.setup_required) { this.showSetup(auth); return; }
                this.wsTimer = setTimeout(() => this.connectWs(), 5000);
                return;
            }
            const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            this.ws = new WebSocket(`${proto}//${window.location.host}/ws?token=${encodeURIComponent(auth.ws_token)}`);
            this.ws.binaryType = 'arraybuffer';
            this.ws.onopen = () => clearTimeout(this.wsTimer);
            this.ws.onmessage = (ev) => {
                if (ev.data instanceof ArrayBuffer) this.onDmxFrame(ev.data);
            };
            this.ws.onclose = () => {
                this.ws = null;
                this.wsTimer = setTimeout(() => this.connectWs(), 5000);
            };
        } catch (e) {
            this.wsTimer = setTimeout(() => this.connectWs(), 5000);
        }
    },

    wsSend(obj) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(obj));
            return true;
        }
        return false;
    },

    /** Binary level frame: [0]=0x02 [1]=bit n = port n+1 live [2]=bit n = port n+1 is an
     *  input [3]=port count, then 512 bytes per port (sent / received levels) */
    onDmxFrame(buf) {
        const b = new Uint8Array(buf);
        if (b.length < 4 || b[0] !== 0x02) return;
        const ports = Math.min(b[3], this.nports);
        for (let i = 0; i < ports; i++) {
            const off = 4 + i * 512;
            if (b.length < off + 512) break;
            this.setLevels(i + 1, b.slice(off, off + 512), !!(b[1] & (1 << i)), !!(b[2] & (1 << i)));
        }
        this.lastFrameAt = Date.now();
    },

    /** Used while no WebSocket frame arrives (blocked or reconnecting) */
    async pollDmx() {
        this.once('dmx', async () => {
            const res = await Promise.allSettled(this.portList().map(p =>
                this.api(`/api/ports/${p}/dmx?count=512`, { quiet: true })));
            res.forEach((r, i) => {
                if (r.status !== 'fulfilled' || !r.value) return;
                const src = Array.isArray(r.value.channels) ? r.value.channels : [];
                const lv = new Uint8Array(512);
                for (let k = 0; k < 512 && k < src.length; k++) lv[k] = src[k] || 0;
                this.setLevels(i + 1, lv, !!r.value.active, !!r.value.input);
            });
        });
    },

    setLevels(port, lv, live, input) {
        // Levels set here that the node has not echoed yet win for a moment
        const local = this.ts.local[port];
        if (local && local.size) {
            const now = Date.now();
            for (const [ch, o] of local) {
                if (now - o.t > 900) local.delete(ch);
                else lv[ch - 1] = o.v;
            }
        }
        this.levels[port] = lv;
        this.live[port] = live;
        this.input[port] = input;
        this.renderLevels(port);
    },

    renderAllLevels() {
        for (const p of this.portList()) if (this.levels[p]) this.renderLevels(p);
    },

    renderLevels(port) {
        const lv = this.levels[port];
        if (!lv) return;
        const live = this.live[port], input = this.input[port];
        let on = 0;
        for (let i = 0; i < 512; i++) if (lv[i]) on++;
        const n = plural(on, 'channel');
        if (this.page === 'overview') {
            const g = this.grids['ov' + port];
            if (g) g.set(lv, live, input);
            let text, cls = '';
            if (input) {
                text = live ? (on ? `Receiving · ${n} on` : 'Receiving · all at zero')
                    : (on ? 'No DMX signal · last levels received' : 'Input · no DMX signal');
                if (live) cls = 'in';
            } else if (live) {
                text = on ? `Live · ${n} on` : 'Live · all at zero';
                cls = 'out';
            } else {
                text = on ? `No source · holding ${n}` : 'No data';
            }
            const badge = $('ovlBadge' + port);
            if (badge) { badge.className = 'chip ' + cls; setText(badge, text); }
        }
        if (this.page === 'tester' && this.ts.tab === port) {
            const g = this.grids['ts' + port];
            if (g) g.set(lv, live || !input, input);
            const role = this.ts.roles[port];
            const badge = $('tsBadge' + port);
            if (badge) {
                let text, cls = '';
                if (role === 'receive') {
                    text = live ? (on ? `Receiving · ${n} on` : 'Receiving · all at zero') : 'No signal · last levels';
                    cls = live ? 'in' : 'bad';
                } else if (role === 'off') {
                    text = 'Off';
                } else {
                    text = on ? `Sending · ${n} on` : 'Sending · all at zero';
                    cls = 'out';
                }
                badge.className = 'chip ' + cls;
                setText(badge, text);
            }
            if (role === 'send' || role === 'rdm') this.renderFaders(port);
        }
    },

    fmtLevel(v) {
        if (!v) return '';
        if (this.fmt === 'pct') return v === 255 ? 'FL' : String(Math.max(1, Math.round(v / 2.55)));
        return String(v);
    },

    setFormat(fmt) {
        this.fmt = fmt === 'pct' ? 'pct' : 'dec';
        this.savePref('dmxFormat', this.fmt);
        this.markFormat();
        for (const k in this.grids) this.grids[k].redraw();
        for (const p of this.portList()) if (this.ts.roles[p]) this.renderFaders(p, true);
    },

    markFormat() {
        document.querySelectorAll('[data-fmt]').forEach(b => {
            const on = b.dataset.fmt === this.fmt;
            b.classList.toggle('active', on);
            b.setAttribute('aria-pressed', on ? 'true' : 'false');
        });
    },

    // ------------------------------------------------------------------------
    // Overview (node mode)
    // ------------------------------------------------------------------------

    buildOverview() {
        const rdm = this.has('rdm');
        $('ovPorts').innerHTML = this.portList().map(n => `
            <div class="card" id="ovp${n}">
                <div class="card-head">
                    <h2>Port ${n}</h2><span class="chip" id="ovpTag${n}" hidden></span>
                    <div class="actions"><span class="chip" id="ovpMode${n}">--</span></div>
                </div>
                <dl class="kv">
                    <dt>Universe</dt><dd id="ovpUni${n}">--</dd>
                    <dt>Refresh</dt><dd id="ovpRate${n}">--</dd>
                    <dt id="ovpFramesL${n}">Frames sent</dt><dd id="ovpFrames${n}">--</dd>
                    <dt>Sync</dt><dd id="ovpSync${n}">--</dd>
                    ${rdm ? `<dt>RDM</dt><dd id="ovpRdm${n}">--</dd>` : ''}
                </dl>
                <h3>Sources</h3>
                <div id="ovpSrc${n}" class="help">--</div>
                <div id="ovpAn${n}" class="subcard" hidden></div>
                <div class="card-foot">
                    <button type="button" class="btn sm" onclick="app.blackoutPort(${n})">Blackout</button>
                    <button type="button" class="btn sm ghost" onclick="app.go('ports')">Settings</button>
                </div>
            </div>`).join('');
        $('ovLevels').innerHTML = this.portList().map(n => `
            <div class="lv-head"><h3>Port ${n}</h3><span class="chip" id="ovlBadge${n}">No data</span></div>
            <div id="ovlGrid${n}"></div>`).join('');
        for (const n of this.portList()) this.grids['ov' + n] = new LevelGrid($('ovlGrid' + n));
    },

    async loadPortsStatus(quietly) {
        try {
            const data = await this.api('/api/ports/status', { quiet: !!quietly });
            if (!Array.isArray(data)) return;
            const now = Date.now();
            const dt = (now - this.lastFramesAt) / 1000;
            data.forEach((p, i) => {
                if (!p) return;
                const n = p.port || i + 1;
                const frames = this.portFrames(p);
                const delta = Math.max(0, frames - (this.lastFrames[n] || 0));
                const rate = p.analysis && this.isInput(p.mode) ? `${p.analysis.refresh_hz} Hz`
                    : (this.lastFramesAt && dt > 0 ? `${(delta / dt).toFixed(1)} Hz` : '--');
                this.lastFrames[n] = frames;
                this.renderPortStatus(n, p, rate);
            });
            this.lastFramesAt = now;
        } catch (_) { /* toast shown unless quiet */ }
    },

    isInput(mode) { return mode === 2 || mode === 4; },
    portFrames(p) { return this.isInput(p.mode) ? (p.frames_received || 0) : (p.frames_sent || 0); },

    renderPortStatus(n, p, rate) {
        const input = this.isInput(p.mode);
        let mode = MODE_NAMES[p.mode] || 'Unknown';
        if (p.scene) mode += ` · scene ${p.scene}`;
        const mc = $('ovpMode' + n);
        if (mc) { mc.className = 'chip ' + (p.mode === 0 ? '' : input ? 'in' : 'out'); setText(mc, mode); }

        const an = p.artnet_universe ?? p.universe;
        const parts = [];
        if (p.protocol !== 'sacn') parts.push(`Art-Net ${an} (${netSubUni(an)})`);
        if (this.has('sacn') && p.protocol !== 'artnet') parts.push(`sACN ${p.sacn_universe ?? '--'}`);
        setText('ovpUni' + n, p.mode === 0 ? '--' : (parts.join(' · ') || '--'));
        if (rate !== undefined) setText('ovpRate' + n, p.mode === 0 ? '--' : rate);
        setText('ovpFramesL' + n, input ? 'Frames received' : 'Frames sent');
        setText('ovpFrames' + n, this.portFrames(p).toLocaleString());
        const syncNames = { artnet: 'ArtSync', sacn: 'sACN sync' };
        setText('ovpSync' + n, input || p.mode === 0 ? '--' : (syncNames[p.sync] || 'Free-running'));
        let rdm = '--';
        if (p.rdm_available) {
            rdm = p.rdm_discovering ? 'Discovering…'
                : p.rdm_tod_valid ? plural(p.rdm_devices, 'device') : 'Waiting for discovery';
        } else if (!input && p.mode !== 0) {
            rdm = 'Off';
        }
        setText('ovpRdm' + n, rdm);

        const tag = $('ovpTag' + n);
        if (tag) {
            let text = '', cls = 'warn';
            if (p.test) text = `TEST · ${p.test.mode}`;
            else if (p.failsafe) { text = 'FAILSAFE'; cls = 'bad'; }
            tag.textContent = text;
            tag.className = 'chip ' + cls;
            tag.hidden = !text;
        }

        const src = $('ovpSrc' + n);
        if (src) {
            let html;
            if (input) {
                html = esc(p.signal ? `DMX in, ${p.slots} slots` : 'No DMX signal');
            } else if (!Array.isArray(p.sources) || !p.sources.length) {
                html = p.mode === 0 ? '--' : 'None';
            } else {
                html = p.sources.map(s => {
                    const who = !s.ip ? esc(s.name) : s.name ? `${esc(s.name)} <small>${esc(s.ip)}</small>` : esc(s.ip);
                    const tags = [s.protocol === 'sACN' ? `sACN p${s.priority}` : s.protocol === 'DMX' ? 'local DMX' : s.protocol];
                    if (s.backup) tags.push('backup');
                    if (s.per_address_priority) tags.push('per-address priority');
                    return `<div${s.contributing ? '' : ' style="opacity:.5"'}>${who} <span class="chip">${esc(tags.join(' · '))}</span></div>`;
                }).join('');
            }
            this._setHtml(src, html);
        }
        this.renderAnalysis(n, p);
    },

    renderAnalysis(n, p) {
        const box = $('ovpAn' + n);
        if (!box) return;
        const a = p.analysis;
        if (!a || !this.isInput(p.mode)) { box.hidden = true; return; }
        const errs = [];
        if (a.err_framing) errs.push(`framing ${a.err_framing}`);
        if (a.err_overflow) errs.push(`overflow ${a.err_overflow}`);
        if (a.err_short) errs.push(`short ${a.err_short}`);
        const other = [];
        if (a.rdm_packets) other.push(`RDM ${a.rdm_packets}`);
        if (a.alt_start_codes) other.push(`start code ${hex2(a.last_alt_start_code)} ×${a.alt_start_codes}`);
        const html = `
            <dl class="kv">
                <dt>Refresh</dt><dd>${a.refresh_hz} Hz${a.interval_max_ms ? ` <small>(gap ${a.interval_min_ms}–${a.interval_max_ms} ms)</small>` : ''}</dd>
                <dt>Slots</dt><dd>${a.slots_max ? (a.slots_min === a.slots_max ? a.slots : `${a.slots} <small>(${a.slots_min}–${a.slots_max})</small>`) : '--'}</dd>
                <dt>Errors</dt><dd>${errs.length ? `<span style="color:var(--warn)">${errs.join(' · ')}</span>` : 'none'}</dd>
                ${other.length ? `<dt>Other packets</dt><dd>${other.join(' · ')}</dd>` : ''}
                <dt>Break / MAB</dt><dd>${this.timingText(a)}</dd>
            </dl>
            <div class="btn-row section">
                ${a.timing_on ? `<button type="button" class="btn xs" onclick="app.measureTiming(${n}, 0)">Stop measuring</button>`
                              : `<button type="button" class="btn xs" onclick="app.measureTiming(${n}, 60)">Measure break / MAB</button>`}
                <button type="button" class="btn xs ghost" onclick="app.resetAnalysis(${n})">Reset counters</button>
            </div>`;
        if (html !== this.analysisHtml[n]) { box.innerHTML = html; this.analysisHtml[n] = html; }
        box.hidden = false;
    },

    timingText(a) {
        if (a.timing_on) {
            return (a.break_us ? `${a.break_us} / ${a.mab_us} µs` : 'waiting…') + ` <small>(${a.timing_left_s} s left)</small>`;
        }
        return a.break_us ? `${a.break_us} / ${a.mab_us} µs <small>(last measured)</small>` : '--';
    },

    async measureTiming(port, seconds) {
        try {
            await this.post('/api/tools/analyzer', { port, action: 'timing', seconds });
            if (seconds) this.toast(`Measuring break / MAB on port ${port} for ${seconds} s`, 'info');
            this.refreshPorts();
        } catch (_) { /* toast shown */ }
    },

    async resetAnalysis(port) {
        try {
            await this.post('/api/tools/analyzer', { port, action: 'reset' });
            this.refreshPorts();
        } catch (_) { /* toast shown */ }
    },

    refreshPorts() { if (this.tester) this.loadTester(); else this.loadPortsStatus(true); },

    // ------------------------------------------------------------------------
    // Signal check: every Art-Net / sACN sender heard, and what looks wrong
    // ------------------------------------------------------------------------

    async loadSources() {
        this.once('sources', async () => {
            try {
                const d = await this.api('/api/network/sources', { quiet: true });
                this.renderSources(d);
            } catch (_) { /* next time */ }
        });
    },

    // Other Art-Net nodes (their ArtPollReply)
    async loadNodes() {
        this.once('nodes', async () => {
            try {
                const d = await this.api('/api/network/nodes', { quiet: true });
                this.nodes = d;
                this.renderNodes(d);
                /* the node just polled: the answers arrive within a second */
                if (d.polled) setTimeout(() => { if (this.page === 'overview') this.loadNodes(); }, 1500);
            } catch (_) { /* next time */ }
        });
    },

    /** One entry per device: nodes send one reply per group of ports */
    groupNodes(list) {
        const map = new Map();
        for (const r of list || []) {
            const key = `${r.ip}|${r.mac}`;
            let g = map.get(key);
            if (!g) {
                g = { ...r, ports: [] };
                map.set(key, g);
            }
            g.ports.push(...(r.ports || []));
            g.age_ms = Math.min(g.age_ms, r.age_ms);
            g.conflict = g.conflict || r.conflict;
            if (!g.short_name && r.short_name) g.short_name = r.short_name;
        }
        return [...map.values()];
    },

    nodeName(n) { return n.short_name || n.long_name || n.ip; },

    renderNodes(d) {
        if (!d) return;
        const nodes = this.groupNodes(d.nodes);
        const chip = $('nodesState');
        if (chip) {
            const clash = nodes.some(n => n.conflict);
            chip.className = 'chip ' + (clash ? 'bad' : nodes.length ? 'ok' : '');
            setText(chip, !d.listening ? 'Off' : clash ? 'Address clash' : nodes.length ? plural(nodes.length, 'node') : 'None found');
        }
        const sig = this.sigData || {};
        const mine = (sig.ports || []);
        const styles = ['', 'controller', 'media server', 'router', 'backup', 'config tool', 'visualiser'];
        const protos = ['', 'MIDI', 'Avab', 'CMX', 'ADB', 'Art-Net', 'DALI'];
        const rows = nodes.map(n => {
            const ports = n.ports.map(p => {
                const bits = [];
                const proto = p.protocol ? ` ${esc(protos[p.protocol] || `type ${p.protocol}`)}` : '';
                if (p.output) {
                    const same = mine.filter(q => q.output && !q.local_only && q.protocol_mode !== 1 && q.artnet === p.out);
                    bits.push(`Out ${p.out} <small>${netSubUni(p.out)}</small>${proto}${p.out_data ? ' <span class="chip ok">data</span>' : ''}` +
                        same.map(q => ` <span class="chip">same as port ${q.port}</span>`).join(''));
                }
                if (p.input) {
                    const same = mine.filter(q => q.input && q.protocol_mode !== 1 && q.artnet === p.in);
                    bits.push(`In ${p.in} <small>${netSubUni(p.in)}</small>${proto}${p.in_data ? ' <span class="chip ok">data</span>' : ''}` +
                        same.map(q => ` <span class="chip warn">port ${q.port} sends this too</span>`).join(''));
                }
                return bits.join(' · ');
            }).filter(Boolean);
            const tags = [];
            if (styles[n.style]) tags.push(styles[n.style]);
            if (n.dhcp) tags.push('DHCP');
            const name = `${esc(this.nodeName(n))}${n.long_name && n.long_name !== n.short_name ? ` <small>${esc(n.long_name)}</small>` : ''}`;
            return `<tr>
                <td>${name}${tags.length ? ` <span class="chip">${esc(tags.join(' · '))}</span>` : ''}</td>
                <td>${esc(n.ip)}${n.from && n.from !== n.ip ? ` <small>from ${esc(n.from)}</small>` : ''}${n.conflict ? ' <span class="chip bad">same IP as this node</span>' : ''}<br><small class="mono">${esc(n.mac)}</small></td>
                <td>${ports.length ? ports.map(p => `<div>${p}</div>`).join('') : '<span class="help" style="display:inline">no ports</span>'}</td>
                <td>${n.age_ms < 6000 ? 'now' : `${Math.round(n.age_ms / 1000)} s ago`}</td>
            </tr>`;
        });
        this._setHtml($('nodesTable'), rows.join('') ||
            `<tr><td colspan="4" class="empty">${d.listening ? 'No other Art-Net nodes answered' : 'Not listening (tester mode)'}</td></tr>`);
    },

    /** Address clashes between this node and the others, for the signal check */
    nodeHints(d, add) {
        const nodes = this.groupNodes(this.nodes && this.nodes.nodes);
        for (const n of nodes.filter(n => n.conflict)) {
            add('bad', 'IP address clash',
                `${this.nodeName(n)} (MAC ${n.mac}) also uses ${n.ip}, this node's address. Devices that share an address lose packets and drop off the network. Give one of them another address.`);
        }
        const byIp = new Map();
        for (const n of nodes.filter(n => !n.conflict)) byIp.set(n.ip, (byIp.get(n.ip) || []).concat([n]));
        for (const [ip, list] of byIp) {
            if (list.length < 2) continue;
            add('bad', `Two devices share ${ip}`,
                `${list.map(n => `${this.nodeName(n)} (MAC ${n.mac})`).join(' and ')} both use ${ip}. Give one of them another address.`);
        }
        /* two senders on one Art-Net universe: receivers merge them or flicker */
        const senders = new Map();
        const note = (u, who) => senders.set(u, [...new Set((senders.get(u) || []).concat([who]))]);
        for (const p of (d.ports || []).filter(p => p.input && p.signal && p.protocol_mode !== 1)) note(p.artnet, `this node's port ${p.port}`);
        for (const n of nodes) {
            for (const p of n.ports.filter(p => p.input && p.in_data)) note(p.in, this.nodeName(n));
        }
        for (const [u, who] of senders) {
            if (who.length < 2) continue;
            add('warn', `Two inputs send ${this.uniText('artnet', u)}`,
                `${who.join(' and ')} both send it. Receivers merge them or flicker between them: give one of them another Port-Address.`);
        }
    },

    uniText(proto, u) { return proto === 'artnet' ? `Art-Net ${u} (${netSubUni(u)})` : `sACN ${u}`; },
    who(s) { return s.name ? `${s.name} (${s.ip})` : s.ip; },
    protoName(p) { return p === 'sacn' ? 'sACN' : 'Art-Net'; },

    /** What looks wrong, worst first: [{cls, title, text}] */
    sourceHints(d) {
        const hints = [];
        const add = (cls, title, text) => hints.push({ cls, title, text });
        if (!d.listening) {
            add('info', 'Tester mode', 'The node is a DMX tester now and does not listen to Art-Net or sACN.');
            return hints;
        }
        const ifs = d.interfaces || [];
        if (!ifs.length) add('bad', 'No network', 'The node has no network connection, so nothing can arrive.');
        this.nodeHints(d, add);
        const live = (d.sources || []).filter(s => s.age_ms < 2500);
        const adverts = d.advertised || [];
        const outs = (d.ports || []).filter(p => p.output);
        const sacnOn = this.has('sacn');
        const wants = p => {
            const w = [];
            if (p.protocol_mode !== 1) w.push(this.uniText('artnet', p.artnet));
            if (sacnOn && p.protocol_mode !== 0) w.push(this.uniText('sacn', p.sacn));
            return w.join(' or ');
        };

        const netOuts = outs.filter(p => !p.local_only);   /* splitters ignore the network */
        if (!live.length && !adverts.length && ifs.length && netOuts.length) {
            const addrs = ifs.map(i => `${i.ip} (${i.name === 'ap' ? 'own Wi-Fi' : i.name === 'wifi' ? 'Wi-Fi' : 'Ethernet'})`).join(', ');
            add('bad', 'Nothing is arriving',
                `No Art-Net${sacnOn ? ' or sACN' : ''} has reached the node in the last few seconds. Check that the console's output is on, ` +
                `that it sends to this network and that a firewall on the computer is not blocking it. The node is at ${addrs}.`);
        }

        for (const p of outs) {
            /* DMX from other ports' inputs (merged in, or instead of the network) */
            const inputs = p.local_inputs || [];
            let localOk = false;
            for (const m of inputs) {
                const src = (d.ports || []).find(q => q.port === m);
                if (src && !src.input) {
                    add('bad', `Port ${p.port}: port ${m} is not a DMX input`,
                        `Port ${p.port} takes port ${m}'s DMX, but port ${m} is set to ${src.output ? 'output' : 'off'}. Set it to DMX input, or change port ${p.port}'s "DMX from another port", on the Ports page.`);
                } else if (src && !src.signal) {
                    add('warn', `Port ${p.port}: no DMX on port ${m}`,
                        `Port ${p.port} takes port ${m}'s DMX input, but nothing arrives there. Check the cable from the desk and that the desk is outputting.`);
                } else {
                    localOk = true;
                }
            }
            if (inputs.length && p.local_only) {
                if (localOk) {
                    if (inputs.length === 1) {
                        add('info', `Port ${p.port} is a splitter`, `It outputs port ${inputs[0]}'s DMX input and ignores the network.`);
                    } else {
                        add('info', `Port ${p.port} merges DMX inputs`,
                            `It merges the DMX inputs of ports ${inputs.slice(0, -1).join(', ')} and ${inputs[inputs.length - 1]} and ignores the network.`);
                    }
                }
                continue;
            }
            const feeding = live.filter(s => (s.ports || []).includes(p.port) && !s.preview);
            if (!feeding.length && localOk) continue;   /* the local DMX is its source */
            if (feeding.length) {
                if (feeding.length > 1 && p.merge_mode === 4) {
                    add('warn', `Port ${p.port}: ${feeding.length} senders, merging off`,
                        `Only the first one is used: ${feeding.map(s => this.who(s)).join(', ')}. Pick a merge mode on the Ports page if they should share the port.`);
                } else if (feeding.length > 1 && (p.merge_mode === 1 || p.merge_mode === 2)) {
                    add('info', `Port ${p.port}: ${feeding.length} senders`,
                        `${feeding.map(s => this.who(s)).join(' and ')} both control it; the latest change wins.`);
                }
                const sacn = feeding.filter(s => s.protocol === 'sacn');
                const top = Math.max(...sacn.map(s => s.priority ?? 100));
                const hidden = sacn.filter(s => (s.priority ?? 100) < top && !s.pap);
                if (hidden.length) {
                    add('info', `Port ${p.port}: lower sACN priority ignored`,
                        `${hidden.map(s => `${this.who(s)} (priority ${s.priority})`).join(', ')} ${hidden.length > 1 ? 'are' : 'is'} not used while a source with priority ${top} sends.`);
                }
                continue;
            }
            if (!live.length && !adverts.length) continue;   /* said above */
            const filtered = live.filter(s => (s.filtered || []).includes(p.port));
            const preview = live.filter(s => s.preview && (s.ports || []).includes(p.port));
            const protoName = PROTOCOLS.find(([v]) => v === p.protocol_mode);
            if (filtered.length) {
                add('bad', `Port ${p.port} ignores what it is sent`,
                    `${filtered.map(s => `${this.who(s)} sends ${this.uniText(s.protocol, s.universe)}`).join('; ')}, but the port's protocol is "${protoName ? protoName[1] : '?'}". Change the protocol on the Ports page.`);
                continue;
            }
            if (preview.length) {
                add('warn', `Port ${p.port}: preview data only`,
                    `${preview.map(s => this.who(s)).join(', ')} sends sACN marked "preview" (for visualisers), which outputs ignore. Turn off preview / blind mode on the console.`);
                continue;
            }
            const announced = adverts.filter(a => p.protocol_mode !== 0 && a.universes.includes(p.sacn));
            if (announced.length) {
                add('bad', `Port ${p.port}: sACN ${p.sacn} announced but not arriving`,
                    `${announced.map(a => this.who(a)).join(', ')} says it sends sACN ${p.sacn}, but none of that data reaches the node. ` +
                    'Check that the console really outputs that universe. A network switch that filters multicast (IGMP snooping with no querier) also does this: switch snooping off, or have the console send unicast to the node.');
                continue;
            }
            const near = live.filter(s => !(s.ports || []).length && !(s.filtered || []).length && (
                (s.protocol === 'artnet' && p.protocol_mode !== 1 && Math.abs(s.universe - p.artnet) === 1) ||
                (s.protocol === 'sacn' && p.protocol_mode !== 0 && Math.abs(s.universe - p.sacn) === 1)));
            const advNear = adverts.filter(a => p.protocol_mode !== 0 && !a.universes.includes(p.sacn) && a.universes.some(u => Math.abs(u - p.sacn) <= 1));
            if (near.length || advNear.length) {
                const what = near.map(s => `${this.who(s)} sends ${this.uniText(s.protocol, s.universe)}`)
                    .concat(advNear.map(a => `${this.who(a)} sends sACN ${a.universes.join(', ')}`));
                add('bad', `Port ${p.port}: universe off by one?`,
                    `The port listens to ${wants(p)}, but ${what.join('; ')}. Many consoles count Art-Net universes from 1 while the Port-Address starts at 0.`);
                continue;
            }
            const others = live.filter(s => !(s.ports || []).length);
            const advOthers = adverts.filter(a => p.protocol_mode !== 0 && !a.universes.includes(p.sacn));
            if (others.length || advOthers.length) {
                const list = others.slice(0, 4).map(s => this.uniText(s.protocol, s.universe))
                    .concat(advOthers.slice(0, 2).map(a => `sACN ${a.universes.slice(0, 6).join(', ')}${a.count > a.universes.length ? '…' : ''} (${this.who(a)})`));
                add('warn', `Port ${p.port}: nothing on its universe`,
                    `It listens to ${wants(p)}. The node hears ${[...new Set(list)].join(', ')}.`);
            } else {
                add('warn', `Port ${p.port}: nothing on its universe`, `It listens to ${wants(p)}.`);
            }
        }
        if (!outs.length && (d.ports || []).length) {
            add('info', 'No output ports', 'Every port is an input or off, so nothing received is output. Set a port to DMX output on the Ports page.');
        }

        /* quality: losses and gaps on what is in use */
        const wifi = ifs.find(i => i.name === 'wifi');
        const onlyWifi = wifi && !ifs.some(i => i.name === 'ethernet');
        for (const s of live.filter(s => (s.ports || []).length)) {
            if (s.lost_recent > 0) {
                const tip = onlyWifi ? (s.protocol === 'sacn'
                    ? ' Over Wi-Fi, sACN multicast is often unreliable: send unicast to the node if the console can.'
                    : ' Wi-Fi drops packets: move the node closer to the access point, or use Ethernet.') : ' Check cables and switches between the console and the node.';
                add('warn', `Losing packets from ${this.who(s)}`, `${plural(s.lost_recent, 'packet')} missing on ${this.uniText(s.protocol, s.universe)} in the last 10–20 s.${tip}`);
            } else if (s.max_gap_ms > 1200 && s.rate >= 2) {
                add('warn', `Gaps from ${this.who(s)}`,
                    `Up to ${(s.max_gap_ms / 1000).toFixed(1)} s between packets on ${this.uniText(s.protocol, s.universe)}. Long gaps can make fixtures flicker or trigger the failsafe.`);
            }
        }
        if (wifi && wifi.rssi !== undefined && wifi.rssi < -72) {
            add('warn', 'Weak Wi-Fi signal', `The node's Wi-Fi signal is ${wifi.rssi} dBm. Below about -70 dBm, DMX over Wi-Fi gets choppy.`);
        }

        /* subnet: Art-Net still arrives, but the console may not find the node */
        const lan = ifs.filter(i => i.name !== 'ap');
        const stray = live.filter(s => s.protocol === 'artnet' && lan.length && !ifs.some(i => sameSubnet(s.ip, i.ip, i.netmask)));
        if (stray.length) {
            const me = lan.map(i => `${i.ip} / ${i.netmask}`).join(', ');
            add('info', 'Console on another subnet',
                `${[...new Set(stray.map(s => s.ip))].join(', ')} ${stray.length > 1 ? 'are' : 'is'} outside the node's network (${me}). Data still arrives, but the console may not list the node or reach it directly. Put both in the same range, e.g. 2.x.x.x / 255.0.0.0 for Art-Net.`);
        }

        for (const p of (d.ports || []).filter(p => p.output && p.rules)) {
            add('info', `Port ${p.port} has channel rules`, 'Patches, limits or parks change what it outputs. See the Ports page if channels look stuck or swapped.');
        }
        return hints;
    },

    renderSources(d) {
        if (!d) return;
        this.sigData = d;
        const hints = this.sourceHints(d);
        const worst = hints.some(h => h.cls === 'bad') ? 'bad' : hints.some(h => h.cls === 'warn') ? 'warn' : '';
        const chip = $('sigState');
        if (chip) {
            chip.className = 'chip ' + (worst || 'ok');
            setText(chip, worst === 'bad' ? 'Needs attention' : worst === 'warn' ? 'Check' : d.listening ? 'All good' : 'Off');
        }
        this._setHtml($('sigHints'), hints.map(h =>
            `<div class="note ${h.cls}"><strong>${esc(h.title)}</strong>${esc(h.text)}</div>`).join(''));

        const rows = (d.sources || []).map(s => {
            const live = s.age_ms < 2500;
            const used = (s.ports || []).map(p => `Port ${p}`);
            const filt = (s.filtered || []).map(p => `Port ${p} (protocol off)`);
            let usedBy = used.concat(filt).join(', ') || '<span class="help" style="display:inline">not used</span>';
            if (s.preview) usedBy += ' <span class="chip warn">preview</span>';
            const tags = [];
            if (s.protocol === 'sacn') tags.push(`p${s.priority}`);
            if (s.pap) tags.push('per-address');
            const lost = s.sequenced === false ? '--'
                : `${s.lost_recent.toLocaleString()}${s.lost ? ` <small>(${s.lost.toLocaleString()} total)</small>` : ''}`;
            return `<tr${live ? '' : ' style="opacity:.55"'}>
                <td>${s.name ? `${esc(s.name)} <small>${esc(s.ip)}</small>` : esc(s.ip)}</td>
                <td>${esc(this.uniText(s.protocol, s.universe))}${tags.length ? ` <small>${esc(tags.join(' · '))}</small>` : ''}</td>
                <td class="num">${live ? `${s.rate.toFixed(s.rate < 10 ? 1 : 0)}/s` : '--'}</td>
                <td class="num"${s.lost_recent ? ' style="color:var(--warn)"' : ''}>${lost}</td>
                <td class="num">${s.max_gap_ms ? `${s.max_gap_ms.toLocaleString()} ms` : '--'}</td>
                <td>${live ? 'now' : `${Math.round(s.age_ms / 1000)} s ago`}</td>
                <td>${usedBy}</td>
            </tr>`;
        });
        this._setHtml($('sigTable'), rows.join('') ||
            `<tr><td colspan="7" class="empty">${d.listening ? 'Nothing heard yet' : 'Not listening (tester mode)'}</td></tr>`);
        const adv = (d.advertised || []).map(a =>
            `${this.who(a)}: sACN ${a.universes.join(', ')}${a.count > a.universes.length ? ` and ${a.count - a.universes.length} more` : ''}`);
        setText('sigAdverts', adv.length ? `sACN discovery — consoles announce: ${adv.join(' · ')}` : '');
    },

    async blackoutPort(port) {
        try {
            await this.post(`/api/ports/${port}/blackout`);
            this.toast(`Port ${port} blacked out`, 'warning');
        } catch (_) { /* toast shown */ }
    },

    // ------------------------------------------------------------------------
    // Port settings (node mode)
    // ------------------------------------------------------------------------

    buildPortForms() {
        const opts = (list, sel) => list.map(([v, t]) => `<option value="${v}"${v === sel ? ' selected' : ''}>${t}</option>`).join('');
        const modes = [[0, 'Disabled'], [1, 'DMX output'], [2, 'DMX input']];
        if (this.has('rdm')) modes.push([3, 'RDM controller'], [4, 'RDM responder']);
        $('portForms').innerHTML = this.portList().map(n => `
            <div class="card">
                <div class="card-head"><h2>Port ${n}</h2></div>
                <form id="portForm${n}" onsubmit="return app.savePort(event, ${n})">
                    <div class="field"><label>Mode</label><select name="mode" onchange="app.portModeChanged(this.form)">${opts(modes, 1)}</select>
                        <small class="help" data-mode-help></small></div>
                    <div class="fields two">
                        <div class="field"><label>Art-Net Port-Address</label>
                            <input type="number" name="universe" min="0" max="32767" value="${n - 1}" oninput="app.showNetSubUni(this)">
                            <small class="help" data-nsu></small></div>
                        ${this.has('sacn') ? `<div class="field"><label>sACN universe</label>
                            <input type="number" name="sacn_universe" min="1" max="63999" value="${n}">
                            <small class="help">E1.31, set apart from the Art-Net address.</small></div>
                        <div class="field"><label>Protocol</label><select name="protocol_mode">${opts(PROTOCOLS, 4)}</select>
                            <small class="help" data-proto-help></small></div>` : ''}
                    </div>
                    <div data-out-only>
                        <div class="fields two">
                            <div class="field"><label>Merge</label><select name="merge_mode">${opts(MERGES, 0)}</select></div>
                            <div class="field"><label>Backup Art-Net Port-Address</label>
                                <input type="number" name="universe_secondary" min="0" max="32767" placeholder="none">
                                <small class="help">Only used by the Backup merge.</small></div>
                        </div>
                        ${this.nports > 1 ? `<div class="field"><label>DMX from another port</label>
                            <select name="local" onchange="app.localChanged(this.form)">${this.localOptions(n)}</select>
                            <small class="help" data-local-help></small></div>` : ''}
                        ${this.has('rdm') ? `<div data-rdm-opt><label class="check"><input type="checkbox" name="rdm_enabled">
                            <span><strong>RDM on this output</strong><small>Find and address fixtures, and answer RDM over Art-Net. RDM controller mode always has it.</small></span></label></div>` : ''}
                        <div class="subcard">
                            <h3 style="margin-top:0">Output timing</h3>
                            <div class="fields">
                                <div class="field"><label>Refresh (Hz)</label><input type="number" name="out_rate_hz" min="1" max="44" value="44"></div>
                                <div class="field"><label>Break (µs)</label><input type="number" name="break_us" min="92" max="1000" value="176"></div>
                                <div class="field"><label>MAB (µs)</label><input type="number" name="mab_us" min="12" max="1000" value="12"></div>
                                <div class="field"><label>Slots</label><input type="number" name="out_slots" min="24" max="512" value="512"></div>
                            </div>
                            <small class="help">The defaults (44 Hz, 176 µs, 12 µs, 512 slots) suit almost every fixture. Lower the refresh or lengthen break / MAB for old or fussy fixtures. With RDM, break stays within 176–352 µs and MAB within 12–88 µs.</small>
                        </div>
                        <div class="subcard">
                            <div class="card-head" style="margin-bottom:4px"><h3 style="margin:0">Channel rules</h3>
                                <div class="actions"><button type="button" class="btn xs" data-rule-add onclick="app.addRule(${n})">Add rule</button></div></div>
                            <div data-rules></div>
                            <small class="help">Change what this port outputs, whatever the console sends. <strong>Patch</strong> outputs one block of channels on others, <strong>Limit</strong> caps channels at a level, <strong>Park</strong> holds them at a level. Patches go first, then limits, then parks. Up to ${RULES_MAX} rules.</small>
                        </div>
                        ${this.has('scenes') ? `<div class="fields two section">
                            <div class="field"><label>Power-on scene</label><select name="startup_scene" data-scene-select><option value="0">None</option></select>
                                <small class="help">Shown from power-on until a console sends data, then it fades out.</small></div>
                            <div class="field"><label>Failsafe scene</label><select name="failsafe_scene" data-scene-select><option value="0">None (zero)</option></select>
                                <small class="help">Shown when every source is lost and the failsafe is set to "Failsafe scene".</small></div>
                        </div>` : ''}
                    </div>
                    <div data-in-only hidden>
                        <div class="fields two">
                            <div class="field"><label>Send to</label><select name="send_to" onchange="app.sendToChanged(this.form)">
                                <option value="all">Everyone (broadcast / multicast)</option>
                                <option value="nodes">Art-Net nodes on this universe</option>
                                <option value="list">These addresses</option></select>
                                <small class="help" data-send-help></small></div>
                            <div class="field" data-send-list><label>Addresses</label>
                                <input type="text" name="send_list" placeholder="10.0.0.20, 10.0.0.21" autocomplete="off" spellcheck="false">
                                <small class="help">Up to ${SEND_LIST_MAX}, separated by commas. Art-Net and sACN both go to each one.</small></div>
                        </div>
                    </div>
                    <div class="btn-row section"><button type="submit" class="btn primary">Apply</button></div>
                </form>
            </div>`).join('');
        this.portList().forEach(n => {
            const f = $('portForm' + n);
            this.showNetSubUni(f.elements.universe);
            f.querySelectorAll('select[data-scene-select]').forEach(sel =>
                sel.addEventListener('change', () => { sel.dataset.want = sel.value; }));
            this.portModeChanged(f);
        });
    },

    // DMX from another port's input: merged with the network, or on its own (splitter)
    localOptions(n) {
        let html = '<option value="0">None</option>';
        for (const m of this.portList()) {
            if (m === n) continue;
            html += `<option value="${m}:merge">Port ${m} input, merged with the network</option>` +
                    `<option value="${m}:only">Port ${m} input only (splitter)</option>`;
        }
        return html;
    },

    readLocal(f) {
        const sel = f.elements.local;
        if (!sel) return null;
        const [m, how] = sel.value.split(':');
        const input = parseInt(m) || 0;
        return { local_inputs: input ? [input] : [], local_only: input > 0 && how === 'only' };
    },

    localChanged(f) {
        const help = f.querySelector('[data-local-help]');
        const l = this.readLocal(f);
        if (!help || !l) return;
        const m = l.local_inputs[0];
        if (!m) {
            setText(help, 'Plug a desk or another DMX line into a port set to DMX input, and this port can output it too.');
            help.classList.remove('bad');
            return;
        }
        const src = $('portForm' + m);
        const srcMode = src ? parseInt(src.elements.mode.value) : 2;
        const text = l.local_only
            ? `Splitter: outputs port ${m}'s DMX frame by frame and ignores the network.`
            : `Port ${m}'s DMX joins the network sources like another console, using this port's merge mode.`;
        const notInput = !this.isInput(srcMode);
        setText(help, notInput ? `${text} Port ${m} is not a DMX input yet: set it to DMX input.` : text);
        help.classList.toggle('bad', notInput);
    },

    readSendList(f) {
        const el = f.elements.send_list;
        return el ? el.value.split(/[\s,;]+/).filter(Boolean) : [];
    },

    sendToChanged(f) {
        const sel = f.elements.send_to;
        if (!sel) return;
        const list = f.querySelector('[data-send-list]');
        if (list) list.hidden = sel.value !== 'list';
        const sacn = this.has('sacn') ? ' sACN stays multicast.' : '';
        setText(f.querySelector('[data-send-help]'), {
            all: 'Every device on the network gets it. On Wi-Fi the access point may hold broadcast and multicast back by a few hundred ms and never resends a lost one: pick unicast there.',
            nodes: `Art-Net goes only to the nodes that answer ArtPoll with an output on this universe, as Art-Net 4 asks (everyone while none answer).${sacn}`,
            list: 'Only these devices get it, unicast: the reliable choice on Wi-Fi.',
        }[sel.value] || '');
    },

    fillPortForm(n, pc) {
        const f = $('portForm' + n);
        if (!f) return;
        const set = (name, v) => { if (f.elements[name] && v !== undefined) f.elements[name].value = v; };
        set('mode', String(pc.mode ?? 1));
        set('universe', pc.universe_primary ?? n - 1);
        set('sacn_universe', pc.sacn_universe ?? n);
        set('protocol_mode', String(pc.protocol_mode ?? 4));
        set('merge_mode', String(pc.merge_mode ?? 0));
        const b = pc.universe_secondary;
        if (f.elements.universe_secondary) f.elements.universe_secondary.value = (b === undefined || b < 0) ? '' : b;
        if (f.elements.rdm_enabled) f.elements.rdm_enabled.checked = pc.rdm_enabled !== false;
        set('out_rate_hz', pc.out_rate_hz ?? 44);
        set('break_us', pc.break_us ?? 176);
        set('mab_us', pc.mab_us ?? 12);
        set('out_slots', pc.out_slots ?? 512);
        for (const k of ['startup_scene', 'failsafe_scene']) {
            const sel = f.elements[k];
            if (sel) { sel.dataset.want = String(pc[k] || 0); sel.value = sel.dataset.want; }
        }
        this.renderRules(n, Array.isArray(pc.rules) ? pc.rules : []);
        if (f.elements.local) {
            const m = (pc.local_inputs || [])[0] || 0;
            f.elements.local.value = m ? `${m}:${pc.local_only ? 'only' : 'merge'}` : '0';
            if (f.elements.local.selectedIndex < 0) f.elements.local.value = '0';
        }
        if (f.elements.send_to) {
            f.elements.send_to.value = pc.send_to || 'all';
            if (f.elements.send_to.selectedIndex < 0) f.elements.send_to.value = 'all';
            f.elements.send_list.value = (pc.send_list || []).join(', ');
        }
        this.showNetSubUni(f.elements.universe);
        this.portModeChanged(f);
    },

    // Channel rules (patch / limit / park), edited in the port form

    readRules(n) {
        const f = $('portForm' + n);
        if (!f) return [];
        return [...f.querySelectorAll('[data-rule]')].map(row => {
            const v = name => { const el = row.querySelector(`[name="${name}"]`); return el ? Number(el.value) : undefined; };
            const r = { type: row.querySelector('[name="r_type"]').value, channel: v('r_channel'), count: v('r_count') };
            if (r.type === 'patch') r.from = v('r_from'); else r.level = v('r_level');
            return r;
        });
    },

    renderRules(n, list) {
        const f = $('portForm' + n);
        const box = f ? f.querySelector('[data-rules]') : null;
        if (!box) return;
        const num = (name, label, v, lo, hi) =>
            `<label>${label}<input type="number" name="${name}" min="${lo}" max="${hi}" value="${esc(v ?? '')}" oninput="app.ruleInput(${n}, this)"></label>`;
        box.innerHTML = list.length ? list.map((r, i) => `
            <div class="rule-row" data-rule="${i}">
                <label class="rule-type">Rule ${i + 1}<select name="r_type" onchange="app.ruleTypeChanged(${n})">${RULE_TYPES.map(([t, l]) =>
                    `<option value="${t}"${t === r.type ? ' selected' : ''}>${l}</option>`).join('')}</select></label>
                <button type="button" class="btn xs ghost" onclick="app.removeRule(${n}, ${i})">Remove</button>
                ${num('r_channel', 'Channel', r.channel, 1, 512)}
                ${num('r_count', 'Count', r.count, 1, 512)}
                ${r.type === 'patch' ? num('r_from', 'From', r.from, 1, 512) : num('r_level', 'Level', r.level, 0, 255)}
                <small></small>
            </div>`).join('') : '<p class="help" style="margin:6px 0 8px">No rules: the port outputs what arrives.</p>';
        box.querySelectorAll('[data-rule]').forEach(row => this.ruleInput(n, row.querySelector('input')));
        const add = f.querySelector('[data-rule-add]');
        if (add) add.disabled = list.length >= RULES_MAX;
    },

    ruleInput(n, input) {
        const row = input && input.closest('[data-rule]');
        if (!row) return;
        const r = this.readRules(n)[Number(row.dataset.rule)];
        row.classList.toggle('bad', !!ruleProblem(r));
        setText(row.querySelector('small'), ruleText(r));
    },

    addRule(n) {
        const list = this.readRules(n);
        if (list.length >= RULES_MAX) return;
        list.push({ type: 'park', channel: 1, count: 1, level: 255 });
        this.renderRules(n, list);
    },

    removeRule(n, i) {
        const list = this.readRules(n);
        list.splice(i, 1);
        this.renderRules(n, list);
    },

    ruleTypeChanged(n) {
        /* a patch needs "from", the others a level: fill in the one it lacks */
        this.renderRules(n, this.readRules(n).map(r => {
            if (r.type === 'patch' && !Number.isInteger(r.from)) r.from = 1;
            if (r.type !== 'patch' && !Number.isInteger(r.level)) r.level = r.type === 'limit' ? 128 : 255;
            return r;
        }));
    },

    portModeChanged(f) {
        const m = parseInt(f.elements.mode.value);
        const out = m === 1 || m === 3;
        f.querySelectorAll('[data-out-only]').forEach(el => { el.hidden = !out; });
        f.querySelectorAll('[data-in-only]').forEach(el => { el.hidden = !this.isInput(m); });
        this.sendToChanged(f);
        const ro = f.querySelector('[data-rdm-opt]');
        if (ro) ro.hidden = m !== 1;
        const help = {
            0: 'The port is off.',
            1: 'Outputs what consoles send on its universe.',
            2: 'Sends the DMX it receives to the network.',
            3: 'Outputs like DMX output, and finds and talks to RDM fixtures.',
            4: 'Receives DMX, and answers RDM like a fixture.',
        }[m] || '';
        setText(f.querySelector('[data-mode-help]'), help);
        const ph = f.querySelector('[data-proto-help]');
        if (ph) setText(ph, this.isInput(m) ? 'What the input sends on the network.' : m === 0 ? '' : 'Which network data the output listens to.');
        /* a port's mode decides whether the others can take its DMX */
        for (const k of this.portList()) {
            const g = $('portForm' + k);
            if (g) this.localChanged(g);
        }
    },

    showNetSubUni(input) {
        const help = input && input.parentElement ? input.parentElement.querySelector('[data-nsu]') : null;
        if (!help) return;
        const [n, s, u] = netSubUni(input.value).split(':');
        help.textContent = `Net ${n} : Sub-Net ${s} : Universe ${u}`;
    },

    async savePort(event, port) {
        event.preventDefault();
        const f = event.target;
        const g = name => f.elements[name];
        const backup = (g('universe_secondary') ? g('universe_secondary').value : '').trim();
        const pc = {
            mode: parseInt(g('mode').value),
            universe_primary: clampInt(g('universe').value, 0, 32767, 0),
            universe_secondary: backup === '' ? -1 : clampInt(backup, 0, 32767, -1),
            merge_mode: parseInt(g('merge_mode').value) || 0,
            out_rate_hz: clampInt(g('out_rate_hz').value, 1, 44, 44),
            break_us: clampInt(g('break_us').value, 92, 1000, 176),
            mab_us: clampInt(g('mab_us').value, 12, 1000, 12),
            out_slots: clampInt(g('out_slots').value, 24, 512, 512),
        };
        if (g('sacn_universe')) pc.sacn_universe = clampInt(g('sacn_universe').value, 1, 63999, port);
        if (g('protocol_mode')) pc.protocol_mode = parseInt(g('protocol_mode').value);
        if (g('rdm_enabled')) pc.rdm_enabled = g('rdm_enabled').checked;
        if (g('startup_scene')) pc.startup_scene = clampInt(g('startup_scene').value, 0, 255, 0);
        if (g('failsafe_scene')) pc.failsafe_scene = clampInt(g('failsafe_scene').value, 0, 255, 0);
        const local = this.readLocal(f);
        if (local) Object.assign(pc, local);
        if (g('send_to')) {
            const list = this.readSendList(f);
            const bad = list.find(a => { const v = ipv4(a); return v === null || v === 0 || v >= 0xE0000000; });
            if (bad) {
                this.toast(`Port ${port}: "${bad}" is not an address to send to`, 'error');
                return false;
            }
            if (list.length > SEND_LIST_MAX) {
                this.toast(`Port ${port}: at most ${SEND_LIST_MAX} addresses`, 'error');
                return false;
            }
            pc.send_to = g('send_to').value;
            if (pc.send_to === 'list' && !list.length) {
                this.toast(`Port ${port}: give at least one address to send to`, 'error');
                return false;
            }
            pc.send_list = list;
        }
        if (f.querySelector('[data-rules]')) {
            pc.rules = this.readRules(port);
            const bad = pc.rules.findIndex(r => ruleProblem(r));
            if (bad >= 0) {
                this.toast(`Port ${port}, rule ${bad + 1}: ${ruleProblem(pc.rules[bad])}`, 'error');
                return false;
            }
        }
        try {
            await this.post('/api/config', { [`port${port}`]: pc });
            this.toast(`Port ${port} applied`, 'success');
            this.loadPortsStatus(true);
            this.loadConfig();
        } catch (_) { /* toast shown */ }
        return false;
    },

    async saveMerge(event) {
        event.preventDefault();
        const f = event.target.elements;
        const body = {
            merge: {
                timeout_seconds: clampInt(f.timeout_seconds.value, 1, 60, 3),
                loss_behavior: parseInt(f.loss_behavior.value) || 0,
            },
        };
        if (this.has('sacn')) body.node_info = { sacn_priority: clampInt(f.sacn_priority.value, 0, 200, 100) };
        try {
            await this.post('/api/config', body);
            this.toast('Merge settings saved', 'success');
        } catch (_) { /* toast shown */ }
        return false;
    },

    // ------------------------------------------------------------------------
    // Scenes
    // ------------------------------------------------------------------------

    async loadSceneOptions() {
        try {
            const scenes = await this.api('/api/scenes', { quiet: true });
            this.scenes = Array.isArray(scenes) ? scenes : [];
            this.fillSceneSelects();
        } catch (_) { /* not important */ }
    },

    fillSceneSelects() {
        document.querySelectorAll('select[data-scene-select]').forEach(sel => {
            const want = sel.dataset.want ?? sel.value ?? '0';
            const first = sel.options[0] ? sel.options[0].outerHTML : '<option value="0">None</option>';
            const ids = new Set(this.scenes.map(s => String(parseInt(s.scene_id))));
            let html = first + this.scenes.map(s =>
                `<option value="${parseInt(s.scene_id)}">${parseInt(s.scene_id)}: ${esc(s.name || '(unnamed)')}</option>`).join('');
            if (want !== '0' && !ids.has(want)) html += `<option value="${esc(want)}">${esc(want)}: (deleted)</option>`;
            sel.innerHTML = html;
            sel.value = want;
        });
    },

    async loadScenes() {
        try {
            const scenes = await this.api('/api/scenes');
            this.scenes = Array.isArray(scenes) ? scenes : [];
            this.fillSceneSelects();
            const tbody = $('scenesTable');
            if (!this.scenes.length) {
                tbody.innerHTML = '<tr><td colspan="4" class="empty">No scenes saved yet</td></tr>';
                return;
            }
            tbody.innerHTML = this.scenes.map(s => `
                <tr>
                    <td class="num">${parseInt(s.scene_id)}</td>
                    <td>${esc(s.name || '(unnamed)')} ${s.active ? '<span class="chip ok">playing</span>' : ''}</td>
                    <td>Port ${parseInt(s.port)}</td>
                    <td style="text-align:right;white-space:nowrap">
                        <button type="button" class="btn xs primary" onclick="app.playScene(${parseInt(s.scene_id)})">Play</button>
                        <button type="button" class="btn xs danger" onclick="app.deleteScene(${parseInt(s.scene_id)})">Delete</button>
                    </td>
                </tr>`).join('');
        } catch (_) { /* toast shown */ }
    },

    async recordScene() {
        const name = $('sceneName').value.trim() || 'Scene';
        const port = parseInt($('scenePort').value) || 1;
        try {
            const r = await this.post('/api/scenes/record', { port, name });
            this.toast(`Scene "${name}" recorded (number ${r.scene_id})`, 'success');
            $('sceneName').value = '';
            this.loadScenes();
        } catch (_) { /* toast shown */ }
    },

    sceneFade() { return clampInt($('sceneFade').value, 0, 60000, 0); },

    async playScene(id) {
        try {
            await this.post('/api/scenes/playback', { scene_id: id, fade_ms: this.sceneFade() });
            this.toast(`Scene ${id} playing`, 'success');
            this.loadScenes();
        } catch (_) { /* toast shown */ }
    },

    async releaseScenes() {
        try {
            await this.post('/api/scenes/release', { port: 0, fade_ms: this.sceneFade() });
            this.toast('Scenes released. The outputs follow the network again.', 'success');
            this.loadScenes();
        } catch (_) { /* toast shown */ }
    },

    async deleteScene(id) {
        if (!confirm(`Delete scene ${id}?`)) return;
        try {
            await this.api(`/api/scenes/${id}`, { method: 'DELETE' });
            this.toast(`Scene ${id} deleted`, 'success');
            this.loadScenes();
        } catch (_) { /* toast shown */ }
    },

    // ------------------------------------------------------------------------
    // RDM
    // ------------------------------------------------------------------------

    async rdmDiscover(port) {
        const btns = document.querySelectorAll('[data-rdm-scan]');
        const main = $('rdmScanBtn');
        btns.forEach(b => { b.disabled = true; });
        if (main) main.disabled = true;
        this.rdmStatus('Discovering… this takes a few seconds.', 'info', port);
        try {
            const devices = await this.post('/api/rdm/discover');
            this.rdmDevices = Array.isArray(devices) ? devices : [];
            this.renderRdm();
            const mine = port ? this.rdmDevices.filter(d => parseInt(d.port) === port) : this.rdmDevices;
            this.rdmStatus(mine.length ? `Found ${plural(mine.length, 'device')}.`
                : 'No RDM devices answered. Check the cable, the terminator and that the fixtures have RDM.', mine.length ? 'ok' : '', port);
        } catch (e) {
            if (!e.silent) this.rdmStatus('Discovery failed: ' + e.message, 'bad', port);
        } finally {
            btns.forEach(b => { b.disabled = false; });
            if (main) main.disabled = false;
        }
    },

    rdmStatus(text, cls, port) {
        const el = port ? $('tsRdmStatus' + port) : $('rdmStatus');
        if (!el) return;
        el.className = 'note' + (cls ? ' ' + cls : '');
        el.textContent = text;
    },

    async loadRdmDevices() {
        try {
            const d = await this.api('/api/rdm/devices', { quiet: true });
            if (Array.isArray(d)) { this.rdmDevices = d; this.renderRdm(); }
        } catch (_) { /* keeps its placeholder */ }
    },

    rdmRow(d, withPort) {
        const port = parseInt(d.port);
        const uid = esc(d.uid);
        const model = d.model_description ? esc(d.model_description) : `model ${hex2(parseInt(d.model) || 0).replace('0x', '0x00')}`;
        const fp = d.footprint ? `<small>${parseInt(d.footprint)} ch</small>` : '';
        const pers = d.personality_count > 1 ? `<small> · personality ${parseInt(d.personality)}/${parseInt(d.personality_count)}</small>` : '';
        const on = this.rdmIdent.has(`${port}-${d.uid}`);
        return `<tr>
            ${withPort ? `<td>${port}</td>` : ''}
            <td><strong>${esc(d.label) || model}</strong><br><small>${d.label ? model : ''}${pers}</small></td>
            <td><code>${uid}</code></td>
            <td>${esc(d.manufacturer)}<br><small>${esc(d.software || '')}</small></td>
            <td style="white-space:nowrap"><input type="number" min="1" max="512" value="${parseInt(d.dmx_address) || 1}" data-rdm-addr="${port}-${uid}" aria-label="DMX address">
                <button type="button" class="btn xs" onclick="app.setRdmAddress(${port},'${uid}')">Set</button> ${fp}</td>
            <td><button type="button" class="btn xs ${on ? 'on' : ''}" onclick="app.identifyRdm(${port},'${uid}')">${on ? 'Identifying' : 'Identify'}</button></td>
        </tr>`;
    },

    renderRdm() {
        const t = $('rdmTable');
        if (t) {
            t.innerHTML = this.rdmDevices.length ? this.rdmDevices.map(d => this.rdmRow(d, true)).join('')
                : '<tr><td colspan="6" class="empty">No devices found yet</td></tr>';
        }
        for (const p of this.portList()) {
            const tt = $('tsRdmTable' + p);
            if (!tt) continue;
            const mine = this.rdmDevices.filter(d => parseInt(d.port) === p);
            tt.innerHTML = mine.length ? mine.map(d => this.rdmRow(d, false)).join('')
                : '<tr><td colspan="5" class="empty">No devices found yet</td></tr>';
        }
    },

    async setRdmAddress(port, uid) {
        const input = document.querySelector(`[data-rdm-addr="${port}-${uid}"]`);
        const address = input ? parseInt(input.value) : NaN;
        if (!(address >= 1 && address <= 512)) { this.toast('The DMX address must be 1–512', 'error'); return; }
        try {
            await this.post('/api/rdm/address', { port, uid, address });
            this.toast(`${uid} is now at DMX address ${address}`, 'success');
            const d = this.rdmDevices.find(x => x.uid === uid && parseInt(x.port) === port);
            if (d) d.dmx_address = address;
        } catch (_) { /* toast shown */ }
    },

    async identifyRdm(port, uid) {
        const key = `${port}-${uid}`;
        const enable = !this.rdmIdent.has(key);
        try {
            await this.post('/api/rdm/identify', { port, uid, enable });
            if (enable) this.rdmIdent.add(key); else this.rdmIdent.delete(key);
            this.renderRdm();
        } catch (_) { /* toast shown */ }
    },

    // ------------------------------------------------------------------------
    // Test patterns and the loopback self-test
    // ------------------------------------------------------------------------

    testModeChanged(f) {
        if (!f) return;
        const m = f.elements.mode.value;
        const lvl = f.querySelector('[data-test="level"]');
        const spd = f.querySelector('[data-test="speed"]');
        if (lvl) lvl.hidden = !(m === 'static' || m === 'chase');
        if (spd) spd.hidden = !(m === 'chase' || m === 'ramp');
    },

    async loadTests() {
        if (!this.has('dmx_tools')) return;
        try { this.renderTests(await this.api('/api/tools/test', { quiet: true })); } catch (_) { /* ignore */ }
    },

    testText(t) {
        const what = t.mode === 'full' ? 'full' : t.mode === 'ramp' ? `ramp, ${t.speed_ms} ms`
            : t.mode === 'chase' ? `chase at ${t.level}, ${t.speed_ms} ms a step` : `level ${t.level}`;
        return `${what}, channels ${t.first}–${t.last}`;
    },

    renderTests(list) {
        if (!Array.isArray(list)) return;
        const el = $('testStatus');
        const running = list.filter(t => t.running);
        if (el) {
            el.hidden = !running.length;
            el.innerHTML = running.map(t => `<div class="btn-row" style="margin-bottom:6px"><span class="chip warn">TEST</span>
                Port ${t.port}: ${esc(this.testText(t))}
                <button type="button" class="btn xs" onclick="app.stopTest(${t.port})">Stop</button></div>`).join('');
        }
        const f = $('testForm');
        if (f) {
            for (const opt of f.elements.port.options) {
                const t = list.find(x => x.port === parseInt(opt.value));
                opt.textContent = `Port ${opt.value}` + (t && !t.output ? ' (not an output)' : '');
            }
        }
        for (const t of list) {
            const s = $('tsTestStat' + t.port);
            if (s) {
                s.innerHTML = t.running ? `<span class="chip warn">RUNNING</span> ${esc(this.testText(t))}` : 'No pattern running: the faders drive the port.';
            }
        }
    },

    async startTest(event) {
        event.preventDefault();
        const f = event.target.elements;
        const first = clampInt(f.first.value, 1, 512, 1);
        const body = {
            port: parseInt(f.port.value),
            mode: f.mode.value,
            first,
            last: clampInt(f.last.value, first, 512, 512),
            level: clampInt(f.level.value, 0, 255, 255),
            speed_ms: clampInt(f.speed_ms.value, 20, 60000, 500),
        };
        try {
            this.renderTests(await this.post('/api/tools/test', body));
            this.toast(`Test pattern running on port ${body.port}`, 'warning');
            this.refreshPorts();
        } catch (_) { /* toast shown */ }
        return false;
    },

    async stopTest(port) {
        let list = null;
        for (const p of port ? [port] : this.portList()) {
            try { list = await this.post('/api/tools/test', { port: p, mode: 'off' }); } catch (_) { /* toast shown */ }
        }
        if (list) this.renderTests(list);
        this.refreshPorts();
    },

    async loadSelfTest() {
        if (!this.has('dmx_tools')) return;
        try {
            const r = await this.api('/api/tools/selftest', { quiet: true });
            if (r.state !== 'idle') this.renderSelfTest(r);
            if (r.state === 'running') this.pollSelfTest();
        } catch (_) { /* ignore */ }
    },

    async runSelfTest(outId, inId, resultId, btnId) {
        const out = parseInt($(outId).value), inp = parseInt($(inId).value);
        if (out === inp) { this.toast('Pick two different ports', 'error'); return; }
        const what = this.tester ? 'Both ports go back to their jobs afterwards.' : 'Normal output on both ports stops meanwhile.';
        if (!confirm(`Port ${out} sends test data and port ${inp} listens for about 6 seconds. ${what} Run the test?`)) return;
        this.st.resultId = resultId;
        this.st.btnId = btnId;
        try {
            this.renderSelfTest(await this.post('/api/tools/selftest', { out_port: out, in_port: inp }));
            this.pollSelfTest();
        } catch (_) { /* toast shown */ }
    },

    pollSelfTest() {
        clearTimeout(this.st.timer);
        this.st.timer = setTimeout(async () => {
            try {
                const r = await this.api('/api/tools/selftest', { quiet: true });
                this.renderSelfTest(r);
                if (r.state === 'running') { this.pollSelfTest(); return; }
                this.refreshPorts();
            } catch (_) { this.pollSelfTest(); }
        }, 600);
    },

    renderSelfTest(r) {
        const box = $(this.st.resultId);
        const btn = $(this.st.btnId);
        if (btn) btn.disabled = r.state === 'running';
        if (!box) return;
        const cls = r.state === 'passed' ? 'ok' : r.state === 'failed' ? 'bad' : 'info';
        const steps = (r.steps || []).map(s => {
            let res = '<small>…</small>';
            if (s.done) {
                res = s.passed ? '<span class="chip ok">PASS</span>'
                    : `<span><span class="chip bad">FAIL</span> <small>${s.slots} slots` +
                      (s.first_bad ? `, ${s.mismatches} wrong, first at channel ${s.first_bad}: sent ${s.sent}, got ${s.got}` : '') + '</small></span>';
            }
            return `<li><span>${esc(s.name)}</span>${res}</li>`;
        }).join('');
        const extra = r.state === 'running' ? '' :
            `<p class="help section">Measured on port ${r.in_port}: ${r.refresh_hz} frames/s` +
            (r.break_us ? `, break ${r.break_us} µs, MAB ${r.mab_us} µs` : '') +
            `, ${plural(r.errors, 'line error')}.</p>`;
        box.innerHTML = `<div class="note ${cls}">${esc(r.message || '')}</div><ul class="steps">${steps}</ul>${extra}`;
    },

    // ------------------------------------------------------------------------
    // DMX tester mode
    // ------------------------------------------------------------------------

    async setTesterMode(on) {
        const msg = on
            ? 'Switch to DMX tester mode?\n\nThe node restarts. While it is a tester it ignores Art-Net and sACN and consoles no longer see it; ' +
              'the ports do only what you set on the Tester page. It stays a tester after a power cycle until you switch back.'
            : 'Go back to node mode?\n\nThe node restarts and the ports use their normal settings again.';
        if (!confirm(msg)) return;
        try {
            const r = await this.post('/api/tester/mode', { enabled: on });
            if (r.restarting) this.waitForRestart(on ? 'Starting the DMX tester…' : 'Starting the node…');
            else window.location.reload();
        } catch (_) { /* toast shown */ }
    },

    buildTester() {
        const roles = ['off', 'send', 'receive'].concat(this.has('rdm') ? ['rdm'] : []);
        $('tsTabs').innerHTML = this.portList().map(n => `
            <button type="button" class="port-tab" id="tsTab${n}" onclick="app.tsSelect(${n})">
                <span class="pt-name">Port ${n} <span class="chip" id="tsTabRole${n}">--</span></span>
                <span class="pt-sub" id="tsTabSub${n}">--</span>
            </button>`).join('') + (this.nports > 1 && this.has('dmx_tools') ? `
            <button type="button" class="port-tab" id="tsTabcable" onclick="app.tsSelect('cable')">
                <span class="pt-name">Cable test</span><span class="pt-sub">port to port loopback</span>
            </button>` : '');

        const tile = (id, label) => `<div class="tile" id="${id}"><div class="tile-label">${label}</div><div class="tile-value">--</div><div class="tile-sub">--</div></div>`;
        const presets = TIMING_PRESETS.map((p, i) => `<button type="button" class="btn xs" title="${esc(p.help)}" onclick="app.tsPreset(N, ${i})">${esc(p.name)}</button>`).join('');

        $('tsPanels').innerHTML = this.portList().map(n => `
        <div id="tsp${n}" hidden>
            <div class="card role-card">
                <div class="seg big" role="group" aria-label="Job of port ${n}">
                    ${roles.map(r => `<button type="button" data-role="${r}" onclick="app.tsSetRole(${n}, '${r}')">${ROLES[r].label}</button>`).join('')}
                </div>
                <div class="role-help" id="tsRoleHelp${n}"></div>
            </div>
            <div class="split section" id="tsBody${n}">
                <div class="stack">
                    <div class="tiles" data-roles="receive" style="margin:0">
                        ${tile('tsSig' + n, 'Signal')}${tile('tsRate' + n, 'Refresh rate')}${tile('tsSlots' + n, 'Slots')}${tile('tsIv' + n, 'Frame interval')}
                        ${tile('tsBrk' + n, 'Break / MAB')}${tile('tsErr' + n, 'Line errors')}${tile('tsSc' + n, 'Start codes')}${tile('tsFlick' + n, 'Changing now')}
                    </div>
                    <div class="card">
                        <div class="card-head"><h2 id="tsGridTitle${n}">Levels</h2><div class="actions"><span class="chip" id="tsBadge${n}">--</span></div></div>
                        <div id="tsGrid${n}"></div>
                        <div class="lv-legend" data-roles="receive"><span><i style="background:var(--lv-in)"></i>received</span>
                            <span><i style="box-shadow:inset 0 0 0 2px var(--warn)"></i>changed in the last second</span></div>
                        <div class="lv-legend" data-roles="send rdm"><span><i style="background:var(--lv-out)"></i>on the wire</span>
                            <span><i style="box-shadow:inset 0 0 0 2px var(--accent)"></i>on the faders below. Click a channel to move them there.</span></div>
                    </div>
                    <div class="card" data-roles="send rdm">
                        <div class="card-head"><h2>Faders</h2>
                            <div class="actions bank-nav">
                                <button type="button" class="btn sm" onclick="app.tsBank(${n}, -16)" aria-label="Previous 16 channels">‹</button>
                                <span class="mono" id="tsBankLbl${n}">1–16</span>
                                <button type="button" class="btn sm" onclick="app.tsBank(${n}, 16)" aria-label="Next 16 channels">›</button>
                                <input type="number" min="1" max="512" placeholder="Go to" aria-label="Go to channel" onchange="app.tsBankGo(${n}, this.value); this.value=''">
                            </div>
                        </div>
                        <div class="faders" id="tsFaders${n}"></div>
                        <small class="help">Click a channel number to switch it between 0 and full.</small>
                    </div>
                    <div class="card" data-roles="rdm">
                        <div class="card-head"><h2>RDM devices</h2><div class="actions"><button type="button" class="btn sm primary" data-rdm-scan onclick="app.rdmDiscover(${n})">Discover</button></div></div>
                        <div class="note" id="tsRdmStatus${n}">Discovery finds every RDM fixture on this line.</div>
                        <div class="table-wrap section"><table>
                            <thead><tr><th>Device</th><th>UID</th><th>Manufacturer</th><th>DMX address</th><th>Identify</th></tr></thead>
                            <tbody id="tsRdmTable${n}"><tr><td colspan="5" class="empty">No devices found yet</td></tr></tbody>
                        </table></div>
                    </div>
                    <div class="card" data-roles="receive">
                        <div class="card-head"><h2>Flicker watch</h2>
                            <div class="actions"><label class="check" style="margin:0"><input type="checkbox" id="tsHl${n}" onchange="app.tsHighlight(this.checked)"${this.ts.highlight ? ' checked' : ''}><span>Mark in the grid</span></label></div></div>
                        <p class="help" style="margin:0 0 10px">Channels that changed since the statistics were reset, most changes first. A channel the console holds still but that keeps changing is flicker:
                            look for a missing terminator, a long or damaged cable, two controllers on one line, or a merge fight upstream.</p>
                        <div class="table-wrap"><table>
                            <thead><tr><th class="num">Channel</th><th class="num">Now</th><th class="num">Lowest</th><th class="num">Highest</th><th>Range</th><th class="num">Changes</th><th></th></tr></thead>
                            <tbody id="tsWatch${n}"><tr><td colspan="7" class="empty">Nothing has changed yet</td></tr></tbody>
                        </table></div>
                    </div>
                </div>
                <div class="stack">
                    <div class="card" data-roles="send rdm">
                        <div class="card-head"><h2>Set channels</h2></div>
                        <form id="tsQuick${n}" onsubmit="return app.tsQuick(event, ${n})">
                            <div class="fields">
                                <div class="field"><label>First</label><input type="number" name="first" min="1" max="512" value="1"></div>
                                <div class="field"><label>Last</label><input type="number" name="last" min="1" max="512" value="512"></div>
                                <div class="field"><label>Level</label><input type="number" name="value" min="0" max="255" value="255"></div>
                            </div>
                            <div class="btn-row">
                                <button type="submit" class="btn primary">Set</button>
                                <button type="button" class="btn" onclick="app.tsQuick(null, ${n}, 255)">Full</button>
                                <button type="button" class="btn" onclick="app.tsQuick(null, ${n}, 128)">50 %</button>
                                <button type="button" class="btn" onclick="app.tsQuick(null, ${n}, 0)">Zero</button>
                            </div>
                        </form>
                        <div class="card-foot"><button type="button" class="btn danger sm" onclick="app.tsBlackout(${n})">Blackout port</button><small>All 512 channels to zero, test pattern off.</small></div>
                    </div>
                    ${this.has('dmx_tools') ? `<div class="card" data-roles="send rdm">
                        <div class="card-head"><h2>Test pattern</h2></div>
                        <form id="tsTest${n}" onsubmit="return app.startTest(event)">
                            <input type="hidden" name="port" value="${n}">
                            <div class="fields">
                                <div class="field"><label>Pattern</label><select name="mode" onchange="app.testModeChanged(this.form)">
                                    <option value="chase">Chase</option><option value="ramp">Ramp</option><option value="static">Level</option><option value="full">Full</option></select></div>
                                <div class="field"><label>First</label><input type="number" name="first" min="1" max="512" value="1"></div>
                                <div class="field"><label>Last</label><input type="number" name="last" min="1" max="512" value="512"></div>
                                <div class="field" data-test="level"><label>Level</label><input type="number" name="level" min="0" max="255" value="255"></div>
                                <div class="field" data-test="speed"><label>Speed (ms)</label><input type="number" name="speed_ms" min="20" max="60000" value="250"></div>
                            </div>
                            <div class="btn-row"><button type="submit" class="btn primary">Start</button><button type="button" class="btn" onclick="app.stopTest(${n})">Stop</button></div>
                        </form>
                        <p class="help section" id="tsTestStat${n}" style="margin-bottom:0">--</p>
                    </div>` : ''}
                    <div class="card" data-roles="send rdm">
                        <div class="card-head"><h2>Output timing</h2></div>
                        <div class="presets">${presets.replace(/\bN\b/g, n)}</div>
                        <form id="tsTiming${n}" onsubmit="return app.tsTiming(event, ${n})" oninput="this.dataset.dirty='1'">
                            <div class="fields">
                                <div class="field"><label>Refresh (Hz)</label><input type="number" name="rate_hz" min="1" max="44"></div>
                                <div class="field"><label>Break (µs)</label><input type="number" name="break_us" min="92" max="1000"></div>
                                <div class="field"><label>MAB (µs)</label><input type="number" name="mab_us" min="12" max="1000"></div>
                                <div class="field"><label>Slots</label><input type="number" name="slots" min="24" max="512"></div>
                            </div>
                            <div class="btn-row"><button type="submit" class="btn primary">Apply</button></div>
                        </form>
                        <p class="help section" id="tsTimingNow${n}" style="margin-bottom:0">--</p>
                    </div>
                    <div class="card" data-roles="receive">
                        <div class="card-head"><h2>Frame timing</h2></div>
                        <dl class="kv" id="tsIvKv${n}"></dl>
                        <h3>Time between frames</h3>
                        <div class="hist" id="tsHist${n}"></div>
                    </div>
                    <div class="card" data-roles="receive">
                        <div class="card-head"><h2>Line</h2></div>
                        <dl class="kv" id="tsLineKv${n}"></dl>
                        <div class="btn-row section" id="tsLineBtns${n}"></div>
                        <label class="check section"><input type="checkbox" onchange="app.tsBeep(this.checked)"${this.ts.beep ? ' checked' : ''} data-beep>
                            <span><strong>Beep when the signal is lost</strong><small>While this page is open.</small></span></label>
                        <div class="card-foot"><button type="button" class="btn sm" onclick="app.tsReset(${n})">Reset statistics</button><small>Counters, flicker watch and frame timing start again.</small></div>
                    </div>
                </div>
            </div>
        </div>`).join('');

        for (const n of this.portList()) {
            this.ts.bank[n] = 1;
            this.grids['ts' + n] = new LevelGrid($('tsGrid' + n), { onClick: ch => this.tsBankGo(n, ch) });
            this.buildFaders(n);
            const tf = $('tsTest' + n);
            if (tf) this.testModeChanged(tf);
        }
        this.cableNames();
        let tab = 1;
        try { const t = localStorage.getItem('tsTab'); if (t === 'cable' || (parseInt(t) >= 1 && parseInt(t) <= this.nports)) tab = t === 'cable' ? 'cable' : parseInt(t); } catch (_) { /* storage blocked */ }
        this.tsSelect(tab);
    },

    tsSelect(tab) {
        if (tab === 'cable' && !$('tsTabcable')) tab = 1;
        this.ts.tab = tab;
        this.savePref('tsTab', String(tab));
        for (const n of this.portList()) {
            $('tsp' + n).hidden = tab !== n;
            $('tsTab' + n).classList.toggle('active', tab === n);
        }
        const ct = $('tsTabcable');
        if (ct) ct.classList.toggle('active', tab === 'cable');
        $('tsCable').hidden = tab !== 'cable';
        if (tab !== 'cable') {
            const g = this.grids['ts' + tab];
            if (g) g.layout();
            this.renderLevels(tab);
            this.loadChannels();
        } else {
            this.cableDefaults();
        }
    },

    async loadTester() {
        this.once('tester', async () => {
            const d = await this.api('/api/tester', { quiet: true });
            this.ts.data = d;
            this.renderTester(d);
            this.setConn(true);
        });
    },

    renderTester(d) {
        if (!d || !Array.isArray(d.ports)) return;
        for (const p of d.ports) {
            const n = p.port;
            if (!$('tsp' + n)) continue;
            if (this.ts.roles[n] !== p.role) this.tsApplyRole(n, p.role);
            const role = p.role;
            // Tab
            const rc = $('tsTabRole' + n);
            rc.className = 'chip ' + (ROLES[role] ? ROLES[role].chip : '');
            setText(rc, ROLES[role] ? ROLES[role].label : role);
            let sub;
            const a = p.analysis || {};
            if (!p.active && role !== 'off') sub = 'starting…';
            else if (role === 'off') sub = 'switched off';
            else if (role === 'receive') sub = p.signal ? `${a.refresh_hz ?? '--'} Hz · ${a.slots ?? '--'} slots` : 'no signal';
            else if (p.test) sub = `test pattern: ${p.test.mode}`;
            else if (role === 'rdm') sub = p.rdm_discovering ? 'discovering…' : (p.rdm_tod_valid ? plural(p.rdm_devices, 'device') : 'sending');
            else sub = p.timing ? `sending ${p.timing.rate_hz} Hz` : 'sending';
            setText('tsTabSub' + n, sub);
            const tab = $('tsTab' + n);
            tab.style.borderColor = role === 'receive' && p.active && !p.signal ? 'var(--bad)' : '';

            if (role === 'receive') this.renderRx(n, p);
            if (role === 'send' || role === 'rdm') this.renderTx(n, p);
            // Signal-loss alarm
            const sig = role === 'receive' ? !!p.signal : null;
            const prev = this.ts.signal[n];
            if (prev === true && sig === false) {
                this.toast(`Port ${n}: DMX signal lost`, 'error');
                if (this.ts.beep) this.beep();
            } else if (prev === false && sig === true) {
                this.toast(`Port ${n}: DMX signal back`, 'success');
            }
            this.ts.signal[n] = sig;
        }
    },

    tsApplyRole(n, role) {
        this.ts.roles[n] = role;
        const panel = $('tsp' + n);
        panel.querySelectorAll('.seg [data-role]').forEach(b => b.classList.toggle('active', b.dataset.role === role));
        panel.querySelectorAll('[data-roles]').forEach(el => { el.hidden = !el.dataset.roles.split(' ').includes(role); });
        $('tsBody' + n).hidden = role === 'off';
        setText('tsRoleHelp' + n, ROLES[role] ? ROLES[role].help : '');
        setText('tsGridTitle' + n, role === 'receive' ? 'Levels received' : 'Levels sent');
        const g = this.grids['ts' + n];
        if (g) {
            g.mark('sel', role === 'send' || role === 'rdm' ? this.bankSet(n) : []);
            g.mark('flick', []);
            g.layout();
        }
        const tf = $('tsTiming' + n);
        if (tf) delete tf.dataset.dirty;
        this.renderLevels(n);
    },

    async tsSetRole(n, role) {
        if (this.ts.roles[n] === role) return;
        try {
            await this.post('/api/tester/port', { port: n, role });
            this.tsApplyRole(n, role);
            this.ts.signal[n] = null;
            setTimeout(() => this.loadTester(), 400);
        } catch (_) { /* toast shown */ }
    },

    tile(id, value, sub, cls) {
        const t = $(id);
        if (!t) return;
        t.className = 'tile' + (cls ? ' ' + cls : '');
        this._setHtml(t.children[1], value);
        setText(t.children[2], sub);
    },

    renderRx(n, p) {
        const a = p.analysis || {};
        const rx = p.rx || {};
        const alarm = p.active && !p.signal;
        const losses = rx.losses || 0;
        this.tile('tsSig' + n, p.signal ? 'OK' : (p.active ? 'NO SIGNAL' : '--'),
            losses ? `lost ${losses}× · last ${ago(rx.last_loss_s)}` : 'never lost since reset',
            p.signal ? 'ok' : (alarm ? 'bad alarm' : ''));
        this.tile('tsRate' + n, p.signal ? `${a.refresh_hz}<small>Hz</small>` : '--', `${a.packets_per_s ?? 0} packets/s`,
            p.signal && a.refresh_hz && a.refresh_hz < 20 ? 'warn' : '');
        const slotsVar = a.slots_max && a.slots_min !== a.slots_max;
        this.tile('tsSlots' + n, a.slots_max ? String(a.slots) : '--',
            a.slots_max ? (slotsVar ? `varies ${a.slots_min}–${a.slots_max}` : 'steady') : 'no frame yet', slotsVar ? 'warn' : '');
        const jit = rx.interval_jitter_us || 0;
        this.tile('tsIv' + n, rx.interval_avg_us ? ms(rx.interval_avg_us, 1) : '--',
            rx.interval_avg_us ? `± ${ms(jit, 2)} jitter` : '--', jit > 5000 ? 'warn' : '');
        this.tile('tsBrk' + n, a.break_us ? `${a.break_us}<small>/ ${a.mab_us} µs</small>` : '--',
            a.timing_on ? `measuring, ${a.timing_left_s} s left` : (a.break_us ? 'last measured' : 'not measured'),
            a.break_us && (a.break_us < 88 || a.mab_us < 8) ? 'warn' : '');
        const errs = (a.err_framing || 0) + (a.err_overflow || 0) + (a.err_short || 0);
        this.tile('tsErr' + n, String(errs), errs ? `framing ${a.err_framing} · overflow ${a.err_overflow} · short ${a.err_short}` : 'clean line', errs ? 'warn' : '');
        this.tile('tsSc' + n, a.alt_start_codes ? `${a.alt_start_codes}<small>other</small>` : '0x00',
            [a.alt_start_codes ? `last ${hex2(a.last_alt_start_code)}` : 'levels only', a.rdm_packets ? `RDM ${a.rdm_packets}` : ''].filter(Boolean).join(' · '));
        this.tile('tsFlick' + n, String(rx.changing ?? '--'), `${rx.changed ?? 0} changed since reset`, rx.changing > 0 ? 'warn' : '');

        const kv = (rows) => rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
        this._setHtml($('tsIvKv' + n), kv([
            ['Average', rx.interval_avg_us ? `${ms(rx.interval_avg_us)} <small>(${(1e6 / rx.interval_avg_us).toFixed(1)} Hz)</small>` : '--'],
            ['Jitter (std. dev.)', rx.interval_avg_us ? ms(jit) : '--'],
            ['Shortest', ms(rx.interval_min_us)],
            ['Longest', ms(rx.interval_max_us)],
            ['Frames', (rx.frames || 0).toLocaleString()],
            ['Since reset', this.formatUptime(rx.seconds) === '--' ? '0s' : this.formatUptime(rx.seconds)],
        ]));
        const edges = (this.ts.data && this.ts.data.hist_edges_ms) || [20, 24, 28, 35, 50, 100, 250, 1000];
        const hist = rx.hist || [];
        const total = hist.reduce((s, x) => s + x, 0);
        const top = Math.max(1, ...hist);
        const avgMs = (rx.interval_avg_us || 0) / 1000;
        const rows = hist.map((c, k) => {
            const lo = k === 0 ? 0 : edges[k - 1], hi = edges[k];
            const label = k === 0 ? `< ${hi} ms` : hi === undefined ? `≥ ${lo} ms` : `${lo}–${hi} ms`;
            const main = avgMs && avgMs >= lo && (hi === undefined || avgMs < hi);
            const pct = total ? (100 * c / total) : 0;
            return `<div class="hist-row${main ? ' peak' : ''}"><span>${label}</span><div class="hist-bar"><i style="width:${(100 * c / top).toFixed(1)}%"></i></div>` +
                `<span>${pct >= 0.1 || !c ? pct.toFixed(1) : '<0.1'}%</span></div>`;
        }).join('');
        this._setHtml($('tsHist' + n), rows || '<small>No frames yet</small>');

        this._setHtml($('tsLineKv' + n), kv([
            ['Packets per second', a.packets_per_s ?? '--'],
            ['Level frames', (a.frames || 0).toLocaleString()],
            ['Framing errors', a.err_framing ?? 0],
            ['Overflow', a.err_overflow ?? 0],
            ['Frames cut short', a.err_short ?? 0],
            ['Frames skipped for a newer one', a.dropped_late ?? 0],
            ['RDM packets', a.rdm_packets ?? 0],
            ['Other start codes', a.alt_start_codes ? `${a.alt_start_codes} <small>(last ${hex2(a.last_alt_start_code)})</small>` : 'none'],
            ['Break / MAB', this.timingText(a)],
            ['Signal lost', losses ? `${losses}× <small>(last ${ago(rx.last_loss_s)})</small>` : 'never'],
        ]));
        this._setHtml($('tsLineBtns' + n), a.timing_on
            ? `<button type="button" class="btn sm" onclick="app.measureTiming(${n}, 0)">Stop measuring</button>`
            : `<button type="button" class="btn sm" onclick="app.measureTiming(${n}, 60)">Measure break / MAB for 60 s</button>`);
    },

    renderTx(n, p) {
        const tm = p.timing;
        setText('tsTimingNow' + n, tm ? `Running: ${tm.rate_hz} Hz · break ${tm.break_us} µs · MAB ${tm.mab_us} µs · ${tm.slots} slots` +
            (p.role === 'rdm' ? '. RDM keeps break within 176–352 µs and MAB within 12–88 µs.' : '') : '--');
        const f = $('tsTiming' + n);
        if (f && !f.dataset.dirty && !f.contains(document.activeElement)) {
            f.elements.rate_hz.value = p.rate_hz;
            f.elements.break_us.value = p.break_us;
            f.elements.mab_us.value = p.mab_us;
            f.elements.slots.value = p.slots;
        }
        if (p.test === undefined) {
            const s = $('tsTestStat' + n);
            if (s && s.textContent.startsWith('RUNNING')) this.loadTests();
        }
        const s = $('tsTestStat' + n);
        if (s) {
            this._setHtml(s, p.test ? `<span class="chip warn">RUNNING</span> ${esc(this.testText(p.test))}`
                : 'No pattern running: the faders drive the port.');
        }
    },

    async loadChannels() {
        if (!this.tester || this.page !== 'tester') return;
        const n = this.ts.tab;
        if (typeof n !== 'number' || this.ts.roles[n] !== 'receive') return;
        this.once('channels', async () => {
            const c = await this.api(`/api/tester/channels?port=${n}`, { quiet: true });
            this.renderChannels(n, c);
        });
    },

    renderChannels(n, c) {
        if (!c || !Array.isArray(c.changes)) return;
        const g = this.grids['ts' + n];
        if (g) g.mark('flick', this.ts.highlight ? c.changing : []);
        const changing = new Set(c.changing || []);
        const rows = [];
        for (let i = 0; i < 512; i++) if (c.changes[i]) rows.push(i);
        rows.sort((a, b) => c.changes[b] - c.changes[a] || a - b);
        const body = $('tsWatch' + n);
        if (!rows.length) { this._setHtml(body, '<tr><td colspan="7" class="empty">Nothing has changed yet</td></tr>'); return; }
        const more = rows.length > 32 ? `<tr><td colspan="7" class="empty">and ${rows.length - 32} more channels</td></tr>` : '';
        this._setHtml(body, rows.slice(0, 32).map(i => {
            const lo = c.min[i], hi = c.max[i];
            return `<tr><td class="num watch-ch">${i + 1}</td><td class="num">${c.level[i]}</td><td class="num">${lo}</td><td class="num">${hi}</td>
                <td><div class="meter" title="${lo}–${hi}"><i style="margin-left:${(lo / 2.55).toFixed(1)}%;width:${Math.max(1, (hi - lo) / 2.55).toFixed(1)}%"></i></div></td>
                <td class="num">${c.changes[i].toLocaleString()}</td><td>${changing.has(i + 1) ? '<span class="chip warn">changing</span>' : ''}</td></tr>`;
        }).join('') + more);
    },

    tsHighlight(on) {
        this.ts.highlight = on;
        this.savePref('tsHighlight', on ? '1' : '0');
        document.querySelectorAll('[id^="tsHl"]').forEach(cb => { cb.checked = on; });
        if (!on) for (const n of this.portList()) { const g = this.grids['ts' + n]; if (g) g.mark('flick', []); }
        this.loadChannels();
    },

    tsBeep(on) {
        this.ts.beep = on;
        this.savePref('tsBeep', on ? '1' : '0');
        document.querySelectorAll('[data-beep]').forEach(cb => { cb.checked = on; });
        if (on) this.beep(0.15);   /* also unlocks audio in the browser */
    },

    beep(len = 0.6) {
        try {
            const Ctx = window.AudioContext || window.webkitAudioContext;
            if (!Ctx) return;
            if (!this.ts.audio) this.ts.audio = new Ctx();
            const ctx = this.ts.audio;
            const o = ctx.createOscillator(), g = ctx.createGain();
            o.type = 'square';
            o.frequency.value = 880;
            g.gain.value = 0.06;
            o.connect(g).connect(ctx.destination);
            o.start();
            o.stop(ctx.currentTime + len);
        } catch (_) { /* no audio */ }
    },

    async tsReset(n) {
        try {
            await this.post('/api/tester/reset', { port: n });
            this.toast(`Port ${n}: statistics reset`, 'info');
            this.loadTester();
            this.loadChannels();
        } catch (_) { /* toast shown */ }
    },

    // ---- Faders -------------------------------------------------------------

    buildFaders(n) {
        const box = $('tsFaders' + n);
        const frag = document.createDocumentFragment();
        for (let k = 0; k < 16; k++) {
            const d = document.createElement('div');
            d.className = 'fader';
            d.innerHTML = `<span class="fv">0</span><input type="range" min="0" max="255" step="1" value="0"><button type="button" class="btn xs fc">1</button>`;
            const input = d.querySelector('input');
            input.addEventListener('input', () => this.faderInput(n, k, parseInt(input.value)));
            d.querySelector('button').addEventListener('click', () => {
                const ch = this.ts.bank[n] + k;
                const cur = (this.levels[n] || [])[ch - 1] || 0;
                this.setLevel(n, ch, cur ? 0 : 255);
            });
            frag.appendChild(d);
        }
        box.replaceChildren(frag);
        this.renderFaders(n, true);
    },

    bankSet(n) {
        const s = new Set();
        for (let k = 0; k < 16; k++) if (this.ts.bank[n] + k <= 512) s.add(this.ts.bank[n] + k);
        return s;
    },

    tsBank(n, d) { this.tsBankGo(n, this.ts.bank[n] + d); },

    tsBankGo(n, ch) {
        ch = clampInt(ch, 1, 512, 1);
        this.ts.bank[n] = Math.floor((ch - 1) / 16) * 16 + 1;
        const g = this.grids['ts' + n];
        if (g) g.mark('sel', this.bankSet(n));
        this.renderFaders(n, true);
    },

    renderFaders(n, force) {
        const box = $('tsFaders' + n);
        if (!box) return;
        const base = this.ts.bank[n] || 1;
        setText('tsBankLbl' + n, `${base}–${Math.min(512, base + 15)}`);
        const lv = this.levels[n] || new Uint8Array(512);
        const local = this.ts.local[n];
        [...box.children].forEach((d, k) => {
            const ch = base + k;
            const input = d.children[1];
            if (ch > 512) { d.hidden = true; return; }
            d.hidden = false;
            const v = lv[ch - 1] || 0;
            const busy = !force && (document.activeElement === input || (local && local.has(ch)));
            if (!busy && parseInt(input.value) !== v) input.value = v;
            setText(d.children[0], this.fmtLevel(parseInt(input.value)) || '0');
            setText(d.children[2], ch);
            input.setAttribute('aria-label', `Channel ${ch}`);
        });
    },

    faderInput(n, k, v) {
        const ch = this.ts.bank[n] + k;
        if (ch > 512) return;
        this.setLevel(n, ch, v);
    },

    /** One channel of a send port, sent at once over the WebSocket */
    setLevel(n, ch, v) {
        if (!this.ts.local[n]) this.ts.local[n] = new Map();
        this.ts.local[n].set(ch, { v, t: Date.now() });
        if (this.levels[n]) this.levels[n][ch - 1] = v;
        if (!this.wsSend({ command: 'set_channel', port: n, channel: ch, value: v })) {
            if (!this.ts.pending[n]) this.ts.pending[n] = new Map();
            this.ts.pending[n].set(ch, v);
            clearTimeout(this.ts.flushTimer);
            this.ts.flushTimer = setTimeout(() => this.flushLevels(), 80);
        }
        this.renderLevels(n);
        this.renderFaders(n);
    },

    /** No WebSocket: send the channels changed since the last flush as one range */
    async flushLevels() {
        for (const n of Object.keys(this.ts.pending)) {
            const m = this.ts.pending[n];
            if (!m.size) continue;
            const chans = [...m.keys()];
            const lo = Math.min(...chans), hi = Math.max(...chans);
            const lv = this.levels[n] || new Uint8Array(512);
            const values = [];
            for (let c = lo; c <= hi; c++) values.push(m.has(c) ? m.get(c) : lv[c - 1] || 0);
            m.clear();
            try { await this.post('/api/tester/levels', { port: parseInt(n), start: lo, values }, { quiet: true }); } catch (_) { /* next change */ }
        }
    },

    async tsQuick(event, n, value) {
        if (event) event.preventDefault();
        const f = $('tsQuick' + n).elements;
        const first = clampInt(f.first.value, 1, 512, 1);
        const last = clampInt(f.last.value, first, 512, 512);
        const v = value !== undefined ? value : clampInt(f.value.value, 0, 255, 255);
        try {
            await this.post('/api/tester/levels', { port: n, first, last, value: v });
            const now = Date.now();
            if (!this.ts.local[n]) this.ts.local[n] = new Map();
            for (let c = first; c <= last; c++) {
                this.ts.local[n].set(c, { v, t: now });
                if (this.levels[n]) this.levels[n][c - 1] = v;
            }
            this.renderLevels(n);
            this.renderFaders(n, true);
        } catch (_) { /* toast shown */ }
        return false;
    },

    async tsBlackout(n) {
        try {
            if (this.has('dmx_tools')) await this.post('/api/tools/test', { port: n, mode: 'off' }, { quiet: true }).catch(() => {});
            await this.post('/api/tester/levels', { port: n, first: 1, last: 512, value: 0 });
            if (this.levels[n]) this.levels[n].fill(0);
            if (this.ts.local[n]) this.ts.local[n].clear();
            this.renderLevels(n);
            this.renderFaders(n, true);
            this.loadTests();
            this.toast(`Port ${n} blacked out`, 'warning');
        } catch (_) { /* toast shown */ }
    },

    tsPreset(n, i) {
        const p = TIMING_PRESETS[i], f = $('tsTiming' + n);
        if (!p || !f) return;
        f.elements.rate_hz.value = p.rate_hz;
        f.elements.break_us.value = p.break_us;
        f.elements.mab_us.value = p.mab_us;
        f.elements.slots.value = p.slots;
        f.dataset.dirty = '1';
    },

    async tsTiming(event, n) {
        event.preventDefault();
        const f = event.target;
        const e = f.elements;
        const body = {
            port: n,
            rate_hz: clampInt(e.rate_hz.value, 1, 44, 44),
            break_us: clampInt(e.break_us.value, 92, 1000, 176),
            mab_us: clampInt(e.mab_us.value, 12, 1000, 12),
            slots: clampInt(e.slots.value, 24, 512, 512),
        };
        try {
            await this.post('/api/tester/port', body);
            delete f.dataset.dirty;
            this.toast(`Port ${n}: output timing applied`, 'success');
            setTimeout(() => this.loadTester(), 300);
        } catch (_) { /* toast shown */ }
        return false;
    },

    // ---- Cable test ---------------------------------------------------------

    cableDefaults() {
        const d = this.ts.data;
        const o = $('tcOut'), i = $('tcIn');
        if (!d || !o || !i || o.dataset.touched) return;
        const send = d.ports.find(p => p.role === 'send' || p.role === 'rdm');
        const recv = d.ports.find(p => p.role === 'receive' && (!send || p.port !== send.port));
        if (send) o.value = String(send.port);
        if (recv) i.value = String(recv.port);
        this.cableNames();
    },

    cableNames() {
        const o = $('tcOut'), i = $('tcIn');
        if (!o || !i) return;
        setText('tcOutName', `Port ${o.value}`);
        setText('tcInName', `Port ${i.value}`);
    },

    // ------------------------------------------------------------------------
    // Network: status, mode, Wi-Fi networks, Ethernet, access point
    // ------------------------------------------------------------------------

    setupNetworkForms() {
        const ef = $('ethForm');
        if (ef) ef.querySelectorAll('input[name="eth_addr"]').forEach(r => r.addEventListener('change', () => this.showEthStatic()));
        // Which submit button sent the Wi-Fi form (Enter = the first one)
        const wf = $('wifiForm');
        if (wf) wf.querySelectorAll('button[type="submit"]').forEach(b =>
            b.addEventListener('click', () => { wf.dataset.connect = b.dataset.connect; }));
        $('wifiList').addEventListener('click', (e) => {
            const b = e.target.closest('button[data-act]');
            if (!b || b.disabled) return;
            const i = parseInt(b.dataset.i);
            const act = b.dataset.act;
            if (act === 'connect') this.wifiConnect(this.wifiProfiles[i].ssid);
            else if (act === 'up') this.wifiMove(i, -1);
            else if (act === 'down') this.wifiMove(i, 1);
            else if (act === 'edit') this.openWifiForm(i);
            else if (act === 'forget') this.wifiForget(i);
        });
        $('wifiScanResults').addEventListener('click', (e) => {
            const b = e.target.closest('button[data-i]');
            if (!b || b.disabled) return;
            this.pickScanned(this.scanList[parseInt(b.dataset.i)]);
        });
        $('netBanner').addEventListener('click', (e) => {
            const b = e.target.closest('button[data-act]');
            if (!b) return;
            if (b.dataset.act === 'apoff') this.apOff();
            if (b.dataset.act === 'dismiss') { this.netWatch = null; this.renderNetBanner(); }
        });
    },

    applyNetworkConfig(net) {
        this.netCfg = net;
        const mf = $('netModeForm');
        if (mf) {
            const want = String(net.mode ?? 0);
            const radio = mf.querySelector(`input[name="mode"][value="${want}"]`);
            if (radio && !radio.closest('[hidden]')) radio.checked = true;
            else mf.querySelector('input[name="mode"][value="0"]').checked = true;
            mf.elements.ap_always.checked = !!net.ap_always;
        }
        const ef = $('ethForm');
        if (ef && 'eth_use_static_ip' in net) {
            ef.querySelector(`input[name="eth_addr"][value="${net.eth_use_static_ip ? 'static' : 'dhcp'}"]`).checked = true;
            ef.elements.eth_static_ip.value = net.eth_static_ip || '';
            ef.elements.eth_netmask.value = net.eth_netmask || '255.255.255.0';
            ef.elements.eth_gateway.value = net.eth_gateway || '';
            this.showEthStatic();
        }
        const af = $('apForm');
        if (af) {
            af.elements.ap_ssid.value = net.ap_ssid || '';
            af.elements.ap_password.value = '';
            af.elements.ap_open.checked = !!net.ap_open;
            af.elements.ap_channel.value = net.ap_channel || 1;
            if (af.elements.ap_captive) af.elements.ap_captive.checked = net.ap_captive !== false;
            this.showApOpen();
        }
        const profiles = Array.isArray(net.wifi_profiles) ? net.wifi_profiles.map(p => ({ ...p })) : [];
        profiles.sort((a, b) => (a.priority ?? 0) - (b.priority ?? 0));
        this.wifiProfiles = profiles;
        this.renderWifiList();
    },

    showEthStatic() {
        const ef = $('ethForm');
        const sel = ef ? ef.querySelector('input[name="eth_addr"]:checked') : null;
        $('ethStaticFields').hidden = !sel || sel.value !== 'static';
    },

    /** Replace innerHTML only when it changed (keeps clicks and focus working) */
    _setHtml(el, html) {
        if (!el || el._html === html) return;
        el._html = html;
        el.innerHTML = html;
    },

    bars(rssi) {
        if (rssi === undefined || rssi === null) return '';
        const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
        let h = `<span class="bars" title="${rssi} dBm" aria-label="Signal ${n} of 4">`;
        for (let i = 1; i <= 4; i++) h += `<i${i <= n ? ' class="on"' : ''}></i>`;
        return h + '</span>';
    },

    async refreshNet() {
        try { this.renderNetwork(await this.api('/api/network/status', { quiet: true })); } catch (_) { /* offline */ }
    },

    renderNetwork(ns) {
        if (!ns) return;
        this.netStatus = ns;
        const eth = ns.ethernet || {}, wifi = ns.wifi || {}, ap = ns.ap || {};

        const primary = { ethernet: 'Ethernet', wifi: 'Wi-Fi', ap: 'Own access point' }[ns.primary] || 'Not connected';
        setText('ovNet', primary);
        setText('ovIp', ns.ip_address ? (ns.primary === 'wifi' && wifi.ssid ? `${ns.ip_address} · ${wifi.ssid}` : ns.ip_address) : 'no address');

        const iface = (id, cls, text) => {
            const el = $(id);
            if (!el) return;
            el.classList.remove('ok', 'busy', 'bad');
            if (cls) el.classList.add(cls);
            setText(id + 'Text', text);
        };
        const addr = (o) => `${o.ip} / ${o.netmask}${o.gateway && o.gateway !== '0.0.0.0' ? ` · gateway ${o.gateway}` : ''}`;
        const offMode = { ethernet: 'Ethernet only', wifi: 'Wi-Fi only', ap: 'Access point only' }[ns.net_mode];

        if (ns.ethernet) {
            if (!eth.enabled) iface('ifEth', '', `Off (mode: ${offMode || 'Automatic'})`);
            else if (!eth.present) iface('ifEth', '', 'Not detected: no W5500 Ethernet chip found');
            else if (eth.ready) iface('ifEth', 'ok', `${addr(eth)} · ${eth.dhcp ? 'DHCP' : 'static'}`);
            else if (eth.link) iface('ifEth', 'busy', eth.dhcp === false ? 'Cable in, starting…' : 'Cable in, waiting for an address from DHCP…');
            else iface('ifEth', '', 'No cable');
        }

        const nets = Array.isArray(wifi.networks) ? wifi.networks : [];
        const lastFail = nets.find(n => n.reason && n.ssid === wifi.ssid) || nets.find(n => n.reason);
        if (wifi.state === 'connected') {
            const extra = !wifi.enabled && wifi.testing ? ' · test only, not used in this mode' : '';
            iface('ifWifi', 'ok', `${wifi.ssid} · ${addr(wifi)} · ${wifi.rssi} dBm, channel ${wifi.channel}${extra}`);
        } else if (wifi.state === 'connecting') {
            iface('ifWifi', 'busy', `Connecting to ${wifi.ssid}…`);
        } else if (wifi.state === 'getting_ip') {
            iface('ifWifi', 'busy', `Joined ${wifi.ssid}, waiting for an address…`);
        } else if (!this.wifiProfiles || this.wifiProfiles.length === 0) {
            iface('ifWifi', '', 'No networks saved');
        } else if (!wifi.enabled) {
            iface('ifWifi', '', eth.ready && ns.net_mode === 'auto' ? 'Standby: Ethernet is connected' : `Off (mode: ${offMode || 'Automatic'})`);
        } else if (lastFail) {
            const next = wifi.retry_in ? ` · next try in ${wifi.retry_in} s` : '';
            iface('ifWifi', 'bad', `${lastFail.ssid}: ${lastFail.reason_text || 'failed'}${next}`);
        } else {
            iface('ifWifi', 'busy', wifi.retry_in ? `Next try in ${wifi.retry_in} s` : 'Starting…');
        }

        if (ap.active) {
            const why = { standalone: ' · access point mode', always: ' · kept on', rescue: ' · on because there is no other connection', linger: ` · turns off in ${ap.off_in} s` }[ap.reason] || '';
            iface('ifAp', 'ok', `${ap.ssid} · ${ap.ip} · channel ${ap.channel} · ${plural(ap.clients, 'device')}${why}`);
        } else {
            iface('ifAp', '', ns.net_mode === 'ap' ? 'Starting…' : 'Off · comes on by itself when the node has no other connection');
        }

        setText('netHostname', ns.hostname && this.has('mdns')
            ? `On ${this.has('ethernet') ? 'Ethernet and ' : ''}Wi-Fi the node also answers at http://${ns.hostname}.local/ (most computers and phones).` : '');
        this.renderWifiList();
        this.renderNetBanner();
    },

    /** Which interface this browser reaches the node through, if known */
    browserVia(ns) {
        const h = window.location.hostname;
        if (!ns) return null;
        if (ns.ethernet && ns.ethernet.ip && h === ns.ethernet.ip) return 'ethernet';
        if (ns.wifi && ns.wifi.ip && h === ns.wifi.ip) return 'wifi';
        if (h === '192.168.4.1' || (ns.ap && ns.ap.ip && h === ns.ap.ip)) return 'ap';
        return null;
    },

    renderNetBanner() {
        const el = $('netBanner');
        const ns = this.netStatus || {};
        const ap = ns.ap || {};
        const w = this.netWatch;
        let cls = 'info', html = '', actions = '';

        if (w) {
            const name = `<strong>${esc(w.ssid)}</strong>`;
            if (!w.done) {
                html = w.lost
                    ? `Lost contact with the node while it joins ${name}. That is normal for a few seconds when you are on its access point (it moves to the network's channel). Waiting…`
                    : w.gettingIp ? `Joined ${name}, waiting for an address…` : `Connecting to ${name}…`;
            } else if (w.done.ok) {
                cls = 'ok';
                const url = `http://${w.done.ip}/`;
                const host = ns.hostname && this.has('mdns') ? `http://${ns.hostname}.local/` : '';
                html = `Connected to ${name}. Address <a href="${esc(url)}">${esc(url)}</a>` + (host ? ` (or <a href="${esc(host)}">${esc(host)}</a>)` : '') + '.';
                if (w.done.standby) {
                    html += ns.net_mode === 'auto'
                        ? ' The password works. While Ethernet is connected the node uses Ethernet, so it leaves Wi-Fi again; Wi-Fi takes over when the cable is unplugged.'
                        : ' The password works, but this connection mode does not use Wi-Fi, so the node leaves it again.';
                } else if (this.browserVia(ns) === 'ap') {
                    html += ` To keep using this page, join ${name} on this device and open that address.`;
                }
            } else {
                cls = 'bad';
                html = `Could not connect to ${name}: ${esc(w.done.text)}`;
            }
            if (w.done) actions += '<button type="button" class="btn sm" data-act="dismiss">Dismiss</button>';
        }
        if (ap.active && ap.reason === 'linger') {
            const who = ap.clients ? ` (${ap.clients} device${ap.clients === 1 ? ' is' : 's are'} on it)` : '';
            html += (html ? '<br><br>' : '') +
                `The access point <strong>${esc(ap.ssid)}</strong> stays on for another ${ap.off_in} s${who} so you can note the new address.`;
            actions = '<button type="button" class="btn sm" data-act="apoff">Turn the access point off now</button>' + actions;
        }
        el.hidden = !html;
        el.className = `note net-banner ${cls}`;
        this._setHtml(el, html + (actions ? `<div class="btn-row">${actions}</div>` : ''));
    },

    async saveNetMode(event) {
        event.preventDefault();
        const f = event.target;
        const sel = f.querySelector('input[name="mode"]:checked');
        const mode = sel ? parseInt(sel.value) : NaN;
        if (isNaN(mode)) { this.toast('Choose a connection mode', 'error'); return false; }
        const apAlways = f.elements.ap_always.checked;
        const ns = this.netStatus || {};
        const via = this.browserVia(ns);
        const apName = (ns.ap && ns.ap.ssid) || (this.netCfg && this.netCfg.ap_ssid) || 'the node\'s access point';
        let warn = '';
        if (mode === 1 && via === 'wifi') warn = 'Wi-Fi switches off, and this page is open over Wi-Fi.';
        else if (mode === 2 && via === 'ethernet') warn = 'Ethernet switches off, and this page is open over Ethernet.';
        else if (mode === 3 && (via === 'ethernet' || via === 'wifi')) {
            warn = `The node leaves ${via === 'wifi' ? 'Wi-Fi' : 'Ethernet'}. To reach it again, join "${apName}" and open http://192.168.4.1/.`;
        }
        if (mode === 2 && !this.wifiProfiles.length) {
            warn = (warn ? warn + '\n\n' : '') + `No Wi-Fi networks are saved yet: until you add one the node is only reachable on "${apName}".`;
        }
        if (warn && !confirm(warn + '\n\nApply anyway?')) return false;
        try {
            await this.post('/api/config', { network: { mode, ap_always: apAlways } });
            if (this.netCfg) { this.netCfg.mode = mode; this.netCfg.ap_always = apAlways; }
            this.toast('Connection mode applied', 'success');
            setTimeout(() => this.refreshNet(), 1500);
        } catch (_) { /* toast shown */ }
        return false;
    },

    renderWifiList() {
        const ul = $('wifiList');
        const list = this.wifiProfiles || [];
        const wifi = (this.netStatus && this.netStatus.wifi) || {};
        const byName = {};
        (Array.isArray(wifi.networks) ? wifi.networks : []).forEach(n => { byName[n.ssid] = n; });
        let html;
        if (!list.length) {
            html = '<li class="wifi-empty">No networks saved yet. Add your home, venue or theatre network.</li>';
        } else {
            html = list.map((p, i) => {
                const n = byName[p.ssid] || {};
                let cls = '', text = 'Saved', connected = false;
                if (n.connected) { cls = 'ok'; connected = true; text = `Connected · ${wifi.ip}`; }
                else if (wifi.ssid === p.ssid && (wifi.state === 'connecting' || wifi.state === 'getting_ip')) {
                    cls = 'busy';
                    text = wifi.state === 'getting_ip' ? 'Joined, waiting for an address…' : 'Connecting…';
                } else if (n.reason) { cls = 'bad'; text = n.reason_text || `Failed (reason ${n.reason})`; }
                else if (n.visible) text = 'In range';
                else if (n.tried) text = 'Not seen in the last scan';
                const flags = [p.has_password ? 'Password saved' : 'Open network', p.use_static_ip ? `Static ${p.static_ip}` : 'DHCP'];
                return `<li class="wifi-item">
                    <div class="wifi-main">
                        <div class="wifi-name">${esc(p.ssid)}${n.visible ? this.bars(n.rssi) : ''}</div>
                        <div class="wifi-state ${cls}">${esc(text)}</div>
                        <div class="wifi-flags">${esc(flags.join(' · '))}</div>
                    </div>
                    <div class="wifi-actions">
                        ${connected ? '' : `<button type="button" class="btn xs primary" data-act="connect" data-i="${i}">Connect</button>`}
                        <button type="button" class="btn xs" data-act="up" data-i="${i}" title="Try earlier" aria-label="Move up"${i === 0 ? ' disabled' : ''}>↑</button>
                        <button type="button" class="btn xs" data-act="down" data-i="${i}" title="Try later" aria-label="Move down"${i === list.length - 1 ? ' disabled' : ''}>↓</button>
                        <button type="button" class="btn xs" data-act="edit" data-i="${i}">Edit</button>
                        <button type="button" class="btn xs danger" data-act="forget" data-i="${i}">Forget</button>
                    </div>
                </li>`;
            }).join('');
        }
        this._setHtml(ul, html);
        $('wifiAddBtn').disabled = list.length >= 5;
    },

    openWifiForm(i = -1) {
        const f = $('wifiForm');
        const list = this.wifiProfiles || [];
        if (i < 0 && list.length >= 5) { this.toast('Up to 5 networks can be saved. Forget one first.', 'error'); return; }
        const p = i >= 0 ? list[i] : null;
        this.wifiEditIndex = i;
        f.reset();
        f.elements.orig_ssid.value = p ? p.ssid : '';
        f.elements.ssid.value = p ? p.ssid : '';
        f.elements.use_static_ip.checked = !!(p && p.use_static_ip);
        f.elements.static_ip.value = (p && p.static_ip) || '';
        f.elements.netmask.value = (p && p.netmask) || '255.255.255.0';
        f.elements.gateway.value = (p && p.gateway) || '';
        const pw = f.elements.password;
        pw.type = 'password';
        const pwBtn = pw.parentElement.querySelector('button');
        if (pwBtn) pwBtn.textContent = 'Show';
        $('wifiStaticFields').hidden = !f.elements.use_static_ip.checked;
        setText('wifiPwHelp', p && p.has_password ? 'A password is saved. Leave empty to keep it.' : 'Leave empty for an open network.');
        setText('wifiFormError', '');
        $('wifiScanResults').hidden = true;
        f.hidden = false;
        $('wifiAddBtn').hidden = true;
        f.elements.ssid.focus();
        if (!p) this.scanWifi();
    },

    closeWifiForm() {
        const f = $('wifiForm');
        f.reset();
        f.hidden = true;
        this.wifiEditIndex = -1;
        $('wifiAddBtn').hidden = false;
    },

    toggleWifiStatic(cb) { $('wifiStaticFields').hidden = !cb.checked; },

    togglePw(btn) {
        const input = btn.parentElement.querySelector('input');
        if (!input) return;
        const show = input.type === 'password';
        input.type = show ? 'text' : 'password';
        btn.textContent = show ? 'Hide' : 'Show';
    },

    async scanWifi() {
        const box = $('wifiScanResults');
        const btn = $('wifiScanBtn');
        box.hidden = false;
        box.innerHTML = '<div class="scan-note">Scanning…</div>';
        btn.disabled = true;
        try {
            const nets = await this.api('/api/network/wifi-scan', { quiet: true });
            this.scanList = (Array.isArray(nets) ? nets : []).filter(n => n.ssid);
            this.scanList.sort((a, b) => b.rssi - a.rssi);
            if (!this.scanList.length) {
                box.innerHTML = '<div class="scan-note">No 2.4 GHz networks found. You can still type the name.</div>';
            } else {
                box.innerHTML = this.scanList.map((n, i) => {
                    const unsupported = n.enterprise ? 'Enterprise login (not supported)' : n.wep ? 'WEP (not supported)' : '';
                    const tags = [unsupported || (n.secure ? '' : 'Open'), n.saved ? 'Saved' : ''].filter(Boolean).join(' · ');
                    return `<button type="button" class="scan-item" data-i="${i}"${unsupported ? ' disabled' : ''}>
                        <span>${esc(n.ssid)}</span><span class="scan-meta">${esc(tags)}${this.bars(n.rssi)}</span></button>`;
                }).join('');
            }
        } catch (e) {
            if (!e.silent) box.innerHTML = `<div class="scan-note">Scan failed: ${esc(e.message)}</div>`;
        } finally {
            btn.disabled = false;
        }
    },

    pickScanned(n) {
        const f = $('wifiForm');
        if (!n) return;
        f.elements.ssid.value = n.ssid;
        $('wifiScanResults').hidden = true;
        const editing = this.wifiEditIndex >= 0 ? this.wifiProfiles[this.wifiEditIndex] : null;
        setText('wifiPwHelp', editing && editing.ssid === n.ssid && editing.has_password
            ? 'A password is saved. Leave empty to keep it.'
            : n.secure ? 'This network needs its password.' : 'Open network: leave empty.');
        if (n.secure) f.elements.password.focus();
    },

    scanSecure(ssid) {
        const n = (this.scanList || []).find(x => x.ssid === ssid);
        return !!(n && n.secure);
    },

    /** Problem with a static address, '' when it is fine. Gateway optional. */
    ipProblem(ip, mask, gw) {
        const a = ipv4(ip);
        if (a === null) return 'Enter a valid IP address, for example 10.0.0.50.';
        const first = a >>> 24;
        if (first === 0 || first === 127 || first >= 224) return `${ip.trim()} can't be used as a device address.`;
        const m = ipv4(mask);
        if (m === null || m === 0 || ((~m >>> 0) & ((~m >>> 0) + 1)) !== 0) return 'Enter a valid subnet mask, for example 255.255.255.0.';
        const host = (a & ~m) >>> 0;
        if (m !== 0xFFFFFFFF && m !== 0xFFFFFFFE && (host === 0 || host === (~m >>> 0))) {
            return `${ip.trim()} is the network's own (or broadcast) address with mask ${mask.trim()}.`;
        }
        if ((gw || '').trim()) {
            const g = ipv4(gw);
            if (g === null) return 'Enter a valid gateway, or leave it empty.';
            if (((g & m) >>> 0) !== ((a & m) >>> 0)) return 'The gateway must be in the same subnet as the IP address.';
            if (g === a) return 'The gateway can\'t be the node\'s own address.';
        }
        return '';
    },

    async submitWifiForm(event) {
        event.preventDefault();
        const f = event.target;
        const connect = f.dataset.connect !== '0';
        f.dataset.connect = '1';
        const err = $('wifiFormError');
        const ssid = f.elements.ssid.value;
        const pw = f.elements.password.value;
        const orig = f.elements.orig_ssid.value;
        const idx = this.wifiEditIndex;
        const list = (this.wifiProfiles || []).map(p => ({ ...p }));
        const old = idx >= 0 ? list[idx] : null;
        const useStatic = f.elements.use_static_ip.checked;
        const mask = f.elements.netmask.value.trim() || '255.255.255.0';

        let problem = '';
        if (!ssid.trim()) problem = 'Enter the network name.';
        else if (new TextEncoder().encode(ssid).length > 32) problem = 'Network names are at most 32 bytes long.';
        else if (list.some((p, i) => i !== idx && p.ssid === ssid)) problem = 'That network is already saved.';
        else if (pw && (pw.length < 8 || pw.length > 64 || (pw.length === 64 && !/^[0-9a-fA-F]{64}$/.test(pw)))) {
            problem = 'Wi-Fi passwords are 8–63 characters (or 64 hex digits).';
        } else if (!pw && old && old.has_password && ssid !== orig) {
            problem = 'Enter the password again: the network name changed.';
        } else if (!pw && this.scanSecure(ssid) && !(old && old.has_password)) {
            problem = 'This network needs a password.';
        } else if (useStatic) {
            problem = this.ipProblem(f.elements.static_ip.value, mask, f.elements.gateway.value);
        }
        err.textContent = problem;
        if (problem) return false;

        const prof = {
            ssid, use_static_ip: useStatic, static_ip: f.elements.static_ip.value.trim(), netmask: mask,
            gateway: f.elements.gateway.value.trim(), has_password: old ? old.has_password : false,
        };
        // No password sent = the node keeps the one saved for this name
        if (pw || !old || ssid !== orig) prof.password = pw;
        if (old) list[idx] = prof; else list.push(prof);

        const buttons = f.querySelectorAll('button');
        buttons.forEach(b => { b.disabled = true; });
        try {
            await this.saveProfiles(list);
        } catch (_) {
            return false;
        } finally {
            buttons.forEach(b => { b.disabled = false; });
        }
        this.closeWifiForm();
        if (connect) this.wifiConnect(ssid);
        else this.toast(`Saved "${ssid}"`, 'success');
        return false;
    },

    /** POST the whole list; priority = position. Passwords only when entered. */
    async saveProfiles(list) {
        const body = list.map((p, i) => {
            const o = { ssid: p.ssid, priority: i, use_static_ip: !!p.use_static_ip, static_ip: p.static_ip || '', netmask: p.netmask || '', gateway: p.gateway || '' };
            if (p.password !== undefined) o.password = p.password;
            return o;
        });
        await this.post('/api/config', { network: { wifi_profiles: body } });
        list.forEach((p, i) => {
            if (p.password !== undefined) { p.has_password = p.password.length > 0; delete p.password; }
            p.priority = i;
        });
        this.wifiProfiles = list;
        this.renderWifiList();
        setTimeout(() => this.refreshNet(), 800);
    },

    async wifiMove(i, d) {
        const list = (this.wifiProfiles || []).map(p => ({ ...p }));
        const j = i + d;
        if (j < 0 || j >= list.length) return;
        [list[i], list[j]] = [list[j], list[i]];
        try { await this.saveProfiles(list); } catch (_) { /* toast shown */ }
    },

    async wifiForget(i) {
        const list = (this.wifiProfiles || []).map(p => ({ ...p }));
        const p = list[i];
        if (!p) return;
        const wifi = (this.netStatus && this.netStatus.wifi) || {};
        let msg = `Forget "${p.ssid}" and its password?`;
        if (wifi.state === 'connected' && wifi.ssid === p.ssid) {
            msg += this.browserVia(this.netStatus) === 'wifi'
                ? '\n\nThe node is on this network now and this page is open through it: you will lose the connection.'
                : '\n\nThe node is on this network now and will leave it.';
        }
        if (!confirm(msg)) return;
        list.splice(i, 1);
        try {
            await this.saveProfiles(list);
            this.toast(`Forgot "${p.ssid}"`, 'info');
        } catch (_) { /* toast shown */ }
    },

    async wifiConnect(ssid) {
        try {
            await this.post('/api/network/wifi/connect', { ssid });
        } catch (_) {
            return;
        }
        this.netWatch = { ssid, t0: Date.now(), seen: false, lost: false, gettingIp: false, done: null };
        this.renderNetBanner();
        const banner = $('netBanner');
        if (banner.scrollIntoView) banner.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
        clearTimeout(this.netPollTimer);
        // The supervisor picks the command up within a tick; don't read the old state
        this.netPollTimer = setTimeout(() => this.pollConnect(), 1000);
    },

    async pollConnect() {
        const w = this.netWatch;
        if (!w || w.done) return;
        try {
            const ns = await this.api('/api/network/status', { quiet: true });
            w.lost = false;
            this.checkConnect(w, ns);
            this.renderNetwork(ns);
        } catch (e) {
            if (e.silent) return;
            w.lost = true;
            this.renderNetBanner();
        }
        if (this.netWatch !== w || w.done) return;
        if (Date.now() - w.t0 > 45000) {
            w.done = { ok: false, text: w.lost ? 'the node did not answer for a while. Reload this page, or look for its access point.' : 'no result after 45 seconds.' };
            this.renderNetBanner();
            return;
        }
        this.netPollTimer = setTimeout(() => this.pollConnect(), 1000);
    },

    checkConnect(w, ns) {
        const wifi = ns.wifi || {};
        const mine = wifi.ssid === w.ssid;
        if (mine && wifi.state === 'connected') { w.done = { ok: true, ip: wifi.ip, standby: !wifi.enabled }; return; }
        if (mine && (wifi.state === 'connecting' || wifi.state === 'getting_ip')) {
            w.seen = true;
            w.gettingIp = wifi.state === 'getting_ip';
            return;
        }
        if (wifi.testing && mine) return;
        if (w.seen || Date.now() - w.t0 > 5000) {
            const n = (Array.isArray(wifi.networks) ? wifi.networks : []).find(x => x.ssid === w.ssid);
            if (n && n.reason) w.done = { ok: false, text: n.reason_text || `error ${n.reason}` };
            else if (w.seen && !wifi.testing) w.done = { ok: false, text: 'the attempt ended without a connection.' };
        }
    },

    async apOff() {
        try {
            await this.post('/api/network/ap/off');
            this.toast('The access point is turning off', 'info');
            setTimeout(() => this.refreshNet(), 1000);
        } catch (_) { /* toast shown */ }
    },

    async saveEthernet(event) {
        event.preventDefault();
        const f = event.target;
        const sel = f.querySelector('input[name="eth_addr"]:checked');
        const isStatic = !!sel && sel.value === 'static';
        const ip = f.elements.eth_static_ip.value.trim();
        const mask = f.elements.eth_netmask.value.trim() || '255.255.255.0';
        const gw = f.elements.eth_gateway.value.trim();
        const net = { eth_use_static_ip: isStatic };
        if (isStatic) {
            const problem = this.ipProblem(ip, mask, gw);
            if (problem) { this.toast(problem, 'error'); return false; }
            Object.assign(net, { eth_static_ip: ip, eth_netmask: mask, eth_gateway: gw });
        }
        const via = this.browserVia(this.netStatus);
        try {
            await this.post('/api/config', { network: net });
            if (this.netCfg) Object.assign(this.netCfg, net);
            this.toast(via === 'ethernet'
                ? `Ethernet settings applied. If this page stops answering, open ${isStatic ? `http://${ip}/` : 'the new DHCP address'}.`
                : 'Ethernet settings applied', 'success');
            setTimeout(() => this.refreshNet(), 2000);
        } catch (_) { /* toast shown */ }
        return false;
    },

    /** AP form: no password field while "Open network" is ticked. */
    showApOpen() {
        const f = $('apForm');
        const open = f.elements.ap_open.checked;
        const wasOpen = !!(this.netCfg && this.netCfg.ap_open);
        $('apPwField').hidden = open;
        f.elements.ap_password.disabled = open;
        f.elements.ap_password.placeholder = wasOpen ? 'none' : 'unchanged';
        setText('apPwHelp', wasOpen ? '8–63 characters. The network is open now: enter one to close it.'
                                    : '8–63 characters. Leave empty to keep the current one.');
    },

    async saveAp(event) {
        event.preventDefault();
        const f = event.target;
        const ssid = f.elements.ap_ssid.value.trim();
        const open = f.elements.ap_open.checked;
        const wasOpen = !!(this.netCfg && this.netCfg.ap_open);
        const pw = open ? '' : f.elements.ap_password.value;
        const ch = parseInt(f.elements.ap_channel.value);
        if (!ssid) { this.toast('Enter a name for the access point', 'error'); return false; }
        if (new TextEncoder().encode(ssid).length > 31) { this.toast('The access point name is at most 31 bytes long', 'error'); return false; }
        if (pw) {
            const problem = this.apPasswordProblem(pw);
            if (problem) { this.toast(problem, 'error'); return false; }
        } else if (!open && wasOpen) {
            this.toast('Enter a password for the access point, or keep it open', 'error');
            return false;
        }
        if (!(ch >= 1 && ch <= 13)) { this.toast('The channel must be 1–13', 'error'); return false; }
        const net = { ap_ssid: ssid, ap_channel: ch };
        if (f.elements.ap_captive) net.ap_captive = f.elements.ap_captive.checked;
        if (pw || open !== wasOpen) net.ap_password = pw;
        const changedLogin = 'ap_password' in net || (this.netCfg && this.netCfg.ap_ssid !== ssid);
        const warn = [];
        if (open && !wasOpen) {
            warn.push('Anyone nearby will be able to join the access point' +
                      (this.loginRequired === false ? ' and change settings: sign-in is off too.' : '.'));
        }
        if (this.browserVia(this.netStatus) === 'ap' && changedLogin) {
            warn.push('This page is open over the access point. Devices on it disconnect and must join again' +
                      (pw ? ' with the new password.' : open ? ` (open network "${ssid}").` : ` as "${ssid}".`));
        }
        if (warn.length && !confirm(warn.join('\n\n') + '\n\nApply?')) return false;
        try {
            await this.post('/api/config', { network: net });
            if (this.netCfg) {
                this.netCfg.ap_ssid = ssid;
                this.netCfg.ap_channel = ch;
                if ('ap_captive' in net) this.netCfg.ap_captive = net.ap_captive;
                if ('ap_password' in net) this.netCfg.ap_open = open;
            }
            f.elements.ap_password.value = '';
            this.showApOpen();
            this.toast('Access point settings applied', 'success');
            setTimeout(() => this.refreshNet(), 1500);
        } catch (_) { /* toast shown */ }
        return false;
    },

    async saveIdentity(event) {
        event.preventDefault();
        const f = event.target;
        const node = {
            short_name: f.elements.short_name.value.trim().slice(0, 17),
            long_name: f.elements.long_name.value.trim().slice(0, 63),
            allow_ip_prog: f.elements.allow_ip_prog.checked,
        };
        if (!node.short_name) { this.toast('Enter a short name', 'error'); return false; }
        try {
            await this.post('/api/config', { node_info: node });
            setText('brandName', node.short_name);
            this.toast('Node name saved', 'success');
        } catch (_) { /* toast shown */ }
        return false;
    },

    // ------------------------------------------------------------------------
    // System: restart, factory reset, firmware, backup
    // ------------------------------------------------------------------------

    /** Show the wait dialog and reload once the node answers again */
    async waitForRestart(title, text, minMs = 4000) {
        this.stopActivity();
        setText('restartTitle', title);
        setText('restartText', text || 'This page reloads by itself when the node is back.');
        $('restartOverlay').hidden = false;
        const t0 = Date.now();
        await sleep(minMs);
        for (;;) {
            try {
                const r = await fetch('/api/auth/status', { cache: 'no-store' });
                if (r.ok) { window.location.reload(); return; }
            } catch (_) { /* still down */ }
            if (Date.now() - t0 > 60000) {
                setText('restartText', 'The node has not come back here yet. If its address changed (new network, factory reset), connect to it again and reload this page.');
            }
            await sleep(1500);
        }
    },

    async reboot() {
        if (!confirm('Restart the node?')) return;
        try {
            await this.post('/api/system/restart');
            this.waitForRestart('Restarting…');
        } catch (_) { /* toast shown */ }
    },

    async factoryReset() {
        if (!confirm('Factory reset? Every setting, the admin password and the Wi-Fi networks are erased.')) return;
        if (!confirm('This cannot be undone. Continue?')) return;
        try {
            await this.post('/api/system/factory-reset');
            this.waitForRestart('Factory reset…', 'The node restarts with factory settings: join its own access point to set it up again.');
        } catch (_) { /* toast shown */ }
    },

    async firmwareRollback() {
        if (!confirm('Roll back to the previous firmware? The node restarts.')) return;
        try {
            await this.post('/api/system/firmware-rollback');
            this.waitForRestart('Rolling back…', 'Starting the previous firmware.', 6000);
        } catch (_) { /* toast shown */ }
    },

    async uploadFirmware(event) {
        event.preventDefault();
        const file = $('firmwareFile').files[0];
        if (!file) { this.toast('Choose a firmware file first', 'error'); return false; }
        if (!file.name.endsWith('.bin')) { this.toast('Choose a .bin file', 'error'); return false; }
        const box = $('uploadProgress'), bar = $('uploadBar'), txt = $('uploadText');
        box.hidden = false;
        bar.style.width = '0%';
        txt.textContent = 'Uploading…';
        try {
            await new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.open('POST', '/api/system/firmware-upload');
                xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
                xhr.upload.onprogress = (e) => {
                    if (!e.lengthComputable) return;
                    const pct = Math.round(100 * e.loaded / e.total);
                    bar.style.width = pct + '%';
                    txt.textContent = pct + '%';
                };
                xhr.onload = () => {
                    if (xhr.status === 200) { resolve(); return; }
                    try {
                        const b = JSON.parse(xhr.responseText);
                        reject(new Error(b.message || b.error || 'Upload failed'));
                    } catch (_) {
                        reject(new Error('Upload failed: HTTP ' + xhr.status));
                    }
                };
                xhr.onerror = () => reject(new Error('Network error during the upload'));
                xhr.send(file);
            });
            bar.style.width = '100%';
            txt.textContent = '100%';
            this.waitForRestart('Installing the new firmware…', 'The node restarts with the new firmware.', 6000);
        } catch (e) {
            this.toast('Firmware update failed: ' + e.message, 'error');
            box.hidden = true;
        }
        return false;
    },

    async downloadBackup() {
        try {
            const cfg = await this.api('/api/config');
            const name = (cfg.node_info && cfg.node_info.short_name) || 'esp-node';
            cfg._backup = {
                format: 1,
                created: new Date().toISOString(),
                node: name,
                firmware: (this.info && this.info.firmware_version) || '',
                board: (this.info && this.info.board) || '',
            };
            const day = new Date().toISOString().slice(0, 10);
            this.saveBlob(new Blob([JSON.stringify(cfg, null, 2)], { type: 'application/json' }),
                `${name.replace(/[^A-Za-z0-9_-]+/g, '-')}-settings-${day}.json`);
        } catch (_) { /* toast shown */ }
    },

    async restoreBackup(event) {
        event.preventDefault();
        const f = event.target;
        const file = f.elements.file.files[0];
        if (!file) { this.toast('Choose a backup file first', 'error'); return false; }
        let cfg;
        try {
            cfg = JSON.parse(await file.text());
        } catch (_) {
            this.toast('That file is not a settings backup (not JSON)', 'error');
            return false;
        }
        if (!cfg || typeof cfg !== 'object' || !(cfg.port1 || cfg.node_info || cfg.merge)) {
            this.toast('That file is not a settings backup', 'error');
            return false;
        }
        const meta = cfg._backup || {};
        delete cfg._backup;
        if (cfg.tester) delete cfg.tester.enabled;   // the mode switch stays where it is
        const withNet = f.elements.network.checked;
        if (!withNet) delete cfg.network;
        if (cfg.network) {
            delete cfg.network.ap_password;   // never in a backup: keep the node's own
            (cfg.network.wifi_profiles || []).forEach(p => { delete p.password; delete p.has_password; });
        }
        const from = meta.node ? ` from "${meta.node}"${meta.created ? ` (${meta.created.slice(0, 10)})` : ''}` : '';
        if (!confirm(`Restore the settings${from}? The current settings are replaced` +
                     (withNet ? ' and the network may reconnect at a different address.' : ' (network settings are kept).'))) {
            return false;
        }
        try {
            await this.post('/api/config', cfg);
            this.toast('Settings restored', 'success');
            f.reset();
            this.loadConfig();
            this.refreshPorts();
        } catch (_) { /* toast shown */ }
        return false;
    },

    // ------------------------------------------------------------------------
    // Diagnostics: log, crash report
    // ------------------------------------------------------------------------

    startLog() {
        if (this.log.timer) return;
        this.pollLog();
        this.log.timer = setInterval(() => this.pollLog(), 2000);
    },

    stopLog() {
        clearInterval(this.log.timer);
        this.log.timer = null;
    },

    toggleLogPause() {
        this.log.paused = !this.log.paused;
        setText('logPauseBtn', this.log.paused ? 'Resume' : 'Pause');
        if (!this.log.paused) this.pollLog();
    },

    clearLogView() {
        $('logView').textContent = '';
        this.log.lines = 0;
    },

    async pollLog() {
        if (this.log.paused || this.log.busy) return;
        this.log.busy = true;
        try {
            for (let i = 0; i < 4; i++) {   // catch up in a few requests at most
                const r = await this.api(`/api/diag/log?pos=${this.log.pos}`, { quiet: true });
                if (r.pos < this.log.pos) { this.clearLogView(); this.log.partial = ''; }   // node restarted
                if (r.truncated) this.appendLog('--- older lines were dropped ---\n');
                this.appendLog(r.text || '');
                const grew = r.pos !== this.log.pos;
                this.log.pos = r.pos;
                setText('logInfo', `${Math.round((r.capacity || 0) / 1024)} KB buffer`);
                if (!grew || (r.text || '').length < 4000) break;
            }
        } catch (_) { /* offline: try again later */ }
        this.log.busy = false;
    },

    appendLog(text) {
        const v = $('logView');
        if (!text) return;
        const parts = (this.log.partial + text).split('\n');
        this.log.partial = parts.pop();
        if (!parts.length) return;
        const atEnd = v.scrollTop + v.clientHeight >= v.scrollHeight - 30;
        const frag = document.createDocumentFragment();
        for (const line of parts) {
            const d = document.createElement('div');
            d.textContent = line;
            const c = line.charAt(0);
            if (line.charAt(1) === ' ' && line.charAt(2) === '(') d.className = c === 'E' ? 'le' : c === 'W' ? 'lw' : c === 'I' ? 'li' : '';
            frag.appendChild(d);
        }
        v.appendChild(frag);
        this.log.lines += parts.length;
        while (this.log.lines > 3000 && v.firstChild) { v.removeChild(v.firstChild); this.log.lines--; }
        if ($('logFollow').checked || atEnd) v.scrollTop = v.scrollHeight;
    },

    downloadLog() { this.download('/api/diag/log.txt', 'esp-node-log.txt'); },

    async loadHealth() {
        const body = $('healthBody');
        if (!body) return;
        try {
            const h = await this.api('/api/diag/health', { quiet: true });
            const lowRam = h.internal_min_free < h.internal_low;
            const stacks = h.stacks || [];
            const rows = stacks.slice(0, 8).map(s => {
                const low = s.free < h.stack_low;
                return `<dt>${esc(s.task)}</dt><dd>${low ? `<span class="chip bad">${s.free} bytes</span>` : `${s.free} bytes`}</dd>`;
            }).join('');
            body.innerHTML = `
                <dl class="kv">
                    <dt>Internal RAM free</dt><dd>${this.formatBytes(h.internal_free)}</dd>
                    <dt>Lowest since start</dt><dd>${lowRam ? `<span class="chip bad">${this.formatBytes(h.internal_min_free)}</span>` : this.formatBytes(h.internal_min_free)}</dd>
                    <dt>Largest free block</dt><dd>${this.formatBytes(h.internal_largest)}</dd>
                    ${h.psram_free ? `<dt>PSRAM free</dt><dd>${this.formatBytes(h.psram_free)} <small>(lowest ${this.formatBytes(h.psram_min_free)})</small></dd>` : ''}
                </dl>
                ${rows ? `<h3>Tightest task stacks</h3><dl class="kv">${rows}</dl>
                <small class="help">Least stack each task has had left since start. Below ${h.stack_low} bytes a task is close to overflowing: send the log with a bug report.</small>` : ''}`;
        } catch (_) {
            body.innerHTML = '<small>Could not read the memory figures.</small>';
        }
    },

    async loadCrash() {
        const body = $('crashBody');
        try {
            const r = await this.api('/api/diag/crash', { quiet: true });
            this._setHtml($('crashReset'), r.reset_was_crash ? `<span class="chip bad">${esc(r.reset_reason)}</span>` : esc(r.reset_reason));
            if (!r.present) { body.innerHTML = '<small>No crash report stored.</small>'; return; }
            const bt = (r.backtrace || []).join(' ');
            body.innerHTML = `
                <dl class="kv">
                    <dt>Task</dt><dd>${esc(r.task || '?')}</dd>
                    <dt>PC</dt><dd class="mono">${esc(r.pc)}</dd>
                    ${r.reason ? `<dt>Reason</dt><dd>${esc(r.reason)}</dd>` : ''}
                </dl>
                <h3>Backtrace${r.backtrace_corrupted ? ' (incomplete)' : ''}</h3>
                <div class="mono" style="word-break:break-all">${esc(bt) || '--'}</div>
                <p class="help section">${r.same_firmware ? 'Recorded by the firmware running now.' : 'Recorded by different firmware than is running now: decode it with that build.'}
                    Decode it on the build computer with <code>idf.py coredump-info -c esp-node-coredump.bin</code>.</p>
                <div class="btn-row">
                    <button type="button" class="btn sm primary" onclick="app.downloadCrash()">Download core dump (${this.formatBytes(r.size)})</button>
                    <button type="button" class="btn sm danger" onclick="app.clearCrash()">Delete</button>
                </div>`;
        } catch (_) {
            body.innerHTML = '<small>Could not read the crash report.</small>';
        }
    },

    downloadCrash() { this.download('/api/diag/crash.bin', 'esp-node-coredump.bin'); },

    async clearCrash() {
        if (!confirm('Delete the stored crash report?')) return;
        try {
            await this.post('/api/diag/crash/clear');
            this.toast('Crash report deleted', 'success');
            this.loadCrash();
        } catch (_) { /* toast shown */ }
    },

    /** Download through fetch so the sign-in travels with it */
    async download(url, fallbackName) {
        try {
            const r = await fetch(url, { headers: { 'X-Requested-With': 'XMLHttpRequest' } });
            if (r.status === 401) { this.handleUnauthorized(); return; }
            if (!r.ok) {
                const b = await r.json().catch(() => ({}));
                throw new Error(b.message || `HTTP ${r.status}`);
            }
            const m = /filename="([^"]+)"/.exec(r.headers.get('Content-Disposition') || '');
            this.saveBlob(await r.blob(), m ? m[1] : fallbackName);
        } catch (e) {
            this.toast(`Download failed: ${e.message}`, 'error');
        }
    },

    saveBlob(blob, name) {
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = name;
        document.body.appendChild(a);
        a.click();
        setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
    },

    // ------------------------------------------------------------------------
    // Identify, toasts, formatting
    // ------------------------------------------------------------------------

    async identify() {
        const on = this.identifyLeft > 0;
        try {
            const r = await this.post('/api/system/identify', { seconds: on ? 0 : 30 });
            this.showIdentify(r.identify_left || 0);
        } catch (_) { /* toast shown */ }
    },

    showIdentify(left) {
        this.identifyLeft = left;
        const b = $('identifyBtn');
        b.classList.toggle('on', left > 0);
        setText(b.querySelector('span'), left > 0 ? `Stop (${left} s)` : 'Identify');
        clearTimeout(this.identifyTimer);
        if (left > 0) this.identifyTimer = setTimeout(() => this.showIdentify(left - 1), 1000);
    },

    toast(message, type = 'info') {
        const box = $('toasts');
        const t = document.createElement('div');
        t.className = `toast ${type}`;
        t.setAttribute('role', type === 'error' ? 'alert' : 'status');
        t.textContent = message;
        box.appendChild(t);
        while (box.children.length > 4) box.removeChild(box.firstChild);
        setTimeout(() => {
            t.classList.add('out');
            setTimeout(() => t.remove(), 300);
        }, type === 'error' ? 7000 : 4500);
    },

    formatBytes(bytes) {
        if (!bytes) return '--';
        const sizes = ['B', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(1024));
        return Math.round(bytes / Math.pow(1024, i) * 10) / 10 + ' ' + sizes[i];
    },

    formatUptime(seconds) {
        if (!seconds) return '--';
        const d = Math.floor(seconds / 86400), h = Math.floor((seconds % 86400) / 3600);
        const m = Math.floor((seconds % 3600) / 60), s = Math.floor(seconds % 60);
        if (d > 0) return `${d}d ${h}h ${m}m`;
        if (h > 0) return `${h}h ${m}m`;
        if (m > 0) return `${m}m ${s}s`;
        return `${s}s`;
    },
};

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => app.init());
} else {
    app.init();
}

window.app = app;
