# SPDX-License-Identifier: MIT
"""Checks what Glasir does with an HDR capture, end to end, without a card.

Writes a 10-bit HDR10 test picture (P010: grey bars at 0, 50, 100, 203, 400,
1000, 4000 and 10000 nits) to Glasir.exe --input-raw, the path a card's frames
take, and reads back over Glasir's IPC pipe what mpv really puts out
(video-target-params), not what its options ask for. With the Python package
imagecodecs it also presses s and measures the bars in Glasir's HDR
screenshot (.jxr: the back buffer as it goes to the screen), in nits; with Pillow, the
bars of an SDR picture too, as 8-bit values.

Each case is one start of Glasir in a 1280x720 window for a few seconds:

    auto     glasir.ini's defaults: HDR10 as the console sends it when Windows
             has HDR on for the screen (the bars come out at their own nits),
             else tone mapped to SDR
    fit      [hdr] fit_to_screen = 1 with display_peak_nits = 600: HDR10, no
             bar brighter than 600 nits (HDR screens only)
    menu     what the in-game menu's "Fit highlights to this screen" does
             while playing: fitted, then as the console sends it again (HDR
             screens only)
    sdr      [hdr] output = sdr: an SDR swap chain, tone mapped
    capture  an SDR capture (NV12): an SDR swap chain

    python tools/hdr_check.py                 # every case
    python tools/hdr_check.py --case auto --case sdr
    python tools/hdr_check.py --keep          # keep the screenshots

Needs numpy. No ffmpeg. Screenshots it makes are removed from
out\\Glasir\\Screenshots unless --keep.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

import numpy as np

try:
    import imagecodecs  # reads the .jxr screenshot
except ImportError:
    imagecodecs = None
try:
    from PIL import Image  # reads the SDR screenshot
except ImportError:
    Image = None

PIPE = r'\\.\pipe\glasir'
W, H, FPS = 1280, 720, 60
BARS = [0, 50, 100, 203, 400, 1000, 4000, 10000]  # nits
SOURCE_PEAK = 1000  # glasir.ini's video.hdr_peak_nits here: the frames' metadata


def pq(nits):
    """SMPTE ST 2084: nits to the 0-1 signal."""
    y = np.asarray(nits, dtype=np.float64) / 10000.0
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    return ((c1 + c2 * y ** m1) / (1 + c3 * y ** m1)) ** m2


def p010_frame(step):
    """Grey bars (limited range, neutral chroma) and a small moving block, so
    that every other frame is new (dedup passes only new frames)."""
    y = np.zeros((H, W), np.uint16)
    bar = W // len(BARS)
    for i, nits in enumerate(BARS):
        y[:, i * bar:(i + 1) * bar] = int(round(64 + 876 * float(pq(nits))))
    x = (step * 16) % (W - 64)
    y[H - 40:H - 16, x:x + 64] = int(round(64 + 876 * float(pq(100))))
    uv = np.full((H // 2, W), 512, np.uint16)
    return (y << 6).astype('<u2').tobytes() + (uv << 6).astype('<u2').tobytes()


def nv12_frame(step):
    y = np.zeros((H, W), np.uint8)
    bar = W // len(BARS)
    for i in range(len(BARS)):
        y[:, i * bar:(i + 1) * bar] = 16 + round(219 * i / (len(BARS) - 1))
    x = (step * 16) % (W - 64)
    y[H - 40:H - 16, x:x + 64] = 128
    return y.tobytes() + np.full((H // 2, W), 128, np.uint8).tobytes()


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

    def target(self):
        return self('get_property', 'video-target-params') or {}


def describe(t):
    return (f"{t.get('primaries')} {t.get('gamma')} {t.get('pixelformat')}, peak {t.get('max-luma')} nits "
            f"(sig-peak {t.get('sig-peak', 0):.2f})")


def bar_nits(path):
    """The bars' brightness in a Glasir HDR screenshot (scRGB, 1.0 = 80 nits)."""
    with open(path, 'rb') as f:
        image = np.asarray(imagecodecs.jpegxr_decode(f.read()), dtype=np.float64)
    h, w = image.shape[:2]
    bar = w // len(BARS)
    out = []
    for i in range(len(BARS)):
        patch = image[h // 4:h // 2, i * bar + bar // 4:(i + 1) * bar - bar // 4, :3].mean(axis=(0, 1))
        out.append(80.0 * float(0.2126 * patch[0] + 0.7152 * patch[1] + 0.0722 * patch[2]))
    return out


def bar_codes(path):
    """The bars' 8-bit grey values in an SDR screenshot."""
    image = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64)
    h, w = image.shape[:2]
    bar = w // len(BARS)
    return [float(image[h // 4:h // 2, i * bar + bar // 4:(i + 1) * bar - bar // 4].mean()) for i in range(len(BARS))]


def measure_sdr(files):
    """The bars of an SDR screenshot as 8-bit values, or None."""
    png = [f for f in files if f.endswith('.png')]
    if not png or not Image:
        return None
    codes = bar_codes(png[0])
    print('  bars on screen (8-bit): ' + ', '.join(f'{c:.0f}' for c in codes))
    return codes


class Run:
    """Glasir playing the test picture until close()."""

    def __init__(self, app, sdr_capture, settings):
        self.app = app
        args = [os.path.join(app, 'Glasir.exe'), '--no-dialogs', '--input-raw', '-', '--raw-format',
                'nv12' if sdr_capture else 'p010', '--set', f'capture.width={W}', '--set', f'capture.height={H}',
                '--set', f'capture.fps={FPS}', '--set', 'window.mode=windowed', '--set', 'dlss5.enabled=off',
                '--set', f'video.mode={"sdr" if sdr_capture else "hdr"}',
                '--set', f'video.hdr_peak_nits={SOURCE_PEAK}', '--set', 'video.scaling=mpv',
                # Whatever glasir.ini has, each case starts from the defaults.
                '--set', 'hdr.output=auto', '--set', 'hdr.fit_to_screen=0', '--set', 'hdr.display_peak_nits=0',
                '--set', 'hdr.tone_mapping=auto', '--set', 'hdr.dynamic_peak=0']
        for s in settings:
            args += ['--set', s]
        self.glasir = subprocess.Popen(args, stdin=subprocess.PIPE)
        self.stop = threading.Event()
        frames = [(nv12_frame if sdr_capture else p010_frame)(i) for i in range(8)]

        def feed():
            n, due = 0, time.perf_counter()
            try:
                while not self.stop.is_set():
                    self.glasir.stdin.write(frames[(n // 2) % len(frames)])
                    n += 1
                    due += 1 / FPS
                    time.sleep(max(due - time.perf_counter(), 0))
            except OSError:
                pass  # Glasir closed

        threading.Thread(target=feed, daemon=True).start()
        self.mpv = Mpv(20)
        time.sleep(3)  # past mpv's start

    def screenshot(self):
        """Presses s; the new files in Screenshots, once they are written."""
        shots = os.path.join(self.app, 'Screenshots')
        before = set(os.listdir(shots)) if os.path.isdir(shots) else set()
        self.mpv('keypress', 's')
        deadline = time.time() + 8
        new = []
        while time.time() < deadline:
            time.sleep(0.5)
            new = sorted(set(os.listdir(shots)) - before) if os.path.isdir(shots) else []
            if any(f.endswith('.png') for f in new):
                time.sleep(1.0)  # the .jxr is written first; let both finish
                break
        return [os.path.join(shots, f) for f in new]

    def close(self):
        try:
            self.mpv('quit')
        except (OSError, ValueError):
            pass  # mpv may close the pipe before answering
        self.stop.set()
        try:
            self.glasir.wait(15)
        except subprocess.TimeoutExpired:
            self.glasir.kill()
        return self.glasir.returncode

    def log_line(self, key):
        """The last line of Glasir's log with this in it."""
        found = ''
        try:
            with open(os.path.join(self.app, 'logs', 'glasir.log'), encoding='utf-8-sig', errors='replace') as f:
                for line in f:
                    if key in line:
                        found = line.rstrip()
        except OSError:
            pass
        return found


def check(ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {what}")
    return ok


def measure(run, keep, made):
    """The bars in nits from a screenshot, or None (no imagecodecs, or the
    output is SDR: one PNG)."""
    files = run.screenshot()
    made += files
    jxr = [f for f in files if f.endswith('.jxr')]
    if not jxr or not imagecodecs:
        kinds = ', '.join(sorted(os.path.splitext(f)[1] for f in files)) or 'none'
        print(f'  screenshot: {kinds}' + ('' if imagecodecs or not jxr else ' (no imagecodecs: bars not measured)'))
        return None, files
    nits = bar_nits(jxr[0])
    print('  bars on screen (nits): ' + ', '.join(f'{b} -> {n:.0f}' for b, n in zip(BARS, nits)))
    return nits, files


def case_auto(a, made):
    run = Run(a.app, False, [])
    ok = True
    try:
        plan = run.log_line('] HDR: ') or run.log_line('HDR: ')
        said = plan.split('HDR: ', 1)[-1]
        t = run.mpv.target()
        print(f'  Glasir: {said}\n  mpv puts out: {describe(t)}')
        if 'HDR10 to the screen' in said:
            ok &= check(t.get('gamma') == 'pq' and t.get('primaries') == 'bt.2020', 'HDR10 (BT.2020, PQ) to the screen')
            ok &= check(abs((t.get('max-luma') or 0) - SOURCE_PEAK) < 1,
                        f"the capture's own peak ({SOURCE_PEAK} nits) as metadata: nothing is tone mapped")
            ok &= check(run.mpv('get_property', 'target-colorspace-hint-mode') == 'source' and
                        run.mpv('get_property', 'target-peak') == 'auto' and
                        run.mpv('get_property', 'hdr-compute-peak') is False,
                        "mpv's options: source metadata, no target peak, no peak measuring")
            nits, files = measure(run, a.keep, made)
            ok &= check(any(f.endswith('.jxr') for f in files), 'the screenshot is an HDR one (.jxr and .png)')
            if nits:
                # Every bar as sent, the ones above the metadata's peak too.
                ok &= check(all(abs(n - b) <= max(2.0, b * 0.03) for b, n in zip(BARS, nits)),
                            'every bar leaves at its own brightness (within 3 %)')
        else:
            ok &= check(t.get('gamma') != 'pq' and t.get('primaries') == 'bt.709', 'tone mapped to SDR (BT.709)')
            _, files = measure(run, a.keep, made)
            ok &= check(not any(f.endswith('.jxr') for f in files), 'the screenshot is an SDR one (.png only)')
    finally:
        ok &= check(run.close() == 0, 'Glasir closed cleanly')
    return ok, 'HDR10 to the screen' in said


def case_fit(a, made):
    run = Run(a.app, False, ['hdr.fit_to_screen=1', 'hdr.display_peak_nits=600', 'hdr.output=hdr'])
    ok = True
    try:
        t = run.mpv.target()
        print(f"  Glasir: {run.log_line('HDR: ').split('HDR: ', 1)[-1]}\n  mpv puts out: {describe(t)}")
        ok &= check(t.get('gamma') == 'pq' and abs((t.get('max-luma') or 0) - 600) < 1, 'HDR10, peak 600 nits')
        nits, _ = measure(run, a.keep, made)
        if nits:
            ok &= check(max(nits) <= 600 * 1.03, 'no bar brighter than 600 nits')
            # The default curve (BT.2390) only rolls off the highlights.
            ok &= check(abs(nits[2] - 100) <= 3 and abs(nits[3] - 203) <= 8,
                        'the 100 and 203 nit bars stay as they are: only the highlights change')
            ok &= check(all(b >= x - 0.5 for x, b in zip(nits, nits[1:])), 'brighter bars are never darker')
    finally:
        ok &= check(run.close() == 0, 'Glasir closed cleanly')
    return ok


def case_menu(a, made):
    run = Run(a.app, False, ['hdr.output=hdr'])
    ok = True
    try:
        mpv = run.mpv
        ok &= check(abs((mpv.target().get('max-luma') or 0) - SOURCE_PEAK) < 1, 'starts as the console sends it')
        # What "Fit highlights to this screen" sends (src/overlay/menu.cpp).
        mpv('set', 'target-peak', '500')
        mpv('set', 'hdr-compute-peak', 'no')
        mpv('set', 'target-colorspace-hint-mode', 'target')
        time.sleep(1.5)
        t = mpv.target()
        print(f'  fitted: {describe(t)}')
        ok &= check(t.get('gamma') == 'pq' and abs((t.get('max-luma') or 0) - 500) < 1, 'fitted to 500 nits while playing')
        mpv('set', 'tone-mapping', 'hable')
        mpv('set', 'hdr-compute-peak', 'yes')
        time.sleep(1.0)
        ok &= check(mpv('get_property', 'tone-mapping') == 'hable' and
                    mpv('get_property', 'hdr-compute-peak') is True, 'the curve and peak measuring change while playing')
        nits, _ = measure(run, a.keep, made)
        if nits:
            ok &= check(max(nits) <= 500 * 1.03, 'no bar brighter than 500 nits')
        mpv('set', 'target-colorspace-hint-mode', 'source')
        mpv('set', 'target-peak', 'auto')
        mpv('set', 'hdr-compute-peak', 'no')
        time.sleep(1.5)
        t = mpv.target()
        print(f'  back: {describe(t)}')
        ok &= check(abs((t.get('max-luma') or 0) - SOURCE_PEAK) < 1, 'and back to as the console sends it')
        nits, _ = measure(run, a.keep, made)
        if nits:
            ok &= check(all(abs(n - b) <= max(2.0, b * 0.03) for b, n in zip(BARS, nits)),
                        'every bar at its own brightness again')
    finally:
        ok &= check(run.close() == 0, 'Glasir closed cleanly')
    return ok


def case_sdr(a, made):
    run = Run(a.app, False, ['hdr.output=sdr'])
    ok = True
    try:
        t = run.mpv.target()
        print(f"  Glasir: {run.log_line('HDR: ').split('HDR: ', 1)[-1]}\n  mpv puts out: {describe(t)}")
        ok &= check(t.get('gamma') != 'pq' and t.get('primaries') == 'bt.709', 'an SDR picture (BT.709), tone mapped')
        ok &= check(run.mpv('get_property', 'd3d11-output-csp') == 'srgb' and
                    run.mpv('get_property', 'target-colorspace-hint') is False, 'an SDR swap chain is asked for')
        _, files = measure(run, a.keep, made)
        ok &= check(len(files) == 1 and files[0].endswith('.png'), 'the screenshot is an SDR one (.png only)')
        codes = measure_sdr(files)
        if codes:
            ok &= check(codes[0] < 2 and all(b >= x for x, b in zip(codes, codes[1:])) and codes[3] > 60 and
                        codes[5] > codes[3] + 20,
                        'black is black, the bars rise, and 1000 nits is well above 203')
    finally:
        ok &= check(run.close() == 0, 'Glasir closed cleanly')
    return ok


def case_capture(a, made):
    run = Run(a.app, True, [])
    ok = True
    try:
        t = run.mpv.target()
        print(f'  mpv puts out: {describe(t)}')
        ok &= check(t.get('gamma') != 'pq' and t.get('primaries') == 'bt.709', 'an SDR capture stays SDR (BT.709)')
        _, files = measure(run, a.keep, made)
        ok &= check(len(files) == 1 and files[0].endswith('.png'), 'the screenshot is an SDR one (.png only)')
        codes = measure_sdr(files)
        if codes:
            # Limited range 16-235 opened up to 0-255 and nothing else: with
            # HDR on, Windows then shows it as bright as any SDR window.
            want = [255 * i / (len(BARS) - 1) for i in range(len(BARS))]
            ok &= check(all(abs(c - w) <= 2 for c, w in zip(codes, want)),
                        'the grey steps reach the screen as they are (no tone mapping, no brightness change)')
    finally:
        ok &= check(run.close() == 0, 'Glasir closed cleanly')
    return ok


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--app', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'Glasir'))
    p.add_argument('--case', action='append', choices=['auto', 'fit', 'menu', 'sdr', 'capture'])
    p.add_argument('--keep', action='store_true', help='keep the screenshots')
    a = p.parse_args()
    cases = a.case or ['auto', 'fit', 'menu', 'sdr', 'capture']
    system = subprocess.run([os.path.join(a.app, 'Glasir.exe'), '--system', '--no-dialogs'], capture_output=True,
                            text=True, errors='replace').stdout.strip()
    print(system)
    if not imagecodecs:
        print('imagecodecs is not installed: the bars are not measured (pip install imagecodecs)')
    hdr_screen = 'HDR on' in system
    made, ok = [], True
    try:
        for name in cases:
            if name in ('fit', 'menu') and not hdr_screen:
                print(f'{name}: skipped (Windows has HDR off for the screen)')
                continue
            print(f'{name}:')
            if name == 'auto':
                passed, was_hdr = case_auto(a, made)
                ok &= passed
                ok &= check(was_hdr == hdr_screen, 'auto follows the screen: HDR10 only with HDR on in Windows')
            else:
                ok &= {'fit': case_fit, 'menu': case_menu, 'sdr': case_sdr, 'capture': case_capture}[name](a, made)
            time.sleep(1.0)  # the last Glasir lets go of its lock
    finally:
        if not a.keep:
            for f in made:
                try:
                    os.remove(f)
                except OSError:
                    pass
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
