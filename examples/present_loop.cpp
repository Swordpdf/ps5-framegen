// present_loop.cpp: how a Vulkan renderer drives the frame generation, as a sketch to adapt. It compiles against src/ but leaves the
// renderer's own parts, marked RENDERER, to you. docs/integration.md explains each step. PS5SX2's own version is in its
// pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp and GSRenderer.cpp, RPCS3-PS5's in rpcs3/Emu/RSX/VK/VKPresent.cpp.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5_framegen.h"
#include "ps5_framegen_pacing.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace example
{
	// RENDERER: what the sketch needs from your renderer
	struct renderer
	{
		VkPhysicalDevice physical;
		VkDevice device;
		VkSwapchainKHR swapchain;
		VkFormat swapchain_format;   // R8G8B8A8_UNORM or B8G8R8A8_UNORM
		std::uint32_t width, height; // the swapchain's size
		double display_hz;           // the display's refresh rate (119.88, 59.94, ...)
		double source_hz;            // the clock that paces the game: an emulated machine's vblank (59.94, 50), or your game loop's

		VkCommandBuffer begin_frame_commands();                         // this frame's command buffer, recording
		void draw_game_frame_into(VkCommandBuffer cmd, VkImage target); // scaled and colour-processed exactly as it is shown
		void blit_to_swapchain(VkCommandBuffer cmd, VkImage source, std::uint32_t image_index);
		void draw_overlays(VkCommandBuffer cmd, std::uint32_t image_index);
		void submit_and_present(VkCommandBuffer cmd, VkSemaphore acquired, std::uint32_t image_index);
		void present_game_frame(VkCommandBuffer cmd); // the usual present of the game's frame
		void set_frame_limit(double fps);             // 0: none
		double emulation_speed_percent();             // 100 in a native port
		bool limiter_nominal();                       // false in turbo or slow motion
		VkSemaphore next_acquire_semaphore();
	};

	struct frame_generation
	{
		ps5::framegen::interpolator fg;
		ps5::framegen::pacer pacing;
		std::uint32_t multiplier = 2;
		std::uint32_t presents_since_frame = 0; // presents since the last new frame was given to the pacer (`already`)
		bool backlog = false;
		double held_at = -1.0;

		// 2: one generated frame between two of the game's, 3: two, 4: three (smoother, one refresh more lag for each).
		bool start(renderer& r, std::uint32_t times = 2)
		{
			std::string error;
			multiplier = std::clamp<std::uint32_t>(times, 2, 4);
			// the frames in the swapchain's format, so that the renderer's own pipelines can draw into frame_image()
			if (!fg.create(r.physical, r.device, r.width, r.height, error, r.swapchain_format))
			{
				std::fprintf(stderr, "frame generation off: %s\n", error.c_str());
				return false;
			}
			pacing = {};
			return true;
		}

		// A tick that brings no new frame of the game (an emulator's vblank repeating the last frame). While frames are
		// generated it presents nothing: the generated frame took its refresh.
		void repeat(renderer& r)
		{
			if (pacing.state() == ps5::framegen::pacer_state::Generating)
				return;
			r.present_game_frame(r.begin_frame_commands());
			presents_since_frame++;
		}

		// Once per new frame of the game, instead of presenting it directly. `ticks`: source ticks since the last new frame (1 at
		// 60 fps on a 60 Hz source, 2 at 30). `frame_limit`: the game's frame limit (0: none).
		void flip(renderer& r, std::uint32_t ticks, double frame_limit)
		{
			const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
			const ps5::framegen::pacer_decision d = pacing.frame(ticks, presents_since_frame, r.source_hz, r.display_hz, now,
				r.emulation_speed_percent(), r.limiter_nominal());
			if (const auto e = pacing.take_event(); e != ps5::framegen::pacer_event::None)
				std::printf("[framegen] safety net: event %d, state %d\n", static_cast<int>(e), static_cast<int>(pacing.state()));

			VkCommandBuffer cmd = r.begin_frame_commands();

			if (!d.engaged)
			{
				// Uneven or too fast for the display, warming up, paused: the game's own frame. The interpolator gets a reset on
				// the next engaged frame (d.reset), so skipping it here is fine.
				r.present_game_frame(cmd);
				presents_since_frame = 1;
				if (held_at != frame_limit) // the user's own limit again
				{
					held_at = frame_limit;
					r.set_frame_limit(frame_limit);
				}
				return;
			}

			// 1. this frame, exactly as it will be shown
			fg.prepare(cmd);
			r.draw_game_frame_into(cmd, fg.frame_image());

			// 2. how many in-between frames: one shown d.presents times, or (multiplier > 2) up to multiplier - 1 shown once each,
			// never more than the refreshes this frame's ticks are worth (an x4 game slower than 30 fps would bunch them)
			std::uint32_t frames = d.presents > 0 ? 1 : 0;
			std::uint32_t shown_each = d.presents;
			if (multiplier > 2 && d.presents > 0)
			{
				const std::uint32_t fit = static_cast<std::uint32_t>(ticks * r.display_hz / r.source_hz + 0.03);
				const std::uint32_t left = fit > presents_since_frame ? fit - presents_since_frame : 0;
				frames = std::min(multiplier - 1, left);
				shown_each = 1;
			}
			fg.set_generated(std::max<std::uint32_t>(frames, 1));

			// 3. optical flow (always) and the first frame in between (unless reset)
			const bool made = fg.record(cmd, d.reset);

			// 4. latency: hold the game a hair under the display where the presents would fill every refresh
			hold(r, frame_limit, frames > 1 ? frames + 1.0 : d.presents + 1.0);

			// 5. the generated frames (left out once when the last frame's first acquire found the queue full)
			std::uint32_t presented = 0;
			if (made && frames > 0 && !backlog)
			{
				for (std::uint32_t i = 0; i < frames; i++)
				{
					if (i > 0)
						fg.interpolate_next(cmd, i); // the next moment, into output_image(), after the copy of the one before

					for (std::uint32_t k = 0; k < shown_each; k++)
					{
						const VkSemaphore acquired = r.next_acquire_semaphore();
						std::uint32_t index = 0;
						const auto t0 = std::chrono::steady_clock::now();
						if (vkAcquireNextImageKHR(r.device, r.swapchain, 100000000ull, acquired, VK_NULL_HANDLE, &index) < 0)
							break;
						// the frame's first present found no image free before a refresh: every image was queued or on screen
						if (presented == 0 &&
							std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 2.0)
							backlog = true;

						r.blit_to_swapchain(cmd, fg.output_image(), index);
						r.draw_overlays(cmd, index);                // or they blink
						r.submit_and_present(cmd, acquired, index); // submitted before the present waits on it
						cmd = r.begin_frame_commands();
						presented++;
					}
				}
			}
			else
				backlog = false;

			// 6. the game's frame as usual, then this frame becomes the previous one
			r.present_game_frame(cmd);
			presents_since_frame = 1;
			fg.advance();
		}

	private:
		// Where a game frame brings as many presents as it has refreshes, nothing drains a FIFO queue that a game a hair faster
		// than the display fills: hold it at display / presents - 0.05 (docs/integration.md#latency). No limit counts as tight:
		// V-Sync alone would hold the game at the display and keep the queue full.
		void hold(renderer& r, double frame_limit, double presents)
		{
			const bool tight = frame_limit <= 0.0 || r.display_hz / frame_limit - presents < 0.25;
			const double cap = r.display_hz / presents - 0.05;
			const double want = tight ? (frame_limit > 0.0 ? std::min(cap, frame_limit) : cap) : frame_limit;
			if (want != held_at)
			{
				held_at = want;
				r.set_frame_limit(want);
			}
		}
	};
} // namespace example
