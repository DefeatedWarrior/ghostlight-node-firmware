#!/usr/bin/env python3
"""Repeatable RDM-over-Art-Net test against a Ghostlight node (or any Art-Net 4 RDM gateway).

    python3 tools/artnet_rdm_test.py 10.0.0.50                    read-only checks, every device found
    python3 tools/artnet_rdm_test.py 10.0.0.50 --universe 3       devices on Port-Address 3
    python3 tools/artnet_rdm_test.py 10.0.0.50 --uid 4a4c:01000000
    python3 tools/artnet_rdm_test.py 10.0.0.50 --ghostlight      + the checks for a Ghostlight port
                                                                   in RDM responder mode
    python3 tools/artnet_rdm_test.py 10.0.0.50 --write           + label, address and identify
                                                                   changes, each put back after

It asks the node for its device list (ArtTodRequest), then sends each device RDM
requests in ArtRdm packets and checks the answers: the right transaction number,
command class and parameter, and the answer E1.20 asks for. A burst of requests
sent without waiting checks that the node queues them rather than dropping them.

Read-only by default: without --write the only SET requests are ones every device
must refuse (SET DEVICE_INFO), plus, with --ghostlight, values out of range that
a Ghostlight responder refuses. Nothing changes DMX levels.

It listens on UDP port 6454, as Art-Net replies go there; close other Art-Net
software on this computer first. Exit code 0 when every check passed.
Python 3 standard library only.
"""
import argparse
import json
import socket
import statistics
import struct
import sys
import time
import urllib.request

ARTNET_PORT = 6454
OP_TOD_REQUEST, OP_TOD_DATA, OP_RDM = 0x8000, 0x8100, 0x8300
GET, SET = 0x20, 0x30
SC_RDM = 0xCC
NACK_REASON = {0: 'UNKNOWN_PID', 1: 'FORMAT_ERROR', 2: 'HARDWARE_FAULT', 3: 'PROXY_REJECT',
               4: 'WRITE_PROTECT', 5: 'UNSUPPORTED_COMMAND_CLASS', 6: 'DATA_OUT_OF_RANGE',
               7: 'BUFFER_FULL', 8: 'PACKET_SIZE_UNSUPPORTED', 9: 'SUB_DEVICE_OUT_OF_RANGE',
               10: 'PROXY_BUFFER_FULL'}
RESPONSE = {0: 'ACK', 1: 'ACK_TIMER', 2: 'NACK', 3: 'ACK_OVERFLOW'}
# This tool's own controller UID: the prototype range (0x7FF0) E1.20 sets aside
CONTROLLER = bytes.fromhex('7ff0000000a7')
NOBODY = bytes.fromhex('7ff012345678')


def uid_text(u):
    return '%04x:%08x' % struct.unpack('>HI', u)


def uid_bytes(text):
    man, dev = text.split(':')
    return struct.pack('>HI', int(man, 16), int(dev, 16))


def artnet_header(opcode):
    return b'Art-Net\x00' + struct.pack('<H', opcode) + bytes([0, 14])


class Answer:
    """One RDM answer, or none (kind = 'none')"""

    def __init__(self, kind, ms=None, cc=None, pid=None, pd=b'', reason=None):
        self.kind, self.ms, self.cc, self.pid, self.pd, self.reason = kind, ms, cc, pid, pd, reason

    def __str__(self):
        if self.kind == 'none':
            return 'no answer'
        if self.kind == 'NACK':
            return 'NACK %s' % NACK_REASON.get(self.reason, self.reason)
        if self.kind == 'ACK_TIMER':
            return 'ACK_TIMER %s ms' % (self.reason * 100 if self.reason is not None else '?')
        return '%s, %d bytes%s' % (self.kind, len(self.pd),
                                   ', pid 0x%04x' % self.pid if self.pid in (0x0030,) else '')


