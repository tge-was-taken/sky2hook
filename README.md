# sky2hook

A native renderer for PlayStation 2 games built on RenderWare 3.x and its `sky2` (PS2) driver.

The library sits at the engine's own API boundary. A host runs the original PS2 code however it
likes (an emulator, a recompiler) and hooks the RenderWare and sky2 entry points; sky2hook decodes
the guest structures those calls point at (atomics, world sectors, Im2D/Im3D primitives, Rt2d paths,
PTank particles, rasters, lights, cameras) and draws them with OpenGL, in its own render thread.

It never reads GIF or VIF packets to find out what to draw, with one exception: pre-instanced
native meshes, whose vertex data only exists as a VIF chain, are decoded by a VIF1 unpack emulation
(`src/pipelines/sector/vif.cpp`). Everything else comes from the draw call's arguments.

The reference is the PS2's Graphics Synthesizer as a software rasterizer emulates it, down to the
frame buffer: blending, the alpha and destination-alpha tests, per-vertex clamped lighting, the
region clamp, frame-buffer feedback. What the hardware does after the frame buffer (FSAA line
averaging, interlacing, TV filtering) is out of scope. Enhancements that deviate on purpose
(multisampling, higher-resolution shadows, per-fragment lighting, motion-trail removal, frame
interpolation) are each a toggle, and each is off or inert at PS2 native resolution, so a
pixel-comparison against the emulator stays meaningful.

## What a host has to provide

```c++
sky2::setEnvPrefix("MYGAME_");   // optional: your own prefix for the debug switches
sky2::setConfig(myToggles);      // sky2::Config, renderer toggles
sky2::setAddresses(myAddresses); // sky2::Addresses, the disc's RW/sky2 entry points and globals
sky2::setHost(myHost);           // optional: symbols and draw tagging (sky2::Host)
sky2::guest::setRam(ram, size);  // the guest's 32 MB of main RAM

sky2::Device device;             // the host owns it
sky2::api::setDevice(device);
sky2::api::open();               // starts the render thread

for (const sky2::hooks::Entry& e : sky2::hooks::table())
    if (e.address)
        myRuntimeAddHook(e);     // e.handler takes a sky2::hooks::Context
```

A handler reads only `sky2::hooks::Context`, a named view of the EE register file, so nothing in the
library knows which runtime it is hooked into. Registration, hook ordering and the thunk that fills
a `Context` belong to the host.

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

The result is a static library, `sky2hook`. Link it into the host that drives the guest code.
