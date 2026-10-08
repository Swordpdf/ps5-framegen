# ps5-framegen

[![CI](https://github.com/Swordpdf/ps5-framegen/actions/workflows/ci.yml/badge.svg)](https://github.com/Swordpdf/ps5-framegen/actions/workflows/ci.yml)

Frame generation for PS5 homebrew on Vulkan. From two frames of a game it makes the frame halfway between them and shows it in between:
a game at 60 fps is shown at 120 on a 120 Hz display, and a game at 30 fps at 60. It needs nothing from the game but its frames: no
motion vectors and no depth. That makes it fit emulators and ports, where the game never hands those over.

It runs on the console today in two emulators:

- **[RPCS3-PS5](https://github.com/Swordpdf)** (PS3) on the RADV driver: GTA IV, inFamous, Ratchet & Clank: Quest for Booty;
- **[PS5SX2](https://github.com/Swordpdf/PS5SX2)** (PS2) on the ps5vk driver, since 2.00.

This repository is that code on its own, as a template to start from. Press **Use this template** and copy `src/` into your
renderer.

## What you get

- **The interpolator:** `ps5::framegen::interpolator` (`src/ps5_framegen.{h,cpp}`), plain Vulkan in one class. It has no dependency on
  an emulator, the console's SDK or AMD's SDK backend, and works on RADV, ps5vk, lavapipe and SwiftShader.
- **The pacing:** `ps5::framegen::pacer` (`src/ps5_framegen_pacing.h`, header-only). It decides when generated frames help and for
  how many refreshes each is shown: only at an even frame rate with room on the display, never self-feeding, and with a safety net
  that backs off when they cost an emulator its speed.
- **The shaders:** AMD FidelityFX FSR3's optical flow (SDK 2.3.0, MIT), compiled from HLSL to SPIR-V with glslang (no DXC or
  DirectX), and an interpolation pass written for this in place of FSR3's (below).
- **Tests you can run without a console:** `tools/fg-test` runs the same class and shaders on a CPU Vulkan driver against synthetic
  scenes with a known middle frame. `tests/test_pacing.cpp` checks the pacing. CI runs both on every push (`.github/workflows/ci.yml`).
- **A worked integration:** `examples/present_loop.cpp`, with the pacing, the latency hold and the presents.
- **The PS5 specifics, written down:** the 120 Hz mode's param.json, ps5vk's binding numbers, swapchain depth
  ([docs/integration.md](docs/integration.md#the-ps5)).

## Why not just FSR3's interpolation

FSR3's frame interpolation expects the game's motion vectors and depth. Without them it falls back to its optical flow, whose 8x8
blocks drag whatever stays still on screen along with whatever moves behind it: the HUD, text, and the character the camera
follows. The interpolation here (`computeFrameinterpolationQuad` in
`shaders/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation.h`) tries several motions for every 2x2 quad
of pixels:

- no motion;
- FSR3's vector field;
- the optical flow of the quad's block and of its 4 neighbours;
- the quad's motion in the last generated frame;
- the best of these refined to a fraction of a pixel.

Each pixel keeps the motion under which the previous and the current frame agree. A motion that is not the optical flow's own where
it takes its samples is penalised, because two dark samples can agree by chance. Where nothing agrees, something in front covers
or uncovers what is behind it, and the sample that another motion explains is dropped. A pixel that has not changed for 8 frames
(HUD, text) stays still. [docs/algorithm.md](docs/algorithm.md) has the details and the reasons, case by case.

## Layout

| Path | What |
|---|---|
| `src/ps5_framegen.h`, `src/ps5_framegen.cpp` | the interpolator: images, pipelines, the passes in order, a GPU timer |
| `src/ps5_framegen_pacing.h` | the pacer: when to generate, how many presents, the safety net |
| `src/framegen/fg_shaders.inc` | the compiled shaders (SPIR-V arrays with their bindings), made by `tools/build-shaders.sh` |
| `shaders/` | the HLSL: FSR3's optical flow (unchanged but for portability) and the cut-down interpolation passes ([shaders/README.md](shaders/README.md)) |
| `examples/present_loop.cpp` | how a renderer drives it |
| `tests/test_pacing.cpp` | the pacer's unit test (911 checks) |
| `tools/fg-test.cpp` | the host test on any Vulkan device: synthetic scenes with a known middle frame, real frames |
| `tools/check-scenes.py` | the scene regression check CI runs, with its limits |
| `tools/truth.py` | real-frame tests: the frame generated between frame i and i+2 against the real frame i+1 |
| `docs/` | the algorithm, the integration (pacing, latency, colour, PS5 specifics), testing and results |

## Quick start

On a PC with no GPU (Ubuntu 24.04 shown; any Vulkan device works):

```sh
sudo apt install mesa-vulkan-drivers libvulkan-dev   # lavapipe, a CPU Vulkan driver

CXX=g++ VULKAN_HEADERS=/usr/include tools/build-fg-test.sh build/fg-test
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
build/fg-test --scene all out.png                     # a head, a HUD and a square over a panning background, scored by region
python3 tools/check-scenes.py build/fg-test           # every scene against its limits, as CI does
c++ -std=c++17 -Isrc -o build/test_pacing tests/test_pacing.cpp && build/test_pacing

# after changing shaders/ (glslang 16.2.0 with HLSL; Ubuntu's 15.x can't parse FSR3's HLSL)
GLSLANG=glslangValidator tools/build-shaders.sh       # -> src/framegen/fg_shaders.inc
```

In a renderer (see [docs/integration.md](docs/integration.md) and `examples/present_loop.cpp`):

```cpp
ps5::framegen::interpolator fg;
ps5::framegen::pacer pacing;
fg.create(physical_device, device, width, height, error, swapchain_format);  // the size of the picture shown

// every new frame of the game (ticks: the source clock's vblanks since the last one; already: presents made since then):
const auto d = pacing.frame(ticks, already, source_hz, display_hz, now_seconds, speed_percent, limiter_nominal);
if (!d.engaged) { present_the_game_frame(); return; }
fg.prepare(cmd);
draw_this_frame_into(fg.frame_image());                // exactly as it will be shown (colour, scaling)
if (fg.record(cmd, d.reset))                            // optical flow always, the in-between frame unless reset
    present(fg.output_image(), d.presents);             // the in-between frame first...
present_the_game_frame();                               // ...then the game's frame as usual
fg.advance();
```

## On the PS5

- Vulkan 1.1 with `shaderStorageImageExtendedFormats` and subgroup quad operations in compute. RADV and ps5vk both have them.
- **120 Hz:** the drivers offer the 119.88 Hz mode only to an app whose `sce_sys/param.json` has `"attribute3": 524352`. Without
  it you get 60 Hz, where only 30 fps games gain anything.
- **ps5vk:** `create()` renumbers the shaders' bindings 0..n-1, which ps5vk's pipeline compiler needs. RADV doesn't mind.

[docs/integration.md](docs/integration.md#the-ps5) has the details.

## Cost and latency

- **GPU time:** on the PS5 at 3840x2160, about 2.1-2.5 ms per generated frame (RADV). The status line splits it into the optical flow
  and the interpolation.
- **Input lag:** interpolation shows each game frame half a frame later than it would be shown otherwise (8 ms at 60 fps, 17 ms at
  30), plus the GPU time. Nothing can remove that half frame.
- **What the integration avoids on top of that:** a FIFO swapchain that fills up (three refreshes more, 25 ms at 120 Hz, when a
  game runs a hair faster than the display). [docs/integration.md](docs/integration.md#latency) has the hold that prevents it and
  the fallback that drains it.

## Results

On real GTA IV frames (1920x1080, the frame between frames i and i+2 against the real i+1, aligned per block):

| Version | Error | Bad pixels | Specks | PSNR |
|---|---|---|---|---|
| plain blend of the two frames | 3.913 | 92,189 | 516 | 34.44 |
| FSR3-style single candidate set (m73d) | 1.200 | 13,585 | 1,749 | 40.66 |
| m73g (+ neighbours, + consistency penalty) | 1.259 | 13,279 | 1,730 | 40.68 |
| **m74 (this)** | **1.237** | **12,887** | **1,449** | **40.72** |

On the synthetic `all` scene CI runs, the generated frame's error is 0.22 against 4.93 for a plain blend, and 3.2-3.5 against 66 at
the moving square's edge. [docs/testing.md](docs/testing.md) has every scene, the variants that were tried and dropped, and how to
run all of it.

## Licences

- **`shaders/`:** AMD FidelityFX SDK files under the MIT licence ([shaders/LICENSE.txt](shaders/LICENSE.txt)). The changes and the
  new passes there are under the same licence.
- **`src/`, `tests/`, `tools/`, `examples/`, `docs/`:** GPL-3.0-or-later ([COPYING](COPYING)), copyright swordpdf: a project
  that includes this code is distributed under the GPL 3 as well (as PCSX2 and PS5SX2 are). Ask if you need another licence for a
  non-GPL project.
- **`tools/stb/`:** stb_image and stb_image_write (public domain or MIT, as stated in the files).

Nothing here comes from Sony's SDK. The code uses only Khronos Vulkan.