class Node:
    def __init__(self, ip, universe, timeout, bind):
        self.ip, self.universe, self.timeout = ip, universe, timeout
        self.tn = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, 'SO_REUSEPORT'):
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        try:
            self.sock.bind((bind, ARTNET_PORT))
        except OSError as e:
            sys.exit('Cannot listen on UDP port %d (%s): is other Art-Net software running?'
                     % (ARTNET_PORT, e))

    # ---- device list -----------------------------------------------------------

    def tod(self, wait_s):
        """The node's device list for the universe (ArtTodRequest -> ArtTodData)"""
        net, addr = (self.universe >> 8) & 0x7F, self.universe & 0xFF
        req = artnet_header(OP_TOD_REQUEST) + bytes(9) + bytes([net, 0, 1, addr]) + bytes(31)
        end = time.time() + wait_s
        uids = {}
        while time.time() < end:
            self.sock.sendto(req, (self.ip, ARTNET_PORT))
            got_answer = False
            stop = time.time() + 1.5
            while time.time() < stop:
                d = self.receive(stop)
                if d is None:
                    break
                if d[8:10] != struct.pack('<H', OP_TOD_DATA) or len(d) < 28:
                    continue
                if d[21] != net or d[23] != addr:
                    continue
                got_answer = True
                if d[22] == 0xFF:          # TodNak: no list yet (discovery running)
                    continue
                n = d[27]
                for i in range(n):
                    u = d[28 + 6 * i:34 + 6 * i]
                    if len(u) == 6:
                        uids[u] = True
                total = (d[24] << 8) | d[25]
                if total and len(uids) >= total:   # 0: maybe not discovered yet
                    return list(uids)
            if got_answer and uids:
                return list(uids)
            time.sleep(1.0)       # the node may still be discovering
        return list(uids)

    # ---- one request ---------------------------------------------------------------

    def receive(self, until):
        while True:
            left = until - time.time()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                data, (ip, _port) = self.sock.recvfrom(1500)
            except socket.timeout:
                return None
            if ip == self.ip and data[:8] == b'Art-Net\x00':
                return data

    def packet(self, dest, cc, pid, pd=b'', sub=0, pdl=None, checksum=True):
        """ArtRdm with one RDM request; returns (packet, transaction number)"""
        self.tn = (self.tn + 1) & 0xFF
        pdl = len(pd) if pdl is None else pdl
        rdm = (bytes([0x01, 24 + len(pd)]) + dest + CONTROLLER +
               bytes([self.tn, 1, 0]) + struct.pack('>HBHB', sub, cc, pid, pdl) + pd)
        if checksum:   # Art-Net 4 sends it; the node also takes packets without
            rdm += struct.pack('>H', (SC_RDM + sum(rdm)) & 0xFFFF)
        net, addr = (self.universe >> 8) & 0x7F, self.universe & 0xFF
        head = artnet_header(OP_RDM) + bytes([1]) + bytes(8) + bytes([net, 0, addr])
        assert len(head) == 24
        return head + rdm, self.tn

    def parse(self, data, tn, cc, pid, dest):
        if data[8:10] != struct.pack('<H', OP_RDM):
            return None
        r = data[24:]
        if len(r) < 23 or r[14] != tn or r[8:14] != dest or r[2:8] != CONTROLLER:
            return None
        rtype, cc_r, pid_r, pdl = r[15], r[19], struct.unpack('>H', r[20:22])[0], r[22]
        pd = r[23:23 + pdl]
        # QUEUED_MESSAGE is answered with the queued message's own PID, or with
        # STATUS_MESSAGES when nothing is queued (E1.20 10.3.1)
        if cc_r != cc + 1 or (pid_r != pid and pid != 0x0020):
            return Answer('mismatch: cc 0x%02x pid 0x%04x' % (cc_r, pid_r))
        word = struct.unpack('>H', pd[:2])[0] if len(pd) >= 2 else None
        kind = RESPONSE.get(rtype, 'type %d' % rtype)
        return Answer(kind, cc=cc_r, pid=pid_r, pd=pd, reason=word)

    def ask(self, dest, cc, pid, pd=b'', **kw):
        pkt, tn = self.packet(dest, cc, pid, pd, **kw)
        t0 = time.time()
        self.sock.sendto(pkt, (self.ip, ARTNET_PORT))
        until = t0 + self.timeout
        while True:
            d = self.receive(until)
            if d is None:
                return Answer('none')
            a = self.parse(d, tn, cc, pid, dest)
            if a:
                a.ms = (time.time() - t0) * 1000
                return a

    def burst(self, dest, count):
        """@p count GET DEVICE_INFO sent back to back; how many were answered"""
        sent = {}
        for _ in range(count):
            pkt, tn = self.packet(dest, GET, 0x0060)
            sent[tn] = False
            self.sock.sendto(pkt, (self.ip, ARTNET_PORT))
        until = time.time() + self.timeout + count * 0.1
        while not all(sent.values()):
            d = self.receive(until)
            if d is None:
                break
            r = d[24:]
            if (d[8:10] == struct.pack('<H', OP_RDM) and len(r) >= 23 and r[14] in sent and
                    r[8:14] == dest and r[15] == 0 and r[19] == GET + 1):
                sent[r[14]] = True
        return sum(sent.values())


