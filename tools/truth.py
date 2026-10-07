#!/usr/bin/env python3
# truth.py [--tests N] FG_TEST[,FG_TEST...] FRAME... : the frame generation measured on real frames of a game.
#
# FRAME...: consecutive frames of a game as PNG files, in order (a frame dump: the game's own frames, not the generated ones). The frame
# generated between frame i and frame i+2 is compared with the real frame i+1 that was left out (the test has twice the motion of a
# real use, which makes it harder, not easier). Each test also feeds the frames before, every other one, so that the history the
# interpolation keeps (still pixels, the last frame's motion) is there.
#
# The game's motion is not even (a camera speeds up and slows down), so the real frame i+1 is not exactly where an interpolation puts
# things: each 32x32 block of the result is compared with the real frame shifted by the best whole-pixel offset within 6 pixels. That
# measures artifacts (specks, ghosts, smears, tears), not timing.
#
# Printed per fg-test build: the mean error (0-255, the mean of the 3 channels), the mean where frame i and i+2 differ (moving parts),
# the pixels off by more than 40 ("bad"), the bad ones with no bad neighbours ("specks"), and the PSNR of the aligned error.
#
# Needs python3 with numpy and Pillow. FG_TEST: tools/build-fg-test.sh's result; VK_ICD_FILENAMES as for fg-test.
#
# SPDX-License-Identifier: GPL-2.0-only
import os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageFilter

def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float32)

def aligned_error(out, truth, B=32, R=6):
    h, w, _ = out.shape
    hb, wb = h // B, w // B
    o = out[:hb * B, :wb * B]
    best = np.full((hb, wb), 1e9, dtype=np.float32)
    emap = np.zeros((hb, B, wb, B), dtype=np.float32)
    tp = np.pad(truth, ((R, R), (R, R), (0, 0)), mode='edge')
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            t = tp[R + dy:R + dy + hb * B, R + dx:R + dx + wb * B]
            e4 = (np.abs(o - t).sum(axis=2) * (1.0 / 3.0)).reshape(hb, B, wb, B)
            bm = e4.mean(axis=(1, 3))
            imp = bm < best
            if imp.any():
                best = np.where(imp, bm, best)
                emap = np.where(imp[:, None, :, None], e4, emap)
    return emap.reshape(hb * B, wb * B)

def metrics(out, truth, a, b):
    e = aligned_error(out, truth)
    moving = (np.abs(a - b).mean(axis=2) > 12)[:e.shape[0], :e.shape[1]]
    big = e > 40
    med = np.asarray(Image.fromarray(np.clip(e, 0, 255).astype(np.uint8)).filter(ImageFilter.MedianFilter(5)), dtype=np.float32)
    speck = big & (med < 15)
    psnr = 10 * np.log10(255 ** 2 / np.mean(e ** 2 + 1e-6))
    return np.array([e.mean(), e[moving].mean() if moving.any() else 0.0, big.sum(), speck.sum(), psnr])

def main():
    args = sys.argv[1:]
    tests = 4
    if len(args) >= 2 and args[0] == '--tests':
        tests = int(args[1])
        args = args[2:]
    if len(args) < 4:
        print(__doc__ if __doc__ else 'usage: truth.py [--tests N] FG_TEST[,FG_TEST...] FRAME FRAME FRAME ...')
        sys.exit(2)
    binaries = args[0].split(',')
    frames = args[1:]
    # the last `tests` frames i that have a frame i-1 to compare with and two frames of history (i-2, i-4) of their parity
    picks = [i for i in range(4, len(frames))][-tests:]
    tmp = tempfile.mkdtemp()
    for fg in binaries:
        total = np.zeros(5)
        for i in picks:
            seq = [frames[j] for j in range(i % 2, i + 1, 2)]
            out = os.path.join(tmp, 'out.png')
            p = subprocess.run([fg, '--seq', out] + seq, capture_output=True, text=True)
            if 'wrote' not in p.stdout:
                print(p.stdout[-400:], p.stderr[-400:])
                sys.exit(1)
            total += metrics(load(out), load(frames[i - 1]), load(frames[i - 2]), load(frames[i]))
        n = len(picks)
        print('%-40s error %.3f  moving %.2f  bad %7d  specks %6d  PSNR %.2f' % (fg[-40:], total[0] / n, total[1] / n, total[2], total[3], total[4] / n), flush=True)

if __name__ == '__main__':
    main()
