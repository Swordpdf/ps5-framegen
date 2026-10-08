// ps5-framegen: when to show generated frames and for how long (the pacing), counted in the source's own frame clock.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header-only and independent of Vulkan, so tests/test_pacing.cpp runs it on a PC. It comes from PS5SX2 (builds vk-285-133 to
// 135), where it replaced a first pacing that timed the game's frames on the wall clock between presents.
//
// Why not the wall clock: at 60 Hz that fed itself. Once a generated frame had been shown (a slow first second was enough), it
// took a refresh of its own, so the game's frames came every two refreshes. That looked like a 30 fps game that should get a
// generated frame, and so on. Ratchet & Clank ran at 30 fps and half speed, its emulation waiting on the display.
//
// Here the interval is the number of ticks between two new frames of the game: the vblanks of the machine being emulated (a
// PS2's 59.94 or 50 Hz), or of whatever clock paces the game in a port. The presents can't change that count, and a frame's
// presents never exceed the display refreshes its ticks are worth:
//     refreshes = ticks x display Hz / source Hz
// - The game's frame is one present.
// - Frames are generated only when the game's frames have two refreshes or more (the median of the last 8 intervals): a 60 fps
//   game on a 120 Hz display, or a 30 fps one at 60 Hz. A 60 fps game at 60 Hz gets none.
// - The generated frame is presented for about half of the refreshes (once at 2, twice at 4, three times at 6). The game's frame
//   stays up until the next one.
// - A frame never gets more presents than its own interval had refreshes, less the presents already made in it (the game's last
//   frame, or a repeat the renderer presented).
// - While frames are generated, ticks that only repeat the game's last frame should present nothing: the generated frame used
//   their refresh.
// - At the start, and after a load or a pause (more than 8 ticks without a new frame), 8 frames come before any is generated.
//
// It also generates only while the game's frame rate is steady: 7 of the last 8 intervals the same length to start, 6 to go on.
// In Ratchet & Clank, where frames take 1 or 2 vblanks (45-58 fps), generated frames felt worse than none. One generated frame
// before each of the game's frames of uneven length makes the motion speed up and slow down from refresh to refresh. That comes on
// top of the in-between frames' artifacts and the frame of lag. An even rate (60 to 120, 30 to 60) is what it doubles well, so
// uneven stretches show the game's own frames.
//
// The safety net is for an emulator whose speed the generated frames' GPU time can cost:
// - When the game runs below 95% of its target speed for 3 seconds while frames are generated, none are for 10 s.
// - If the game is back at full speed (97%+) in that pause, they were costing it, so the next pause is twice as long (up to 160 s;
//   back to 10 s after 5 minutes without one).
// - If it is as slow without them, the game is slow by itself there. Frames are generated again, and for a minute that speed
//   (less 3 points) is the floor.
// - Nothing is generated while the frame limiter isn't at normal speed (turbo, slow motion).
// A native port passes speed 100 and nominal true.
//
// Use (docs/integration.md#pacing):
//     ps5::framegen::pacer pacing;
//     // each new frame of the game:
//     const auto d = pacing.frame(ticks, already, source_hz, display_hz, now_seconds, speed_percent, limiter_nominal);
//     if (d.engaged) { fg.record(cmd, d.reset); present the generated frame d.presents times (none if 0); }
//     else           { the game's frame alone; the interpolator starts over (reset) on the next engaged frame }

#pragma once

#include <algorithm>
#include <cstdint>

namespace ps5::framegen
{
	struct pacer_decision
	{
		bool engaged = false;  // frames are generated for the game now: the interpolator takes every new frame
		bool reset = false;    // the interpolator's last frame is no neighbour of this one (a gap, a pause): it starts over
		uint32_t presents = 0; // presents of the generated frame before the game's, for this frame (0: none this time)
	};

	// Where the pacing is, for the log.
	enum class pacer_state
	{
		Warming,    // the start, or a load or a pause: 8 frames first
		NoRoom,     // the game's frames have fewer than two refreshes each
		Unsteady,   // the game's frames aren't of one length (fewer than 7 of the last 8, or 6 once generating)
		NotNominal, // the frame limiter isn't at normal speed
		Paused,     // the safety net's pause
		Generating,
	};

	// The safety net's news, for the log.
	enum class pacer_event
	{
		None,
		Paused,          // the game slowed while frames were generated: none for pause_seconds()
		ResumedCostly,   // the game was at full speed without them: the next pause is longer
		ResumedSlowGame, // it was as slow without them: a lower floor for a minute
	};

