# pw-video-simple-interface

[English](README.md) · [简体中文](README.zh-CN.md)

A small C++ static library that publishes a caller-rendered frame as a **PipeWire video node**,
selectable in OBS through [obs-pwvideo](https://github.com/tasokait/obs-pwvideo). It registers
the node, handles buffer and frame-rate negotiation, and calls back for each frame a consumer
pulls. Rendering, layout, text and network I/O stay with the caller.

## API

```cpp
#include "pwvideo.hpp"

pwvideo::Options opt;
opt.width = 460;  opt.height = 690;
opt.fpsCap = 30;
opt.nodeName = "my-overlay";        // what OBS connects to
opt.nodeDescription = "My Overlay"; // what the OBS dropdown shows
opt.appName = "my-overlay-daemon";
opt.onStreaming = [](bool on) { /* consumer attached / detached */ };

pwvideo::VideoNode node(opt, [](uint8_t* dst, int stride, int w, int h) {
  // Fill w x h BGRA (premultiplied alpha) at the given stride.
});

node.start();
node.run();   // blocks until quit() or SIGINT/SIGTERM
```

| Method | Notes |
| --- | --- |
| `start()` | Connects to PipeWire and registers the node. Throws `std::runtime_error` on failure |
| `run()` | Runs the PipeWire main loop; the frame callback executes on this thread |
| `quit()` | Makes `run()` return. Callable from any thread |
| `stop()` | Stops the driver thread and releases resources; the destructor calls it. Call `quit()` and join `run()` first if it is executing on another thread |
| `streaming()` | Whether a consumer is currently consuming |

The frame callback contract:

- Runs on the PipeWire main-loop thread, not the caller's thread.
- Fills exactly `stride * h` bytes of premultiplied-alpha BGRA.
- Performs no network I/O, no blocking and no allocation.
- Is not called while no consumer is attached.
- Receives the negotiated `w`/`h`, which may differ from the configured size.

`Options` fields, `onStreaming` semantics and the full contract are documented in
[`src/pwvideo.hpp`](src/pwvideo.hpp).

## Build

```bash
make        # -> libpwvideo.a + demo
make run    # runs the demo node
```

Dependencies are distribution libraries only: `g++` and `libpipewire-0.3`, plus `pkg-config`.

## Verify

```bash
pw-dump | grep -A20 pwnode-demo

gst-launch-1.0 -q pipewiresrc target-object=pwnode-demo num-buffers=10 \
    ! video/x-raw,format=BGRA ! filesink location=/tmp/f.raw
```

The file written by the second command is `width * height * 4 * frames` bytes. In OBS, set the
source dimensions to `Options::width`/`Options::height`; alpha passes through unchanged.

## Using it as a submodule

```bash
git submodule add https://github.com/zlinux-live-util/pw-video-simple-interface.git \
    lib/pw-video-simple-interface
```

Consumers compile `lib/pw-video-simple-interface/src/*.cpp` with their own flags and add
`-I lib/pw-video-simple-interface/src`. Building the submodule's own `libpwvideo.a` in place
also works, but leaves object files inside the submodule directory.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/pwvideo.hpp` | Public API and frame callback contract |
| `src/pwvideo.cpp` | PipeWire node, driver thread, buffer and frame-rate negotiation |
| `examples/demo.cpp` | Minimal node used by the commands above |
| `docs/internals.md` | Stream setup constraints and the measurements behind them |
| `Makefile` | `libpwvideo.a` and `demo` |

## Contributing

- Behaviour changes: include the command and its output.
- Performance claims: include the measurement.
- Read [docs/internals.md](docs/internals.md) before changing the stream configuration.

## License

MIT, copyright ZokuTe (from 2026). PipeWire is used through dynamic linking and remains under
its own license (MIT, LGPL-2.1-or-later).
