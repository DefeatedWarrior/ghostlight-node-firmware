#!/usr/bin/env python3
"""
Mock of the node's web API, for working on the web UI without hardware.

Serves components/web_server/web_ui/ (the plain, not-gzipped files) and fakes
every /api/ endpoint plus the /ws level stream, with simulated DMX: a console
on the input / receive ports (with one flickering channel and a signal loss
every 45 s), Art-Net sources on the outputs, RDM fixtures and a log.

    python3 tools/ui_mock.py                 # node mode, http://localhost:8090/
    python3 tools/ui_mock.py --tester        # start in DMX tester mode
    python3 tools/ui_mock.py --no-ethernet --ports 4 --off rdm,scenes
    python3 tools/ui_mock.py --setup         # first-run setup screen
    python3 tools/ui_mock.py --no-login      # sign-in turned off

Switching modes, restarting and factory reset take the mock "down" for a
few seconds like the real node. No sign-in is asked for. Standard library only.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import random
import select
import socket
import struct
import threading
import time
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'components', 'web_server', 'web_ui')
HIST_EDGES_MS = [20, 24, 28, 35, 50, 100, 250, 1000]
ROLES = ['off', 'send', 'receive', 'rdm']
FEATURES = ['ethernet', 'sacn', 'rdm', 'scenes', 'dmx_tools', 'web_log', 'mdns', 'tester']

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--port', type=int, default=8090)
ap.add_argument('--ports', type=int, default=2, help='DMX ports (1-4)')
ap.add_argument('--tester', action='store_true', help='start in DMX tester mode')
ap.add_argument('--no-ethernet', action='store_true', help='firmware built without Ethernet')
ap.add_argument('--off', default='', help='comma-separated features to leave out: ' + ','.join(FEATURES))
ap.add_argument('--setup', action='store_true', help='show the first-run setup')
ap.add_argument('--notice', default='', help='settings notice to show (as after a damaged settings file)')
ap.add_argument('--safe-mode', action='store_true', help='start in safe mode (boot-loop guard: DMX ports off)')
ap.add_argument('--no-login', action='store_true', help='sign-in turned off (setup done without a password)')
ap.add_argument('--signal', default='good', choices=['good', 'offbyone', 'nothing', 'filtered', 'lossy', 'multicast', 'clash'],
                help='what the signal check hears (default: good)')
ap.add_argument('--root', default=ROOT, help='folder with index.html and main.js (default: the firmware\'s web_ui)')
args = ap.parse_args()
ROOT = args.root

NP = max(1, min(4, args.ports))
FEAT = {f: True for f in FEATURES}
for f in filter(None, args.off.split(',')):
    FEAT[f.strip()] = False
if args.no_ethernet:
    FEAT['ethernet'] = False

lock = threading.RLock()
BOOT = time.time()


def default_port(n):
    return {
        'mode': 1 if n % 2 == 1 else 2, 'universe_primary': n - 1, 'universe_secondary': -1,
        'sacn_universe': n, 'protocol_mode': 4, 'merge_mode': 0, 'rdm_enabled': True,
        'out_rate_hz': 44, 'break_us': 176, 'mab_us': 12, 'out_slots': 512,
        'startup_scene': 0, 'failsafe_scene': 0,
        'rules': [{'type': 'park', 'channel': 512, 'count': 1, 'level': 255}] if n == 1 else [],
        'local_inputs': [], 'local_only': False,
        'loss_behavior': -1, 'delta_output': False,
        'send_to': 'all', 'send_list': [],
    }


def default_config():
    c = {
        'network': {
            'mode': 0, 'ap_always': False, 'ap_ssid': 'ESP-NODE-1A2B', 'ap_open': False, 'ap_channel': 6, 'ap_captive': True,
            'wifi_profiles': [
                {'ssid': 'Theatre-Tech', 'priority': 0, 'use_static_ip': False, 'static_ip': '', 'netmask': '255.255.255.0', 'gateway': '', 'has_password': True},
                {'ssid': 'Home', 'priority': 1, 'use_static_ip': True, 'static_ip': '192.168.1.60', 'netmask': '255.255.255.0', 'gateway': '192.168.1.1', 'has_password': True},
            ],
        },
        'merge': {'timeout_seconds': 3, 'loss_behavior': 0},
        'node_info': {'short_name': 'Stage Left', 'long_name': 'Stage left DMX node', 'sacn_priority': 100, 'allow_ip_prog': True},
        'tester': {'enabled': args.tester, 'ports': [
            {'role': 'send' if n % 2 == 1 else 'receive', 'rate_hz': 44, 'break_us': 176, 'mab_us': 12, 'slots': 512}
            for n in range(1, NP + 1)]},
    }
    if FEAT['ethernet']:
        c['network'].update({'eth_use_static_ip': False, 'eth_static_ip': '', 'eth_netmask': '255.255.255.0', 'eth_gateway': ''})
    for n in range(1, NP + 1):
        c[f'port{n}'] = default_port(n)
    return c


def new_rx():
    return {'frames': 0, 't0': time.time(), 'losses': 0, 'last_loss': None, 'ivs': [], 'min': 0, 'max': 0,
            'hist': [0] * 9, 'cmin': [255] * 512, 'cmax': [0] * 512, 'changes': [0] * 512, 'last': None,
            'recent': {}, 'last_frame': None}


def new_an():
    return {'frames': 0, 'err_framing': 0, 'err_overflow': 0, 'err_short': 0, 'alt': 0, 'last_alt': 0,
            'rdm': 0, 'timing_until': 0, 'break_us': 0, 'mab_us': 0, 'slots_min': 0, 'slots_max': 0}


S = {
    'config': default_config(),
    'tester': args.tester,          # mode running now (read at "boot")
    'setup': args.setup,
    'login': not args.no_login,     # the web UI asks for a password
    'down_until': 0,
    'levels': [bytearray(512) for _ in range(NP)],       # what each port outputs / receives
    'tester_levels': [bytearray(512) for _ in range(NP)],
    'tests': {},                     # port -> test dict
    'rx': [new_rx() for _ in range(NP)],
    'an': [new_an() for _ in range(NP)],
    'signal': [False] * NP,
    'frames_sent': [0] * NP,
    'scenes': [{'scene_id': 1, 'name': 'Preset warm', 'port': 1, 'active': False},
               {'scene_id': 2, 'name': 'Work lights', 'port': 1, 'active': False}],
    'rdm': [],
    'selftest': {'state': 'idle'},
    'identify_until': 0,
    'log': '',
    'crash': True,
    'notice': args.notice,
    'safe': ('The node restarted after a crash 3 times in a row, each within a minute of starting '
             '(last: crash (panic)), so it started with the DMX ports off. The network and the '
             'settings work as usual. The crash report is under System.') if args.safe_mode else '',
    'wifi': {'state': 'connected', 'ssid': 'Theatre-Tech', 'connect_at': 0},
    'ap_linger_until': 0,
}


def log(level, tag, msg):
    with lock:
        t = int((time.time() - BOOT) * 1000)
        S['log'] += f'{level} ({t}) {tag}: {msg}\n'
        if len(S['log']) > 24000:
            S['log'] = S['log'][-20000:]


def boot():
    """What the real node does at start-up"""
    with lock:
        S['tester'] = bool(S['config']['tester']['enabled']) and FEAT['tester']
        S['tests'].clear()
        S['rx'] = [new_rx() for _ in range(NP)]
        S['an'] = [new_an() for _ in range(NP)]
        S['frames_sent'] = [0] * NP
        S['tester_levels'] = [bytearray(512) for _ in range(NP)]
        S['log'] = ''
    log('I', 'main', 'ESP-NODE mock starting')
    log('I', 'main', 'DMX tester mode: web UI only' if S['tester'] else 'Art-Net / sACN node')
    log('W', 'net', 'Ethernet: no cable' if FEAT['ethernet'] else 'Ethernet not built in')
    log('I', 'net', 'Wi-Fi: connected to Theatre-Tech, 192.168.1.47')


def restart(seconds=3.5):
    with lock:
        S['down_until'] = time.time() + seconds
    threading.Timer(seconds, boot).start()


def port_role(n):
    return S['config']['tester']['ports'][n - 1]['role']


def port_mode(n):
    """dmx_mode_t the port runs with now"""
    if S['tester']:
        return {'off': 0, 'send': 1, 'receive': 2, 'rdm': 3}[port_role(n)]
    m = S['config'][f'port{n}']['mode']
    return m if FEAT['rdm'] or m < 3 else 1


def is_input(n):
    return port_mode(n) in (2, 4)


def timing(n):
    if S['tester']:
        t = S['config']['tester']['ports'][n - 1]
        r = {'rate_hz': t['rate_hz'], 'break_us': t['break_us'], 'mab_us': t['mab_us'], 'slots': t['slots']}
    else:
        p = S['config'][f'port{n}']
        r = {'rate_hz': p['out_rate_hz'], 'break_us': p['break_us'], 'mab_us': p['mab_us'], 'slots': p['out_slots']}
    if port_mode(n) == 3 or (not S['tester'] and port_mode(n) == 1 and S['config'][f'port{n}']['rdm_enabled'] and FEAT['rdm']):
        r['break_us'] = min(352, max(176, r['break_us']))
        r['mab_us'] = min(88, r['mab_us'])
    return r


# ---------------------------------------------------------------------------
# Simulation: ~40 frames/s
# ---------------------------------------------------------------------------

def console_frame(t, port):
    """What a console upstream sends: slow fades, one flickering channel"""
    f = bytearray(512)
    for c in range(24):
        f[c] = int(127 + 127 * math.sin(t * 0.4 + c * 0.35)) if c % 4 != 3 else 0
    f[31] = 255
    f[32:40] = bytes([200, 0, 0, 255, 64, 64, 64, 0])
    f[100] = 128
    if random.random() < 0.3:
        f[6] = max(0, min(255, f[6] + random.choice([-3, -2, -1, 1, 2])))   # flicker
    f[44] = 180 if random.random() > 0.02 else random.randint(0, 255)        # glitch now and then
    return f


def test_frame(test, t):
    f = bytearray(512)
    a, b = test['first'] - 1, test['last']
    if test['mode'] == 'full':
        f[a:b] = b'\xff' * (b - a)
    elif test['mode'] == 'static':
        f[a:b] = bytes([test['level']]) * (b - a)
    elif test['mode'] == 'ramp':
        v = int((t * 1000 / test['speed_ms']) * 255) % 256
        f[a:b] = bytes([v]) * (b - a)
    elif test['mode'] == 'chase':
        i = a + int(t * 1000 / test['speed_ms']) % max(1, b - a)
        f[i] = test['level']
    return f


def sim():
    last = time.time()
    while True:
        time.sleep(0.025 + random.uniform(-0.002, 0.003))
        now = time.time()
        iv_us = int((now - last) * 1e6)
        last = now
        with lock:
            if now < S['down_until']:
                continue
            t = now - BOOT
            lost = 40 < (t % 45) < 44      # the console "goes away" every 45 s
            for i in range(NP):
                n = i + 1
                mode = port_mode(n)
                if mode == 0:
                    S['signal'][i] = False
                    continue
                if is_input(n):
                    an, rx = S['an'][i], S['rx'][i]
                    if lost:
                        if S['signal'][i]:
                            S['signal'][i] = False
                            rx['losses'] += 1
                            rx['last_loss'] = now
                            rx['last_frame'] = None
                            log('W', 'dmx', f'Port {n}: DMX signal lost')
                        continue
                    if not S['signal'][i]:
                        log('I', 'dmx', f'Port {n}: DMX signal back')
                    S['signal'][i] = True
                    frame = console_frame(t, n)
                    if random.random() < 0.004:
                        an['err_framing'] += 1
                    if random.random() < 0.01:
                        an['rdm'] += 1
                    if random.random() < 0.002:
                        an['alt'] += 1
                        an['last_alt'] = 0x17
                    an['frames'] += 1
                    an['slots_min'] = 512 if not an['slots_min'] else min(an['slots_min'], 512)
                    an['slots_max'] = 512
                    if now < an['timing_until']:
                        an['break_us'] = random.choice([176, 180, 184])
                        an['mab_us'] = random.choice([12, 16])
                    S['levels'][i] = frame
                    if S['tester']:
                        rx_frame(rx, frame, now)
                else:
                    test = S['tests'].get(n)
                    if test:
                        frame = test_frame(test, t)
                    elif S['tester']:
                        frame = bytearray(S['tester_levels'][i])
                    else:
                        frame = console_frame(t * 0.7, n)
                        frame[0:24] = bytes(int(v * 0.8) for v in frame[0:24])
                    S['levels'][i] = frame
                    S['frames_sent'][i] += 1
                    S['signal'][i] = True
            st = S['selftest']
            if st.get('state') == 'running' and now >= st['_end']:
                finish_selftest()


def rx_frame(rx, frame, now):
    if rx['last_frame'] is not None:
        iv = (now - rx['last_frame']) * 1e6
        if iv <= 1e6:
            rx['ivs'].append(iv)
            if len(rx['ivs']) > 2000:
                rx['ivs'] = rx['ivs'][-2000:]
        rx['min'] = iv if not rx['min'] else min(rx['min'], iv)
        rx['max'] = max(rx['max'], iv)
        k = 0
        while k < len(HIST_EDGES_MS) and iv / 1000 >= HIST_EDGES_MS[k]:
            k += 1
        rx['hist'][k] += 1
    rx['last_frame'] = now
    rx['frames'] += 1
    prev = rx['last']
    for c in range(512):
        v = frame[c]
        if v < rx['cmin'][c]:
            rx['cmin'][c] = v
        if v > rx['cmax'][c]:
            rx['cmax'][c] = v
        if prev is not None and prev[c] != v:
            rx['changes'][c] += 1
            rx['recent'][c + 1] = now
    rx['last'] = bytes(frame)


def rx_json(i):
    rx = S['rx'][i]
    now = time.time()
    ivs = rx['ivs']
    avg = sum(ivs) / len(ivs) if ivs else 0
    jit = math.sqrt(sum((x - avg) ** 2 for x in ivs) / len(ivs)) if ivs else 0
    changing = [c for c, t in rx['recent'].items() if now - t < 1.0]
    return {
        'frames': rx['frames'], 'seconds': int(now - rx['t0']), 'losses': rx['losses'],
        'last_loss_s': int(now - rx['last_loss']) if rx['last_loss'] else -1,
        'interval_avg_us': int(avg), 'interval_jitter_us': int(jit),
        'interval_min_us': int(rx['min']), 'interval_max_us': int(rx['max']), 'hist': rx['hist'],
        'changing': len(changing), 'changed': sum(1 for c in rx['changes'] if c),
    }


def analysis_json(i):
    an = S['an'][i]
    now = time.time()
    sig = S['signal'][i]
    return {
        'refresh_hz': 40 if sig else 0, 'packets_per_s': 40 if sig else 0, 'slots': 512 if an['slots_max'] else 0,
        'slots_min': an['slots_min'], 'slots_max': an['slots_max'], 'frames': an['frames'],
        'err_framing': an['err_framing'], 'err_overflow': an['err_overflow'], 'err_short': an['err_short'],
        'alt_start_codes': an['alt'], 'last_alt_start_code': an['last_alt'], 'rdm_packets': an['rdm'],
        'interval_min_ms': 22 if sig else 0, 'interval_max_ms': 29 if sig else 0,
        'timing_on': now < an['timing_until'], 'timing_left_s': max(0, int(an['timing_until'] - now)),
        'break_us': an['break_us'], 'mab_us': an['mab_us'], 'dropped_late': 0,
    }


def test_json(n):
    t = S['tests'].get(n)
    return dict(t, port=n) if t else None


def tests_list():
    out = []
    for n in range(1, NP + 1):
        t = S['tests'].get(n) or {'mode': 'off', 'first': 1, 'last': 512, 'level': 255, 'speed_ms': 500}
        out.append(dict(t, port=n, running=n in S['tests'], output=not is_input(n) and port_mode(n) != 0))
    return out


def rdm_devices_for(port):
    base = [
        ('Wash 1', 'PAR Wash RGBW', 'Example Lighting', 8, 1, 3),
        ('Spot 2', 'Moving Spot 150', 'Example Lighting', 16, 2, 4),
        ('', 'LED Dimmer 4ch', 'Acme Dimmers', 4, 1, 1),
    ]
    out = []
    for k, (label, model, mfr, fp, pers, npers) in enumerate(base):
        out.append({'port': port, 'uid': f'4A4C:{port:02X}00{k:04X}', 'label': label, 'model_description': model,
                    'model': 0x100 + k, 'manufacturer': mfr, 'software': f'v1.{k}.0', 'footprint': fp,
                    'personality': pers, 'personality_count': npers, 'dmx_address': 1 + 20 * k})
    return out


def finish_selftest():
    st = S['selftest']
    st['state'] = 'passed'
    st['message'] = f"Passed: port {st['out_port']} reaches port {st['in_port']} and every value arrived."
    for s in st['steps']:
        s.update(done=True, passed=True, slots=512, mismatches=0, first_bad=0, sent=0, got=0)
    st.update(refresh_hz=40, break_us=180, mab_us=16, errors=0)


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

def system_info():
    up = int(time.time() - BOOT)
    board = 'Ghostlight Node rev1'
    chip = 'ESP32-S3'
    return {
        'firmware_version': '2.1.0-mock', 'build_date': 'Nov 21 2026', 'build_time': '22:00:00',
        'board': board, 'chip': chip, 'hardware': f'{board} ({chip})', 'ports': NP,
        'idf_version': 'v5.2.6', 'partition': 'ota_0', 'free_heap': 182344 - random.randint(0, 3000),
        'min_free_heap': 150112, 'uptime_sec': up, 'features': FEAT,
        'tester': {'available': FEAT['tester'], 'enabled': S['tester'], 'next_boot': bool(S['config']['tester']['enabled'])},
        'board_id': 'ghostlight-rev1',
        **({'settings_notice': S['notice']} if S['notice'] else {}),
        **({'safe_mode': S['safe']} if S['safe'] else {}),
    }


def network_status():
    now = time.time()
    w = S['wifi']
    if w['state'] == 'connecting' and now >= w['connect_at']:
        w['state'] = 'connected'
    profiles = S['config']['network']['wifi_profiles']
    nets = []
    for p in profiles:
        nets.append({'ssid': p['ssid'], 'connected': w['state'] == 'connected' and w['ssid'] == p['ssid'],
                     'visible': p['ssid'] != 'Home', 'rssi': -58, 'tried': True, 'reason': 0, 'reason_text': ''})
    linger = now < S['ap_linger_until']
    ns = {
        'state': 2, 'primary': 'wifi', 'connected': True, 'ip_address': '192.168.1.47', 'net_mode': ['auto', 'ethernet', 'wifi', 'ap'][S['config']['network']['mode']],
        'hostname': 'stage-left', 'mode': 'WiFi',
        'wifi': {'enabled': True, 'state': w['state'], 'ssid': w['ssid'], 'ip': '192.168.1.47', 'netmask': '255.255.255.0',
                 'gateway': '192.168.1.1', 'rssi': -58, 'channel': 6, 'testing': False, 'retry_in': 0, 'networks': nets},
        'ap': {'active': linger, 'ssid': S['config']['network']['ap_ssid'], 'ip': '192.168.4.1', 'channel': 6,
               'clients': 1 if linger else 0, 'reason': 'linger' if linger else 'off', 'off_in': max(0, int(S['ap_linger_until'] - now))},
    }
    if FEAT['ethernet']:
        ns['ethernet'] = {'enabled': True, 'present': True, 'link': False, 'ready': False, 'dhcp': True,
                          'ip': '0.0.0.0', 'netmask': '0.0.0.0', 'gateway': '0.0.0.0', 'mac': '24:6F:28:00:11:22'}
    return ns


def ports_status():
    out = []
    for i in range(NP):
        n = i + 1
        pc = S['config'][f'port{n}']
        mode = port_mode(n)
        inp = is_input(n)
        o = {
            'port': n, 'active': mode != 0, 'mode': mode, 'universe': pc['universe_primary'],
            'artnet_universe': pc['universe_primary'], 'sacn_universe': pc['sacn_universe'],
            'protocol': ['artnet', 'sacn', 'both', 'both', 'both'][pc['protocol_mode']] if FEAT['sacn'] else 'artnet',
            'frames_sent': S['frames_sent'][i], 'frames_received': S['an'][i]['frames'], 'errors': S['an'][i]['err_framing'],
            'signal': S['signal'][i], 'slots': 512, 'rdm_available': FEAT['rdm'] and (mode == 3 or (mode == 1 and pc['rdm_enabled'])),
            'rdm_discovering': time.time() < S.get('rdm_until', 0), 'rdm_devices': len([d for d in S['rdm'] if d['port'] == n]),
            'rdm_tod_valid': any(d['port'] == n for d in S['rdm']), 'output_active': mode in (1, 3),
            'sync': 'none', 'scene': 0, 'failsafe': False, 'timing': timing(n),
        }
        if n in S['tests']:
            o['test'] = test_json(n)
        if inp:
            o['analysis'] = analysis_json(i)
            o['sources'] = []
        elif mode:
            local, only = pc.get('local_inputs', []), pc.get('local_only', False)
            o['sources'] = [] if local and only else [
                {'protocol': 'Art-Net', 'ip': '192.168.1.20', 'name': 'Lighting desk', 'priority': 0,
                 'per_address_priority': False, 'backup': False, 'contributing': True, 'age_ms': 23, 'packets': S['frames_sent'][i]}]
            if FEAT['sacn'] and n == 1 and not only:
                o['sources'].append({'protocol': 'sACN', 'ip': '192.168.1.31', 'name': 'Backup desk', 'priority': 90,
                                     'per_address_priority': False, 'backup': False, 'contributing': False, 'age_ms': 140, 'packets': 1200})
            for m in local:
                if is_input(m) and S['signal'][m - 1]:
                    o['sources'].append({'protocol': 'DMX', 'ip': '', 'port': m, 'name': f'Port {m} DMX in', 'priority': 100,
                                         'per_address_priority': False, 'backup': False, 'contributing': True, 'age_ms': 25,
                                         'packets': S['an'][m - 1]['frames']})
        out.append(o)
    return out


def port_takes(pc, proto, universe):
    """(used, filtered) as web_signal.c works it out"""
    if pc['mode'] not in (1, 3) or (pc.get('local_inputs') and pc.get('local_only')):
        return False, False
    if proto == 'artnet':
        match = universe == pc['universe_primary'] or (
            pc['merge_mode'] == 3 and pc['universe_secondary'] >= 0 and universe == pc['universe_secondary'])
        blocked = pc['protocol_mode'] == 1
    else:
        match = universe == pc['sacn_universe']
        blocked = pc['protocol_mode'] == 0
    return (match and not blocked), (match and blocked)


def network_sources():
    """GET /api/network/sources for the --signal scenario"""
    cfg = S['config']
    sc = args.signal
    p1 = cfg['port1']
    src = []

    def add(proto, ip, uni, name='', prio=100, rate=44.0, lost=0, recent=0, gap=40, age=20, preview=False, pap=False):
        s = {'protocol': proto, 'ip': ip, 'name': name, 'universe': uni, 'slots': 512, 'sequenced': True,
             'packets': int(rate * (time.time() - BOOT)), 'lost': lost, 'lost_recent': recent, 'late': 0,
             'rate': rate, 'max_gap_ms': gap, 'age_ms': age}
        if proto == 'sacn':
            s.update({'priority': prio, 'pap': pap, 'preview': preview})
        s['ports'] = [n for n in range(1, NP + 1) if port_takes(cfg[f'port{n}'], proto, uni)[0]]
        s['filtered'] = [n for n in range(1, NP + 1) if port_takes(cfg[f'port{n}'], proto, uni)[1]]
        src.append(s)

    adverts = []
    if sc == 'good':
        add('artnet', '192.168.1.20', p1['universe_primary'])
        if FEAT['sacn']:
            add('sacn', '192.168.1.31', p1['sacn_universe'], 'Backup desk', prio=90, rate=22.1, gap=180)
            add('sacn', '192.168.1.31', 7, 'Backup desk', prio=90, rate=0.0, age=41000)
            adverts.append({'ip': '192.168.1.31', 'name': 'Backup desk', 'universes': [p1['sacn_universe'], 7], 'count': 2, 'age_ms': 3200})
    elif sc == 'offbyone':
        add('artnet', '2.0.0.10', p1['universe_primary'] + 1)
        if FEAT['sacn']:
            adverts.append({'ip': '2.0.0.10', 'name': 'ETC Eos', 'universes': [p1['sacn_universe'] + 1, p1['sacn_universe'] + 2], 'count': 2, 'age_ms': 5000})
    elif sc == 'filtered':
        add('artnet', '192.168.1.20', p1['universe_primary'])
        cfg['port1']['protocol_mode'] = 1
        for s in src:
            s['ports'], s['filtered'] = [], [1]
    elif sc == 'lossy':
        add('sacn' if FEAT['sacn'] else 'artnet', '192.168.1.31', p1['sacn_universe'] if FEAT['sacn'] else p1['universe_primary'],
            'Tablet', rate=31.5, lost=240, recent=37, gap=1450)
    elif sc == 'multicast':
        adverts.append({'ip': '192.168.1.31', 'name': 'Backup desk', 'universes': [p1['sacn_universe']], 'count': 1, 'age_ms': 2100})

    ifs = [{'name': 'wifi', 'ip': '192.168.1.47', 'netmask': '255.255.255.0', 'rssi': -78 if sc == 'lossy' else -58,
            'ssid': S['wifi']['ssid'], 'channel': 6}]
    ports = []
    for n in range(1, NP + 1):
        pc = cfg[f'port{n}']
        ports.append({'port': n, 'mode': pc['mode'], 'output': pc['mode'] in (1, 3), 'protocol_mode': pc['protocol_mode'],
                      'merge_mode': pc['merge_mode'], 'artnet': pc['universe_primary'], 'artnet_backup': pc['universe_secondary'],
                      'sacn': pc['sacn_universe'], 'live_sources': 0, 'contributing': 0, 'rules': bool(pc.get('rules')),
                      'local_inputs': pc.get('local_inputs', []) if pc['mode'] in (1, 3) else [],
                      'local_only': pc['mode'] in (1, 3) and bool(pc.get('local_inputs')) and bool(pc.get('local_only')),
                      'input': is_input(n)})
        if is_input(n):
            ports[-1]['signal'] = S['signal'][n - 1]
    return {'listening': not cfg['tester']['enabled'], 'interfaces': ifs, 'ports': ports,
            'sources': src, 'advertised': adverts}


LAST_POLL = [0.0]


def network_nodes():
    """GET /api/network/nodes: other Art-Net nodes for the --signal scenario"""
    now = time.time()
    polled = now - LAST_POLL[0] >= 5
    if polled:
        LAST_POLL[0] = now
    sc = args.signal

    def port(out=None, inp=None, out_data=True, in_data=True):
        return {'output': out is not None, 'input': inp is not None, 'protocol': 0,
                'in': inp or 0, 'out': out or 0, 'in_data': inp is not None and in_data,
                'out_data': out is not None and out_data}

    def node(ip, mac, short, long='', ports=(), style=0, bind=1, conflict=False, age=1200, dhcp=False):
        return {'ip': ip, 'mac': mac, 'short_name': short, 'long_name': long, 'firmware': 0x0104, 'oem': 0x2BC0,
                'esta': 0x7FF0, 'style': style, 'bind_index': bind, 'dhcp': dhcp, 'conflict': conflict,
                'replies': 12, 'age_ms': age, 'ports': list(ports)}

    nodes = []
    if sc not in ('nothing',):
        nodes.append(node('192.168.1.20', '00:1c:2d:00:11:22', 'Lighting desk', 'Desk, Art-Net out',
                          [port(inp=0), port(inp=2)], style=1, age=800))
        nodes.append(node('192.168.1.60', '24:6f:28:aa:bb:01', 'Stage left', 'Stage left rack',
                          [port(out=0)], bind=1, dhcp=True))
        nodes.append(node('192.168.1.60', '24:6f:28:aa:bb:01', 'Stage left', 'Stage left rack',
                          [port(out=4, out_data=False)], bind=2, dhcp=True))
    if sc == 'clash':
        nodes.append(node('192.168.1.47', 'b8:27:eb:12:34:56', 'Pi visualiser', '', [], style=6, conflict=True, age=2100))
        nodes.append(node('192.168.1.71', '00:50:c2:01:02:03', 'Old splitter', 'Two-port gateway', [port(inp=1)]))
        nodes.append(node('192.168.1.80', '00:50:c2:0a:0b:0c', 'FOH node', '', [port(out=7)]))
        nodes.append(node('192.168.1.80', '00:50:c2:0d:0e:0f', 'Spare node', '', [port(out=8)]))
    return {'listening': not S['config']['tester']['enabled'], 'polled': polled, 'nodes': nodes}


def tester_json():
    ports = []
    for i in range(NP):
        n = i + 1
        t = S['config']['tester']['ports'][i]
        mode = port_mode(n)
        o = dict(t, port=n, active=mode != 0, mode=mode, signal=S['signal'][i], frames_sent=S['frames_sent'][i],
                 frames_received=S['an'][i]['frames'], errors=S['an'][i]['err_framing'],
                 rdm_devices=len([d for d in S['rdm'] if d['port'] == n]), rdm_discovering=time.time() < S.get('rdm_until', 0),
                 rdm_tod_valid=any(d['port'] == n for d in S['rdm']), timing=timing(n))
        if n in S['tests']:
            o['test'] = test_json(n)
        if is_input(n):
            o['analysis'] = analysis_json(i)
        if S['tester']:
            o['rx'] = rx_json(i)
        ports.append(o)
    return {'available': FEAT['tester'], 'enabled': S['tester'], 'next_boot': bool(S['config']['tester']['enabled']),
            'hist_edges_ms': HIST_EDGES_MS, 'ports': ports}


def merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            merge(dst[k], v)
        else:
            dst[k] = v


class Err(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


class H(SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def log_message(self, fmt, *a):
        pass

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n) if n else b''
        if self.headers.get('Content-Type', '').startswith('application/json') and raw:
            try:
                return json.loads(raw)
            except ValueError:
                raise Err(400, 'Invalid JSON')
        return raw

    def down(self):
        if time.time() < S['down_until']:
            self.send_response(503)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return True
        return False

    def do_GET(self):
        self.route('GET')

    def do_POST(self):
        self.route('POST')

    def do_DELETE(self):
        self.route('DELETE')

    def route(self, method):
        if self.down():
            return
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        p = u.path
        if p == '/ws':
            return self.websocket()
        if not p.startswith('/api/'):
            if method != 'GET':
                return self.send_json({'error': 'not_found'}, 404)
            return super().do_GET()
        try:
            b = self.body() if method == 'POST' else None
            with lock:
                r = self.api(method, p, q, b)
            if r is None:
                return   # already answered
            if isinstance(r, tuple):
                ctype, data, name = r
                self.send_response(200)
                self.send_header('Content-Type', ctype)
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Content-Disposition', f'attachment; filename="{name}"')
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send_json(r)
        except Err as e:
            self.send_json({'error': 'error', 'message': str(e)}, e.code)

    def api(self, m, p, q, b):
        cfg = S['config']
        b = b if isinstance(b, dict) else {}

        if p == '/api/auth/status':
            return {'authenticated': not S['setup'], 'setup_required': S['setup'], 'locked': False,
                    'login_required': S['login'] and not S['setup'],
                    'username': 'admin', 'ws_token': 'mock', 'ap_ssid': cfg['network']['ap_ssid']}
        if S['setup'] and p != '/api/setup':
            self.send_json({'error': 'setup_required', 'message': 'Set a password first'}, 403)
            return None
        if p == '/api/setup':
            no_pw = b.get('no_password') is True
            ap_pw = b.get('ap_password')
            if not no_pw and not 8 <= len(b.get('password') or '') <= 63:
                raise Err(400, 'Admin password must be 8-63 characters')
            if not isinstance(ap_pw, str) or (ap_pw and not 8 <= len(ap_pw) <= 63) or ap_pw == '12345678':
                raise Err(400, 'AP password must be 8-63 characters (or empty for an open network)')
            S['setup'] = False
            S['login'] = not no_pw
            cfg['network']['ap_open'] = ap_pw == ''
            restart(4)
            return {'status': 'ok'}
        if p == '/api/auth/password':
            if S['login'] and len(b.get('current_password', '')) < 1:
                raise Err(403, 'Current password is wrong')
            if b.get('no_password') is True:
                S['login'] = False
            elif not 8 <= len(b.get('new_password') or '') <= 63:
                raise Err(400, 'New password must be 8-63 characters')
            else:
                S['login'] = True
            return {'status': 'ok'}

        if p == '/api/system/info':
            return system_info()
        if p == '/api/system/stats':
            return {'artnet': {'packets': sum(S['frames_sent']) * 2, 'dmx_packets': sum(S['frames_sent'])},
                    'sacn': {'packets': sum(S['frames_sent']) // 3, 'data_packets': sum(S['frames_sent']) // 4, 'subscriptions': NP}}
        if p in ('/api/system/restart', '/api/system/firmware-rollback'):
            restart()
            return {'status': 'restarting'}
        if p == '/api/system/factory-reset':
            S['config'] = default_config()
            S['config']['tester']['enabled'] = False
            restart()
            return {'status': 'restarting'}
        if p == '/api/system/firmware-upload':
            restart(5)
            return {'status': 'ok'}
        if p == '/api/system/identify':
            if m == 'POST':
                S['identify_until'] = time.time() + int(b.get('seconds', 30))
            return {'identify_left': max(0, int(S['identify_until'] - time.time()))}

        if p == '/api/config':
            if m == 'GET':
                return cfg
            if 'tester' in b:
                for k, t in enumerate(b['tester'].get('ports', [])[:NP]):
                    cfg['tester']['ports'][k].update(t)
                if 'enabled' in b['tester']:
                    cfg['tester']['enabled'] = bool(b['tester']['enabled'])
                b = {k: v for k, v in b.items() if k != 'tester'}
            if 'network' in b and 'wifi_profiles' in b['network']:
                old = {x['ssid']: x for x in cfg['network']['wifi_profiles']}
                profs = []
                for x in b['network']['wifi_profiles']:
                    hp = bool(x.get('password')) if 'password' in x else old.get(x['ssid'], {}).get('has_password', False)
                    x = {k: v for k, v in x.items() if k != 'password'}
                    x['has_password'] = hp
                    profs.append(x)
                cfg['network']['wifi_profiles'] = profs
                b['network'] = {k: v for k, v in b['network'].items() if k != 'wifi_profiles'}
            if 'network' in b:
                if 'ap_password' in b['network']:
                    b['network']['ap_open'] = b['network'].pop('ap_password') == ''
                if not FEAT['ethernet'] and b['network'].get('mode') == 1:
                    b['network']['mode'] = 0
            for n in range(1, NP + 1):
                lp = (b.get(f'port{n}') or {}).get('local_inputs')
                if lp is not None:
                    if not isinstance(lp, list) or any(not isinstance(m, int) or isinstance(m, bool) or not 1 <= m <= NP for m in lp):
                        raise Err(400, 'Routed inputs must be a list of port numbers')
                    if n in lp:
                        raise Err(400, 'A port cannot take its own DMX input')
                    b[f'port{n}']['local_inputs'] = sorted(set(lp))
            merge(cfg, b)
            for n in range(1, NP + 1):
                pc = cfg[f'port{n}']
                if not pc.get('local_inputs'):
                    pc['local_only'] = False
            log('I', 'config', 'Configuration saved')
            return {'status': 'ok'}

        if p == '/api/network/status':
            return network_status()
        if p == '/api/network/sources':
            return network_sources()
        if p == '/api/network/nodes':
            return network_nodes()
        if p == '/api/network/wifi-scan':
            time.sleep(0.8)
            saved = {x['ssid'] for x in cfg['network']['wifi_profiles']}
            nets = [('Theatre-Tech', -58, True), ('FOH-Guest', -67, False), ('Office', -74, True), ('Campus', -70, True)]
            out = [{'ssid': s, 'rssi': r, 'secure': sec, 'saved': s in saved, 'enterprise': s == 'Campus', 'wep': False, 'channel': 6} for s, r, sec in nets]
            return out
        if p == '/api/network/wifi/connect':
            S['wifi'] = {'state': 'connecting', 'ssid': b.get('ssid', ''), 'connect_at': time.time() + 3}
            return {'status': 'ok'}
        if p == '/api/network/ap/off':
            S['ap_linger_until'] = 0
            return {'status': 'ok'}

        if p == '/api/ports/status':
            return ports_status()
        if p.startswith('/api/ports/'):
            parts = p.split('/')
            n = int(parts[3])
            if not 1 <= n <= NP:
                raise Err(404, 'No such port')
            if parts[4] == 'dmx':
                return {'port': n, 'channels': list(S['levels'][n - 1]), 'active': S['signal'][n - 1], 'input': is_input(n)}
            if parts[4] == 'blackout':
                S['tests'].pop(n, None)
                S['tester_levels'][n - 1] = bytearray(512)
                return {'status': 'ok'}

        if p == '/api/scenes':
            return S['scenes']
        if p == '/api/scenes/record':
            sid = max([s['scene_id'] for s in S['scenes']] + [0]) + 1
            S['scenes'].append({'scene_id': sid, 'name': b.get('name', 'Scene'), 'port': b.get('port', 1), 'active': False})
            return {'status': 'ok', 'scene_id': sid}
        if p == '/api/scenes/playback':
            for s in S['scenes']:
                s['active'] = s['scene_id'] == b.get('scene_id')
            return {'status': 'ok'}
        if p == '/api/scenes/release':
            for s in S['scenes']:
                s['active'] = False
            return {'status': 'ok'}
        if p.startswith('/api/scenes/') and m == 'DELETE':
            sid = int(p.rsplit('/', 1)[1])
            S['scenes'] = [s for s in S['scenes'] if s['scene_id'] != sid]
            return {'status': 'ok'}

        if p == '/api/rdm/discover':
            # As the node: starts it, answers at once, and the page follows
            # rdm_discovering in /api/ports/status
            S['rdm'] = []
            started = []
            for n in range(1, NP + 1):
                if port_mode(n) == 3 or (not S['tester'] and port_mode(n) == 1 and cfg[f'port{n}']['rdm_enabled']):
                    S['rdm'] += rdm_devices_for(n)
                    started.append(n)
            S['rdm_until'] = time.time() + 2.5
            return {'started': started}
        if p == '/api/rdm/devices':
            return S['rdm']
        if p == '/api/rdm/address':
            for d in S['rdm']:
                if d['uid'] == b.get('uid'):
                    d['dmx_address'] = b.get('address')
            return {'status': 'ok'}
        if p == '/api/rdm/identify':
            return {'status': 'ok'}

        if p == '/api/tools/test':
            if m == 'POST':
                n = int(b.get('port', 1))
                if b.get('mode') == 'off':
                    S['tests'].pop(n, None)
                else:
                    if is_input(n) or port_mode(n) == 0:
                        raise Err(409, f'Port {n} is not an output')
                    S['tests'][n] = {k: b[k] for k in ('mode', 'first', 'last', 'level', 'speed_ms') if k in b}
            return tests_list()
        if p == '/api/tools/analyzer':
            n = int(b.get('port', 1))
            if b.get('action') == 'reset':
                S['an'][n - 1] = new_an()
            else:
                S['an'][n - 1]['timing_until'] = time.time() + int(b.get('seconds', 0))
            return {'status': 'ok'}
        if p == '/api/tools/selftest':
            if m == 'POST':
                names = ['All channels at zero', 'All channels full', 'Ramp 0-255', 'Channel numbers', 'Short frames (24 slots)']
                S['selftest'] = {'state': 'running', 'out_port': b.get('out_port'), 'in_port': b.get('in_port'),
                                 'message': 'Running…', 'steps': [{'name': s, 'done': False} for s in names], '_end': time.time() + 5}
            st = S['selftest']
            if st.get('state') == 'running':
                left = st['_end'] - time.time()
                done = int((5 - left) / 1.0)
                for k, s in enumerate(st['steps']):
                    if k < done and not s['done']:
                        s.update(done=True, passed=True, slots=512)
            return {k: v for k, v in st.items() if not k.startswith('_')}

        if p == '/api/tester':
            return tester_json()
        if p == '/api/tester/channels':
            if not S['tester']:
                raise Err(409, 'The node is not in DMX tester mode')
            n = int(q.get('port', 1))
            rx = S['rx'][n - 1]
            now = time.time()
            return {'port': n, 'level': list(S['levels'][n - 1]), 'min': [0 if v == 255 and rx['cmax'][c] == 0 else v for c, v in enumerate(rx['cmin'])],
                    'max': rx['cmax'], 'changes': rx['changes'], 'changing': sorted(c for c, t in rx['recent'].items() if now - t < 1.0)}
        if p == '/api/tester/mode':
            on = bool(b.get('enabled'))
            if on and not FEAT['tester']:
                raise Err(400, 'This firmware was built without the DMX tester')
            cfg['tester']['enabled'] = on
            changed = on != S['tester']
            if changed:
                restart()
            return {'status': 'ok', 'enabled': on, 'restarting': changed}
        if p == '/api/tester/port':
            n = int(b.get('port', 0))
            if not 1 <= n <= NP:
                raise Err(400, 'Invalid port')
            t = cfg['tester']['ports'][n - 1]
            if 'role' in b:
                if b['role'] not in ROLES or (b['role'] == 'rdm' and not FEAT['rdm']):
                    raise Err(400, 'Unknown role')
                t['role'] = b['role']
                S['tests'].pop(n, None)
                S['rx'][n - 1] = new_rx()
                S['an'][n - 1] = new_an()
            for k, lo, hi in (('rate_hz', 1, 44), ('break_us', 92, 1000), ('mab_us', 12, 1000), ('slots', 24, 512)):
                if k in b:
                    t[k] = max(lo, min(hi, int(b[k])))
            return {'status': 'ok'}
        if p == '/api/tester/levels':
            if not S['tester']:
                raise Err(409, 'The node is not in DMX tester mode')
            n = int(b.get('port', 1))
            lv = S['tester_levels'][n - 1]
            if 'values' in b:
                s = int(b.get('start', 1))
                for k, v in enumerate(b['values']):
                    if 1 <= s + k <= 512:
                        lv[s + k - 1] = max(0, min(255, int(v)))
            else:
                a, z, v = int(b.get('first', 1)), int(b.get('last', 512)), int(b.get('value', 0))
                for c in range(a, z + 1):
                    lv[c - 1] = v
            return {'status': 'ok'}
        if p == '/api/tester/reset':
            n = int(b.get('port', 0))
            for k in ([n] if n else range(1, NP + 1)):
                S['rx'][k - 1] = new_rx()
                S['an'][k - 1] = new_an()
            return {'status': 'ok'}

        if p == '/api/diag/log':
            pos = int(q.get('pos', 0))
            text = S['log']
            return {'pos': len(text), 'text': text[pos:] if pos <= len(text) else text, 'truncated': False, 'capacity': 16384}
        if p == '/api/diag/log.txt':
            return ('text/plain', S['log'].encode(), 'esp-node-log.txt')
        if p == '/api/diag/crash':
            r = {'reset_reason': 'Power on', 'reset_was_crash': False, 'present': S['crash']}
            if S['crash']:
                r.update(task='dmx_out', pc='0x42012ab4', reason='LoadProhibited', backtrace=['0x42012ab4:0x3fc9a0b0', '0x42011f02:0x3fc9a0e0'],
                         backtrace_corrupted=False, same_firmware=False, size=65536)
            return r
        if p == '/api/diag/crash.bin':
            return ('application/octet-stream', b'\0' * 64, 'esp-node-coredump.bin')
        if p == '/api/diag/crash/clear':
            S['crash'] = False
            return {'status': 'ok'}
        if p == '/api/system/safe-mode/leave':
            S['safe'] = ''
            return {'status': 'ok'}
        if p == '/api/system/notice/clear':
            S['notice'] = ''
            return {'status': 'ok'}
        if p == '/api/diag/health':
            stacks = [('led_task', 1980), ('reset_btn', 684), ('IDLE1', 696), ('IDLE0', 716), ('ipc0', 724),
                      ('ipc1', 768), ('dmx_out_1', 1532), ('rdm_bridge', 1780), ('tiT', 1948), ('dmx_in_2', 2900)]
            return {'internal_free': 59403 - random.randint(0, 2000), 'internal_min_free': 38919,
                    'internal_largest': 24576, 'internal_low': 16384, 'psram_free': 8348740,
                    'psram_min_free': 8348436, 'stack_low': 512,
                    'stacks': [{'task': t, 'free': f} for t, f in sorted(stacks, key=lambda x: x[1])]}
        raise Err(404, f'No such endpoint: {m} {p}')

    # -- WebSocket: binary level frames at 10 Hz, JSON commands in ----------

    def websocket(self):
        key = self.headers.get('Sec-WebSocket-Key')
        if not key:
            return self.send_json({'error': 'websocket only'}, 400)
        acc = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', acc)
        self.end_headers()
        sock = self.connection
        sock.setblocking(True)
        nxt = 0
        buf = b''
        try:
            while True:
                if time.time() < S['down_until']:
                    break
                now = time.time()
                if now >= nxt:
                    nxt = now + 0.1
                    with lock:
                        live = sum(1 << i for i in range(NP) if S['signal'][i])
                        inp = sum(1 << i for i in range(NP) if is_input(i + 1))
                        payload = bytes([2, live, inp, NP]) + b''.join(bytes(S['levels'][i]) for i in range(NP))
                    sock.sendall(ws_frame(payload, 2))
                r, _, _ = select.select([sock], [], [], max(0, nxt - time.time()))
                if r:
                    d = sock.recv(65536)
                    if not d:
                        break
                    buf += d
                    while True:
                        msg, buf, op = ws_parse(buf)
                        if msg is None:
                            break
                        if op == 8:
                            raise ConnectionError
                        if op == 1:
                            self.ws_command(msg)
        except (ConnectionError, OSError):
            pass
        self.close_connection = True

    def ws_command(self, msg):
        try:
            c = json.loads(msg)
        except ValueError:
            return
        with lock:
            if c.get('command') == 'set_channel' and S['tester']:
                n, ch, v = int(c.get('port', 1)), int(c.get('channel', 1)), int(c.get('value', 0))
                if 1 <= n <= NP and 1 <= ch <= 512:
                    S['tester_levels'][n - 1][ch - 1] = max(0, min(255, v))
            elif c.get('command') == 'blackout':
                for i in range(NP):
                    S['tester_levels'][i] = bytearray(512)


def ws_frame(payload, op):
    n = len(payload)
    if n < 126:
        head = struct.pack('!BB', 0x80 | op, n)
    elif n < 65536:
        head = struct.pack('!BBH', 0x80 | op, 126, n)
    else:
        head = struct.pack('!BBQ', 0x80 | op, 127, n)
    return head + payload


def ws_parse(buf):
    if len(buf) < 2:
        return None, buf, 0
    op = buf[0] & 0x0F
    masked = buf[1] & 0x80
    n = buf[1] & 0x7F
    i = 2
    if n == 126:
        if len(buf) < 4:
            return None, buf, 0
        n = struct.unpack('!H', buf[2:4])[0]
        i = 4
    elif n == 127:
        if len(buf) < 10:
            return None, buf, 0
        n = struct.unpack('!Q', buf[2:10])[0]
        i = 10
    mask = b''
    if masked:
        if len(buf) < i + 4:
            return None, buf, 0
        mask = buf[i:i + 4]
        i += 4
    if len(buf) < i + n:
        return None, buf, 0
    data = bytearray(buf[i:i + n])
    if masked:
        for k in range(n):
            data[k] ^= mask[k % 4]
    return data.decode('utf-8', 'replace'), buf[i + n:], op


if __name__ == '__main__':
    boot()
    threading.Thread(target=sim, daemon=True).start()
    srv = ThreadingHTTPServer(('127.0.0.1', args.port), H)
    srv.daemon_threads = True
    mode = 'tester' if args.tester else 'node'
    print(f'UI mock ({mode} mode, {NP} ports) on http://localhost:{args.port}/  serving {os.path.normpath(ROOT)}', flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
