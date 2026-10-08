# Testing

There is no debugger on the console, so the frame generation is developed on a PC. `tools/fg-test` runs the very same `interpolator`
and shaders on any Vulkan device. On SwiftShader (a CPU Vulkan driver) that means no GPU at all: a 1080p frame takes a few seconds.

```sh
CXX=clang++ VULKAN_HEADERS=/path/to/Vulkan-Headers/include tools/build-fg-test.sh build/fg-test
export VK_ICD_FILENAMES=/path/to/vk_swiftshader_icd.json
```

## What CI runs

`.github/workflows/ci.yml`, on every push and pull request:

| Check | Command | What fails it |
|---|---|---|
| Synthetic scenes on lavapipe | `python3 tools/check-scenes.py build/fg-test` | a region's error above its limit, a generated frame above a fifth of the plain blend's error, or B8G8R8A8 frames giving another picture than R8G8B8A8 |
| Pacing unit test | `tests/test_pacing.cpp` (911 checks), GCC and Clang with `-Werror` | any check |
| The example compiles | `examples/present_loop.cpp`, `-fsyntax-only` | a compile error |
| Shaders rebuilt | `tools/build-shaders.sh` with glslang 16.2.0, compared with `src/framegen/fg_shaders.inc` | a shader changed in `shaders/` without the `.inc` rebuilt (or the other way round) |

The same on a PC (Ubuntu 24.04: `apt install mesa-vulkan-drivers libvulkan-dev`):

```sh
CXX=g++ VULKAN_HEADERS=/usr/include tools/build-fg-test.sh build/fg-test
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json python3 tools/check-scenes.py build/fg-test
c++ -std=c++17 -Wall -Wextra -Isrc -o build/test_pacing tests/test_pacing.cpp && build/test_pacing
```

glslang 15 (Ubuntu 24.04's `glslang-tools`) can't parse FSR3's HLSL. Use 16.2.0: the Vulkan SDK's, or a source build with
`-DENABLE_HLSL=ON -DENABLE_OPT=OFF` (the workflow shows one).

The limits in `check-scenes.py` are about 1.5x today's scores on the default noise background (960x540, 12 px pan, the square at 20
px a frame). SwiftShader and lavapipe give nearly the same:

| Scene | Whole frame | Head edge | HUD | Square edge | Plain blend |
|---|---|---|---|---|---|
| `all` on SwiftShader | 0.22 | 1.27 | 0.72 | 3.16 | 4.93 |
| `all` on lavapipe | 0.22 | 1.28 | 0.73 | 3.48 | 4.93 |
| `head --head-move 6` on lavapipe | 0.29 | 1.75 | | | 4.37 |
| `hud` on lavapipe | 0.08 | | 1.04 | | 3.52 |

When a change makes a scene better, lower its limit in the same commit. When one makes it worse on purpose, say why in the commit.

## Synthetic scenes (a known middle frame)

```sh
build/fg-test --scene NAME out.png [--bg BG.png] [--pan N] [--obj N] [--head-move N] [--debug 1]
```

A 960x540 background (noise, or a 1280x720 or larger picture such as a frame of a game) pans N pixels a frame (12). On top of it, in
place on screen like a game's HUD or the character the camera follows:

| Scene | What it adds |
|---|---|
| `pan` | the background alone |
| `head` | a head and a body in the middle; `--head-move N` moves them N px a frame |
| `hud` | HUD text, a minimap and a button prompt (cut from BG.png when given) |
| `all` | the head and the HUD, and a checkerboard square moving `--obj` px a frame (20) |

The true middle frame is rendered too. The result is scored against it by region (head edge, HUD, square edge; mean absolute error,
0-255), next to the plain blend and the two frames. `out.png.cmp.png` shows A, B, the truth, the blend, the result and its error
around each region.

`--gen N` makes N frames between the last two (multi-frame generation): `out.png`, `out.png.1.png`, ... For a scene, each one is
measured against its own true frame. On the "all" scene:

| Multiplier | Moment | Error | Head edge | HUD | Square edge |
|---|---|---|---|---|---|
| x2 | 1/2 | 0.76 | 4.79 | 3.45 | 10.25 |
| x3 | 1/3 | 0.86 | 4.49 | 3.05 | 13.61 |
| x3 | 2/3 | 0.87 | 4.66 | 2.91 | 15.72 |
| x4 | 1/4 | 0.61 | 4.13 | 2.48 | 8.22 |
| x4 | 1/2 | 0.74 | 4.88 | 3.16 | 10.40 |
| x4 | 3/4 | 0.64 | 4.25 | 2.43 | 11.14 |

## Real frames

```sh
build/fg-test --seq out.png f1.png f2.png ... fN.png     # the frame between the last two
python3 tools/truth.py build/fg-test,other/fg-test frames/*.png
```

`truth.py` takes consecutive frames of a game. For each of the last frames `i`, it feeds every other frame up to `i` and compares
the frame generated between `i-2` and `i` with the real `i-1`. That is twice the motion of real use, so it is a hard test. The real
middle frame is rarely where an even motion would put it (cameras speed up and slow down), so each 32x32 block is compared after the
best shift within 6 pixels: the score is artifacts, not timing. Several fg-test builds (variants made with
`OUT=... tools/build-shaders.sh -DFG_...` and `tools/build-fg-test.sh OUT INC_DIR`) are compared in one run.

## Results of m74 (RPCS3-PS5, 2026-10-07)

These came from frames of a game, which can't be shipped here, so CI can't repeat them. Real frames: GTA IV, 1920x1080, Niko walking while the camera turns, 4 tests.

| Variant | Error | Moving | Bad (>40) | Specks | PSNR |
|---|---|---|---|---|---|
| plain blend | 3.913 | 11.92 | 92,189 | 516 | 34.44 |
| m73d: FSR3's field, its cells, the block's flow | 1.200 | 2.95 | 13,585 | 1,749 | 40.66 |
| m73g: + the 4 neighbour blocks, own match, consistency penalty | 1.259 | 2.86 | 13,279 | 1,730 | 40.68 |
| + sub-pixel refinement, off-screen samples | 1.230 | 2.85 | 12,806 | 1,310 | 40.72 |
| + a local search where nothing agrees (dropped) | 1.230 | 2.99 | 13,738 | 1,769 | 40.17 |
| + skipping unchanged quads (< 0.02) (dropped) | 1.346 | 2.85 | 12,952 | 1,448 | 40.51 |
| **m74: refinement where the error is > 0.03, off-screen, last frame's motion** | **1.237** | **2.84** | **12,887** | **1,449** | **40.72** |

Synthetic scenes (a GTA IV background; mean absolute error of the region):

| Variant | head-move 0 | head-move 2 | head-move 4 | hud: HUD | all: head | all: HUD | all: square |
|---|---|---|---|---|---|---|---|
| m73g | 4.67 | 9.73 | 8.92 | 5.58 | 4.57 | 3.04 | 11.39 |
| m74 | 5.35 | 8.14 | 9.01 | 6.75 | 4.79 | 3.45 | 10.25 |
| m74 with unchanged quads skipped (dropped) | 5.34 | 8.12 | 8.98 | 6.75 | 4.76 | 3.44 | 32.47 |

The HUD regions' extra error in m74 is in the moving background between the letters, not in the text: 0.20 on the text pixels
against 0.18 for m73g.

## On the console

RPCS3-PS5 logs a status line every 10 s (PS5SX2's `[fg]` lines: one a while, plus one whenever the pacing's state changes):

- the game's frame rate and the rate shown;
- the GPU time per generated frame, split into optical flow and interpolation;
- how often a present had to wait for an image, and the generated frames left out to shorten the queue.

It can also dump bursts of frames (real and generated, flagged) for `fg-test --seq` and `truth.py`.