	class pacer
	{
	public:
		static constexpr uint32_t kGapTicks = 8;       // a longer wait between two frames: a load, a pause, a still picture
		static constexpr uint32_t kWindow = 8;         // the intervals the median is taken over; as many frames after a gap
		static constexpr double kEngage = 1.97;        // two refreshes a frame, less rounding (an HD mode's 60 Hz on 119.88: 1.998)
		static constexpr double kDisengage = 1.6;      // ...kept down to here, each frame still capped by its own interval
		static constexpr double kSlowSpeed = 95.0;     // percent of the target speed
		static constexpr double kFullSpeed = 97.0;
		static constexpr int kSlowSeconds = 3;
		static constexpr double kFirstPause = 10.0;    // seconds
		static constexpr double kMaxPause = 160.0;
		static constexpr double kPauseForget = 300.0;  // a pause this long ago no longer lengthens the next one
		static constexpr double kSettle = 2.0;         // a pause's first seconds aren't the game's speed without them yet
		static constexpr double kFloorSeconds = 60.0;
		static constexpr double kCostly = 3.0;         // points of speed a pause has to win back to blame the generated frames
		static constexpr uint32_t kSteadyStart = 7;    // intervals of the window's usual length, to start generating...
		static constexpr uint32_t kSteadyKeep = 6;     // ...and to go on

		// One new frame of the game: `ticks` since the last new frame (1 at 60 fps, 2 at 30 on a 60 Hz source), `already` the
		// presents made since the last one was given here (its own game frame and any repeat presented after it), the source's and
		// the display's rates in Hz, `now` in seconds (any steady clock), the emulation's speed in percent of its target, and
		// whether the limiter is at normal speed.
		pacer_decision frame(uint32_t ticks, uint32_t already, double source_hz, double display_hz, double now, double speed, bool nominal)
		{
			pacer_decision d;
			if (ticks == 0)
				ticks = 1;
			if (!(source_hz >= 20.0 && source_hz <= 100.0))
				source_hz = 59.94;
			if (!(display_hz >= 20.0 && display_hz <= 250.0))
				display_hz = 59.94;
			const double ratio = display_hz / source_hz;

			// A long wait: the window starts again, and so does the interpolator.
			const bool gap = ticks > kGapTicks;
			if (gap)
			{
				m_count = 0;
				m_reset = true;
			}
			else
			{
				m_window[m_next] = ticks;
				m_next = (m_next + 1) % kWindow;
				m_count = m_count < kWindow ? m_count + 1 : kWindow;
			}
			m_median = median();
			m_steady = usual_count();
			m_refreshes = m_median * ratio;
			const double now_refreshes = ticks * ratio;

			// The generated frame's presents: about half the refreshes a frame has, kept from flickering between two counts.
			const double half = m_refreshes / 2.0;
			while (m_repeats < 3 && half >= m_repeats + 0.9)
				m_repeats++;
			while (m_repeats > 1 && half < m_repeats - 0.2)
				m_repeats--;

			second(now, speed);

			const bool warm = m_count >= kWindow;
			m_room = warm && m_refreshes >= (m_room ? kDisengage : kEngage);
			// steady enough, with the same hysteresis (the state just before says which threshold).
			const bool was_generating = m_state == pacer_state::Generating || m_state == pacer_state::Paused || m_state == pacer_state::NotNominal;
			const bool steady = warm && m_steady >= (was_generating ? kSteadyKeep : kSteadyStart);
			m_state = !warm ? pacer_state::Warming : !m_room ? pacer_state::NoRoom : !steady ? pacer_state::Unsteady : !nominal ? pacer_state::NotNominal :
			          paused() ? pacer_state::Paused : pacer_state::Generating;
			d.engaged = m_state == pacer_state::Generating;
			if (!d.engaged)
			{
				m_reset = true; // the frames skipped meanwhile weren't given to the interpolator
				return d;
			}

			// This frame's presents: never more than the refreshes its own interval had, less the presents already made in it.
			const uint32_t fit = static_cast<uint32_t>(now_refreshes + 0.03);
			const uint32_t left = fit > already ? fit - already : 0;
			d.presents = std::min(m_repeats, left);
			d.reset = m_reset;
			m_reset = false;
			if (d.presents > 0)
				m_generated_this_second = true;
			return d;
		}

		pacer_event take_event()
		{
			const pacer_event e = m_event;
			m_event = pacer_event::None;
			return e;
		}

