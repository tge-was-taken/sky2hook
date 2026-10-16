// The renderer's runtime toggles. The host owns loading them (a file, a command line, its own
// defaults) and calls setConfig() once at startup, before the device opens.
#pragma once

#include <set>

namespace sky2
{
	struct Config
	{
		bool texLog = false; // dump each decoded texture to experiments/dumps/
		bool strict = false; // abort on a would-be exception instead of logging it
		bool uvDebug = false; // render 3D meshes with texcoords as color
		int drawOnly = -1; // render just this draw index (-1 = all)
		int drawUpto = -1; // render draws 0..N inclusive (-1 = all)
		// Draws to drop, by unified index. Answers "what is this draw worth" for a set the other two
		// cannot express, e.g. every draw the hardware rejects on a test we do not model yet.
		std::set<int> drawSkip;
		int feedbackScale = 1; // screen-effect capture res as a multiple of the original PS2 buffer (1=original, 2/4=sharper)
		bool geomCache = true; // cache the parsed atomic geometry (ObjectManager); off = parse fresh each frame
		bool gpuResident = true; // keep 3D geometry GPU-resident (upload once); off = re-upload each frame
		bool sectorCache = true; // cache the world-sector parse by sector address
		bool geomArena = true; // per-frame geometry into one frame arena instead of a buffer per batch
		bool stateDedupe = true; // dedupe per-draw state into Frame::states
		bool lightDedupe = true; // dedupe gathered lighting into Frame::lights
		bool framePool = true; // reuse the frame ring's storage; off = allocate a fresh frame each frame
		bool poolPoison = false; // debug: scrub recycled frame storage so a use-after-reset shows up
		bool regionClamp = true; // honour the GS CLAMP_1 region clamp; off = plain GL sampling
		// Depth-test the Im2D path against the GS Z the game submits. Games clip UI panels by writing
		// a Z barrier and drawing the contents behind it, which has no other expression.
		bool depth2d = true;
		// The MatFX envmap reflection pass. Without it the reflective materials render with no
		// reflection at all, which is silently wrong rather than slightly wrong.
		bool matfxEnvmap = true;
		// Decoded-texture cache budget in MB. Past it the least-recently-used content is released; the
		// cache is content-keyed, so a scene that keeps producing new pixels has no other bound.
		int texCacheMb = 96;
		bool interpolate = false; // render between guest ticks instead of re-presenting the same frame
		// Upscale compensation. Inert at PS2 native resolution, so it cannot disturb a reference
		// comparison; only a render larger than 640x448 sees any of it.
		bool upscaleFilter = true; // sample point-filtered textures bilinearly when magnified
		// Renders the off-screen character-shadow silhouette at this multiple of the guest's 128x128,
		// which softens the stair-stepped edge the projected shadow inherits. 1 = stock.
		int shadowRtScale = 1;
		// Multisampling, which the PS2 has none of: 1 = off, and off is what a reference comparison uses.
		int msaa = 1; // samples for the display passes
		int msaaOffscreen = 1; // samples for off-screen camera targets (the shadow silhouette)
		// Phong instead of the GS's own per-vertex Gouraud shading: off is what a reference comparison uses.
		bool perPixelLighting = false;
		// The PS2 screen effects that sample the presented frame trail behind moving geometry, because
		// each frame adds a fraction of the last one. On (the default) is what the hardware does; off
		// redirects those taps to the frame so far and rescales them to the same steady-state
		// intensity, which drops the trail without dimming the effect.
		bool feedbackBlur = true;
		// Reject a 3D triangle with any vertex past the camera's far plane, as PS2All's VU1 does
		// through the GS ADC bit. Off leaves GL to clip it and keep the near part, which paints the
		// stale scratch data some native VIF packets carry.
		bool farCull = true;
		// Present on the display's own cadence instead of pacing with a sleep. Interpolated motion is
		// sampled once per presented frame, so an unsynchronised present lands it at an arbitrary point
		// in the scanout and judders however good the blend is.
		bool vsync = false;
		int fpsCap = 60; // present-rate cap in fps (0 = uncapped), ignored while vsync is on
		int windowWidth = 640; // initial window size, until the disc presents its own
		int windowHeight = 448;
		// Whether that size was asked for. Unasked, the renderer adopts the resolution the game
		// presents, which is not a constant: a PAL disc presents 640x512 where NTSC presents 640x448.
		bool windowSizeExplicit = false;
		bool takeFocus = true; // grab input focus when the window opens; off for unattended playback
		// Map the self-created window at all. Unattended runs leave it unmapped: under a compositor
		// that decides focus outside X (WSLg's does, and its _NET_SUPPORTED lists no
		// _NET_WM_USER_TIME), mapping a window takes focus and no X hint can prevent it. Rendering is
		// unaffected, since every pass targets an FBO and the window is only the present surface.
		bool showWindow = true;
	};

	void setConfig(const Config& c);
	const Config& config();
} // namespace sky2