class Checks:
    def __init__(self):
        self.passed = self.failed = 0

    def check(self, name, answer, want):
        """@p want: 'ACK', 'NACK <reason>', 'NACK', 'answer', 'none' or a function"""
        if callable(want):
            ok, wanted = want(answer), 'see the test'
        else:
            wanted = want
            text = str(answer)
            ok = (text.startswith(want) if want not in ('answer', 'none') else
                  (answer.kind != 'none') if want == 'answer' else answer.kind == 'none')
            if want == 'ACK':
                ok = answer.kind == 'ACK'
        ms = '' if answer.ms is None else ' (%.0f ms)' % answer.ms
        print('  %-46s %s%s%s' % (name, answer, ms, '' if ok else '   FAILED, wanted %s' % wanted))
        if ok:
            self.passed += 1
        else:
            self.failed += 1
        return ok

    def note(self, name, ok, detail):
        print('  %-46s %s%s' % (name, detail, '' if ok else '   FAILED'))
        if ok:
            self.passed += 1
        else:
            self.failed += 1


def test_device(node, uid, args, c):
    print('\n%s' % uid_text(uid))
    info = node.ask(uid, GET, 0x0060)
    c.check('GET DEVICE_INFO', info, lambda a: a.kind == 'ACK' and len(a.pd) == 19 and a.pd[:2] == b'\x01\x00')
    footprint = struct.unpack('>H', info.pd[10:12])[0] if len(info.pd) == 19 else 0
    personalities = info.pd[13] if len(info.pd) == 19 else 0
    c.check('GET DEVICE_INFO, no checksum', node.ask(uid, GET, 0x0060, checksum=False), 'ACK')
    c.check('GET SUPPORTED_PARAMETERS', node.ask(uid, GET, 0x0050),
            lambda a: a.kind in ('ACK', 'ACK_OVERFLOW'))
    c.check('GET SOFTWARE_VERSION_LABEL', node.ask(uid, GET, 0x00C0), 'ACK')
    address = node.ask(uid, GET, 0x00F0)
    if footprint:
        c.check('GET DMX_START_ADDRESS', address, lambda a: a.kind == 'ACK' and len(a.pd) == 2)
    else:
        c.check('GET DMX_START_ADDRESS (footprint 0)', address, 'answer')
    c.check('GET unknown PID 0x8123', node.ask(uid, GET, 0x8123), 'NACK UNKNOWN_PID')
    c.check('GET sub-device 0x0201 (out of range)', node.ask(uid, GET, 0x0060, sub=0x0201),
            'NACK SUB_DEVICE_OUT_OF_RANGE')
    c.check('SET DEVICE_INFO (GET only)', node.ask(uid, SET, 0x0060), 'NACK UNSUPPORTED_COMMAND_CLASS')
    c.check('GET to a UID nobody has', node.ask(NOBODY, GET, 0x0060), 'none')

    times, lost = [], 0
    for _ in range(args.repeat):
        a = node.ask(uid, GET, 0x0060)
        if a.kind == 'ACK':
            times.append(a.ms)
        else:
            lost += 1
    if times:
        detail = '%d of %d answered, %.0f / %.0f / %.0f ms (min / median / max)' % (
            len(times), args.repeat, min(times), statistics.median(times), max(times))
    else:
        detail = 'none of %d answered' % args.repeat
    c.note('GET DEVICE_INFO x %d' % args.repeat, lost == 0, detail)

    if args.burst:
        n = node.burst(uid, args.burst)
        c.note('%d GET DEVICE_INFO sent at once' % args.burst, n == args.burst,
               '%d answered' % n)

    if args.ghostlight:
        print('  -- Ghostlight responder')
        c.check('GET QUEUED_MESSAGE advisory', node.ask(uid, GET, 0x0020, b'\x02'), 'ACK')   # any PID
        c.check('GET PID 0', node.ask(uid, GET, 0x0000), 'NACK UNKNOWN_PID')
        c.check('GET sub-device 5 (none exist)', node.ask(uid, GET, 0x0060, sub=5),
                'NACK SUB_DEVICE_OUT_OF_RANGE')
        c.check('GET sub-device ALL', node.ask(uid, GET, 0x0060, sub=0xFFFF),
                'NACK SUB_DEVICE_OUT_OF_RANGE')
        c.check('GET with PDL 3 but no data', node.ask(uid, GET, 0x0060, pdl=3), 'NACK FORMAT_ERROR')
        c.check('GET PERSONALITY_DESCRIPTION past the last',
                node.ask(uid, GET, 0x00E1, bytes([personalities + 1])), 'NACK DATA_OUT_OF_RANGE')
        c.check('SET DMX_START_ADDRESS 0', node.ask(uid, SET, 0x00F0, b'\x00\x00'),
                'NACK DATA_OUT_OF_RANGE')
        c.check('SET DMX_START_ADDRESS 513', node.ask(uid, SET, 0x00F0, b'\x02\x01'),
                'NACK DATA_OUT_OF_RANGE')
        if footprint > 1:
            past = 512 - footprint + 2     # the footprint would end at slot 513
            c.check('SET DMX_START_ADDRESS %d (runs past 512)' % past,
                    node.ask(uid, SET, 0x00F0, struct.pack('>H', past)), 'NACK DATA_OUT_OF_RANGE')
        c.check('SET DMX_START_ADDRESS, 1 byte', node.ask(uid, SET, 0x00F0, b'\x01'),
                'NACK FORMAT_ERROR')
        c.check('SET IDENTIFY_DEVICE 2', node.ask(uid, SET, 0x1000, b'\x02'), 'NACK DATA_OUT_OF_RANGE')
        c.check('SET DEVICE_LABEL, 33 bytes', node.ask(uid, SET, 0x0082, b'x' * 33), 'NACK FORMAT_ERROR')
        if address.kind == 'ACK':
            c.check('GET DMX_START_ADDRESS (unchanged)', node.ask(uid, GET, 0x00F0),
                    lambda a: a.kind == 'ACK' and a.pd == address.pd)

    if args.write:
        print('  -- changes, each put back')
        label = node.ask(uid, GET, 0x0082)
        if c.check('GET DEVICE_LABEL', label, 'ACK'):
            c.check('SET DEVICE_LABEL "artnet_rdm_test"', node.ask(uid, SET, 0x0082, b'artnet_rdm_test'), 'ACK')
            c.check('GET DEVICE_LABEL', node.ask(uid, GET, 0x0082),
                    lambda a: a.kind == 'ACK' and a.pd == b'artnet_rdm_test')
            c.check('SET DEVICE_LABEL back', node.ask(uid, SET, 0x0082, label.pd), 'ACK')
            c.check('GET DEVICE_LABEL (the original)', node.ask(uid, GET, 0x0082),
                    lambda a: a.kind == 'ACK' and a.pd == label.pd)
        last = 513 - footprint     # the highest address the footprint fits at
        if address.kind == 'ACK' and footprint and last <= 1:
            print('  %-46s skipped: a %d-slot footprint only fits at 1' % ('SET DMX_START_ADDRESS', footprint))
        elif address.kind == 'ACK' and footprint:
            old = struct.unpack('>H', address.pd)[0]
            new = old + 1 if old + 1 <= last else old - 1
            c.check('SET DMX_START_ADDRESS %d' % new,
                    node.ask(uid, SET, 0x00F0, struct.pack('>H', new)), 'ACK')
            c.check('GET DMX_START_ADDRESS', node.ask(uid, GET, 0x00F0),
                    lambda a: a.kind == 'ACK' and a.pd == struct.pack('>H', new))
            c.check('SET DMX_START_ADDRESS %d (back)' % old, node.ask(uid, SET, 0x00F0, address.pd), 'ACK')
            c.check('GET DMX_START_ADDRESS (the original)', node.ask(uid, GET, 0x00F0),
                    lambda a: a.kind == 'ACK' and a.pd == address.pd)
        c.check('SET IDENTIFY_DEVICE on', node.ask(uid, SET, 0x1000, b'\x01'), 'ACK')
        c.check('GET IDENTIFY_DEVICE', node.ask(uid, GET, 0x1000), lambda a: a.kind == 'ACK' and a.pd == b'\x01')
        c.check('SET IDENTIFY_DEVICE off', node.ask(uid, SET, 0x1000, b'\x00'), 'ACK')


