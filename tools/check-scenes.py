#!/usr/bin/env python3
# check-scenes.py FG_TEST [OUT_DIR]: the regression check CI runs. It runs tools/fg-test's synthetic scenes (a known true frame) and
# fails when the generated frames get worse than the limits below, which are about 1.5x what SwiftShader and lavapipe give today
# (docs/testing.md). It also checks that frames in B8G8R8A8_UNORM (create()'s frame_format) give the very same picture.
#
# Needs a Vulkan device: VK_ICD_FILENAMES=.../lvp_icd.json (Mesa's lavapipe) or SwiftShader's ICD, or a GPU.
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-2.0-only
import os
import re
import subprocess
import sys

# name, fg-test arguments after OUT.png, limits: 'all' is the whole frame, the others fg-test's regions (mean abs error, 0-255).
# Every generated frame is also held to at most a fifth of the plain blend's error.
CASES = [
    ('pan', ['--scene', 'pan'], {'all': 0.05, 'rest': 0.05}),
    ('hud', ['--scene', 'hud'], {'all': 0.2, 'HUD': 1.4}),
    ('head', ['--scene', 'head', '--head-move', '6'], {'all': 0.5, 'head edge': 3.0}),
    ('all', ['--scene', 'all'], {'all': 0.4, 'head edge': 2.0, 'HUD': 1.1, 'square edge': 5.5}),
    ('all-x3', ['--scene', 'all', '--gen', '2'], {'all': 0.6, 'head edge': 2.0, 'HUD': 1.1, 'square edge': 13.0}),
    ('all-x4', ['--scene', 'all', '--gen', '3'], {'all': 0.4, 'head edge': 2.0, 'HUD': 1.1, 'square edge': 8.0}),
]
BLEND_SHARE = 0.2


def run(fg_test, out, args, env=None):
    cmd = [fg_test, args[0], args[1], out] + args[2:]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit('FAIL: %s exited with %d' % (' '.join(cmd), r.returncode))
    return r.stdout + r.stderr


def parse(text):
    # The first frame's table, then each further generated frame's block (--gen).
    frames = []
    m = re.search(r'interpolated ([\d.]+), plain blend of A and B ([\d.]+)', text)
    if not m:
        raise SystemExit('FAIL: no score in the output:\n' + text[-2000:])
    first = {'all': float(m.group(1)), 'blend': float(m.group(2))}
    table = text[m.end():].split('\n')
    for line in table[2:]:
        r = re.match(r'\s{2}(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+[\d.]+\s+[\d.]+\s+\d+$', line)
        if not r:
            break
        first[r.group(1).strip()] = float(r.group(2))
    frames.append(first)
    for block in re.finditer(r'mean abs error against its true frame: ([\d.]+) \(plain blend ([\d.]+)\)((?:\n {2}\S[^\n]*?\s{2,}[\d.]+)*)', text):
        f = {'all': float(block.group(1)), 'blend': float(block.group(2))}
        for line in block.group(3).strip('\n').split('\n'):
            r = re.match(r'\s{2}(\S.*?)\s{2,}([\d.]+)$', line)
            if r:
                f[r.group(1).strip()] = float(r.group(2))
        frames.append(f)
    return frames


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__ if __doc__ else 'usage: check-scenes.py FG_TEST [OUT_DIR]')
    fg_test = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else 'build/scenes'
    os.makedirs(out_dir, exist_ok=True)
    failures = 0
    for name, args, limits in CASES:
        frames = parse(run(fg_test, os.path.join(out_dir, name + '.png'), args))
        for i, f in enumerate(frames):
            label = name if len(frames) == 1 else '%s frame %d/%d' % (name, i + 1, len(frames))
            bad = []
            for key, limit in limits.items():
                if key in f and f[key] > limit:
                    bad.append('%s %.2f > %.2f' % (key, f[key], limit))
            if f['all'] > f['blend'] * BLEND_SHARE:
                bad.append('%.2f is more than %d%% of the plain blend\'s %.2f' % (f['all'], BLEND_SHARE * 100, f['blend']))
            scores = ', '.join('%s %.2f' % (k, v) for k, v in f.items() if k != 'blend')
            print('%-4s %-18s %s (blend %.2f)%s' % ('FAIL' if bad else 'ok', label, scores, f['blend'],
                                                  ': ' + '; '.join(bad) if bad else ''))
            failures += 1 if bad else 0

    # The same scene with B8G8R8A8 frames: the same picture, byte for byte.
    rgba = os.path.join(out_dir, 'all.png')
    bgra = os.path.join(out_dir, 'all-bgra.png')
    run(fg_test, bgra, ['--scene', 'all'], dict(os.environ, FG_TEST_BGRA='1'))
    same = open(rgba, 'rb').read() == open(bgra, 'rb').read()
    print('%-4s %-18s B8G8R8A8 frames give %s picture' % ('ok' if same else 'FAIL', 'all-bgra', 'the same' if same else 'another'))
    failures += 0 if same else 1

    if failures:
        raise SystemExit('FAIL: %d check(s)' % failures)
    print('PASS: every scene within its limits')


if __name__ == '__main__':
    main()