		pacer_state state() const { return m_state; }
		double median_ticks() const { return m_median; }
		uint32_t steady_frames() const { return m_steady; } // the window's intervals of its usual length
		double refreshes() const { return m_refreshes; }
		uint32_t repeats() const { return m_repeats; }
		uint32_t warm_frames() const { return m_count; }
		bool paused() const { return m_paused_until > 0.0; }
		double pause_seconds() const { return m_pause_length; }
		double slow_speed() const { return m_fg_speed; }     // the speed that started the last pause
		double pause_floor() const { return m_pause_floor; } // the floor it fell below
		double no_fg_speed() const { return m_nofg_speed; }   // the speed during the last pause
		double next_pause() const { return m_next_pause; }
		double floor(double now) const { return now < m_floor_until ? m_floor : kSlowSpeed; }

	private:
		double median() const
		{
			if (m_count == 0)
				return 0.0;
			uint32_t v[kWindow];
			for (uint32_t i = 0; i < m_count; i++)
				v[i] = m_window[(m_next + kWindow - 1 - i) % kWindow];
			std::sort(v, v + m_count);
			return (m_count & 1) ? v[m_count / 2] : (v[m_count / 2 - 1] + v[m_count / 2]) / 2.0;
		}

		// how many of the window's intervals have its most common length.
		uint32_t usual_count() const
		{
			uint32_t best = 0;
			for (uint32_t i = 0; i < m_count; i++)
			{
				uint32_t same = 0;
				for (uint32_t j = 0; j < m_count; j++)
					same += m_window[(m_next + kWindow - 1 - j) % kWindow] == m_window[(m_next + kWindow - 1 - i) % kWindow] ? 1 : 0;
				best = std::max(best, same);
			}
			return best;
		}

		// Once a second: the speed against the floor while frames are generated, and the pauses.
		void second(double now, double speed)
		{
			if (!m_second_started)
			{
				m_second_started = true;
				m_second = now;
				return;
			}
			if (now - m_second < 1.0)
				return;
			m_second = now;
			const bool generated = m_generated_this_second;
			m_generated_this_second = false;

			if (paused())
			{
				if (now >= m_pause_start + kSettle)
				{
					m_nofg_sum += speed;
					m_nofg_n++;
				}
				if (now < m_paused_until)
					return;
				m_paused_until = 0.0;
				m_last_pause_end = now;
				m_slow = 0;
				m_slow_sum = 0.0;
				m_nofg_speed = m_nofg_n > 0 ? m_nofg_sum / m_nofg_n : speed;
				m_nofg_sum = 0.0;
				m_nofg_n = 0;
				if (m_nofg_speed >= kFullSpeed && m_nofg_speed >= m_fg_speed + kCostly)
				{
					m_next_pause = std::min(m_next_pause * 2.0, kMaxPause);
					m_event = pacer_event::ResumedCostly;
				}
				else
				{
					m_floor = std::min(m_nofg_speed - kCostly, kSlowSpeed);
					m_floor_until = now + kFloorSeconds;
					m_next_pause = kFirstPause;
					m_event = pacer_event::ResumedSlowGame;
				}
				m_reset = true;
				return;
			}

			if (m_last_pause_end > 0.0 && now - m_last_pause_end >= kPauseForget)
				m_next_pause = kFirstPause;
			if (generated && speed < floor(now))
			{
				m_slow++;
				m_slow_sum += speed;
			}
			else
			{
				m_slow = 0;
				m_slow_sum = 0.0;
			}
			if (m_slow >= kSlowSeconds)
			{
				m_fg_speed = m_slow_sum / m_slow;
				m_pause_floor = floor(now);
				m_pause_length = m_next_pause;
				m_pause_start = now;
				m_paused_until = now + m_pause_length;
				m_slow = 0;
				m_slow_sum = 0.0;
				m_event = pacer_event::Paused;
			}
		}

		uint32_t m_window[kWindow] = {};
		uint32_t m_next = 0, m_count = 0;
		double m_median = 0.0;
		uint32_t m_steady = 0;
		double m_refreshes = 0.0;
		uint32_t m_repeats = 1;
		bool m_room = false;
		bool m_reset = true;
		bool m_generated_this_second = false;
		pacer_state m_state = pacer_state::Warming;
		pacer_event m_event = pacer_event::None;

		bool m_second_started = false;
		double m_second = 0.0;
		int m_slow = 0;
		double m_slow_sum = 0.0;
		double m_fg_speed = 0.0;
		double m_pause_floor = kSlowSpeed;
		double m_pause_start = 0.0;
		double m_paused_until = 0.0;
		double m_last_pause_end = 0.0;
		double m_pause_length = kFirstPause;
		double m_next_pause = kFirstPause;
		double m_nofg_sum = 0.0;
		int m_nofg_n = 0;
		double m_nofg_speed = 0.0;
		double m_floor = kSlowSpeed;
		double m_floor_until = 0.0;
	};
} // namespace ps5::framegen
