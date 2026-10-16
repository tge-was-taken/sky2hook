# sky2hook

An OpenGL renderer for PlayStation 2 games built on RenderWare 3.x and its `sky2` (PS2) driver.

## Status: abandoned experiment

This was an experiment in how far AI agents could get on a hard, measurable problem. Nearly all of
the code was written by Claude Code against a pixel-comparison harness, with the emulated PS2
Graphics Synthesizer as the reference. It is not maintained, nothing here is supported, and the
history is a research log rather than a release.

It is worth reading if you want the renderer's method or its decisions. It is not a product.

**It is not a native port, and it does not run a game by itself.** The original PS2 code still has to
execute somewhere: sky2hook only intercepts that code's RenderWare draw calls and draws them with
GL. The only host that exists is [p3fes](https://github.com/tge-was-taken/p3fes), which runs the
game in a headless PCSX2 through the
[pcsx2-hook](https://github.com/tge-was-taken/pcsx2-hook) fork. Nothing in this library knows about
PCSX2, so another host (a recompiler, say) could drive it, but none has been written.

## What it renders

Tested against a 50-scene suite across four discs: Persona 3 FES, Persona 4, Sonic Heroes (PAL) and
Shadow the Hedgehog. Each scene is a savestate plus the frame buffer the software-emulated GS
produced for it, compared per channel at PS2 native resolution.

**33 of the 50 scenes land under 10 mean absolute difference per channel; the worst is around 42.**
So: accurate most of the time, visibly wrong some of the time. The open differences are mostly
accumulating screen effects, some alpha and fog cases, and a few lighting corners. Other RenderWare
3.x sky2 discs may well work, since the hook surface is the engine's and not the game's, but only
those four were ever measured.

## How it works

The library sits at the engine's own API boundary. A host hooks the RenderWare and sky2 entry points
and hands over the register file; sky2hook decodes the guest structures those calls point at
(atomics, world sectors, Im2D/Im3D primitives, Rt2d paths, PTank particles, rasters, lights,
cameras) and draws them with OpenGL on its own render thread.

It never reads GIF or VIF packets to find out what to draw, with one exception: pre-instanced native
meshes, whose vertex data only exists as a VIF chain, are decoded by a VIF1 unpack emulation
(`src/pipelines/sector/vif.cpp`).

The reference is the GS as a software rasterizer emulates it, down to the frame buffer: blending,
the alpha and destination-alpha tests, per-vertex clamped lighting, the region clamp, frame-buffer
feedback. What the hardware does after the frame buffer (FSAA line averaging, interlacing, TV
filtering) is out of scope. Enhancements that deviate on purpose (multisampling, higher-resolution
shadows, per-fragment lighting, motion-trail removal, frame interpolation) are each a toggle, and
each is off or inert at PS2 native resolution, so a pixel comparison stays meaningful.

## What a host has to provide

```c++
sky2::setEnvPrefix("MYGAME_");   // optional: your own prefix for the debug switches
sky2::setConfig(myToggles);      // sky2::Config, renderer toggles
sky2::setAddresses(myAddresses); // sky2::Addresses, the disc's RW/sky2 entry points and globals
sky2::setHost(myHost);           // optional: symbols and draw tagging (sky2::Host)
sky2::guest::setRam(ram, size);  // the guest's 32 MB of main RAM

sky2::api::DeviceHandle device;  // owns the renderer for its scope
sky2::api::open();               // starts the render thread

for (const sky2::hooks::Entry& e : sky2::hooks::table())
    if (e.address)
        myRuntimeAddHook(e);     // e.handler takes a sky2::hooks::Context
```

A handler reads only `sky2::hooks::Context`, a named view of the EE register file, so nothing in the
library knows which runtime it is hooked into. Registration, hook ordering and the thunk that fills
a `Context` belong to the host. The per-disc addresses in `sky2::Addresses` are the host's to supply;
p3fes keeps them in a TOML file per disc serial.

## Layout

- `include/sky2/`: the public headers. `api.h` is the render boundary, `hooks.h` the handler table,
  `guest.h` the guest-RAM overlay, `renderware.h` and `gs.h` the RW and GS ABI structs, and
  `config.h` / `addresses.h` / `host.h` / `env.h` what the host injects.
- `src/`: the renderer. `device.cpp` is the composition root; `render_queue.cpp` builds a frame on
  the guest thread and `renderer.cpp` draws it on the render thread; `pipelines/` is one directory
  per RenderWare pipeline; `enhancements/` the opt-in deviations.
- `shaders/`: GLSL, read from disk at runtime (`SKY2_SHADER_DIR`), so editing one needs no rebuild.
- `thirdparty/`: glad (GL loader) and Dear ImGui (the debug overlay).

## Build

Needs clang or gcc with C++20, SDL3, zlib, xxhash, glm, and OpenGL/GLX/X11.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

The result is a static library, `sky2hook`. On its own it draws nothing: link it into a host that
runs the guest code.
