# Putting it in a renderer

`examples/present_loop.cpp` is the same as code. Two real integrations:

- **RPCS3-PS5:** `rpcs3/Emu/RSX/VK/VKPresent.cpp` (the `RPCS3_PLATFORM_PS5` block in `VKGSRender::flip`), on RADV.
- **PS5SX2:** `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp` and `GSRenderer.cpp`, on ps5vk, with the pacer in
  `src/ps5_framegen_pacing.h`.

Copy `src/` into your project: `ps5_framegen.{h,cpp}`, `framegen/fg_shaders.inc` and `ps5_framegen_pacing.h`. Nothing else is
needed at run time. To call Vulkan through your own loader (volk, a function table), build with
`-DPS5_FRAMEGEN_VULKAN_HEADER='"your_header.h"'`. That header must declare the `vk*` commands under their usual names.

## The device

Vulkan 1.1, with two things the shaders use that `create()` does not check:

- the `shaderStorageImageExtendedFormats` feature, enabled on the device (the optical flow's R8 and R16G16 storage images);
- subgroup quad operations in compute shaders (`VK_SUBGROUP_FEATURE_QUAD_BIT`; FSR3's luminance pyramid).

RADV, ps5vk, SwiftShader and lavapipe have both. `tools/fg-test.cpp` shows a device set up for it.

## The frame's picture

The interpolator works on images of the size given to `create()`, normally the size of the picture shown (the swapchain's). The
game's frames are `R8G8B8A8_UNORM` by default. Pass `VK_FORMAT_B8G8R8A8_UNORM` as `create()`'s last argument when your own
pipelines draw into `frame_image()` in a BGRA swapchain's format (PS5SX2 does). The passes only sample the frames, so the result
is the same (CI checks it). `output_image()` is always `R8G8B8A8_UNORM`: blit it, which converts.

1. `prepare(cmd)` once a frame. It puts its images in `VK_IMAGE_LAYOUT_GENERAL` the first time.
2. Copy, blit or draw this frame's picture into `frame_image()`, in `GENERAL` layout.
3. `record(cmd, reset)`. The optical flow always runs, because the next frame needs this one's. The interpolation runs unless
   `reset`. It returns whether `output_image()` was made. The barriers for the caller's transfer reads of `output_image()` and
   `frame_image()` are included.
4. Present `output_image()` (see the pacing below), then the game's frame as usual.
5. `advance()`. This frame becomes the previous one.

**The picture must be exactly what the game's frame will look like on screen.** Same scaling (bilinear, black borders for another
aspect ratio), same colour processing. RPCS3 applies the game's video-out gamma and limited RGB range to its frames. When the
generated frames skipped that, every other frame was brighter: GTA IV sets a gamma of 1.2 and the background seemed to flicker.

**Pass `reset = true`:**

- on the first frame;
- after the size changes;
- after frame generation was off;
- on a frame that does not follow the previous one (a cut you know about; the scene change detection catches the others).

**Overlays** (a frame counter, notifications) must be drawn on the generated frames too, or they blink.

## Pacing

V-Sync must be on, with FIFO presenting: each present is shown for at least one refresh, in order.

`ps5::framegen::pacer` (`src/ps5_framegen_pacing.h`, header-only) decides each frame. You give it the game's new frame counted in
**ticks** of the clock that paces the game, not wall-clock time:

- in an emulator, the emulated machine's vblanks since the last new frame (a PS2: 1 at 60 fps, 2 at 30, at 59.94 or 50 Hz);
- in a port, your game loop's own ticks.

```cpp
const ps5::framegen::pacer_decision d = pacing.frame(ticks, already, source_hz, display_hz, now_s, speed_pct, limiter_nominal);
// d.engaged:  generate for this frame (record() it, with d.reset)
// d.presents: show the generated frame that many times (0: none this time), then the game's frame once
```

What it does (the header has the reasons):

| Game | Display | Decision |
|---|---|---|
| 60 fps | 120 Hz | generated 1 refresh, the game's 1 |
| 30 fps | 120 Hz | generated 2, the game's 2 |
| 30 fps | 60 Hz | generated 1, the game's 1 |
| 60 fps | 60 Hz | none: no room |
| 45-58 fps, uneven | any | none: uneven frame lengths look worse with generated frames than without |
| any, after a load or at the start | | none for 8 frames |

**Why ticks and not the wall clock:** the first pacing (RPCS3-PS5's, and this example's before) timed the game's frames between
presents. At 60 Hz it fed itself. A generated frame took a refresh, the next game frame came a refresh later, and that looked like a
30 fps game that should get a generated frame. A PS2 game ran at half speed. Ticks don't depend on the presents.

`already` is the presents made since the last new frame: the game's last frame (1), plus any repeat your renderer presented. While
the pacer is generating, a tick that only repeats the game's last frame should present nothing, because the generated frame used its
refresh.

**The safety net** (for emulators): when the game falls below 95% speed for 3 s while frames are generated, the pacer pauses them
(10 s, doubling up to 160 s if they were the cause). A native port passes `speed_pct = 100`. Log `take_event()` and `state()`: they
say why nothing is generated.

## More than one frame in between (the multiplier)

`set_generated(n)` before `record()` makes n frames between two of the game's, at 1/(n+1), 2/(n+1), ... of the way:

- `record()` makes the first one;
- `interpolate_next(cmd, i)` makes the i-th of the others into the same `output_image()`, once the caller has copied the one before
  (the barriers are included);
- the first one keeps the still history and the quads' motions, and the others only read them.

Present each generated frame for one refresh, then the game's frame:

| Multiplier | Game rate | Display | Delay of the game's frame |
|---|---|---|---|
| x3 | 40 fps | 120 Hz | 2 refreshes (17 ms), against 1 at x2 |
| x4 | 30 fps | 120 Hz | 3 refreshes (25 ms), against 2 at x2 |

So more frames mean smoother motion for more input lag. RPCS3-PS5 keeps x2 as the default (ps5.yml "Frame Generation Multiplier":
2, 3 or 4). The multiplier is a most: generate as many frames as the game's frame interval has refreshes, less one
(`examples/present_loop.cpp` takes it from the ticks). A
game slower than the multiplier wants would otherwise get its generated frames bunched together and its own frame held for the rest
(x3 at 30 fps: 1/3, 2/3, 1, 1).

**Swapchain images:** with 4 of them, x4 needs one more image than is free (one on screen, the game's frame's acquired in advance,
three generated). The third acquire of a frame waits for a refresh. Count only the first acquire of a frame as a sign of a full
queue.

## Latency

Interpolation shows each game frame half a frame later than it would otherwise be shown (8 ms at 60 fps, 17 ms at 30), plus the
generated frame's GPU time. That half frame cannot be avoided: the frame in between needs the next one. Everything beyond it can be:

1. **No queue in the swapchain.**
   - **The problem:** with FIFO and 4 images, the application can get three presents ahead of the display. Where a game frame
     brings as many presents as it has refreshes (60 fps on 120 Hz: the generated frame and the game's, one refresh each),
     nothing drains that queue. A game a hair faster than the display (60.00 against 59.94) fills it, and from then on every frame
     is shown three refreshes later: 25 ms at 120 Hz.
   - **The hold:** when `refreshes per frame at the frame limit - presents per frame < 0.25`, or when there is no frame limit at
     all, hold the game at `display rate / presents per frame - 0.05` fps (RPCS3: its second frame limit). Take the user's own
     second limit into the frame limit first, as RPCS3's limiter does. The queue then stays empty. Once in a while (every 20 s or
     so) a refresh shows an image twice.
   - **Where it is not needed:** at 30 fps on 120 Hz the game frame brings 3 presents for 4 refreshes, so the queue empties every
     frame anyway. With several generated frames the presents a frame are the multiplier.
2. **Drain it if it fills anyway.** Time the first `vkAcquireNextImageKHR` of the frame's generated presents. If it waited for a
   refresh (over 2 ms), every image was queued or on screen: leave the next frame's generated frames out, so the queue comes down by
   one.
3. **Keep the GPU time low.** The status line reports it split into the optical flow and the interpolation
   (`take_gpu_time(mean, max, count, flow_mean)`), read back a few frames later without waiting.

Showing the generated frame for less time than the game's frame (to show the game's sooner) would make the motion uneven, which is
the opposite of the point.

## GPU timer

`create()` makes a timestamp query pool if the device has timestamps on its queues. `record()` writes three timestamps:

1. after the frame's own work;
2. after the optical flow;
3. after the interpolation.

They are read back `k_slots` frames later, never waiting. On the PS5 (RADV through VideoOut), `vkCmdWriteTimestamp` comes from
`vkGetDeviceProcAddr`.

## The PS5

- **Drivers:** it runs on both Vulkan drivers PS5 homebrew uses:
  - **RADV** (Mesa's AMD driver, ported to the PS5): RPCS3-PS5;
  - **ps5vk** (a native PS5 Vulkan driver): PS5SX2.
  Both present through VideoOut: a `VK_KHR_display` surface, 3840x2160 at 119.88 Hz (or 59.94), FIFO. RADV gives 4 swapchain
  images (see the multiplier above), ps5vk 2 to 3.
- **ps5vk and binding numbers:** FSR3's shaders number their bindings sparsely (0, 100, 200, 300, 405...). ps5vk sizes a set's table
  to its highest binding number and keeps a binding number in a byte. The first PS5SX2 build failed in `vkCreateComputePipelines`
  ("internal error"). `create()` renumbers every pass's bindings 0..n-1 in the SPIR-V, the set layout and the writes, so this is
  handled. A pass added through `tools/build-shaders.sh` gets the same treatment, because its bindings are read back into
  `fg_shaders.inc`.
- **120 Hz needs the app's param.json:** both drivers offer the 119.88 Hz modes only when the title's `sce_sys/param.json`
  declares high frame rates: `"attribute3": 524352` (`0x80040`), as PS5SX2 ships it. Without it, the display stays at 60 Hz and only
  30 fps games get generated frames. ps5vk reads `/app0/sce_sys/param.json`, which after the jailbreak may not lead to the app's
  folder.
  PS5SX2 sets `PS5VK_PARAM_JSON` to the first that opens of: the running app's folder as the sandbox mounts it, `/app0`, the
  install folder. After changing param.json, re-register the app, or the mode change can be refused (`0x80290016`).
- **GPU time:** about 2.1-2.5 ms per generated frame at 3840x2160 on RADV (RPCS3-PS5, m73). PS3 and PS2 games leave most of the
  GPU free.
- **Testing:** there is no debugger on the console. Develop against `tools/fg-test` on lavapipe or SwiftShader (it runs the very
  same `interpolator` and shaders). Then dump frames on the console to compare (RPCS3-PS5 dumps the real and the generated frames,
  flagged, to a zstd file).
