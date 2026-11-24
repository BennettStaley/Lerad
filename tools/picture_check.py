# SPDX-License-Identifier: MIT
"""Checks that the picture goes from SDR to HDR and back while playing, without a capture card.

Writes a 10-bit (P010) test picture to the exe's --input-raw, the path a
card's frames take, and follows over the app's IPC pipe what mpv shows the
frames as: the tags after the filters (video-out-params), what goes to the
screen (video-target-params), target-colorspace-hint, and that it is the same
mpv throughout (no restart). Two cases, each one start of the app in a
1280x720 window for some twenty seconds:

    signal   video.mode = auto: the picture looks like an SDR signal, then
             like an HDR one, and SDR again (strong colours and white, then
             dim and pale: what signal_kind.h tells them apart by). The app
             must follow: SDR, HDR, SDR. Each is changed --each seconds after
             the one before was shown.
    menu     what the in-game menu's Signal choice sends (script-message
             app-picture sdr|hdr|auto) on a picture that looks like HDR,
             started with video.mode = hdr: SDR, Automatic (HDR again, by the
             picture), SDR, HDR. It says how long each took to be shown.

    python tools/picture_check.py                 # both cases
    python tools/picture_check.py --case menu
    python tools/picture_check.py --set video.scaling=vsr --window 2560x1440   # with the scaling filter

What is checked is mpv's side. The swap chain's colour space is in
engine\\ReShade.log ("colour space" lines of the frame generation add-on),
printed at the end with the app's own "picture:" lines. On a first run
ReShade compiles its effects once for SDR and once for HDR at this size (the
picture stops for some seconds each time): run it twice for the times.

Needs numpy. No ffmpeg.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

import numpy as np

from app_name import NAME, EXE, LOG, PIPE, APP_DIR, ENV_PREFIX

W, H, FPS = 1280, 720, 60


def p010(y, u, v):
    uv = np.empty((H // 2, W), np.uint16)
    uv[:, 0::2] = u
    uv[:, 1::2] = v
    return (y << 6).astype('<u2').tobytes() + (uv << 6).astype('<u2').tobytes()


def frames(hdr):
    """Eight frames of colour bars with a moving block (every other frame sent
    is new: dedup passes only new frames). SDR: white at code 940 and strong
    colours. HDR: nothing above code 675, the colours a fifth as far from
    grey, as a PQ picture has them."""
    bars = [(940, 0, 0), (800, -300, 80), (700, 100, -320), (600, -250, -250), (500, 250, 250), (400, -100, 320),
            (300, 300, -80), (200, 0, 0)]
    out = []
    for step in range(8):
        y = np.zeros((H, W), np.uint16)
        u = np.full((H // 2, W // 2), 512, np.uint16)
        v = np.full((H // 2, W // 2), 512, np.uint16)
        bar = W // len(bars)
        for i, (luma, cb, cr) in enumerate(bars):
            if hdr:
                luma, cb, cr = int(64 + (luma - 64) * 0.7), cb // 5, cr // 5
            y[:, i * bar:(i + 1) * bar] = luma
            u[:, i * bar // 2:(i + 1) * bar // 2] = 512 + cb
            v[:, i * bar // 2:(i + 1) * bar // 2] = 512 + cr
        x = (step * 16) % (W - 64)
        y[H - 40:H - 16, x:x + 64] = 450
        out.append(p010(y, u, v))
    return out


class Mpv:
    def __init__(self, timeout):
        deadline = time.time() + timeout
        while True:
            try:
                self.pipe = open(PIPE, 'r+b', buffering=0)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.next_id = 1
        self.buf = b''

    def __call__(self, *command):
        rid = self.next_id
        self.next_id += 1
        self.pipe.write((json.dumps({'command': list(command), 'request_id': rid}) + '\n').encode())
        while True:
            while b'\n' not in self.buf:
                self.buf += self.pipe.read(65536)
            line, self.buf = self.buf.split(b'\n', 1)
            msg = json.loads(line)
            if msg.get('request_id') == rid:
                return msg.get('data')

    def shown(self):
        """What mpv shows the frames as: (tags, to the screen, the hint, the app's word, the filters)."""
        out = self('get_property', 'video-out-params') or {}
        target = self('get_property', 'video-target-params') or {}
        vf = self('get_property', 'vf') or []
        return (f"{out.get('primaries')}/{out.get('gamma')}", f"{target.get('primaries')}/{target.get('gamma')}",
                bool(self('get_property', 'target-colorspace-hint')), self('get_property', 'user-data/app/signal') or '-',
                ' '.join(f.get('label') or f.get('name') for f in vf))


class Run:
    """The app playing the test picture until close()."""

    def __init__(self, app, hdr_picture, settings, window):
        self.app = app
        args = [os.path.join(app, EXE), '--no-dialogs', '--input-raw', '-', '--raw-format', 'p010',
                '--set', f'capture.width={W}', '--set', f'capture.height={H}', '--set', f'capture.fps={FPS}',
                '--set', 'window.mode=windowed', '--set', 'dlss5.enabled=off', '--set', 'video.scaling=mpv',
                '--set', 'video.hdr_peak_nits=1000',
                # Whatever the settings file has, each case starts from the defaults.
                '--set', 'hdr.output=auto', '--set', 'hdr.fit_to_screen=0', '--set', 'hdr.display_peak_nits=0',
                '--set', 'hdr.tone_mapping=auto', '--set', 'hdr.dynamic_peak=0']
        for s in settings:
            args += ['--set', s]
        env = dict(os.environ)
        env[ENV_PREFIX + 'SIGNAL_LOG'] = '1'
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, env=env)
        self.stop = threading.Event()
        self.pictures = {False: frames(False), True: frames(True)}
        self.hdr_picture = hdr_picture

        def feed():
            n, due = 0, time.perf_counter()
            try:
                while not self.stop.is_set():
                    self.process.stdin.write(self.pictures[self.hdr_picture][(n // 2) % 8])
                    n += 1
                    due += 1 / FPS
                    time.sleep(max(due - time.perf_counter(), 0))
            except OSError:
                pass  # the app closed

        threading.Thread(target=feed, daemon=True).start()
        self.mpv = Mpv(20)
        if window:
            self.mpv('set_property', 'geometry', window)
        self.pid = self.mpv('get_property', 'pid')
        # Until the capture's own frames are there (before them, mpv says
        # what its empty window is shown as).
        deadline = time.time() + 30
        while time.time() < deadline:
            if ((self.mpv('get_property', 'video-out-params') or {}).get('pixelformat') or '') in ('p010', 'nv12'):
                break
            time.sleep(0.05)
        self.start = time.time()
        self.changes = []  # (seconds, what mpv shows)

    def look(self):
        """Notes what mpv shows now, if it changed."""
        now = self.mpv.shown()
        if 'None' not in now[0] and (not self.changes or self.changes[-1][1] != now):
            t = time.time() - self.start
            self.changes.append((t, now))
            print(f'  {t:5.1f} s  tags {now[0]}, to the screen {now[1]}, target-colorspace-hint '
                  f'{"yes" if now[2] else "no"}, signal {now[3]}; filters: {now[4]}')
        return now

    def watch(self, seconds):
        """Follows what mpv shows for that long."""
        end = time.time() + seconds
        while time.time() < end:
            self.look()
            time.sleep(0.1)

    def until(self, want, hdr_screen, timeout):
        """Follows what mpv shows until it is this kind ('sdr' or 'hdr'), as it
        should reach the screen. The seconds it took, or None."""
        began = time.time()
        while time.time() - began < timeout:
            if kind(self.look(), hdr_screen) == want:
                return time.time() - began
            time.sleep(0.05)
        return None

    def close(self):
        same = self.mpv('get_property', 'pid') == self.pid
        try:
            self.mpv('quit')
        except (OSError, ValueError):
            pass  # mpv may close the pipe before answering
        self.stop.set()
        try:
            self.process.wait(15)
        except subprocess.TimeoutExpired:
            self.process.kill()
        return same, self.process.returncode


def check(ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {what}")
    return ok


def kind(shown, hdr_screen):
    """'sdr' or 'hdr' by the frames' tags, with a '?' unless it reaches the
    screen as it should: HDR as HDR10 on a screen with HDR on, tone mapped into
    an SDR swap chain otherwise; SDR always into an SDR swap chain."""
    tags, target, hint = shown[:3]
    hdr = tags.endswith('/pq')
    right = 'None' not in target and (
        (target.endswith('/pq') and hint) if hdr and hdr_screen else (not target.endswith('/pq') and not hint))
    return ('hdr' if hdr else 'sdr') + ('' if right else '?')


def judge(run, want, hdr_screen):
    """The kinds of picture in order, each as it should reach this screen."""
    same, code = run.close()
    seen = []
    ok = True
    for _, shown in run.changes:
        k = kind(shown, hdr_screen)
        if not seen or seen[-1] != k:
            seen.append(k)
    # A state on the way from one to the other (the tags changed, the output not yet) is no failure.
    settled = [k for i, k in enumerate(seen) if not k.endswith('?') or i == len(seen) - 1]
    settled = [k for i, k in enumerate(settled) if i == 0 or settled[i - 1] != k]
    ok &= check(settled == want, f'shown as {" -> ".join(settled)} (expected {" -> ".join(want)})')
    ok &= check(same, 'the same mpv throughout (never started again)')
    ok &= check(code == 0, f'{NAME} closed cleanly')
    return ok


def log_lines(app):
    for name, words in ((os.path.join('logs', LOG), ('picture:', 'signal: it is', 'HDR:', 'starting the picture', 'started mpv')),
                        (os.path.join('engine', 'ReShade.log'), ('colour space',))):
        path = os.path.join(app, name)
        if os.path.exists(path):
            with open(path, encoding='utf-8-sig', errors='replace') as f:
                for line in f:
                    if any(w in line for w in words):
                        print('    ' + line.rstrip()[:200])


def case_signal(a, hdr_screen):
    # Starts as the signal was "last time": SDR, which the picture then is.
    run = Run(a.app, False, ['video.mode=auto', 'video.auto_found=sdr'] + a.set, a.window)
    ok = check(run.until('sdr', hdr_screen, 30) is not None, 'starts as SDR')
    for hdr in (True, False):
        run.watch(a.each)
        print(f'  {time.time() - run.start:5.1f} s  the picture now looks like {"HDR" if hdr else "SDR"}')
        run.hdr_picture = hdr
        took = run.until('hdr' if hdr else 'sdr', hdr_screen, 40)
        ok &= check(took is not None, f'followed{"" if took is None else f" after {took:.1f} s"} '
                                      f'({"HDR is believed after 5 s" if hdr else "SDR after 1 s"}, signal_kind.h)')
    run.watch(2)
    ok &= judge(run, ['sdr', 'hdr', 'sdr'], hdr_screen)
    log_lines(a.app)
    return ok


def case_menu(a, hdr_screen):
    run = Run(a.app, True, ['video.mode=hdr'] + a.set, a.window)
    ok = check(run.until('hdr', hdr_screen, 30) is not None, 'starts as HDR')
    # Automatic comes to HDR: that is what the picture looks like.
    for word, want in (('sdr', 'sdr'), ('auto', 'hdr'), ('sdr', 'sdr'), ('hdr', 'hdr')):
        run.watch(3)
        print(f'  {time.time() - run.start:5.1f} s  menu: app-picture {word}')
        run.mpv('script-message', 'app-picture', word)
        took = run.until(want, hdr_screen, 40)
        ok &= check(took is not None, f'{want.upper()}{"" if took is None else f" after {took:.2f} s"}')
    run.watch(2)
    ok &= judge(run, ['hdr', 'sdr', 'hdr', 'sdr', 'hdr'], hdr_screen)
    log_lines(a.app)
    return ok


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--app', default=APP_DIR)
    p.add_argument('--case', action='append', choices=['signal', 'menu'])
    p.add_argument('--each', type=float, default=3, help='signal: seconds a kind of picture is shown before the next')
    p.add_argument('--window', default='', help="mpv's window size (as it comes: 1280x720)")
    p.add_argument('--set', action='append', default=[], help='another settings value, e.g. video.scaling=vsr')
    a = p.parse_args()
    system = subprocess.run([os.path.join(a.app, EXE), '--system', '--no-dialogs'], capture_output=True,
                            text=True, errors='replace').stdout
    # The screen the app plays on (the line marked so), HDR on or off in Windows.
    hdr_screen = any('HDR on' in line for line in system.splitlines() if 'plays here' in line)
    forced = [s.split('=', 1)[1] for s in a.set if s.replace(' ', '').startswith('hdr.output=')]
    if forced:
        hdr_screen = forced[-1] == 'hdr' if forced[-1] in ('hdr', 'sdr') else hdr_screen
    print(f'HDR goes to the screen as {"HDR10" if hdr_screen else "SDR (tone mapped)"}')
    ok = True
    for name in a.case or ['signal', 'menu']:
        print(f'{name}:')
        ok &= {'signal': case_signal, 'menu': case_menu}[name](a, hdr_screen)
        time.sleep(1.0)  # the last instance lets go of its lock
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
