# Putting it in a renderer

`examples/present_loop.cpp` is the same as code. RPCS3-PS5's own integration is in its `rpcs3/Emu/RSX/VK/VKPresent.cpp` (the
`RPCS3_PLATFORM_PS5` block in `VKGSRender::flip`).

## The frame's picture

The interpolator works on `R8G8B8A8_UNORM` images of the size given to `create()`, normally the size of the picture shown (the
swapchain's).

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

The generated frame is presented `repeats` times, then the game's frame once (FIFO holds it until the next present):

```text
half    = (game frame interval in refreshes) / 2
repeats = 1 if half < 1.9, 2 if half < 3.8, 3 above, with 0.2 of hysteresis going down
```

| Game rate | Display | repeats | What is shown |
|---|---|---|---|
| 30 fps | 120 Hz | 2 | generated 2 refreshes, the game's 2 |
| 45 fps | 120 Hz | 1 | generated 1, the game's 1 or 2 |
| 60 fps | 120 Hz | 1 | generated 1, the game's 1 |
| any | 60 Hz | 1 | |

Measure the interval as a running mean of the time between the game's flips (0.85/0.15 here), ignoring gaps over 250 ms.

## Latency

Interpolation shows each game frame half a frame later than it would otherwise be shown (8 ms at 60 fps, 17 ms at 30), plus the
generated frame's GPU time. That half frame cannot be avoided: the frame in between needs the next one. Everything beyond it can be:

1. **No queue in the swapchain.**
   - **The problem:** with FIFO and 4 images, the application can get three presents ahead of the display. Where a game frame
     brings as many presents as it has refreshes (60 fps on 120 Hz: the generated frame and the game's, one refresh each),
     nothing drains that queue. A game a hair faster than the display (60.00 against 59.94) fills it, and from then on every frame
     is shown three refreshes later: 25 ms at 120 Hz.
   - **The hold:** when `refreshes per frame at the frame limit - presents per frame < 0.25`, hold the game at
     `display rate / presents per frame - 0.05` fps (RPCS3: its second frame limit). The queue then stays empty. Once in a while
     (every 20 s or so) a refresh shows an image twice.
   - **Where it is not needed:** at 30 fps on 120 Hz the game frame brings 3 presents for 4 refreshes, so the queue empties every
     frame anyway.
2. **Drain it if it fills anyway.** Time `vkAcquireNextImageKHR` for the generated frame's present. If it waited for a refresh
   (over 2 ms), every image was queued or on screen: leave the next generated frame out, so the queue comes down by one.
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

- **Driver:** Vulkan on the console is RADV for the PS5, presenting through VideoOut. The display is a
  `VK_KHR_display` surface, 3840x2160 at 119.88 Hz (or 59.94), FIFO, 4 swapchain images.
- **GPU time:** about 2.1-2.5 ms per generated frame at 3840x2160 (m73). The PS3 games RPCS3 runs leave most of the GPU free.
- **Testing:** there is no debugger on the console. Develop against `tools/fg-test` on SwiftShader (it runs the very same
  `interpolator` and shaders), and dump frames on the console to compare (RPCS3-PS5 dumps the real and the generated frames,
  flagged, to a zstd file).
