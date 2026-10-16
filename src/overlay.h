// Dear ImGui debug overlay for sky2: a self-contained GL3 renderer + SDL3 input translation (the
// stock backends aren't vendored) and a debug window to inspect/tweak the live RwCamera and RpLights.
// init/frame run on the render thread (GL context there); input events are forwarded from the app
// thread via pushEvent and drained each frame.
#pragma once

#include "common.h" // GL types

#include <SDL3/SDL.h> // SDL_Event, stored by value in the queue

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sky2
{
	class Device;

	// SDL3's SDL_TextInputEvent.text is an SDL-owned pointer valid only during event processing, so
	// copy the string at push time (the render thread drains these later).
	struct QueuedEvent
	{
		SDL_Event ev;
		std::string text;
	};

	struct CameraLock
	{
		bool active = false;
		float vw[2] = {};
		float nearP = 0, farP = 0;
	};

	struct ToggleLock
	{
		bool active = false;
		int32_t value = 0;
	};

	// All overlay state, with the thread that writes each group.
	struct ImguiOverlay
	{
		Device* device = nullptr; // injected: the queue, renderer and shaders the overlay inspects

		void init(); // render thread, GL context current
		void shutdown();
		void frame(int fbWidth, int fbHeight); // render thread: drain input, build UI, draw
		void pushEvent(const void* sdlEvent); // app thread: forward an SDL_Event*
		void toggle(); // show/hide the debug window
		bool wantsInput() const; // app thread: true when the UI is capturing mouse/keyboard

		// Lifecycle. `ready` is render thread only; `show` is flipped by toggle() on the app thread
		// and read by the render thread while building the window.
		bool ready = false;
		bool show = false;
		bool openAll = false; // P3FES_IMGUI_OPEN: expand every section once, for a scripted run

		// The overlay's own GL objects. Render thread only: the GL context lives there.
		GLuint prog = 0, vao = 0, vbo = 0, ebo = 0;
		GLint uProj = -1, uTex = -1;

		// Pushed by the app thread, drained by the render thread each frame; evMutex guards both.
		std::mutex evMutex;
		std::vector<QueuedEvent> events;

		// Written by the render thread after building the UI, read by the app thread to decide
		// whether an input event goes to the overlay or the game.
		std::atomic<bool> wantInput{false};

		// UI hold/selection state, render thread only. camLock/togLock hold edited values and
		// enforceLocks re-writes them into guest RAM every frame so the game's per-frame updates
		// don't undo them. selectedPass is the pass the camera/lights sections inspect; -1 means
		// the main camera global, which is "the scene camera" rather than a specific pass.
		CameraLock camLock;
		ToggleLock togLock[2];
		// A rolling window of per-frame samples. Totals say what a session cost; these say when, which
		// is the only way to tell a load spike from a steady leak.
		struct History
		{
			static constexpr int SAMPLES = 300; // ~5 seconds at 60 fps
			float v[SAMPLES] = {};
			int head = 0; // next write, and the oldest sample: PlotLines takes it as the offset
			float last = 0.0f, peak = 0.0f;
			void push(float x)
			{
				v[head] = x;
				head = (head + 1) % SAMPLES;
				last = x;
				peak = x > peak ? x : peak;
			}
			float windowPeak() const
			{
				float m = 0.0f;
				for (float x : v)
					m = x > m ? x : m;
				return m;
			}
		};

		// One cumulative nanosecond counter, differenced per overlay frame. The peak is kept because a
		// spike lasting one frame is unreadable otherwise.
		struct CostChannel
		{
			uint64_t prevNs = 0;
			float ms = 0.0f, peakMs = 0.0f;
			History hist;
			void sample(uint64_t nowNs)
			{
				ms = float(nowNs - prevNs) / 1.0e6f;
				prevNs = nowNs;
				peakMs = ms > peakMs ? ms : peakMs;
				hist.push(ms);
			}
		};

		// A cumulative event counter as a per-frame rate. The first sample is dropped rather than
		// charging a whole session's worth of events to one frame.
		struct RateChannel
		{
			uint64_t prev = 0;
			bool primed = false;
			History hist;
			void sample(uint64_t total)
			{
				hist.push(primed && total > prev ? float(total - prev) : 0.0f);
				prev = total;
				primed = true;
			}
		};

		struct TexCost
		{
			CostChannel hash, decode, upload;
			RateChannel decodes, swept;
			History resident, cacheMb;
		};
		TexCost texCost;
		struct GeomCost
		{
			CostChannel parse, upload;
			RateChannel parses, uploads, creates, evictions, im3dBatches;
			History poolMb;
		};
		GeomCost geomCost;
		int selectedPass = -1;
	};

	// The one overlay. It is render-thread state and belongs in Renderer as a member; until Renderer

	// Render-thread lifecycle, called from the render loop.
} // namespace sky2
