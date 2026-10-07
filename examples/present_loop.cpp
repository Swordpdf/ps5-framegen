// present_loop.cpp: how a Vulkan renderer drives the frame generation, as a sketch to adapt (it compiles against src/ps5_framegen.h
// but leaves the renderer's own parts, marked RENDERER, to you). docs/integration.md explains each step; RPCS3-PS5's own version is
// in its rpcs3/Emu/RSX/VK/VKPresent.cpp.
//
// SPDX-License-Identifier: GPL-2.0-only

#include "ps5_framegen.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace example
{
	// RENDERER: what the sketch needs from your renderer
	struct renderer
	{
		VkPhysicalDevice physical;
		VkDevice device;
		VkSwapchainKHR swapchain;
		std::uint32_t width, height; // the swapchain's size
		double display_hz;           // the display's refresh rate (119.88, 59.94, ...)

		VkCommandBuffer begin_frame_commands();                        // this frame's command buffer, recording
		void draw_game_frame_into(VkCommandBuffer cmd, VkImage target); // scaled and colour-processed exactly as it is shown
		void blit_to_swapchain(VkCommandBuffer cmd, VkImage source, std::uint32_t image_index);
		void draw_overlays(VkCommandBuffer cmd, std::uint32_t image_index);
		void submit_and_present(VkCommandBuffer cmd, VkSemaphore acquired, std::uint32_t image_index);
		void present_game_frame(VkCommandBuffer cmd);                    // the usual present of the game's frame
		void set_frame_limit(double fps);                                // 0: none
		VkSemaphore next_acquire_semaphore();
	};

	struct frame_generation
	{
		ps5::framegen::interpolator fg;
		bool reset_next = true;
		bool backlog = false;
		double interval_ms = 0.0;
		std::uint32_t repeats = 1;
		std::chrono::steady_clock::time_point last_flip{};
		double held_at = -1.0;

		bool start(renderer& r, std::uint32_t multiplier = 2) // 2: one generated frame between two of the game's, 3: two, 4: three
		{
			std::string error;
			reset_next = true;
			if (!fg.create(r.physical, r.device, r.width, r.height, error))
				return false;
			fg.set_generated(multiplier - 1);
			return true;
		}

		// Once per game frame, instead of presenting the game's frame directly. `frame_limit`: the game's frame limit (0: none).
		void flip(renderer& r, double frame_limit)
		{
			VkCommandBuffer cmd = r.begin_frame_commands();

			// 1. this frame, exactly as it will be shown
			fg.prepare(cmd);
			r.draw_game_frame_into(cmd, fg.frame_image());

			// 2. optical flow (always) and the frame in between (unless reset)
			const bool made = fg.record(cmd, reset_next);
			reset_next = false;

			// 3. pacing: the generated frame for about half of the game's frame interval, in refreshes
			const auto now = std::chrono::steady_clock::now();
			if (last_flip != std::chrono::steady_clock::time_point{})
			{
				const double dt = std::chrono::duration<double, std::milli>(now - last_flip).count();
				if (dt > 4.0 && dt < 250.0)
					interval_ms = interval_ms > 0.0 ? interval_ms * 0.85 + dt * 0.15 : dt;
			}
			last_flip = now;

			if (r.display_hz <= 90.0)
				repeats = 1;
			else if (interval_ms > 0.0)
			{
				const double half = interval_ms * r.display_hz / 1000.0 / 2.0;
				while (repeats < 3 && half >= repeats + 0.9) repeats++;
				while (repeats > 1 && half < repeats - 0.2) repeats--;
			}

			// 4. latency: where the presents fill every refresh, hold the game a hair under the display so the queue stays empty
			const std::uint32_t generated = fg.generated();
			const double presents = generated > 1 ? generated + 1.0 : repeats + 1.0;
			const bool tight = frame_limit > 0.0 && r.display_hz / frame_limit - presents < 0.25;
			const double hold = tight ? r.display_hz / presents - 0.05 : frame_limit;
			if (hold != held_at)
			{
				held_at = hold;
				r.set_frame_limit(hold);
			}

			// 5. the generated frame, `repeats` times, or the generated frames one refresh each (left out once when the last frame's
			// first acquire found the queue full)
			if (made)
			{
				const std::uint32_t shown = backlog ? 0 : generated > 1 ? generated : repeats;
				backlog = false;

				for (std::uint32_t i = 0; i < shown; i++)
				{
					if (generated > 1 && i > 0)
						fg.interpolate_next(cmd, i); // the next moment, into output_image(), after the copy of the one before

					const VkSemaphore acquired = r.next_acquire_semaphore();
					std::uint32_t index = 0;
					const auto t0 = std::chrono::steady_clock::now();
					if (vkAcquireNextImageKHR(r.device, r.swapchain, 100000000ull, acquired, VK_NULL_HANDLE, &index) < 0)
						break;
					// the frame's first present found no image free before a refresh: every image was queued or on screen
					if (i == 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 2.0)
						backlog = true;

					r.blit_to_swapchain(cmd, fg.output_image(), index);
					r.draw_overlays(cmd, index); // or they blink
					r.submit_and_present(cmd, acquired, index);
					cmd = r.begin_frame_commands();
				}
			}

			// 6. the game's frame as usual, then this frame becomes the previous one
			r.present_game_frame(cmd);
			fg.advance();
		}
	};
}
