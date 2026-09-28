# pw-video-simple-interface

[English](README.md) · [简体中文](README.zh-CN.md)

Publishes a frame you rendered as an OBS source, directly as a **PipeWire video node**.

No browser, no CEF, no HTTP, no plugin to install on the rendering side: one small C++ static
library that registers a `Stream/Output/Video` node and calls you back for each frame
[obs-pwvideo](https://github.com/tasokait/obs-pwvideo) pulls. It is the output half of
[pw-mpris-visualcard](https://github.com/zlinux-live-util/pw-mpris-visualcard), extracted
because the hard part is not the rendering — it is the four PipeWire requirements that fail
*silently*.

## Why this exists

Getting a video node to show up in OBS and actually deliver frames depends on four things that
break without any error message: the node looks fine, the state says `streaming`, the format
negotiates, and the picture stays black. All four are solved here and documented in
[docs/internals.md](docs/internals.md):

| Requirement | Symptom when missing |
| --- | --- |
| Declare `SPA_PARAM_Buffers` explicitly | `maxsize=0` buffers, out-of-bounds writes / segfault |
| `PW_STREAM_FLAG_TRIGGER` **without** `PW_STREAM_FLAG_DRIVER` | Everything reports success, 0 frames delivered |
| `PW_STREAM_FLAG_MAP_BUFFERS` | `datas[0].data` is not writable |
| `media.role = "Production"` | The node never appears in the OBS dropdown, and the plugin logs nothing |

## API

The whole public surface is one callback and one options struct:

```cpp
#include "pwvideo.hpp"

pwvideo::Options opt;
opt.width = 460;  opt.height = 690;
opt.fpsCap = 30;
opt.nodeName = "my-overlay";        // what OBS connects to
opt.nodeDescription = "My Overlay"; // what the OBS dropdown shows
opt.appName = "my-overlay-daemon";
opt.onStreaming = [](bool on) { /* reset animation clocks, etc. */ };

pwvideo::VideoNode node(opt, [](uint8_t* dst, int stride, int w, int h) {
  // Fill w×h BGRA (premultiplied alpha) at the given stride.
});

node.start();
node.run();   // blocks until quit() or SIGINT/SIGTERM
```

| Method | Notes |
| --- | --- |
| `start()` | Connect to PipeWire and register the node. Throws `std::runtime_error` on failure |
| `run()` | Blocks on the PipeWire main loop; `process` callbacks happen on this thread |
| `quit()` | Makes `run()` return. Callable from any thread |
| `stop()` | Stops the driver thread and releases resources; the destructor calls it. If `run()` is executing on another thread, call `quit()` and join it first — PipeWire requires stream teardown on the loop thread |
| `streaming()` | Whether a consumer is currently consuming |

The callback contract (also in the header, because it *is* the interface):

- Runs on the PipeWire main-loop thread, not yours — lock shared state or hand over snapshots.
- Must write exactly `stride × h` bytes of premultiplied-alpha BGRA.
- No network, no blocking, no allocation churn: it is the per-frame hot path.
- **Never called when no consumer is attached** — 0 frames means 0 cost, so do not assume it
  ticks regularly.
- The `w`/`h` you receive are the *negotiated* size and may differ from the constructor's.

## Build

```bash
make        # -> libpwvideo.a + demo
make run    # runs the demo node
```

Dependencies are distribution libraries only: `g++` and `libpipewire-0.3` (plus `pkg-config`).

## Verify

```bash
# The node exists, with the three properties obs-pwvideo filters on
pw-dump | grep -A20 pwnode-demo

# Frames really flow: bytes on disk must equal W*H*4*buffers
gst-launch-1.0 -q pipewiresrc target-object=pwnode-demo num-buffers=10 \
    ! video/x-raw,format=BGRA ! filesink location=/tmp/f.raw
```

In OBS the source is **PipeWire Video** from obs-pwvideo: the dropdown lists
`nodeDescription`, while the connection uses `nodeName`. Set the source's width and height to
match `Options::width`/`height`; alpha passes through unchanged.

## Using it as a submodule

```bash
git submodule add https://github.com/zlinux-live-util/pw-video-simple-interface.git \
    lib/pw-video-simple-interface
```

Consumers compile `lib/pw-video-simple-interface/src/*.cpp` with their own flags (one compile
unit set, no ABI to track) and add `-I lib/pw-video-simple-interface/src`. Building the
submodule's own `libpwvideo.a` in place is also fine, but it leaves object files inside the
submodule directory — prefer compiling the sources directly into the consumer's build tree.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/pwvideo.hpp` | Public API and the callback contract |
| `src/pwvideo.cpp` | PipeWire node, driver thread, frame-rate negotiation, buffer handling |
| `examples/demo.cpp` | Minimal node rendering a moving bar; used by the verification commands above |
| `docs/internals.md` | The four hard requirements, how each was pinned down, and the debug commands |
| `Makefile` | `libpwvideo.a` + `demo` |

## Scope

This library deliberately does **not** contain rendering, layout, text, HTTP or a CLI. Those
belong to the consumer; a music card and a danmaku overlay have no shared abstraction there.

## Contributing

Patches from anyone, human or model, are judged on the evidence, not the authorship:

- **Behaviour changes** need the command and its output.
- **Performance claims** need the microbenchmark and its data; unverified numbers do not go
  into the documentation.
- **The constraints in [docs/internals.md](docs/internals.md) are not negotiable** without A/B
  evidence first — each of the four cost real debugging time and fails silently.
- **State how the work was produced** (recommended, not required), so it can be traced.

## License

MIT, copyright ZokuTe (from 2026). PipeWire is used through dynamic linking and remains under
its own license (MIT, LGPL-2.1-or-later).
