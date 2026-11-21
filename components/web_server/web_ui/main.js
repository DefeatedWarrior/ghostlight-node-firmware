/**
 * ESP-NODE-2RDM Web Application
 * Single Page Application for device management
 */

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

const app = {
    ws: null,
    wsReconnectTimer: null,
    updateInterval: null,
    lastFrameCount: { port1: 0, port2: 0 },
    lastFrameTime: Date.now(),

    wsToken: null,
    authReloading: false,
    started: false,

    // DMX levels
    dmxFormat: 'dec',
    dmxCells: { 1: [], 2: [] },
    dmxLast: { 1: null, 2: null },
    dmxLive: { 1: false, 2: false },
    dmxInput: { 1: false, 2: false },
    dmxLastFrame: 0,

    // Network
    netCfg: null,
    netStatus: null,
    netWatch: null,
    netPollTimer: null,
    wifiProfiles: [],
    wifiEditIndex: -1,
    scanList: [],

    // Settings / scenes (for selects and backup)
    config: null,
    scenes: [],

    // Diagnostics and tools
    log: { pos: 0, timer: null, paused: false, partial: '', lines: 0 },
    analysisHtml: { 1: '', 2: '' },
    stTimer: null,
    identifyLeft: 0,
    identifyTimer: null,

    async init() {
        console.log('Initializing ESP-NODE-2RDM Web UI...');
        this.setupTabs();
        this.setupNetworkForms();
        this.setupPortExtras();
        this.setupDmxGrids();
        const auth = await this.fetchAuthStatus();
        if (auth && auth.setup_required) {
            this.showSetup(auth);
            return;
        }
        if (auth && auth.locked) {
            this.updateConnectionStatus(false);
            this.showToast('Too many failed sign-ins. Wait a minute, then reload.', 'error');
            return;
        }
        if (auth && !auth.authenticated) {
            this.handleUnauthorized();
            return;
        }
        this.start();
    },

    start() {
        if (this.started) return;
        this.started = true;
        this.loadSystemInfo();
        this.loadConfig();
        this.loadPortsStatus();
        this.refreshNetStatus();
        this.loadSceneOptions();
        this.updateInterval = setInterval(() => this.updateDashboard(), 2000);
        this.connectWebSocket();
        this.updateConnectionStatus(true);
    },

    /** GET /api/auth/status — never triggers a sign-in prompt. */
    async fetchAuthStatus() {
        try {
            const r = await fetch('/api/auth/status', { cache: 'no-store' });
            if (!r.ok) return null;
            const data = await r.json();
            this.wsToken = data.ws_token || null;
            return data;
        } catch (_) {
            return null;
        }
    },

    /** Credentials missing or stale: reloading the page makes the browser ask again. */
    handleUnauthorized() {
        if (this.authReloading) return;
        this.authReloading = true;
        this.showToast('Signed out — reloading to sign in again', 'warning');
        setTimeout(() => window.location.reload(), 1500);
    },

    showSetup(auth) {
        clearInterval(this.updateInterval);
        if (this.ws) { this.ws.onclose = null; this.ws.close(); this.ws = null; }
        clearTimeout(this.wsReconnectTimer);
        if (auth && auth.ap_ssid) document.getElementById('setupApSsid').textContent = auth.ap_ssid;
        document.getElementById('setupOverlay').hidden = false;
        const first = document.querySelector('#setupForm input[name="password"]');
        if (first) first.focus();
    },

    async submitSetup(event) {
        event.preventDefault();
        const f = new FormData(event.target);
        const pw = f.get('password') || '';
        const ap = f.get('ap_password') || '';
        const err = document.getElementById('setupError');
        let problem = '';
        if (pw.length < 8 || pw.length > 63) problem = 'Admin password must be 8–63 characters.';
        else if (pw !== f.get('password_confirm')) problem = 'Admin passwords do not match.';
        else problem = this.apPasswordProblem(ap);
        err.textContent = problem;
        if (problem) return false;

        const btn = document.getElementById('setupSubmit');
        btn.disabled = true;
        try {
            const r = await fetch('/api/setup', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
                body: JSON.stringify({ password: pw, ap_password: ap })
            });
            const body = await r.json().catch(() => ({}));
            if (!r.ok) throw new Error(body.message || `HTTP ${r.status}`);
            err.textContent = '';
            event.target.reset();
            document.querySelector('#setupOverlay p').textContent =
                'Saved. The node is restarting — this page reloads in 10 seconds. Sign in as "admin" with your new password.';
            event.target.hidden = true;
            setTimeout(() => window.location.reload(), 10000);
        } catch (e) {
            err.textContent = 'Setup failed: ' + e.message;
            btn.disabled = false;
        }
        return false;
    },

    /** Same rule as the firmware: WPA2 length, not the factory default. */
    apPasswordProblem(ap) {
        if (ap.length < 8 || ap.length > 63) return 'Access point password must be 8–63 characters.';
        if (ap === '12345678') return 'Choose an access point password other than the factory default.';
        return '';
    },

    async changePassword(event) {
        event.preventDefault();
        const form = event.target;
        const f = new FormData(form);
        const cur = f.get('current_password') || '';
        const pw = f.get('new_password') || '';
        if (pw.length < 8 || pw.length > 63) { this.showToast('New password must be 8–63 characters', 'error'); return false; }
        if (pw !== f.get('confirm_password')) { this.showToast('New passwords do not match', 'error'); return false; }
        try {
            await this.apiRequest('/api/auth/password', {
                method: 'POST',
                body: JSON.stringify({ current_password: cur, new_password: pw })
            });
            form.reset();
            this.showToast('Password changed — sign in again with the new password', 'success');
            this.authReloading = true;
            setTimeout(() => window.location.reload(), 2000);
        } catch (_) { /* toast already shown */ }
        return false;
    },

    setupTabs() {
        const tabs = document.querySelectorAll('.tab');
        const tabContents = document.querySelectorAll('.tab-content');
        tabs.forEach(tab => {
            tab.addEventListener('click', () => {
                const targetTab = tab.dataset.tab;
                tabs.forEach(t => t.classList.remove('active'));
                tabContents.forEach(tc => tc.classList.remove('active'));
                tab.classList.add('active');
                document.getElementById(targetTab).classList.add('active');
                // Lazy-load tab-specific data
                if (targetTab === 'scenes') this.loadScenes();
                if (targetTab === 'rdm') this.loadRDMDevices();
                if (targetTab === 'dmx') { this.loadTests(); this.loadSceneOptions(); }
                if (targetTab === 'system') { this.loadCrash(); this.startLog(); } else { this.stopLog(); }
            });
        });
    },

    updateConnectionStatus(connected) {
        const indicator = document.getElementById('connectionStatus');
        const text = document.getElementById('connectionText');
        if (connected) {
            indicator.classList.add('connected');
            indicator.classList.remove('disconnected');
            text.textContent = 'Connected';
        } else {
            indicator.classList.remove('connected');
            indicator.classList.add('disconnected');
            text.textContent = 'Offline';
        }
    },

    async connectWebSocket() {
        try {
            // The WebSocket needs a token from an authenticated request
            // (tokens change when the password changes or the node restarts).
            const auth = await this.fetchAuthStatus();
            if (!auth || !auth.ws_token) {
                if (auth && auth.setup_required) { this.showSetup(auth); return; }
                this.wsReconnectTimer = setTimeout(() => this.connectWebSocket(), 5000);
                return;
            }
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            const wsUrl = `${protocol}//${window.location.host}/ws?token=${encodeURIComponent(auth.ws_token)}`;
            this.ws = new WebSocket(wsUrl);
            this.ws.binaryType = 'arraybuffer';
            this.ws.onopen = () => clearTimeout(this.wsReconnectTimer);
            this.ws.onmessage = (event) => {
                if (event.data instanceof ArrayBuffer) {
                    this._onDmxFrame(event.data);
                    return;
                }
                try {
                    this.handleWebSocketMessage(JSON.parse(event.data));
                } catch (e) {
                    console.error('WebSocket message parse error:', e);
                }
            };
            this.ws.onerror = (error) => console.error('WebSocket error:', error);
            this.ws.onclose = () => {
                this.ws = null;
                this.wsReconnectTimer = setTimeout(() => this.connectWebSocket(), 5000);
            };
        } catch (e) {
            console.error('WebSocket connection error:', e);
        }
    },

    handleWebSocketMessage(data) {
        console.log('WebSocket data:', data);
    },

    /** Binary level frame: [0]=0x02 [1]=bit n set = port n+1 live [2]=bit n set = port n+1 is an
     *  input [3]=port count, then 512 bytes per port (sent / received levels) */
    _onDmxFrame(buf) {
        const b = new Uint8Array(buf);
        if (b.length < 4 || b[0] !== 0x02) return;
        const live = b[1], inputs = b[2];
        const ports = Math.min(b[3], 2);
        for (let i = 0; i < ports; i++) {
            const off = 4 + i * 512;
            if (b.length < off + 512) break;
            this._renderDmx(i + 1, b.slice(off, off + 512), !!(live & (1 << i)), !!(inputs & (1 << i)));
        }
        this.dmxLastFrame = Date.now();
    },

    /** fetch + JSON. options.quiet: no error toast (background polling). */
    async apiRequest(url, options = {}) {
        const { quiet, ...opts } = options;
        try {
            const response = await fetch(url, {
                ...opts,
                headers: {
                    'Content-Type': 'application/json',
                    'X-Requested-With': 'XMLHttpRequest',
                    ...opts.headers
                }
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
            console.error('API request failed:', error);
            if (!error.silent && !quiet) this.showToast(`Request failed: ${error.message}`, 'error');
            throw error;
        }
    },

    async loadSystemInfo() {
        try {
            const data = await this.apiRequest('/api/system/info');
            document.getElementById('sysFirmware').textContent = data.firmware_version || '--';
            document.getElementById('sysHardware').textContent = data.hardware || '--';
            document.getElementById('sysIDF').textContent = data.idf_version || '--';
            document.getElementById('sysFreeHeap').textContent = this.formatBytes(data.free_heap);
            document.getElementById('sysUptime').textContent = this.formatUptime(data.uptime_sec);
            document.getElementById('firmware').textContent = data.firmware_version || '--';
            document.getElementById('hardware').textContent = data.hardware || '--';
            document.getElementById('freeHeap').textContent = this.formatBytes(data.free_heap);
            document.getElementById('uptime').textContent = this.formatUptime(data.uptime_sec);
            this.updateConnectionStatus(true);
        } catch (error) {
            this.updateConnectionStatus(false);
        }
    },

    async loadConfig() {
        try {
            const data = await this.apiRequest('/api/config');
            this._applyConfigToForms(data);
        } catch (error) {
            console.error('Failed to load config:', error);
        }
    },

    _applyConfigToForms(data) {
        // Node info
        if (data.node_info) {
            const el = (id) => document.querySelector(`[name="${id}"]`);
            if (el('short_name')) el('short_name').value = data.node_info.short_name || '';
            if (el('long_name')) el('long_name').value = data.node_info.long_name || '';
        }

        // Merge / node-wide DMX settings
        const mf = document.getElementById('mergeSettingsForm');
        if (mf) {
            if (data.merge) {
                mf.timeout_seconds.value = data.merge.timeout_seconds ?? 3;
                mf.loss_behavior.value = data.merge.loss_behavior ?? 0;
            }
            if (data.node_info) mf.sacn_priority.value = data.node_info.sacn_priority ?? 100;
        }

        if (data.network) this._applyNetworkConfig(data.network);

        // Port forms
        for (const n of [1, 2]) {
            const pc = data[`port${n}`];
            if (!pc) continue;
            const f = document.getElementById(`port${n}Form`);
            if (f) {
                f.universe.value = pc.universe_primary ?? (n - 1);
                this.showNetSubUni(f.universe);
                if (f.sacn_universe) f.sacn_universe.value = pc.sacn_universe ?? n;
                if (f.universe_secondary) {
                    const b = pc.universe_secondary;
                    f.universe_secondary.value = (b === undefined || b < 0) ? '' : b;
                }
                f.mode.value = pc.mode ?? 1;
                if (f.protocol_mode) f.protocol_mode.value = pc.protocol_mode ?? 4;
                if (f.rdm_enabled) f.rdm_enabled.checked = pc.rdm_enabled !== false;
                const num = (name, v, d) => { if (f.elements[name]) f.elements[name].value = v ?? d; };
                num('out_rate_hz', pc.out_rate_hz, 44);
                num('break_us', pc.break_us, 176);
                num('mab_us', pc.mab_us, 12);
                num('out_slots', pc.out_slots, 512);
                for (const k of ['startup_scene', 'failsafe_scene']) {
                    const sel = f.elements[k];
                    if (sel) { sel.dataset.want = String(pc[k] || 0); sel.value = sel.dataset.want; }
                }
                this._showPortExtras(f);
            }
            const mm = document.getElementById(`port${n}MergeMode`);
            if (mm) mm.value = pc.merge_mode ?? 0;
        }
        this.config = data;
        this._fillSceneSelects();

        const idf = document.getElementById('identityForm');
        if (idf && data.node_info && idf.elements.allow_ip_prog) {
            idf.elements.allow_ip_prog.checked = data.node_info.allow_ip_prog !== false;
        }
    },

    showNetSubUni(input) {
        const help = input && input.parentElement
            ? input.parentElement.querySelector('[data-netsubuni]') : null;
        if (!help) return;
        const [n, s, u] = netSubUni(input.value).split(':');
        help.textContent = `Net ${n} : Sub ${s} : Universe ${u}`;
    },

    async loadPortsStatus() {
        try {
            const data = await this.apiRequest('/api/ports/status');
            if (Array.isArray(data)) {
                data.slice(0, 2).forEach((p, i) => { if (p) this._renderPortStatus(i + 1, p); });
            }
        } catch (error) {
            console.error('Failed to load ports status:', error);
        }
    },

    /** Frame counter that matters for the port's direction */
    _portFrames(p) {
        return (p.mode === 2 || p.mode === 4) ? (p.frames_received || 0) : (p.frames_sent || 0);
    },

    _renderPortStatus(port, p) {
        const set = (id, text) => { const e = document.getElementById(id); if (e) e.textContent = text; };
        const isInput = p.mode === 2 || p.mode === 4;
        let mode = this.getModeName(p.mode);
        if (p.scene) mode += ` (scene ${p.scene})`;
        set(`port${port}Mode`, mode);

        const an = p.artnet_universe ?? p.universe;
        const parts = [];
        if (p.protocol !== 'sacn') parts.push(`Art-Net ${an} (${netSubUni(an)})`);
        if (p.protocol !== 'artnet') parts.push(`sACN ${p.sacn_universe ?? '--'}`);
        set(`port${port}Universe`, parts.join(' / ') || '--');
        set(`dmxPort${port}Universe`, p.mode === 0 ? 'Disabled'
            : (isInput ? 'DMX input · ' : '') + (parts.join(' / ') || '--'));

        const syncNames = { artnet: 'ArtSync', sacn: 'sACN sync' };
        set(`port${port}Sync`, isInput || p.mode === 0 ? '--' : (syncNames[p.sync] || 'Free-running'));
        let rdm = '--';
        if (p.rdm_available) {
            rdm = p.rdm_discovering ? 'Discovering…'
                : p.rdm_tod_valid ? `${p.rdm_devices} device${p.rdm_devices === 1 ? '' : 's'}`
                : 'Waiting for discovery';
        } else if (!isInput && p.mode !== 0) {
            rdm = 'Off';
        }
        set(`port${port}Rdm`, rdm);

        set(`port${port}FramesLabel`, isInput ? 'Frames Received:' : 'Frames Sent:');
        set(`port${port}Frames`, this._portFrames(p));

        const tag = document.getElementById(`port${port}Tag`);
        if (tag) {
            let text = '', cls = 'test';
            if (p.test) text = `TEST · ${p.test.mode}`;
            else if (p.failsafe) { text = 'FAILSAFE'; cls = 'bad'; }
            tag.textContent = text;
            tag.className = `tag ${cls}`;
            tag.hidden = !text;
        }
        this._renderAnalysis(port, p);

        const srcEl = document.getElementById(`port${port}Sources`);
        if (srcEl) {
            if (isInput) {
                srcEl.textContent = p.signal ? `DMX in, ${p.slots} slots` : 'No DMX signal';
            } else if (!Array.isArray(p.sources) || p.sources.length === 0) {
                srcEl.textContent = p.mode === 0 ? '--' : 'None';
            } else {
                srcEl.innerHTML = p.sources.map(s => {
                    const who = s.name ? `${esc(s.name)} (${esc(s.ip)})` : esc(s.ip);
                    const tags = [s.protocol === 'sACN' ? `sACN p${s.priority}` : esc(s.protocol)];
                    if (s.backup) tags.push('backup');
                    if (s.per_address_priority) tags.push('PAP');
                    const style = s.contributing ? '' : ' style="opacity:.5"';
                    return `<div${style}>${who} <small>[${esc(tags.join(', '))}]</small></div>`;
                }).join('');
            }
        }
    },

    async updateDashboard() {
        const q = { quiet: true };
        try {
            const sysInfo = await this.apiRequest('/api/system/info', q);
            document.getElementById('freeHeap').textContent = this.formatBytes(sysInfo.free_heap);
            document.getElementById('uptime').textContent = this.formatUptime(sysInfo.uptime_sec);
            document.getElementById('sysFreeHeap').textContent = this.formatBytes(sysInfo.free_heap);
            document.getElementById('sysUptime').textContent = this.formatUptime(sysInfo.uptime_sec);

            // A connect in progress polls the status itself, faster
            if (!this.netWatch || this.netWatch.done) {
                try {
                    this.renderNetwork(await this.apiRequest('/api/network/status', q));
                } catch (e) {
                    if (e.silent) throw e;
                }
            }

            const stats = await this.apiRequest('/api/system/stats', q);
            if (stats.artnet) {
                document.getElementById('artnetPackets').textContent = stats.artnet.packets || 0;
                document.getElementById('artnetDmx').textContent = stats.artnet.dmx_packets || 0;
            }
            if (stats.sacn) {
                document.getElementById('sacnPackets').textContent = stats.sacn.packets || 0;
                document.getElementById('sacnData').textContent = stats.sacn.data_packets || 0;
            }

            const portsStatus = await this.apiRequest('/api/ports/status', q);
            if (Array.isArray(portsStatus)) {
                const now = Date.now();
                const timeDelta = (now - this.lastFrameTime) / 1000;
                portsStatus.slice(0, 2).forEach((p, i) => {
                    if (!p) return;
                    const key = `port${i + 1}`;
                    const frames = this._portFrames(p);
                    const delta = Math.max(0, frames - this.lastFrameCount[key]);
                    const rate = p.analysis ? String(p.analysis.refresh_hz)
                        : timeDelta > 0 ? (delta / timeDelta).toFixed(1) : '0.0';
                    this._renderPortStatus(i + 1, p);
                    const rateEl = document.getElementById(`${key}Rate`);
                    if (rateEl) rateEl.textContent = rate + ' Hz';
                    this.lastFrameCount[key] = frames;
                });
                this.lastFrameTime = now;
            }

            // Levels come over the WebSocket; poll only when it is quiet
            if (Date.now() - this.dmxLastFrame > 3000) await this.pollDmx();
            this.updateConnectionStatus(true);
        } catch (error) {
            this.updateConnectionStatus(false);
        }
    },

    // =========================================================================
    // DMX levels (dashboard): all 512 channels of each port
    // =========================================================================
    setupDmxGrids() {
        try {
            const f = localStorage.getItem('dmxFormat');
            if (f === 'dec' || f === 'pct') this.dmxFormat = f;
        } catch (_) { /* storage blocked */ }
        this._markDmxFormat();
        this._layoutDmxGrids();
        let t = null;
        window.addEventListener('resize', () => {
            clearTimeout(t);
            t = setTimeout(() => this._layoutDmxGrids(), 150);
        });
        [1, 2].forEach(port => {
            const grid = document.getElementById(`dmxGrid${port}`);
            if (!grid) return;
            grid.addEventListener('mouseover', (e) => {
                const c = e.target.closest('.dmx-cell');
                if (!c) return;
                const ch = parseInt(c.dataset.ch);
                const v = this.dmxLast[port] ? this.dmxLast[port][ch - 1] : 0;
                c.title = `Channel ${ch}: ${v} (${Math.round(v / 2.55)}%)`;
            });
        });
    },

    /** 32 channels per row when there is room, else 16 */
    _dmxColsFor(grid) {
        const w = grid.clientWidth || (window.innerWidth - 64);
        const cols = w >= 620 ? 32 : 16;
        grid.classList.toggle('dense', (w - 40) / cols < 22);   /* smaller digits */
        return cols;
    },

    _layoutDmxGrids() {
        [1, 2].forEach(port => {
            const grid = document.getElementById(`dmxGrid${port}`);
            if (!grid) return;
            const cols = this._dmxColsFor(grid);
            if (parseInt(grid.dataset.cols) === cols && this.dmxCells[port].length) return;
            grid.dataset.cols = cols;
            grid.style.setProperty('--dmx-cols', cols);
            const frag = document.createDocumentFragment();
            const cells = [];
            for (let ch = 0; ch < 512; ch++) {
                if (ch % cols === 0) {
                    const label = document.createElement('div');
                    label.className = 'dmx-row-label';
                    label.textContent = ch + 1;
                    frag.appendChild(label);
                }
                const c = document.createElement('div');
                c.className = 'dmx-cell zero';
                c.dataset.ch = ch + 1;
                frag.appendChild(c);
                cells.push(c);
            }
            grid.replaceChildren(frag);
            this.dmxCells[port] = cells;
            const last = this.dmxLast[port];
            this.dmxLast[port] = null;
            if (last) this._renderDmx(port, last, this.dmxLive[port], this.dmxInput[port]);
        });
    },

    _dmxText(v) {
        if (v === 0) return '';
        if (this.dmxFormat === 'pct') return v === 255 ? 'FL' : String(Math.max(1, Math.round(v / 2.55)));
        return String(v);
    },

    /** @p levels Uint8Array(512). Only cells whose value changed are touched.
     *  @p input: the port receives DMX (orange cells) instead of sending it. */
    _renderDmx(port, levels, live, input = false) {
        const cells = this.dmxCells[port];
        if (!cells || !cells.length) return;
        const grid = document.getElementById(`dmxGrid${port}`);
        if (grid) grid.classList.toggle('input', input);
        this.dmxInput[port] = input;
        const prev = this.dmxLast[port];
        let on = 0;
        for (let i = 0; i < 512; i++) {
            const v = levels[i] || 0;
            if (v) on++;
            if (prev && prev[i] === v) continue;
            const c = cells[i];
            c.textContent = this._dmxText(v);
            c.style.setProperty('--p', (v / 2.55).toFixed(1) + '%');
            c.classList.toggle('zero', v === 0);
        }
        this.dmxLast[port] = levels;
        this.dmxLive[port] = live;

        const badge = document.getElementById(`dmxPort${port}Live`);
        if (badge) {
            const n = `${on} channel${on === 1 ? '' : 's'}`;
            let text;
            if (input) {
                if (live) text = on ? `Receiving · ${n} on` : 'Receiving · all at zero';
                else text = on ? `No DMX signal · last levels received` : 'Input · no DMX signal';
            } else if (live) {
                text = on ? `Live · ${n} on` : 'Live · all at zero';
            } else {
                text = on ? `No source · holding ${n}` : 'No data';
            }
            badge.textContent = text;
            badge.classList.toggle('on', !!live && !input);
            badge.classList.toggle('in', !!live && input);
        }
    },

    setDmxFormat(fmt) {
        this.dmxFormat = fmt === 'pct' ? 'pct' : 'dec';
        try { localStorage.setItem('dmxFormat', this.dmxFormat); } catch (_) { /* storage blocked */ }
        this._markDmxFormat();
        [1, 2].forEach(port => {
            const last = this.dmxLast[port];
            this.dmxLast[port] = null;
            if (last) this._renderDmx(port, last, this.dmxLive[port], this.dmxInput[port]);
        });
    },

    _markDmxFormat() {
        document.querySelectorAll('[data-dmx-fmt]').forEach(b => {
            const on = b.dataset.dmxFmt === this.dmxFormat;
            b.classList.toggle('active', on);
            b.setAttribute('aria-pressed', on ? 'true' : 'false');
        });
    },

    /** Used while no WebSocket frame arrives (WebSocket blocked or reconnecting) */
    async pollDmx() {
        const res = await Promise.allSettled([1, 2].map(p =>
            this.apiRequest(`/api/ports/${p}/dmx?count=512`, { quiet: true })));
        res.forEach((r, i) => {
            if (r.status !== 'fulfilled' || !r.value) return;
            const src = Array.isArray(r.value.channels) ? r.value.channels : [];
            const levels = new Uint8Array(512);
            for (let k = 0; k < 512 && k < src.length; k++) levels[k] = src[k] || 0;
            this._renderDmx(i + 1, levels, !!r.value.active, !!r.value.input);
        });
    },

    // =========================================================================
    // Firmware Rollback
    // =========================================================================
    async firmwareRollback() {
        if (!confirm('Roll back to the previous firmware? The device will restart.')) return;
        try {
            await this.apiRequest('/api/system/firmware-rollback', { method: 'POST' });
            this.showToast('Rolling back firmware — device restarting...', 'warning');
            setTimeout(() => window.location.reload(), 12000);
        } catch (err) {
            this.showToast('Rollback failed: ' + err.message, 'error');
        }
    },

    // =========================================================================
    // RDM Identify
    // =========================================================================
    async identifyRdmDevice(port, uid, enable) {
        try {
            await this.apiRequest('/api/rdm/identify', {
                method: 'POST',
                body: JSON.stringify({ port, uid, enable })
            });
            this.showToast(`Identify ${enable ? 'ON' : 'OFF'} sent to ${uid}`, 'success');
        } catch (err) {
            this.showToast('Identify failed: ' + err.message, 'error');
        }
    },

    // =========================================================================
    // Scene Manager
    // =========================================================================
    async loadScenes() {
        try {
            const scenes = await this.apiRequest('/api/scenes');
            this.scenes = Array.isArray(scenes) ? scenes : [];
            this._fillSceneSelects();
            const tbody = document.getElementById('scenesTable');
            if (!tbody) return;
            if (!Array.isArray(scenes) || scenes.length === 0) {
                tbody.innerHTML = '<tr><td colspan="4" class="no-data">No scenes saved</td></tr>';
                return;
            }
            tbody.innerHTML = scenes.map(s => `
                <tr>
                    <td>${parseInt(s.scene_id)}</td>
                    <td>${esc(s.name || '(unnamed)')}${s.active ? ' <strong style="color:var(--success)">&#9679; playing</strong>' : ''}</td>
                    <td>Port ${parseInt(s.port)}</td>
                    <td>
                        <button class="btn btn-primary btn-sm" style="font-size:.8rem;padding:.2rem .5rem" onclick="app.playScene(${parseInt(s.scene_id)})">Play</button>
                        <button class="btn btn-danger btn-sm" style="font-size:.8rem;padding:.2rem .5rem;margin-left:.25rem" onclick="app.deleteScene(${parseInt(s.scene_id)})">Delete</button>
                    </td>
                </tr>`).join('');
        } catch (err) {
            this.showToast('Failed to load scenes: ' + err.message, 'error');
        }
    },

    async recordScene() {
        const nameInput = document.getElementById('sceneRecordName');
        const portSelect = document.getElementById('sceneRecordPort');
        const name = nameInput ? nameInput.value.trim() || 'Scene' : 'Scene';
        const port = portSelect ? parseInt(portSelect.value) : 1;
        try {
            const result = await this.apiRequest('/api/scenes/record', {
                method: 'POST',
                body: JSON.stringify({ port, name })
            });
            this.showToast(`Scene "${name}" recorded (ID: ${result.scene_id})`, 'success');
            if (nameInput) nameInput.value = '';
            this.loadScenes();
        } catch (err) {
            this.showToast('Record failed: ' + err.message, 'error');
        }
    },

    async playScene(sceneId) {
        const fadeInput = document.getElementById('sceneFadeMs');
        const fade_ms = fadeInput ? parseInt(fadeInput.value) || 0 : 0;
        try {
            await this.apiRequest('/api/scenes/playback', {
                method: 'POST',
                body: JSON.stringify({ scene_id: sceneId, fade_ms })
            });
            this.showToast(`Scene ${sceneId} playing (fade: ${fade_ms}ms)`, 'success');
            this.loadScenes();
        } catch (err) {
            this.showToast('Playback failed: ' + err.message, 'error');
        }
    },

    async releaseScenes() {
        const fadeInput = document.getElementById('sceneFadeMs');
        const fade_ms = fadeInput ? parseInt(fadeInput.value) || 0 : 0;
        try {
            await this.apiRequest('/api/scenes/release', {
                method: 'POST',
                body: JSON.stringify({ port: 0, fade_ms })
            });
            this.showToast('Scenes released — outputs follow live input again', 'success');
            this.loadScenes();
        } catch (err) {
            this.showToast('Release failed: ' + err.message, 'error');
        }
    },

    async deleteScene(sceneId) {
        if (!confirm(`Delete scene ${sceneId}?`)) return;
        try {
            await this.apiRequest(`/api/scenes/${sceneId}`, { method: 'DELETE' });
            this.showToast(`Scene ${sceneId} deleted`, 'success');
            this.loadScenes();
        } catch (err) {
            this.showToast('Delete failed: ' + err.message, 'error');
        }
    },

    // =========================================================================
    // Network: status, mode, Wi-Fi networks, Ethernet, access point
    // =========================================================================
    setupNetworkForms() {
        const ef = document.getElementById('ethForm');
        if (ef) {
            ef.querySelectorAll('input[name="eth_addr"]').forEach(r =>
                r.addEventListener('change', () => this._showEthStatic()));
        }
        // Which submit button sent the Wi-Fi form (Enter = the first one)
        const wf = document.getElementById('wifiForm');
        if (wf) {
            wf.querySelectorAll('button[type="submit"]').forEach(b =>
                b.addEventListener('click', () => { wf.dataset.connect = b.dataset.connect; }));
        }
        const list = document.getElementById('wifiList');
        if (list) {
            list.addEventListener('click', (e) => {
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
        }
        const scan = document.getElementById('wifiScanResults');
        if (scan) {
            scan.addEventListener('click', (e) => {
                const b = e.target.closest('button[data-i]');
                if (!b || b.disabled) return;
                this._pickScanned(this.scanList[parseInt(b.dataset.i)]);
            });
        }
        const banner = document.getElementById('netBanner');
        if (banner) {
            banner.addEventListener('click', (e) => {
                const b = e.target.closest('button[data-act]');
                if (!b) return;
                if (b.dataset.act === 'apoff') this.apOff();
                if (b.dataset.act === 'dismiss') { this.netWatch = null; this._renderNetBanner(); }
            });
        }
    },

    _applyNetworkConfig(net) {
        this.netCfg = net;
        const mf = document.getElementById('netModeForm');
        if (mf) {
            mf.elements.mode.value = String(net.mode ?? 0);
            mf.elements.ap_always.checked = !!net.ap_always;
        }
        const ef = document.getElementById('ethForm');
        if (ef) {
            ef.elements.eth_addr.value = net.eth_use_static_ip ? 'static' : 'dhcp';
            ef.elements.eth_static_ip.value = net.eth_static_ip || '';
            ef.elements.eth_netmask.value = net.eth_netmask || '255.255.255.0';
            ef.elements.eth_gateway.value = net.eth_gateway || '';
            this._showEthStatic();
        }
        const af = document.getElementById('apForm');
        if (af) {
            af.elements.ap_ssid.value = net.ap_ssid || '';
            af.elements.ap_password.value = '';
            af.elements.ap_channel.value = net.ap_channel || 1;
        }
        const profiles = Array.isArray(net.wifi_profiles) ? net.wifi_profiles.map(p => ({ ...p })) : [];
        profiles.sort((a, b) => (a.priority ?? 0) - (b.priority ?? 0));
        this.wifiProfiles = profiles;
        this.renderWifiList();
    },

    _showEthStatic() {
        const ef = document.getElementById('ethForm');
        const box = document.getElementById('ethStaticFields');
        if (ef && box) box.hidden = ef.elements.eth_addr.value !== 'static';
    },

    /** Replace innerHTML only when it changed (keeps clicks and focus working) */
    _setHtml(el, html) {
        if (!el || el._html === html) return;
        el._html = html;
        el.innerHTML = html;
    },

    _bars(rssi) {
        if (rssi === undefined || rssi === null) return '';
        const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
        let h = `<span class="bars" title="${rssi} dBm" aria-label="Signal ${n} of 4">`;
        for (let i = 1; i <= 4; i++) h += `<i${i <= n ? ' class="on"' : ''}></i>`;
        return h + '</span>';
    },

    async refreshNetStatus() {
        try {
            this.renderNetwork(await this.apiRequest('/api/network/status', { quiet: true }));
        } catch (_) { /* offline indicator covers it */ }
    },

    renderNetwork(ns) {
        if (!ns) return;
        this.netStatus = ns;
        const eth = ns.ethernet || {}, wifi = ns.wifi || {}, ap = ns.ap || {};
        const set = (id, t) => { const e = document.getElementById(id); if (e) e.textContent = t; };

        // Dashboard card
        const primary = {
            ethernet: 'Ethernet',
            wifi: wifi.ssid ? `Wi-Fi (${wifi.ssid})` : 'Wi-Fi',
            ap: 'Own access point',
        }[ns.primary] || 'Not connected';
        set('netMode', primary);
        set('ipAddress', ns.ip_address || '--');
        const parts = [];
        if (ns.primary === 'none') {
            parts.push(wifi.state === 'connecting' || wifi.state === 'getting_ip'
                ? `Connecting to ${wifi.ssid}…` : 'No connection');
        } else {
            parts.push('Connected');
        }
        if (ap.active && ns.primary !== 'ap') {
            parts.push(`access point on (${ap.clients} device${ap.clients === 1 ? '' : 's'})`);
        }
        set('netStatus', parts.join(' · '));

        // Network tab: one row per interface
        const iface = (id, cls, text) => {
            const el = document.getElementById(id);
            if (!el) return;
            el.classList.remove('ok', 'busy', 'bad');
            if (cls) el.classList.add(cls);
            set(id + 'Text', text);
        };
        const addr = (o) => `${o.ip} / ${o.netmask}${o.gateway && o.gateway !== '0.0.0.0' ? ` · gateway ${o.gateway}` : ''}`;
        const offMode = { ethernet: 'Ethernet only', wifi: 'Wi-Fi only', ap: 'Access point only' }[ns.net_mode];

        if (!eth.enabled) iface('ifEth', '', `Off (mode: ${offMode || 'Automatic'})`);
        else if (!eth.present) iface('ifEth', '', 'Not detected: no W5500 Ethernet module found');
        else if (eth.ready) iface('ifEth', 'ok', `${addr(eth)} · ${eth.dhcp ? 'DHCP' : 'static'}`);
        else if (eth.link) iface('ifEth', 'busy', eth.dhcp === false ? 'Cable in, starting…' : 'Cable in, waiting for an address from DHCP…');
        else iface('ifEth', '', 'No cable');

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
            iface('ifWifi', '', eth.ready && ns.net_mode === 'auto'
                ? 'Standby: Ethernet is connected' : `Off (mode: ${offMode || 'Automatic'})`);
        } else if (lastFail) {
            const next = wifi.retry_in ? ` · next try in ${wifi.retry_in} s` : '';
            iface('ifWifi', 'bad', `${lastFail.ssid}: ${lastFail.reason_text || 'failed'}${next}`);
        } else {
            iface('ifWifi', 'busy', wifi.retry_in ? `Next try in ${wifi.retry_in} s` : 'Starting…');
        }

        if (ap.active) {
            const why = {
                always: ' · kept on',
                rescue: ' · on because there is no other connection',
                linger: ` · turns off in ${ap.off_in} s`,
            }[ap.reason] || '';
            iface('ifAp', 'ok', `${ap.ssid} · ${ap.ip} · channel ${ap.channel} · ` +
                  `${ap.clients} device${ap.clients === 1 ? '' : 's'}${why}`);
        } else {
            iface('ifAp', '', ns.net_mode === 'ap' ? 'Starting…'
                : 'Off · comes on by itself when the node has no other connection');
        }

        const host = document.getElementById('netHostname');
        if (host) {
            host.textContent = ns.hostname
                ? `On Ethernet and Wi-Fi the node also answers at http://${ns.hostname}.local/ (on most computers and phones).`
                : '';
        }

        this.renderWifiList();
        this._renderNetBanner();
    },

    /** Which interface this browser reaches the node through, if known */
    _browserVia(ns) {
        const h = window.location.hostname;
        if (!ns) return null;
        if (ns.ethernet && ns.ethernet.ip && h === ns.ethernet.ip) return 'ethernet';
        if (ns.wifi && ns.wifi.ip && h === ns.wifi.ip) return 'wifi';
        if (h === '192.168.4.1' || (ns.ap && ns.ap.ip && h === ns.ap.ip)) return 'ap';
        return null;
    },

    _renderNetBanner() {
        const el = document.getElementById('netBanner');
        if (!el) return;
        const ns = this.netStatus || {};
        const ap = ns.ap || {};
        const w = this.netWatch;
        let cls = '', html = '', actions = '';

        if (w) {
            const name = `<strong>${esc(w.ssid)}</strong>`;
            if (!w.done) {
                html = w.lost
                    ? `Lost contact with the node while it joins ${name}. That is normal for a few seconds ` +
                      `when you are on its access point (it moves to the network's channel). Waiting…`
                    : w.gettingIp ? `Joined ${name}, waiting for an address…` : `Connecting to ${name}…`;
            } else if (w.done.ok) {
                cls = 'ok';
                const url = `http://${w.done.ip}/`;
                const host = ns.hostname ? `http://${ns.hostname}.local/` : '';
                html = `Connected to ${name}. Address <a href="${esc(url)}">${esc(url)}</a>` +
                       (host ? ` (or <a href="${esc(host)}">${esc(host)}</a>)` : '') + '.';
                if (w.done.standby) {
                    html += ns.net_mode === 'auto'
                        ? ' The password works. While Ethernet is connected the node uses Ethernet, so it leaves Wi-Fi again; Wi-Fi takes over when the cable is unplugged.'
                        : ' The password works, but this connection mode does not use Wi-Fi, so the node leaves it again.';
                } else if (this._browserVia(ns) === 'ap') {
                    html += ` To keep using this page, join ${name} on this device and open that address.`;
                }
            } else {
                cls = 'bad';
                html = `Could not connect to ${name}: ${esc(w.done.text)}`;
            }
            if (w.done) actions += '<button type="button" class="btn btn-sm" data-act="dismiss">Dismiss</button>';
        }
        if (ap.active && ap.reason === 'linger') {
            const who = ap.clients ? ` (${ap.clients} device${ap.clients === 1 ? ' is' : 's are'} on it)` : '';
            html += (html ? '<br><br>' : '') +
                `The access point <strong>${esc(ap.ssid)}</strong> stays on for another ${ap.off_in} s${who} ` +
                'so you can note the new address.';
            actions = '<button type="button" class="btn btn-sm" data-act="apoff">Turn access point off now</button>' + actions;
        }

        el.hidden = !html;
        el.className = 'net-banner' + (cls ? ' ' + cls : '');
        if (!el.querySelector('.net-banner-text')) {
            el.innerHTML = '<div class="net-banner-text"></div><div class="net-banner-actions"></div>';
        }
        this._setHtml(el.querySelector('.net-banner-text'), html);
        const act = el.querySelector('.net-banner-actions');
        act.hidden = !actions;
        this._setHtml(act, actions);
    },

    // ---- Connection mode ----------------------------------------------------
    async saveNetMode(event) {
        event.preventDefault();
        const f = event.target;
        const mode = parseInt(f.elements.mode.value);
        if (isNaN(mode)) { this.showToast('Choose a connection mode', 'error'); return false; }
        const apAlways = f.elements.ap_always.checked;
        const ns = this.netStatus || {};
        const via = this._browserVia(ns);
        const apName = (ns.ap && ns.ap.ssid) || (this.netCfg && this.netCfg.ap_ssid) || 'the node\'s access point';
        let warn = '';
        if (mode === 1 && via === 'wifi') warn = 'Wi-Fi switches off, and this page is open over Wi-Fi.';
        else if (mode === 2 && via === 'ethernet') warn = 'Ethernet switches off, and this page is open over Ethernet.';
        else if (mode === 3 && (via === 'ethernet' || via === 'wifi')) {
            warn = `The node leaves ${via === 'wifi' ? 'Wi-Fi' : 'Ethernet'}. To reach it again, join "${apName}" and open http://192.168.4.1/.`;
        }
        if (mode === 2 && (!this.wifiProfiles || this.wifiProfiles.length === 0)) {
            warn = (warn ? warn + '\n\n' : '') + `No Wi-Fi networks are saved yet: until you add one the node is only reachable on "${apName}".`;
        }
        if (warn && !confirm(warn + '\n\nApply anyway?')) return false;
        try {
            await this.apiRequest('/api/config', {
                method: 'POST',
                body: JSON.stringify({ network: { mode, ap_always: apAlways } })
            });
            if (this.netCfg) { this.netCfg.mode = mode; this.netCfg.ap_always = apAlways; }
            this.showToast('Connection mode applied', 'success');
            setTimeout(() => this.refreshNetStatus(), 1500);
        } catch (_) { /* toast already shown */ }
        return false;
    },

    // ---- Wi-Fi networks -----------------------------------------------------
    renderWifiList() {
        const ul = document.getElementById('wifiList');
        if (!ul) return;
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
                if (n.connected) {
                    cls = 'ok'; connected = true;
                    text = `Connected · ${wifi.ip}`;
                } else if (wifi.ssid === p.ssid && (wifi.state === 'connecting' || wifi.state === 'getting_ip')) {
                    cls = 'busy';
                    text = wifi.state === 'getting_ip' ? 'Joined, waiting for an address…' : 'Connecting…';
                } else if (n.reason) {
                    cls = 'bad';
                    text = n.reason_text || `Failed (reason ${n.reason})`;
                } else if (n.visible) {
                    text = 'In range';
                } else if (n.tried) {
                    text = 'Not seen in the last scan';
                }
                const flags = [p.has_password ? 'Password saved' : 'Open network',
                               p.use_static_ip ? `Static ${p.static_ip}` : 'DHCP'];
                return `<li class="wifi-item">
                    <div class="wifi-item-main">
                        <div class="wifi-item-name">${esc(p.ssid)}${n.visible ? this._bars(n.rssi) : ''}</div>
                        <div class="wifi-item-state ${cls}">${esc(text)}</div>
                        <div class="wifi-item-flags">${esc(flags.join(' · '))}</div>
                    </div>
                    <div class="wifi-item-actions">
                        ${connected ? '' : `<button type="button" class="btn btn-sm btn-primary" data-act="connect" data-i="${i}">Connect</button>`}
                        <button type="button" class="btn btn-sm" data-act="up" data-i="${i}" title="Try earlier" aria-label="Move up"${i === 0 ? ' disabled' : ''}>&uarr;</button>
                        <button type="button" class="btn btn-sm" data-act="down" data-i="${i}" title="Try later" aria-label="Move down"${i === list.length - 1 ? ' disabled' : ''}>&darr;</button>
                        <button type="button" class="btn btn-sm" data-act="edit" data-i="${i}">Edit</button>
                        <button type="button" class="btn btn-sm btn-danger" data-act="forget" data-i="${i}">Forget</button>
                    </div>
                </li>`;
            }).join('');
        }
        this._setHtml(ul, html);
        const add = document.getElementById('wifiAddBtn');
        if (add) add.disabled = list.length >= 5;
    },

    openWifiForm(i = -1) {
        const f = document.getElementById('wifiForm');
        if (!f) return;
        const list = this.wifiProfiles || [];
        if (i < 0 && list.length >= 5) {
            this.showToast('Up to 5 networks can be saved. Forget one first.', 'error');
            return;
        }
        const p = i >= 0 ? list[i] : null;
        this.wifiEditIndex = i;
        f.reset();
        f.elements.orig_ssid.value = p ? p.ssid : '';
        f.elements.ssid.value = p ? p.ssid : '';
        f.elements.use_static_ip.checked = !!(p && p.use_static_ip);
        f.elements.static_ip.value = (p && p.static_ip) || '';
        f.elements.netmask.value = (p && p.netmask) || '255.255.255.0';
        f.elements.gateway.value = (p && p.gateway) || '';
        const pwInput = f.elements.password;
        pwInput.type = 'password';
        const pwBtn = pwInput.parentElement.querySelector('button');
        if (pwBtn) pwBtn.textContent = 'Show';
        document.getElementById('wifiStaticFields').hidden = !f.elements.use_static_ip.checked;
        document.getElementById('wifiPwHelp').textContent = p && p.has_password
            ? 'A password is saved. Leave empty to keep it.' : 'Leave empty for an open network.';
        document.getElementById('wifiFormError').textContent = '';
        document.getElementById('wifiScanResults').hidden = true;
        f.hidden = false;
        document.getElementById('wifiAddBtn').hidden = true;
        f.elements.ssid.focus();
        if (!p) this.scanWifi();
    },

    closeWifiForm() {
        const f = document.getElementById('wifiForm');
        if (f) { f.reset(); f.hidden = true; }
        this.wifiEditIndex = -1;
        const add = document.getElementById('wifiAddBtn');
        if (add) add.hidden = false;
    },

    toggleWifiStatic(cb) {
        document.getElementById('wifiStaticFields').hidden = !cb.checked;
    },

    togglePw(btn) {
        const input = btn.parentElement.querySelector('input');
        if (!input) return;
        const show = input.type === 'password';
        input.type = show ? 'text' : 'password';
        btn.textContent = show ? 'Hide' : 'Show';
    },

    async scanWifi() {
        const box = document.getElementById('wifiScanResults');
        const btn = document.getElementById('wifiScanBtn');
        if (!box) return;
        box.hidden = false;
        box.innerHTML = '<div class="scan-note">Scanning…</div>';
        if (btn) btn.disabled = true;
        try {
            const nets = await this.apiRequest('/api/network/wifi-scan', { quiet: true });
            this.scanList = (Array.isArray(nets) ? nets : []).filter(n => n.ssid);
            this.scanList.sort((a, b) => b.rssi - a.rssi);
            if (!this.scanList.length) {
                box.innerHTML = '<div class="scan-note">No 2.4 GHz networks found. You can still type the name.</div>';
            } else {
                box.innerHTML = this.scanList.map((n, i) => {
                    const unsupported = n.enterprise ? 'Enterprise login (not supported)'
                        : n.wep ? 'WEP (not supported)' : '';
                    const tags = [unsupported || (n.secure ? '' : 'Open'), n.saved ? 'Saved' : ''].filter(Boolean).join(' · ');
                    return `<button type="button" class="scan-item" data-i="${i}"${unsupported ? ' disabled' : ''}>
                        <span class="scan-name">${esc(n.ssid)}</span>
                        <span class="scan-meta">${esc(tags)}${this._bars(n.rssi)}</span>
                    </button>`;
                }).join('');
            }
        } catch (e) {
            if (!e.silent) box.innerHTML = `<div class="scan-note">Scan failed: ${esc(e.message)}</div>`;
        } finally {
            if (btn) btn.disabled = false;
        }
    },

    _pickScanned(n) {
        const f = document.getElementById('wifiForm');
        if (!f || !n) return;
        f.elements.ssid.value = n.ssid;
        document.getElementById('wifiScanResults').hidden = true;
        const help = document.getElementById('wifiPwHelp');
        const editing = this.wifiEditIndex >= 0 ? this.wifiProfiles[this.wifiEditIndex] : null;
        if (editing && editing.ssid === n.ssid && editing.has_password) {
            help.textContent = 'A password is saved. Leave empty to keep it.';
        } else {
            help.textContent = n.secure ? 'This network needs its password.' : 'Open network: leave empty.';
        }
        if (n.secure) f.elements.password.focus();
    },

    /** Scan says the network needs a password (unknown = false) */
    _scanSecure(ssid) {
        const n = (this.scanList || []).find(x => x.ssid === ssid);
        return !!(n && n.secure);
    },

    /** Problem with a static address, '' when it is fine. Gateway optional. */
    _ipProblem(ip, mask, gw) {
        const a = ipv4(ip);
        if (a === null) return 'Enter a valid IP address, for example 10.0.0.50.';
        const first = a >>> 24;
        if (first === 0 || first === 127 || first >= 224) return `${ip.trim()} can't be used as a device address.`;
        const m = ipv4(mask);
        if (m === null || m === 0 || ((~m >>> 0) & ((~m >>> 0) + 1)) !== 0) {
            return 'Enter a valid subnet mask, for example 255.255.255.0.';
        }
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
        const err = document.getElementById('wifiFormError');
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
        } else if (!pw && this._scanSecure(ssid) && !(old && old.has_password)) {
            problem = 'This network needs a password.';
        } else if (useStatic) {
            problem = this._ipProblem(f.elements.static_ip.value, mask, f.elements.gateway.value);
        }
        err.textContent = problem;
        if (problem) return false;

        const prof = {
            ssid,
            use_static_ip: useStatic,
            static_ip: f.elements.static_ip.value.trim(),
            netmask: mask,
            gateway: f.elements.gateway.value.trim(),
            has_password: old ? old.has_password : false,
        };
        // No password sent = the node keeps the one saved for this name
        if (pw || !old || ssid !== orig) prof.password = pw;
        if (old) list[idx] = prof; else list.push(prof);

        const buttons = f.querySelectorAll('button');
        buttons.forEach(b => { b.disabled = true; });
        try {
            await this._saveProfiles(list);
        } catch (_) {
            return false;
        } finally {
            buttons.forEach(b => { b.disabled = false; });
        }
        this.closeWifiForm();
        if (connect) this.wifiConnect(ssid);
        else this.showToast(`Saved "${ssid}"`, 'success');
        return false;
    },

    /** POST the whole list; priority = position. Passwords only when entered. */
    async _saveProfiles(list) {
        const body = list.map((p, i) => {
            const o = {
                ssid: p.ssid, priority: i, use_static_ip: !!p.use_static_ip,
                static_ip: p.static_ip || '', netmask: p.netmask || '', gateway: p.gateway || ''
            };
            if (p.password !== undefined) o.password = p.password;
            return o;
        });
        await this.apiRequest('/api/config', {
            method: 'POST',
            body: JSON.stringify({ network: { wifi_profiles: body } })
        });
        list.forEach((p, i) => {
            if (p.password !== undefined) { p.has_password = p.password.length > 0; delete p.password; }
            p.priority = i;
        });
        this.wifiProfiles = list;
        this.renderWifiList();
        setTimeout(() => this.refreshNetStatus(), 800);
    },

    async wifiMove(i, d) {
        const list = (this.wifiProfiles || []).map(p => ({ ...p }));
        const j = i + d;
        if (j < 0 || j >= list.length) return;
        [list[i], list[j]] = [list[j], list[i]];
        try { await this._saveProfiles(list); } catch (_) { /* toast already shown */ }
    },

    async wifiForget(i) {
        const list = (this.wifiProfiles || []).map(p => ({ ...p }));
        const p = list[i];
        if (!p) return;
        const wifi = (this.netStatus && this.netStatus.wifi) || {};
        let msg = `Forget "${p.ssid}" and its password?`;
        if (wifi.state === 'connected' && wifi.ssid === p.ssid) {
            msg += this._browserVia(this.netStatus) === 'wifi'
                ? '\n\nThe node is on this network now and this page is open through it: you will lose the connection.'
                : '\n\nThe node is on this network now and will leave it.';
        }
        if (!confirm(msg)) return;
        list.splice(i, 1);
        try {
            await this._saveProfiles(list);
            this.showToast(`Forgot "${p.ssid}"`, 'info');
        } catch (_) { /* toast already shown */ }
    },

    async wifiConnect(ssid) {
        try {
            await this.apiRequest('/api/network/wifi/connect', {
                method: 'POST',
                body: JSON.stringify({ ssid })
            });
        } catch (_) {
            return;
        }
        this.netWatch = { ssid, t0: Date.now(), seen: false, lost: false, gettingIp: false, done: null };
        this._renderNetBanner();
        const banner = document.getElementById('netBanner');
        if (banner && banner.scrollIntoView) banner.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
        clearTimeout(this.netPollTimer);
        // The supervisor picks the command up within a tick; don't read the old state
        this.netPollTimer = setTimeout(() => this._pollConnect(), 1000);
    },

    async _pollConnect() {
        const w = this.netWatch;
        if (!w || w.done) return;
        try {
            const ns = await this.apiRequest('/api/network/status', { quiet: true });
            w.lost = false;
            this._checkConnect(w, ns);
            this.renderNetwork(ns);
        } catch (e) {
            if (e.silent) return;            // signed out / setup: handled elsewhere
            w.lost = true;
            this._renderNetBanner();
        }
        if (this.netWatch !== w || w.done) return;
        if (Date.now() - w.t0 > 45000) {
            w.done = { ok: false, text: w.lost ? 'the node did not answer for a while. Reload this page, or look for its access point.'
                                               : 'no result after 45 seconds.' };
            this._renderNetBanner();
            return;
        }
        this.netPollTimer = setTimeout(() => this._pollConnect(), 1000);
    },

    _checkConnect(w, ns) {
        const wifi = ns.wifi || {};
        const mine = wifi.ssid === w.ssid;
        if (mine && wifi.state === 'connected') {
            w.done = { ok: true, ip: wifi.ip, standby: !wifi.enabled };
            return;
        }
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
            await this.apiRequest('/api/network/ap/off', { method: 'POST' });
            this.showToast('Access point turning off', 'info');
            setTimeout(() => this.refreshNetStatus(), 1000);
        } catch (_) { /* toast already shown */ }
    },

    // ---- Ethernet, access point, node name ----------------------------------
    async saveEthernet(event) {
        event.preventDefault();
        const f = event.target;
        const isStatic = f.elements.eth_addr.value === 'static';
        const ip = f.elements.eth_static_ip.value.trim();
        const mask = f.elements.eth_netmask.value.trim() || '255.255.255.0';
        const gw = f.elements.eth_gateway.value.trim();
        const net = { eth_use_static_ip: isStatic };
        if (isStatic) {
            const problem = this._ipProblem(ip, mask, gw);
            if (problem) { this.showToast(problem, 'error'); return false; }
            Object.assign(net, { eth_static_ip: ip, eth_netmask: mask, eth_gateway: gw });
        }
        const via = this._browserVia(this.netStatus);
        try {
            await this.apiRequest('/api/config', { method: 'POST', body: JSON.stringify({ network: net }) });
            if (this.netCfg) Object.assign(this.netCfg, net);
            this.showToast(via === 'ethernet'
                ? `Ethernet settings applied. If this page stops answering, open ${isStatic ? `http://${ip}/` : 'the new DHCP address'}.`
                : 'Ethernet settings applied', 'success');
            setTimeout(() => this.refreshNetStatus(), 2000);
        } catch (_) { /* toast already shown */ }
        return false;
    },

    async saveAp(event) {
        event.preventDefault();
        const f = event.target;
        const ssid = f.elements.ap_ssid.value.trim();
        const pw = f.elements.ap_password.value;
        const ch = parseInt(f.elements.ap_channel.value);
        if (!ssid) { this.showToast('Enter a name for the access point', 'error'); return false; }
        if (new TextEncoder().encode(ssid).length > 31) { this.showToast('The access point name is at most 31 bytes long', 'error'); return false; }
        if (pw) {
            const problem = this.apPasswordProblem(pw);
            if (problem) { this.showToast(problem, 'error'); return false; }
        }
        if (!(ch >= 1 && ch <= 13)) { this.showToast('Channel must be 1–13', 'error'); return false; }
        const net = { ap_ssid: ssid, ap_channel: ch };
        if (pw) net.ap_password = pw;
        const changedLogin = pw || (this.netCfg && this.netCfg.ap_ssid !== ssid);
        if (this._browserVia(this.netStatus) === 'ap' && changedLogin &&
            !confirm('This page is open over the access point. Devices on it disconnect and must join again' +
                     (pw ? ' with the new password.' : ` as "${ssid}".`) + '\n\nApply?')) {
            return false;
        }
        try {
            await this.apiRequest('/api/config', { method: 'POST', body: JSON.stringify({ network: net }) });
            if (this.netCfg) { this.netCfg.ap_ssid = ssid; this.netCfg.ap_channel = ch; }
            f.elements.ap_password.value = '';
            this.showToast('Access point settings applied', 'success');
            setTimeout(() => this.refreshNetStatus(), 1500);
        } catch (_) { /* toast already shown */ }
        return false;
    },

    async saveIdentity(event) {
        event.preventDefault();
        const f = event.target;
        const node = {
            short_name: f.elements.short_name.value.trim().slice(0, 17),
            long_name: f.elements.long_name.value.trim().slice(0, 63)
        };
        if (f.elements.allow_ip_prog) node.allow_ip_prog = f.elements.allow_ip_prog.checked;
        if (!node.short_name) { this.showToast('Enter a short name', 'error'); return false; }
        try {
            await this.apiRequest('/api/config', { method: 'POST', body: JSON.stringify({ node_info: node }) });
            this.showToast('Node name saved', 'success');
        } catch (_) { /* toast already shown */ }
        return false;
    },

    async savePortConfig(event, port) {
        event.preventDefault();
        const formData = new FormData(event.target);
        const portKey = `port${port}`;
        const backup = (formData.get('universe_secondary') || '').trim();
        const config = {
            [portKey]: {
                mode: parseInt(formData.get('mode')),
                universe_primary: parseInt(formData.get('universe')) || 0,
                sacn_universe: parseInt(formData.get('sacn_universe')) || port,
                universe_secondary: backup === '' ? -1 : parseInt(backup),
                protocol_mode: parseInt(formData.get('protocol_mode') || '4'),
                rdm_enabled: formData.get('rdm_enabled') === 'on'
            }
        };
        const pc = config[portKey];
        const clamp = (name, lo, hi, d) => {
            const v = parseInt(formData.get(name));
            return Number.isFinite(v) ? Math.min(hi, Math.max(lo, v)) : d;
        };
        if (formData.has('out_rate_hz')) {
            pc.out_rate_hz = clamp('out_rate_hz', 1, 44, 44);
            pc.break_us = clamp('break_us', 92, 1000, 176);
            pc.mab_us = clamp('mab_us', 12, 1000, 12);
            pc.out_slots = clamp('out_slots', 24, 512, 512);
            pc.startup_scene = clamp('startup_scene', 0, 255, 0);
            pc.failsafe_scene = clamp('failsafe_scene', 0, 255, 0);
        }
        try {
            await this.apiRequest('/api/config', {
                method: 'POST',
                body: JSON.stringify(config)
            });
            this.showToast(`Port ${port} configuration applied`, 'success');
            this.loadPortsStatus();
            this.loadConfig();
        } catch (error) {
            this.showToast(`Failed to save Port ${port} configuration`, 'error');
        }
        return false;
    },

    async saveMergeMode(port, mode) {
        const config = { [`port${port}`]: { merge_mode: parseInt(mode) } };
        try {
            await this.apiRequest('/api/config', { method: 'POST', body: JSON.stringify(config) });
            this.showToast(`Port ${port} merge mode updated`, 'success');
        } catch (error) {
            this.showToast('Failed to update merge mode', 'error');
        }
    },

    async saveMergeSettings(event) {
        event.preventDefault();
        const f = new FormData(event.target);
        const config = {
            merge: {
                timeout_seconds: parseInt(f.get('timeout_seconds')) || 3,
                loss_behavior: parseInt(f.get('loss_behavior')) || 0
            },
            node_info: { sacn_priority: parseInt(f.get('sacn_priority')) || 0 }
        };
        try {
            await this.apiRequest('/api/config', { method: 'POST', body: JSON.stringify(config) });
            this.showToast('Merge settings applied', 'success');
        } catch (error) {
            this.showToast('Failed to save merge settings', 'error');
        }
        return false;
    },

    async blackoutPort(port) {
        try {
            await this.apiRequest(`/api/ports/${port}/blackout`, { method: 'POST' });
            this.showToast(`Port ${port} blackout activated`, 'warning');
        } catch (error) {
            this.showToast(`Failed to blackout Port ${port}`, 'error');
        }
    },

    async startRDMDiscovery() {
        const statusDiv = document.getElementById('rdmStatus');
        const tableBody = document.getElementById('rdmDevicesTable');
        statusDiv.textContent = 'Scanning for RDM devices...';
        statusDiv.style.color = 'var(--info)';
        try {
            const devices = await this.apiRequest('/api/rdm/discover', { method: 'POST' });
            this._renderRDMDevices(devices);
            if (Array.isArray(devices) && devices.length > 0) {
                statusDiv.textContent = `Found ${devices.length} device(s)`;
                statusDiv.style.color = 'var(--success)';
            } else {
                statusDiv.textContent = 'No RDM devices found (RDM needs a port in RDM Master mode, or DMX Output with RDM enabled)';
                statusDiv.style.color = 'var(--text-muted)';
            }
        } catch (error) {
            statusDiv.textContent = 'RDM discovery failed: ' + error.message;
            statusDiv.style.color = 'var(--danger)';
            this.showToast('RDM discovery failed: ' + error.message, 'error');
        }
    },

    /** Devices found by the last discovery, without starting a new one */
    async loadRDMDevices() {
        try {
            const devices = await this.apiRequest('/api/rdm/devices');
            if (Array.isArray(devices) && devices.length > 0) this._renderRDMDevices(devices);
        } catch (_) { /* table keeps its placeholder */ }
    },

    _renderRDMDevices(devices) {
        const tableBody = document.getElementById('rdmDevicesTable');
        if (!tableBody) return;
        if (!Array.isArray(devices) || devices.length === 0) {
            tableBody.innerHTML = '<tr><td colspan="7" class="no-data">No devices discovered</td></tr>';
            return;
        }
        const btn = 'class="btn btn-secondary" style="font-size:.75rem;padding:.2rem .5rem;margin-left:.2rem"';
        tableBody.innerHTML = devices.map(d => {
            const port = parseInt(d.port);
            const uid = esc(d.uid);
            const model = d.model_description ? esc(d.model_description)
                                              : `0x${(parseInt(d.model) || 0).toString(16).padStart(4, '0')}`;
            const footprint = d.footprint ? ` <small>(${parseInt(d.footprint)} ch)</small>` : '';
            return `
                <tr>
                    <td>${port}</td>
                    <td><code>${uid}</code></td>
                    <td>${esc(d.label)}</td>
                    <td>
                        <input type="number" min="1" max="512" value="${parseInt(d.dmx_address) || 1}"
                            style="width:4.5rem" data-rdm-addr="${port}-${uid}">${footprint}
                        <button ${btn} onclick="app.setRdmAddress(${port},'${uid}')">Set</button>
                    </td>
                    <td>${esc(d.manufacturer)}</td>
                    <td>${model}</td>
                    <td>
                        <button ${btn} onclick="app.identifyRdmDevice(${port},'${uid}',true)">Identify ON</button>
                        <button ${btn} onclick="app.identifyRdmDevice(${port},'${uid}',false)">OFF</button>
                    </td>
                </tr>`;
        }).join('');
    },

    async setRdmAddress(port, uid) {
        const input = document.querySelector(`[data-rdm-addr="${port}-${uid}"]`);
        const address = input ? parseInt(input.value) : NaN;
        if (!(address >= 1 && address <= 512)) {
            this.showToast('DMX address must be 1-512', 'error');
            return;
        }
        try {
            await this.apiRequest('/api/rdm/address', {
                method: 'POST',
                body: JSON.stringify({ port, uid, address })
            });
            this.showToast(`${uid} set to DMX address ${address}`, 'success');
        } catch (err) {
            this.showToast('Set address failed: ' + err.message, 'error');
        }
    },

    async rebootDevice() {
        if (!confirm('Are you sure you want to reboot the device?')) return;
        try {
            await this.apiRequest('/api/system/restart', { method: 'POST' });
            this.showToast('Device is rebooting...', 'info');
            let countdown = 10;
            const timer = setInterval(() => {
                countdown--;
                if (countdown <= 0) { clearInterval(timer); window.location.reload(); }
                else this.showToast(`Reconnecting in ${countdown} seconds...`, 'info');
            }, 1000);
        } catch (error) {
            this.showToast('Failed to reboot device', 'error');
        }
    },

    async factoryReset() {
        if (!confirm('Factory reset? All settings will be lost!')) return;
        if (!confirm('This action cannot be undone. Continue?')) return;
        try {
            await this.apiRequest('/api/system/factory-reset', { method: 'POST' });
            this.showToast('Factory reset done — device restarting', 'warning');
            setTimeout(() => window.location.reload(), 10000);
        } catch (error) {
            this.showToast('Factory reset failed: ' + error.message, 'error');
        }
    },

    async uploadFirmware(event) {
        event.preventDefault();
        const fileInput = document.getElementById('firmwareFile');
        const file = fileInput.files[0];
        if (!file) { this.showToast('Please select a firmware file', 'error'); return false; }
        if (!file.name.endsWith('.bin')) { this.showToast('Invalid file type. Please select a .bin file', 'error'); return false; }

        const progress = document.getElementById('uploadProgress');
        const progressBar = document.getElementById('uploadProgressBar');
        const progressText = document.getElementById('uploadProgressText');
        progress.style.display = 'block';
        progressBar.style.width = '0%';
        progressText.textContent = 'Uploading...';

        try {
            // Use XMLHttpRequest for upload progress tracking
            await new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.open('POST', '/api/system/firmware-upload');
                xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');

                xhr.upload.onprogress = (e) => {
                    if (e.lengthComputable) {
                        const pct = Math.round((e.loaded / e.total) * 100);
                        progressBar.style.width = pct + '%';
                        progressText.textContent = pct + '%';
                    }
                };

                xhr.onload = () => {
                    if (xhr.status === 200) {
                        resolve(JSON.parse(xhr.responseText));
                    } else {
                        try {
                            const b = JSON.parse(xhr.responseText);
                            reject(new Error(b.message || b.error || 'Upload failed'));
                        } catch (_) {
                            reject(new Error('Upload failed: ' + xhr.status));
                        }
                    }
                };
                xhr.onerror = () => reject(new Error('Network error during upload'));
                xhr.send(file);
            });

            progressBar.style.width = '100%';
            progressText.textContent = '100%';
            this.showToast('Firmware uploaded — device is rebooting', 'success');

            let countdown = 15;
            const timer = setInterval(() => {
                countdown--;
                if (countdown <= 0) { clearInterval(timer); window.location.reload(); }
                else this.showToast(`Reconnecting in ${countdown}s...`, 'info');
            }, 1000);
        } catch (error) {
            this.showToast('Firmware upload failed: ' + error.message, 'error');
            progress.style.display = 'none';
        }
        return false;
    },

    // =========================================================================
    // Port settings: output timing and scenes (added to both port forms)
    // =========================================================================
    setupPortExtras() {
        for (const n of [1, 2]) {
            const f = document.getElementById(`port${n}Form`);
            if (!f || f.querySelector('[data-out-only]')) continue;
            const html = `
                <div class="form-section" data-out-only>
                    <h3>Output timing</h3>
                    <div class="field-row">
                        <div class="form-group"><label>Refresh (Hz)</label>
                            <input type="number" name="out_rate_hz" min="1" max="44" value="44"></div>
                        <div class="form-group"><label>Break (&micro;s)</label>
                            <input type="number" name="break_us" min="92" max="1000" value="176"></div>
                        <div class="form-group"><label>MAB (&micro;s)</label>
                            <input type="number" name="mab_us" min="12" max="1000" value="12"></div>
                        <div class="form-group"><label>Slots</label>
                            <input type="number" name="out_slots" min="24" max="512" value="512"></div>
                    </div>
                    <small class="help-text">The defaults (44 Hz, 176 &micro;s, 12 &micro;s, 512 slots) suit almost every fixture. Lower the refresh or lengthen break / MAB for old or fussy fixtures. With RDM on, break stays within 176-352 &micro;s and MAB within 12-88 &micro;s.</small>
                </div>
                <div class="form-section" data-out-only>
                    <h3>Scenes</h3>
                    <div class="form-group"><label>Power-on scene</label>
                        <select name="startup_scene" data-scene-select><option value="0">None</option></select>
                        <small class="help-text">Shown from power-on until a console sends data to this port, then it fades out.</small></div>
                    <div class="form-group"><label>Failsafe scene</label>
                        <select name="failsafe_scene" data-scene-select><option value="0">None (zero)</option></select>
                        <small class="help-text">Shown when every source is lost and "When All Sources Are Lost" is set to Failsafe scene.</small></div>
                </div>`;
            const actions = f.querySelector('.form-actions');
            actions.insertAdjacentHTML('beforebegin', html);
            f.mode.addEventListener('change', () => this._showPortExtras(f));
            f.querySelectorAll('select[data-scene-select]').forEach(sel =>
                sel.addEventListener('change', () => { sel.dataset.want = sel.value; }));
            this._showPortExtras(f);
        }
        this.testModeChanged();
    },

    _showPortExtras(f) {
        const m = parseInt(f.mode.value);
        const out = m === 1 || m === 3;
        f.querySelectorAll('[data-out-only]').forEach(el => { el.hidden = !out; });
    },

    async loadSceneOptions() {
        try {
            const scenes = await this.apiRequest('/api/scenes', { quiet: true });
            this.scenes = Array.isArray(scenes) ? scenes : [];
            this._fillSceneSelects();
        } catch (_) { /* not important */ }
    },

    _fillSceneSelects() {
        document.querySelectorAll('select[data-scene-select]').forEach(sel => {
            const want = sel.dataset.want ?? sel.value ?? '0';
            const first = sel.options[0] ? sel.options[0].outerHTML : '<option value="0">None</option>';
            const ids = new Set(this.scenes.map(s => String(parseInt(s.scene_id))));
            let html = first + this.scenes.map(s =>
                `<option value="${parseInt(s.scene_id)}">${parseInt(s.scene_id)}: ${esc(s.name || '(unnamed)')}</option>`).join('');
            if (want !== '0' && !ids.has(want)) {
                html += `<option value="${esc(want)}">${esc(want)}: (deleted)</option>`;
            }
            sel.innerHTML = html;
            sel.value = want;
        });
    },

    // =========================================================================
    // Input analyser (dashboard)
    // =========================================================================
    _renderAnalysis(port, p) {
        const box = document.getElementById(`port${port}Analysis`);
        if (!box) return;
        const a = p.analysis;
        if (!a) {
            if (!box.hidden) { box.hidden = true; box.innerHTML = ''; this.analysisHtml[port] = ''; }
            return;
        }
        const row = (label, value) =>
            `<div class="stat-row"><span class="stat-label">${label}</span><span class="stat-value">${value}</span></div>`;
        const rows = [];
        const gap = a.interval_max_ms ? ` <small>(gap ${a.interval_min_ms}-${a.interval_max_ms} ms)</small>` : '';
        rows.push(row('Refresh:', `${a.refresh_hz} Hz${gap}`));
        rows.push(row('Slots:', a.slots_max ? (a.slots_min === a.slots_max ? `${a.slots}`
            : `${a.slots} <small>(${a.slots_min}-${a.slots_max})</small>`) : '--'));
        const errs = [];
        if (a.err_framing) errs.push(`framing ${a.err_framing}`);
        if (a.err_overflow) errs.push(`overflow ${a.err_overflow}`);
        if (a.err_short) errs.push(`short ${a.err_short}`);
        rows.push(row('Errors:', errs.length ? `<span style="color:var(--warning)">${errs.join(' · ')}</span>` : 'none'));
        const other = [];
        if (a.rdm_packets) other.push(`RDM ${a.rdm_packets}`);
        if (a.alt_start_codes) {
            other.push(`start code 0x${a.last_alt_start_code.toString(16).toUpperCase().padStart(2, '0')} ×${a.alt_start_codes}`);
        }
        if (other.length) rows.push(row('Other packets:', other.join(' · ')));
        let timing;
        if (a.timing_on) {
            timing = (a.break_us ? `${a.break_us} / ${a.mab_us} &micro;s` : 'waiting&hellip;') +
                ` <small>(${a.timing_left_s} s left)</small>`;
        } else {
            timing = a.break_us ? `${a.break_us} / ${a.mab_us} &micro;s <small>(last measured)</small>` : '--';
        }
        rows.push(row('Break / MAB:', timing));
        const btn = a.timing_on
            ? `<button class="btn btn-sm" onclick="app.measureTiming(${port}, 0)">Stop measuring</button>`
            : `<button class="btn btn-sm" onclick="app.measureTiming(${port}, 60)">Measure break / MAB</button>`;
        const html = `<h3>Input analyser</h3>${rows.join('')}${btn}
            <button class="btn btn-sm" onclick="app.resetAnalysis(${port})">Reset counters</button>`;
        if (html !== this.analysisHtml[port]) {
            box.innerHTML = html;
            this.analysisHtml[port] = html;
        }
        box.hidden = false;
    },

    async measureTiming(port, seconds) {
        try {
            await this.apiRequest('/api/tools/analyzer', {
                method: 'POST', body: JSON.stringify({ port, action: 'timing', seconds })
            });
            if (seconds) this.showToast(`Measuring break / MAB on port ${port} for ${seconds} s`, 'info');
            this.loadPortsStatus();
        } catch (_) { /* toast shown */ }
    },

    async resetAnalysis(port) {
        try {
            await this.apiRequest('/api/tools/analyzer', {
                method: 'POST', body: JSON.stringify({ port, action: 'reset' })
            });
            this.loadPortsStatus();
        } catch (_) { /* toast shown */ }
    },

    // =========================================================================
    // DMX tools: test patterns and loopback self-test
    // =========================================================================
    testModeChanged() {
        const f = document.getElementById('testForm');
        if (!f) return;
        const m = f.mode.value;
        const lvl = f.querySelector('[data-test="level"]');
        const spd = f.querySelector('[data-test="speed"]');
        if (lvl) lvl.hidden = !(m === 'static' || m === 'chase');
        if (spd) spd.hidden = !(m === 'chase' || m === 'ramp');
    },

    async loadTests() {
        try {
            this._renderTests(await this.apiRequest('/api/tools/test', { quiet: true }));
        } catch (_) { /* ignore */ }
        try {
            const r = await this.apiRequest('/api/tools/selftest', { quiet: true });
            if (r.state !== 'idle') this._renderSelfTest(r);
            if (r.state === 'running') this._pollSelfTest();
        } catch (_) { /* ignore */ }
    },

    _renderTests(list) {
        const el = document.getElementById('testStatus');
        const f = document.getElementById('testForm');
        if (!el || !Array.isArray(list)) return;
        if (f) {
            for (const opt of f.port.options) {
                const t = list.find(x => x.port === parseInt(opt.value));
                opt.textContent = `Port ${opt.value}` + (t && !t.output ? ' (not an output)' : '');
            }
        }
        const running = list.filter(t => t.running);
        if (!running.length) { el.hidden = true; el.innerHTML = ''; return; }
        el.hidden = false;
        el.innerHTML = running.map(t => {
            const what = t.mode === 'full' ? 'full' : t.mode === 'ramp' ? `ramp ${t.speed_ms} ms`
                : t.mode === 'chase' ? `chase at ${t.level}, ${t.speed_ms} ms/step` : `level ${t.level}`;
            return `<div><span class="tag test">TEST</span> Port ${t.port}: ${esc(what)}, channels ${t.first}-${t.last}
                <button class="btn btn-sm" style="margin-left:.5rem" onclick="app.stopTest(${t.port})">Stop</button></div>`;
        }).join('');
    },

    async startTest(event) {
        event.preventDefault();
        const f = event.target;
        const first = Math.min(512, Math.max(1, parseInt(f.first.value) || 1));
        const last = Math.min(512, Math.max(first, parseInt(f.last.value) || 512));
        const body = {
            port: parseInt(f.port.value),
            mode: f.mode.value,
            first, last,
            level: Math.min(255, Math.max(0, parseInt(f.level.value) || 0)),
            speed_ms: Math.min(60000, Math.max(20, parseInt(f.speed_ms.value) || 500))
        };
        try {
            this._renderTests(await this.apiRequest('/api/tools/test', { method: 'POST', body: JSON.stringify(body) }));
            this.showToast(`Test running on port ${body.port}`, 'warning');
            this.loadPortsStatus();
        } catch (_) { /* toast shown */ }
        return false;
    },

    async stopTest(port) {
        const ports = port ? [port] : [1, 2];
        let list = null;
        for (const p of ports) {
            try {
                list = await this.apiRequest('/api/tools/test', {
                    method: 'POST', body: JSON.stringify({ port: p, mode: 'off' })
                });
            } catch (_) { /* toast shown */ }
        }
        if (list) this._renderTests(list);
        this.loadPortsStatus();
    },

    async runSelfTest() {
        const out = parseInt(document.getElementById('stOut').value);
        const inp = parseInt(document.getElementById('stIn').value);
        if (out === inp) { this.showToast('Pick two different ports', 'error'); return; }
        if (!confirm(`Port ${out} will send test data and port ${inp} will listen for about 6 seconds. ` +
                     'Normal output on both ports stops meanwhile. Run the self-test?')) return;
        try {
            this._renderSelfTest(await this.apiRequest('/api/tools/selftest', {
                method: 'POST', body: JSON.stringify({ out_port: out, in_port: inp })
            }));
            this._pollSelfTest();
        } catch (_) { /* toast shown */ }
    },

    _pollSelfTest() {
        clearTimeout(this.stTimer);
        this.stTimer = setTimeout(async () => {
            try {
                const r = await this.apiRequest('/api/tools/selftest', { quiet: true });
                this._renderSelfTest(r);
                if (r.state === 'running') { this._pollSelfTest(); return; }
                this.loadPortsStatus();
            } catch (_) { this._pollSelfTest(); }
        }, 600);
    },

    _renderSelfTest(r) {
        const box = document.getElementById('stResult');
        const btn = document.getElementById('stRun');
        if (!box) return;
        if (btn) btn.disabled = r.state === 'running';
        const cls = r.state === 'passed' ? 'alert-ok' : r.state === 'failed' ? 'alert-danger' : 'alert-info';
        const steps = (r.steps || []).map(s => {
            let res = '<span class="help-text">&hellip;</span>';
            if (s.done) {
                res = s.passed ? '<span class="tag ok">PASS</span>'
                    : `<span class="tag bad">FAIL</span> <small>${s.slots} slots` +
                      (s.first_bad ? `, ${s.mismatches} wrong, first ch ${s.first_bad}: sent ${s.sent} got ${s.got}` : '') + '</small>';
            }
            return `<li><span>${esc(s.name)}</span><span>${res}</span></li>`;
        }).join('');
        const extra = r.state === 'running' ? '' :
            `<div class="help-text" style="margin-top:.5rem">Measured on port ${r.in_port}: ${r.refresh_hz} frames/s` +
            (r.break_us ? `, break ${r.break_us} &micro;s, MAB ${r.mab_us} &micro;s` : '') +
            `, ${r.errors} line error${r.errors === 1 ? '' : 's'}</div>`;
        box.innerHTML = `<div class="alert ${cls}">${esc(r.message || '')}</div><ul class="st-steps">${steps}</ul>${extra}`;
    },

    // =========================================================================
    // Diagnostics: log, crash report
    // =========================================================================
    startLog() {
        if (this.log.timer) return;
        this._pollLog();
        this.log.timer = setInterval(() => this._pollLog(), 2000);
    },

    stopLog() {
        clearInterval(this.log.timer);
        this.log.timer = null;
    },

    toggleLogPause() {
        this.log.paused = !this.log.paused;
        const b = document.getElementById('logPauseBtn');
        if (b) b.textContent = this.log.paused ? 'Resume' : 'Pause';
        if (!this.log.paused) this._pollLog();
    },

    clearLogView() {
        const v = document.getElementById('logView');
        if (v) v.textContent = '';
        this.log.lines = 0;
    },

    async _pollLog() {
        if (this.log.paused || this.log.busy) return;
        this.log.busy = true;
        try {
            for (let i = 0; i < 4; i++) {   // catch up in a few requests at most
                const r = await this.apiRequest(`/api/diag/log?pos=${this.log.pos}`, { quiet: true });
                if (r.pos < this.log.pos) {   // node restarted: start over
                    this.clearLogView();
                    this.log.partial = '';
                }
                if (r.truncated) this._appendLog('--- older lines were dropped ---\n');
                this._appendLog(r.text || '');
                const grew = r.pos !== this.log.pos;
                this.log.pos = r.pos;
                const info = document.getElementById('logInfo');
                if (info) info.textContent = `${Math.round((r.capacity || 0) / 1024)} KB buffer`;
                if (!grew || (r.text || '').length < 4000) break;
            }
        } catch (_) { /* offline: try again later */ }
        this.log.busy = false;
    },

    _appendLog(text) {
        const v = document.getElementById('logView');
        if (!v || !text) return;
        const all = this.log.partial + text;
        const parts = all.split('\n');
        this.log.partial = parts.pop();
        if (!parts.length) return;
        const follow = document.getElementById('logFollow');
        const atEnd = v.scrollTop + v.clientHeight >= v.scrollHeight - 30;
        const frag = document.createDocumentFragment();
        for (const line of parts) {
            const d = document.createElement('div');
            d.textContent = line;
            const c = line.charAt(0);
            if (line.charAt(1) === ' ' && line.charAt(2) === '(') {
                d.className = c === 'E' ? 'le' : c === 'W' ? 'lw' : c === 'I' ? 'li' : '';
            }
            frag.appendChild(d);
        }
        v.appendChild(frag);
        this.log.lines += parts.length;
        while (this.log.lines > 3000 && v.firstChild) {
            v.removeChild(v.firstChild);
            this.log.lines--;
        }
        if ((follow && follow.checked) || atEnd) v.scrollTop = v.scrollHeight;
    },

    downloadLog() {
        this._download('/api/diag/log.txt', 'esp-node-log.txt');
    },

    async loadCrash() {
        const body = document.getElementById('crashBody');
        try {
            const r = await this.apiRequest('/api/diag/crash', { quiet: true });
            const reset = document.getElementById('crashReset');
            if (reset) {
                reset.innerHTML = r.reset_was_crash
                    ? `<span class="tag bad">${esc(r.reset_reason)}</span>` : esc(r.reset_reason);
            }
            if (!body) return;
            if (!r.present) {
                body.innerHTML = '<p class="help-text" style="margin-top:.5rem">No crash report stored.</p>';
                return;
            }
            const bt = (r.backtrace || []).join(' ');
            body.innerHTML = `
                <div class="stat-row"><span class="stat-label">Task:</span><span class="stat-value">${esc(r.task || '?')}</span></div>
                <div class="stat-row"><span class="stat-label">PC:</span><span class="stat-value mono">${esc(r.pc)}</span></div>
                ${r.reason ? `<div class="stat-row"><span class="stat-label">Reason:</span><span class="stat-value">${esc(r.reason)}</span></div>` : ''}
                <div class="form-group" style="margin-top:.5rem"><label>Backtrace${r.backtrace_corrupted ? ' (incomplete)' : ''}</label>
                    <div class="mono" style="word-break:break-all">${esc(bt) || '--'}</div></div>
                <small class="help-text">${r.same_firmware ? 'Recorded by the firmware running now.'
                    : 'Recorded by different firmware than is running now: decode it with that build.'}
                    Decode on the build computer with <span class="mono">idf.py coredump-info -c esp-node-coredump.bin</span>.</small>
                <div class="form-actions" style="margin-top:.75rem">
                    <button type="button" class="btn btn-primary btn-sm" onclick="app.downloadCrash()">Download core dump (${this.formatBytes(r.size)})</button>
                    <button type="button" class="btn btn-danger btn-sm" onclick="app.clearCrash()">Delete</button>
                </div>`;
        } catch (_) {
            if (body) body.innerHTML = '<p class="help-text">Could not read the crash report.</p>';
        }
    },

    downloadCrash() {
        this._download('/api/diag/crash.bin', 'esp-node-coredump.bin');
    },

    async clearCrash() {
        if (!confirm('Delete the stored crash report?')) return;
        try {
            await this.apiRequest('/api/diag/crash/clear', { method: 'POST' });
            this.showToast('Crash report deleted', 'success');
            this.loadCrash();
        } catch (_) { /* toast shown */ }
    },

    /** Download through fetch so the sign-in travels with it */
    async _download(url, fallbackName) {
        try {
            const r = await fetch(url, { headers: { 'X-Requested-With': 'XMLHttpRequest' } });
            if (r.status === 401) { this.handleUnauthorized(); return; }
            if (!r.ok) {
                const b = await r.json().catch(() => ({}));
                throw new Error(b.message || `HTTP ${r.status}`);
            }
            const m = /filename="([^"]+)"/.exec(r.headers.get('Content-Disposition') || '');
            this._saveBlob(await r.blob(), m ? m[1] : fallbackName);
        } catch (e) {
            this.showToast(`Download failed: ${e.message}`, 'error');
        }
    },

    _saveBlob(blob, name) {
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = name;
        document.body.appendChild(a);
        a.click();
        setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
    },

    // =========================================================================
    // Backup / restore (passwords are never part of it)
    // =========================================================================
    async downloadBackup() {
        try {
            const cfg = await this.apiRequest('/api/config');
            const fw = document.getElementById('sysFirmware');
            const name = (cfg.node_info && cfg.node_info.short_name) || 'esp-node';
            cfg._backup = {
                format: 1,
                created: new Date().toISOString(),
                node: name,
                firmware: fw ? fw.textContent : ''
            };
            const day = new Date().toISOString().slice(0, 10);
            const safe = name.replace(/[^A-Za-z0-9_-]+/g, '-');
            this._saveBlob(new Blob([JSON.stringify(cfg, null, 2)], { type: 'application/json' }),
                           `${safe}-settings-${day}.json`);
        } catch (_) { /* toast shown */ }
    },

    async restoreBackup(event) {
        event.preventDefault();
        const f = event.target;
        const file = f.file.files[0];
        if (!file) { this.showToast('Choose a backup file first', 'error'); return false; }
        let cfg;
        try {
            cfg = JSON.parse(await file.text());
        } catch (_) {
            this.showToast('That file is not a settings backup (not JSON)', 'error');
            return false;
        }
        if (!cfg || typeof cfg !== 'object' || !(cfg.port1 || cfg.node_info || cfg.merge)) {
            this.showToast('That file is not a settings backup', 'error');
            return false;
        }
        const meta = cfg._backup || {};
        delete cfg._backup;
        const withNet = f.network.checked;
        if (!withNet) delete cfg.network;
        if (cfg.network) {
            delete cfg.network.ap_password;   // never in a backup; keep the node's own
            (cfg.network.wifi_profiles || []).forEach(p => { delete p.password; delete p.has_password; });
        }
        const from = meta.node ? ` from "${meta.node}"${meta.created ? ` (${meta.created.slice(0, 10)})` : ''}` : '';
        if (!confirm(`Restore the settings${from}? Current settings are replaced` +
                     (withNet ? ' and the network may reconnect at a different address.' : ' (network settings are kept).'))) {
            return false;
        }
        try {
            await this.apiRequest('/api/config', { method: 'POST', body: JSON.stringify(cfg) });
            this.showToast('Settings restored', 'success');
            f.reset();
            this.loadConfig();
            this.loadPortsStatus();
        } catch (_) { /* toast shown */ }
        return false;
    },

    // =========================================================================
    // Identify (flash the status LED)
    // =========================================================================
    async identify() {
        const on = this.identifyLeft > 0;
        try {
            const r = await this.apiRequest('/api/system/identify', {
                method: 'POST', body: JSON.stringify({ seconds: on ? 0 : 30 })
            });
            this._showIdentify(r.identify_left || 0);
        } catch (_) { /* toast shown */ }
    },

    _showIdentify(left) {
        this.identifyLeft = left;
        const b = document.getElementById('identifyBtn');
        const label = b ? b.querySelector('span') : null;
        if (label) label.textContent = left > 0 ? `Stop identify (${left} s)` : 'Identify (flash LED)';
        clearTimeout(this.identifyTimer);
        if (left > 0) this.identifyTimer = setTimeout(() => this._showIdentify(left - 1), 1000);
    },

    showToast(message, type = 'info') {
        const container = document.getElementById('toastContainer');
        const toast = document.createElement('div');
        toast.className = `toast ${type}`;
        const header = document.createElement('div');
        header.className = 'toast-header';
        header.textContent = type.charAt(0).toUpperCase() + type.slice(1);
        const body = document.createElement('div');
        body.className = 'toast-body';
        body.textContent = message;
        toast.appendChild(header);
        toast.appendChild(body);
        container.appendChild(toast);
        setTimeout(() => {
            toast.style.opacity = '0';
            setTimeout(() => { if (toast.parentNode) container.removeChild(toast); }, 300);
        }, 5000);
    },

    formatBytes(bytes) {
        if (!bytes) return '--';
        const sizes = ['B', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(1024));
        return Math.round(bytes / Math.pow(1024, i) * 100) / 100 + ' ' + sizes[i];
    },

    formatUptime(seconds) {
        if (!seconds) return '--';
        const days = Math.floor(seconds / 86400);
        const hours = Math.floor((seconds % 86400) / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        const secs = seconds % 60;
        if (days > 0) return `${days}d ${hours}h ${minutes}m`;
        if (hours > 0) return `${hours}h ${minutes}m ${secs}s`;
        if (minutes > 0) return `${minutes}m ${secs}s`;
        return `${secs}s`;
    },

    getModeName(mode) {
        const modes = { 0: 'Disabled', 1: 'DMX Output', 2: 'DMX Input', 3: 'RDM Master', 4: 'RDM Responder' };
        return modes[mode] || 'Unknown';
    }
};

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => app.init());
} else {
    app.init();
}

window.app = app;
