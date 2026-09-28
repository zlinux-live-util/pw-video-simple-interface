// 示例：把一块自己画的 BGRA 帧发布成 PipeWire 视频节点。
//
// 跑起来之后：
//   pw-dump | grep -A20 pwnode-demo          # 看节点在不在、属性对不对
//   gst-launch-1.0 -q pipewiresrc target-object=pwnode-demo num-buffers=10
//       ! video/x-raw,format=BGRA ! filesink location=/tmp/f.raw
//   # 落盘字节数应等于 320*240*4*10
//
// OBS 侧：用 obs-pwvideo 插件的「PipeWire Video」源，下拉框里显示的是
// nodeDescription（Demo Node），实际连接的是 nodeName（pwnode-demo）。
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
    std::printf("消费者 %s\n", streaming ? "接入" : "断开");
    std::fflush(stdout);
  };

  pwvideo::VideoNode node(opt, [&](uint8_t* dst, int stride, int w, int h) {
    // 一帧热路径：只写像素，不联网、不加锁、不分配。
    const uint64_t t = ticks.fetch_add(1, std::memory_order_relaxed);
    const int bar = static_cast<int>((t * 4) % static_cast<uint64_t>(w));
    for (int y = 0; y < h; ++y) {
      uint8_t* row = dst + static_cast<size_t>(y) * stride;
      for (int x = 0; x < w; ++x) {
        const bool inBar = (x >= bar && x < bar + 24);  // 左右移动的亮条
        row[x * 4 + 0] = inBar ? 0 : static_cast<uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));
        row[x * 4 + 1] = inBar ? 220 : static_cast<uint8_t>(y * 255 / (h > 1 ? h - 1 : 1));
        row[x * 4 + 2] = inBar ? 255 : 40;
        row[x * 4 + 3] = 255;
      }
    }
  });

  node.start();
  std::printf("节点 %s 已注册 (%dx%d @ %d fps)，Ctrl-C 退出\n", opt.nodeName.c_str(), kW,
              kH, opt.fpsCap);
  std::fflush(stdout);
  node.run();
  std::printf("主循环返回，共渲染 %llu 帧\n",
              static_cast<unsigned long long>(ticks.load()));
  return 0;
}
