// Minimal example: publishes a frame drawn by the caller as a PipeWire video node.
//
// While it runs:
//   pw-dump | grep -A20 pwnode-demo
//   gst-launch-1.0 -q pipewiresrc target-object=pwnode-demo num-buffers=10
//       ! video/x-raw,format=BGRA ! filesink location=/tmp/f.raw
// The file written should be 320*240*4*10 bytes.
//
// In OBS, select the PipeWire Video source provided by obs-pwvideo; the dropdown lists
// nodeDescription ("Demo Node") and connects to nodeName ("pwnode-demo").
#include "pwvideo.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>

int main() {
  const int kW = 320, kH = 240;
  std::atomic<uint64_t> ticks{0};

  pwvideo::Options opt;
  opt.width = kW;
  opt.height = kH;
  opt.fpsCap = 30;
  opt.nodeName = "pwnode-demo";
  opt.nodeDescription = "Demo Node";
  opt.appName = "pwvideo-demo";
  opt.verbose = true;
  opt.onStreaming = [](bool streaming) {
    std::printf("consumer %s\n", streaming ? "attached" : "detached");
    std::fflush(stdout);
  };

  pwvideo::VideoNode node(opt, [&](uint8_t* dst, int stride, int w, int h) {
    // Per-frame path: write pixels only, no network, no locks, no allocation.
    const uint64_t t = ticks.fetch_add(1, std::memory_order_relaxed);
    const int bar = static_cast<int>((t * 4) % static_cast<uint64_t>(w));
    for (int y = 0; y < h; ++y) {
      uint8_t* row = dst + static_cast<size_t>(y) * stride;
      for (int x = 0; x < w; ++x) {
        const bool inBar = (x >= bar && x < bar + 24);  // Moving highlight.
        row[x * 4 + 0] = inBar ? 0 : static_cast<uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));
        row[x * 4 + 1] = inBar ? 220 : static_cast<uint8_t>(y * 255 / (h > 1 ? h - 1 : 1));
        row[x * 4 + 2] = inBar ? 255 : 40;
        row[x * 4 + 3] = 255;
      }
    }
  });

  node.start();
  std::printf("node %s registered (%dx%d @ %d fps), Ctrl-C to exit\n", opt.nodeName.c_str(),
              kW, kH, opt.fpsCap);
  std::fflush(stdout);
  node.run();
  std::printf("main loop returned, %llu frames rendered\n",
              static_cast<unsigned long long>(ticks.load()));
  return 0;
}