def uptime(ip):
    """The node's uptime from its web API, None if that isn't reachable"""
    try:
        with urllib.request.urlopen('http://%s/api/system/info' % ip, timeout=5) as r:
            return json.load(r).get('uptime_sec')
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 epilog=__doc__.split('\n\n', 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('node', help="the node's IP address")
    ap.add_argument('--universe', type=int, default=0, help='Art-Net Port-Address of the RDM port (default 0)')
    ap.add_argument('--uid', action='append', default=[], help='test this device (MMMM:DDDDDDDD); '
                    'repeat for several. Default: every device the node lists')
    ap.add_argument('--ghostlight', action='store_true',
                    help='also the checks for a Ghostlight port in RDM responder mode')
    ap.add_argument('--write', action='store_true',
                    help='also change the label, address and identify, putting each back')
    ap.add_argument('--repeat', type=int, default=20, help='GET DEVICE_INFO this many times (default 20)')
    ap.add_argument('--burst', type=int, default=30,
                    help='GET DEVICE_INFO sent at once, all to be answered (default 30, 0 = skip)')
    ap.add_argument('--timeout', type=float, default=2.0, help='seconds to wait for an answer (default 2)')
    ap.add_argument('--tod-wait', type=float, default=30, help='seconds to wait for the device list (default 30)')
    ap.add_argument('--bind', default='', help='local address to listen on (default all)')
    args = ap.parse_args()

    node = Node(args.node, args.universe, args.timeout, args.bind)
    up_before = uptime(args.node)
    if args.uid:
        uids = [uid_bytes(u) for u in args.uid]
    else:
        print('Asking %s for the devices on Port-Address %d...' % (args.node, args.universe))
        uids = node.tod(args.tod_wait)
        if not uids:
            sys.exit('No devices listed. Is the port an RDM output on that Port-Address, '
                     'with RDM on and something connected?')
        print('%d device(s): %s' % (len(uids), ', '.join(uid_text(u) for u in uids)))

    c = Checks()
    for uid in uids:
        test_device(node, uid, args, c)

    up_after = uptime(args.node)
    if up_before is not None and up_after is not None:
        c.note('node did not restart', up_after >= up_before,
               'uptime %s s -> %s s' % (up_before, up_after))
    print('\n%d passed, %d failed' % (c.passed, c.failed))
    sys.exit(0 if c.failed == 0 else 1)


if __name__ == '__main__':
    main()
