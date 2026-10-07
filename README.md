# ps5-framegen

Frame generation for PS5 homebrew on Vulkan: from two frames of a game, the frame halfway between them, shown in between. A game held
at 30 fps is shown at 60 on a 120 Hz display, a game at 60 at 120. It needs nothing from the game but its frames: no motion vectors and
no depth. That makes it fit emulators and ports, where the game never hands those over.

It can also make 2 or 3 frames in between (multi-frame generation: 40 fps or 30 fps shown at 120), at the cost of more input lag.

It comes from [RPCS3-PS5](https://github.com/Swordpdf)'s frame generation (the PS3 emulator's PS5 port), where it runs on the
console in GTA IV, inFamous and others. This repository is that code taken out on its own, as a template to start from.

- **Optical flow:** AMD FidelityFX FSR3's (SDK 2.3.0, MIT), compiled from HLSL to SPIR-V with glslang (no DXC, no DirectX).
- **Interpolation:** written for this, in place of FSR3's (see below), running FSR3's optical-flow-only path.
- **Host side:** plain Vulkan in one class, `ps5::framegen::interpolator` (`src/ps5_framegen.{h,cpp}`). It has no dependency on
  the emulator, the console's SDK or AMD's SDK backend.

## Why not just FSR3's interpolation

FSR3's frame interpolation expects the game's motion vectors and depth. Without them it falls back to its optical flow, whose 8x8
blocks drag whatever stays still on screen along with whatever moves behind it. That includes the HUD, text and the character the
camera follows. The interpolation here (`computeFrameinterpolationQuad` in
`shaders/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation.h`) tries several motions for every 2x2 quad
of pixels:

- no motion;
- FSR3's vector field;
- the optical flow of the quad's block and of its 4 neighbours;
- the quad's motion in the last generated frame;
- the best of these refined to a fraction of a pixel.

Each pixel keeps the motion under which the previous and the current frame agree. A motion that is not the optical flow's own where
it takes its samples is penalised, because two dark samples agree by chance. Where nothing agrees, something in front covers or
uncovers what is behind it, and the sample that another motion explains is dropped. A pixel that has not changed for 8 frames (HUD,
text) stays still. [docs/algorithm.md](docs/algorithm.md) has the details and the reasons, case by case.

## Layout

| Path | What |
|---|---|
| `src/ps5_framegen.h`, `src/ps5_framegen.cpp` | the interpolator: images, pipelines, the passes in order, a GPU timer |
| `src/framegen/fg_shaders.inc` | the compiled shaders (SPIR-V arrays with their bindings), made by `tools/build-shaders.sh` |
| `shaders/` | the HLSL: FSR3's optical flow (unchanged but for portability), the cut-down interpolation passes ([shaders/README.md](shaders/README.md)) |
| `tools/fg-test.cpp` | a host test on any Vulkan device, SwiftShader included: synthetic scenes with a known middle frame, real frames |
| `tools/truth.py` | real-frame tests: the frame generated between frame i and i+2 against the real frame i+1 |
| `examples/present_loop.cpp` | how a renderer drives it: pacing, the latency hold, the presents |
| `docs/` | the algorithm, the integration (pacing, latency, colour, PS5 specifics), testing and results |

## Quick start

```sh
# shaders -> src/framegen/fg_shaders.inc (glslangValidator with HLSL: the Vulkan SDK's)
GLSLANG=glslangValidator tools/build-shaders.sh

# the host test, on a software Vulkan device (SwiftShader's ICD comes with Chromium builds)
CXX=clang++ VULKAN_HEADERS=/path/to/Vulkan-Headers/include tools/build-fg-test.sh build/fg-test
export VK_ICD_FILENAMES=/path/to/vk_swiftshader_icd.json
build/fg-test --scene all out.png            # a head, a HUD and a square over a panning background, scored by region
build/fg-test --seq out.png f1.png f2.png f3.png  # real frames in order: writes the frame between the last two
python3 tools/truth.py build/fg-test frames/*.png  # real-frame scores
```

In a renderer (see [docs/integration.md](docs/integration.md) and `examples/present_loop.cpp`):

```cpp
ps5::framegen::interpolator fg;
fg.create(physical_device, device, width, height, error);   // the size of the picture shown

// every game frame, in the frame's command buffer:
fg.prepare(cmd);
copy_or_draw_this_frame_into(fg.frame_image());              // exactly as it will be shown (colour, scaling)
if (fg.record(cmd, reset))                                    // optical flow always, interpolation unless reset
    present(fg.output_image(), repeats);                      // the in-between frame first, then the game's frame as usual
fg.advance();
```

## Cost and latency

- **GPU time:** on the PS5 at 3840x2160, about 2.1-2.5 ms per generated frame (m73). The renderer's status line splits it into
  the optical flow and the interpolation (m74).
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

[docs/testing.md](docs/testing.md) has the synthetic scenes, the variants that were tried and dropped, and how to run all of it.

## Licences

- **`shaders/`:** AMD FidelityFX SDK files under the MIT licence ([shaders/LICENSE.txt](shaders/LICENSE.txt)). The changes and
  the new passes there are under the same licence.
- **`src/`, `tools/`, `examples/`, `docs/`:** written for RPCS3-PS5 and GPL-2.0-only like it
  ([LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt)). The author can relicense this part (for example to MIT) if it should
  go into non-GPL projects.
- **`tools/stb/`:** stb_image and stb_image_write (public domain or MIT, as stated in the files).

Nothing here comes from Sony's SDK. The code uses only Khronos Vulkan and works on any Vulkan 1.1 device. On the PS5 it was run on
RADV for the PS5 (presenting through VideoOut).
