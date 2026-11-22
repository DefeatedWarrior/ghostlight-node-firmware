#!/usr/bin/env python3
"""Put a node on a Wi-Fi network over its USB setup console.

Asks for the network name and password on the terminal (the password is not
echoed or printed), saves them as the node's Wi-Fi profile with the USB `set`
command, asks the node to join, and prints the address it gets.

    . tools/idf_env.sh       # for pyserial
    python tools/usb_join_wifi.py /dev/cu.usbmodem1101
"""
import getpass
import json
import sys
import time

import serial

PREFIX, REPLY = '#GL ', '#GL> '


class Console:
    def __init__(self, port):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.05
        s.dtr = s.rts = True          # open without resetting the board
        s.open()
        s.rts = False
        s.dtr = False
        self.s, self.buf, self.id = s, b'', 0

    def send(self, cmd, timeout=4.0, **args):
        self.id += 1
        line = (PREFIX + json.dumps(dict(id=self.id, cmd=cmd, **args)) + '\n').encode()
        for o in range(0, len(line), 64):
            self.s.write(line[o:o + 64])
            self.s.flush()
            time.sleep(0.008)
        end = time.time() + timeout
        while time.time() < end:
            self.buf += self.s.read(4096)
            while b'\n' in self.buf:
                raw, self.buf = self.buf.split(b'\n', 1)
                text = raw.decode('utf-8', 'replace')
                if REPLY in text:
                    msg = json.loads(text[text.index(REPLY) + len(REPLY):])
                    if msg.get('id') == self.id:
                        return msg
        return None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem1101'
    c = Console(port)
    info = None
    for _ in range(10):
        info = c.send('info', 1.5)
        if info:
            break
    if not info:
        sys.exit('No answer from the node on ' + port)
    print('Node:', info.get('board'), info.get('firmware_version'))

    scan = c.send('scan', 12) or {}
    nets = scan.get('networks') or []
    for n in nets[:15]:
        print('  %-32s %4s dBm' % (n.get('ssid'), n.get('rssi')))
    ssid = input('Wi-Fi network name: ').strip()
    if not ssid:
        sys.exit('No network given')
    password = getpass.getpass('Password (not shown, empty for an open network): ')

    r = c.send('set', 6, config={'network': {'wifi_profiles': [{'ssid': ssid, 'password': password}]}})
    password = None
    if not r or not r.get('ok'):
        sys.exit('Saving the network failed: %s' % (r and r.get('error')))
    r = c.send('connect', 6, ssid=ssid)
    if not r or not r.get('ok'):
        sys.exit('Connect failed: %s' % (r and r.get('error')))

    print('Joining', ssid, '...')
    end = time.time() + 45
    while time.time() < end:
        time.sleep(2)
        info = c.send('info', 2) or {}
        wifi = (info.get('network') or {}).get('wifi') or {}
        if wifi.get('ip'):
            print('Joined. Node address:', wifi['ip'])
            return
        if wifi.get('state') == 'off' and wifi.get('reason'):
            sys.exit('Could not join (reason %s). Check the password and try again.' % wifi['reason'])
    sys.exit('No address after 45 s')


if __name__ == '__main__':
    main()
