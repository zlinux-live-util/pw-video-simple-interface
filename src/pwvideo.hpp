#pragma once
// pw-video-simple-interface —— 把一个「调用方渲染好的 BGRA 帧」发布成 PipeWire 视频节点。
//
// 这一层只做一件事：注册 media.class = Stream/Output/Video 的节点（obs-pwvideo 只认这个
// class），在需要的时候回调你填一帧，然后推给消费者。它不渲染、不布局、不联网——那些是
// 调用方的事。
//
// ── 线程契约（改调用方代码前先读）────────────────────────────────────────────
//   * provider 在 PipeWire 主循环线程（也就是 run() 阻塞的那条线程）里被调用，
//     不在你自己的线程里。共享状态该加锁就加锁，或者按「只读快照」的方式传递。
//   * 必须写满 stride × h 字节的 BGRA、预乘 alpha。这是给 OBS 的裸帧，没有格式转换环节。
//   * 里面**不要联网、不要阻塞、不要频繁分配**：它每帧都会被调一次。
//   * 没有消费者连接时它**一次都不会被调用**（0 帧 = 0 开销），不要假设它定期跑。
//   * provider 收到的 w/h 是协商后的尺寸，可能与构造时给的不同；按收到的值画。
//
// ── 为什么它值得单独存在 ───────────────────────────────────────────────────
// PipeWire 输出流有四条硬性要求，全都是**静默失败**（状态、日志、格式协商看起来全正常，
// 就是没画面）。四条已经在本实现里解决，改动前先读 docs/internals.md。
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pwvideo {

/** 取一帧：往 dst 写 width×height 的 BGRA（预乘 alpha），stride 由调用方给定。 */
using FrameProvider = std::function<void(uint8_t* dst, int dstStride, int width,
                                         int height)>;

struct Options {
  /** 输出尺寸。声明给消费者的规格；消费者协商出的尺寸会原样传给 provider。 */
  int width = 360;
  int height = 360;

  /** 帧率上限。对外声明的区间是 [max(1, fpsCap/4), fpsCap]：消费者可以要得更低，
   *  不可能更高；实际推帧频率取协商值并 clamp 到 [min(5, fpsCap), fpsCap]。 */
  int fpsCap = 30;

  /** PipeWire 节点名。OBS 的 obs-pwvideo 下拉框里**连接**用的是这个字段。 */
  std::string nodeName = "pwvideo";

  /** 节点描述。OBS 下拉框里**显示**的是这个字段（node.description / node.nick），
   *  不是 nodeName —— 两者别混。 */
  std::string nodeDescription = "Video Node";

  /** 写进 PipeWire 的 application.name，用来在 pw-top / pw-dump / 混音器里认出是谁。 */
  std::string appName = "pwvideo";

  /** 往 stderr 打协商、推帧、状态变化。 */
  bool verbose = false;

  /** 是否由本类安装 SIGINT/SIGTERM 处理（经 pw_loop_add_signal，退出动作在主循环里做，
   *  不在信号上下文里做，因此是安全的）。嵌进别的进程、你要自己管退出时置 false，
   *  然后调 quit()。 */
  bool handleSignals = true;

  /** 消费者接入/断开时回调（true = 进入 STREAMING，false = 离开）。
   *  在 PipeWire 主循环线程里调用。典型用途：接入时重置动画时间基，避免断连期间
   *  时间累积后在接回的第一帧跳一大步。 */
  std::function<void(bool streaming)> onStreaming;
};

class VideoNode {
 public:
  VideoNode(Options opt, FrameProvider provider);
  ~VideoNode();
  VideoNode(const VideoNode&) = delete;
  VideoNode& operator=(const VideoNode&) = delete;

  /** 建立 PipeWire 连接并注册节点。失败抛 std::runtime_error。 */
  void start();

  /** 阻塞跑主循环，直到 quit() 被调用或收到 SIGINT/SIGTERM。 */
  void run();

  /** 让 run() 返回。可从任意线程调用，也是 handleSignals=false 时的退出路径。
   *  调用后必须等 run() 真正返回（join 那条线程）再调 stop()。 */
  void quit();

  /** 停止驱动线程并释放全部资源；析构会自动调用。可重复调用。
   *  ⚠️ run() 正在另一条线程上跑时，必须先 quit() 并等它返回再调 stop()：PipeWire 要求
   *  stream 的销毁发生在主循环线程上，否则会打
   *  "pw_stream_destroy called from wrong context, check thread and locking"，
   *  资源不一定释放干净。 */
  void stop();

  /** 当前是否有消费者正在消费（即流处于 STREAMING）。 */
  bool streaming() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pwvideo
