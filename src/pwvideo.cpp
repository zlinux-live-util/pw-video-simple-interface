// PipeWire 视频输出实现
//
// 注册成 media.class = Stream/Output/Video 的节点 —— obs-pwvideo 正好只认这个 class。
//
// 踩过的四个坑，改之前先看这里：
//   1) 必须在 pw_stream_update_params() 里声明 SPA_PARAM_Buffers。不声明的话端口上
//      的 Buffers 参数是空的，PipeWire 不分配内存，process 里拿到的 spa_buffer
//      全是 maxsize=0，照 stride 盲写会直接踩崩。
//   2) 用 PW_STREAM_FLAG_TRIGGER（绝不要加 DRIVER）：输出流不会自动调度，必须自己调
//      pw_stream_trigger_process() 启动图周期。定时器走 PipeWire 主循环不生效，
//      用独立线程（官方文档推荐的辅助线程做法）。
//      ⚠️ 加 DRIVER 会静默失效：节点属性更漂亮、状态也是 running、OBS 也报 streaming，
//         但 process 回调一次都不进，OBS 里一片空白。实测 A/B 过。
//   3) 需要 PW_STREAM_FLAG_MAP_BUFFERS 才能拿到可写的 datas[0].data。
//   4) media.role 必须是 "Production"。obs-pwvideo 的 on_registry_global_cb 里写死了
//      三个 strcmp：media.type=="Video"、media.class=="Stream/Output/Video"、
//      media.role=="Production"，差一个它的下拉框就看不到这个节点。
#include "pwvideo.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace pwvideo {
namespace {

/** pw_init/pw_deinit 是进程级的，必须引用计数：否则第二个 VideoNode 析构时会把第一个
 *  还在用的 PipeWire 全局状态拆掉。 */
std::mutex gInitMu;
int gInitCount = 0;

void pwInitRetain() {
  std::lock_guard lock(gInitMu);
  if (gInitCount++ == 0) pw_init(nullptr, nullptr);
}

void pwInitRelease() {
  std::lock_guard lock(gInitMu);
  if (--gInitCount == 0) pw_deinit();
}

}  // namespace

struct VideoNode::Impl {
  Options opt;
  FrameProvider provider;

  pw_main_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_stream* stream = nullptr;

  std::thread driverThread;
  std::atomic<bool> driveRunning{false};
  std::atomic<bool> streamingFlag{false};
  std::atomic<uint64_t> frames{0};

  std::atomic<int> negW{0};
  std::atomic<int> negH{0};
  std::atomic<int> negFps{0};  // 协商出来的帧率（消费者可以要得更低）

  /** 把一帧写进 PipeWire 缓冲；按 maxsize 校验，绝不越界 */
  void produce() {
    pw_buffer* b = pw_stream_dequeue_buffer(stream);
    if (!b) return;

    spa_buffer* buf = b->buffer;
    if (buf && buf->n_datas >= 1 && buf->datas[0].data) {
      spa_data& d = buf->datas[0];
      const int w = negW.load(std::memory_order_relaxed);
      const int h = negH.load(std::memory_order_relaxed);
      int stride = d.chunk ? d.chunk->stride : 0;
      if (stride <= 0) stride = w * 4;
      const size_t need = static_cast<size_t>(stride) * static_cast<size_t>(h);

      if (w > 0 && h > 0 && d.maxsize >= need) {
        provider(static_cast<uint8_t*>(d.data), stride, w, h);
        if (d.chunk) {
          d.chunk->offset = 0;
          d.chunk->stride = stride;
          d.chunk->size = static_cast<uint32_t>(need);
          d.chunk->flags = 0;
        }
        const uint64_t n = frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (opt.verbose && (n % 60) == 1)
          std::fprintf(stderr, "[pw] 已推送 %llu 帧 (%dx%d)\n",
                       static_cast<unsigned long long>(n), w, h);
      } else if (opt.verbose) {
        std::fprintf(stderr, "[pw] 跳过一帧：maxsize=%u < need=%zu\n", d.maxsize, need);
      }
    }
    pw_stream_queue_buffer(stream, b);
  }

  void onParamChanged(uint32_t id, const spa_pod* param) {
    if (id != SPA_PARAM_Format || !param) return;
    uint32_t mediaType = 0, mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
    if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;

    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    negW.store(static_cast<int>(info.size.width));
    negH.store(static_cast<int>(info.size.height));
    if (info.framerate.denom > 0 && info.framerate.num > 0) {
      const int r = static_cast<int>(info.framerate.num / info.framerate.denom);
      if (r > 0) negFps.store(r);
    }
    if (opt.verbose)
      std::fprintf(stderr, "[pw] 协商格式 %ux%u format=%d framerate=%u/%u\n",
                   info.size.width, info.size.height, static_cast<int>(info.format),
                   info.framerate.num, info.framerate.denom);
  }

  /**
   * 作为 TRIGGER 流，必须自己按目标帧率启动图周期。
   * 注意 process 本身只在 STREAMING 状态下才应该触发。
   */
  void driverLoop(pw_stream* st) {
    const int cap = opt.fpsCap > 0 ? opt.fpsCap : 30;
    const int floorFps = std::min(5, cap);  // 消费者要得太低时保底，别把画面冻住
    auto next = std::chrono::steady_clock::now();
    uint64_t n = 0;
    while (driveRunning.load(std::memory_order_relaxed)) {
      int rate = negFps.load(std::memory_order_relaxed);
      if (rate <= 0) rate = cap;
      rate = std::clamp(rate, floorFps, cap);
      next += std::chrono::nanoseconds(1000000000LL / rate);
      const auto state = pw_stream_get_state(st, nullptr);
      // 不查 is_driving()：消费者（OBS）也在图里，谁被选成 driver 由 WirePlumber 决定，
      // 但数据是我们产的，只要流进了 STREAMING 就该由我们推进图周期。
      if (state == PW_STREAM_STATE_STREAMING) pw_stream_trigger_process(st);
      if (opt.verbose && (++n % 30) == 1)
        std::fprintf(stderr, "[pw] tick %llu state=%d driving=%d\n",
                     static_cast<unsigned long long>(n), static_cast<int>(state),
                     pw_stream_is_driving(st) ? 1 : 0);
      std::this_thread::sleep_until(next);
    }
  }

  void onState(pw_stream_state state, const char* err) {
    if (opt.verbose)
      std::fprintf(stderr, "[pw] 状态 -> %d %s\n", static_cast<int>(state), err ? err : "");
    if (state == PW_STREAM_STATE_ERROR && err)
      std::fprintf(stderr, "[pw] 流错误: %s\n", err);

    const bool nowStreaming = (state == PW_STREAM_STATE_STREAMING);
    if (nowStreaming != streamingFlag.exchange(nowStreaming) && opt.onStreaming)
      opt.onStreaming(nowStreaming);
  }

  static void onProcess(void* data) { static_cast<Impl*>(data)->produce(); }

  static void onParamChangedCb(void* data, uint32_t id, const spa_pod* param) {
    static_cast<Impl*>(data)->onParamChanged(id, param);
  }

  static void onStateChanged(void* data, pw_stream_state /*old*/, pw_stream_state state,
                             const char* err) {
    static_cast<Impl*>(data)->onState(state, err);
  }

  static void onSignal(void* data, int /*signal*/) {
    pw_main_loop_quit(static_cast<pw_main_loop*>(data));
  }

  static const pw_stream_events& events() {
    static const pw_stream_events e = [] {
      pw_stream_events s{};
      s.version = PW_VERSION_STREAM_EVENTS;
      s.state_changed = &Impl::onStateChanged;
      s.param_changed = &Impl::onParamChangedCb;
      s.process = &Impl::onProcess;
      return s;
    }();
    return e;
  }
};

VideoNode::VideoNode(Options opt, FrameProvider provider) : impl_(std::make_unique<Impl>()) {
  if (opt.width <= 0 || opt.height <= 0)
    throw std::runtime_error("VideoNode: width/height 必须为正");
  impl_->opt = std::move(opt);
  impl_->provider = std::move(provider);
  if (!impl_->provider) throw std::runtime_error("VideoNode: provider 不能为空");
}

VideoNode::~VideoNode() { stop(); }

void VideoNode::start() {
  Impl& s = *impl_;
  pwInitRetain();

  s.loop = pw_main_loop_new(nullptr);
  if (!s.loop) {
    pwInitRelease();
    throw std::runtime_error("pw_main_loop_new 失败");
  }
  pw_loop* l = pw_main_loop_get_loop(s.loop);
  if (s.opt.handleSignals) {
    // 经 pw_loop_add_signal 处理：退出动作在主循环里做，不在信号上下文里做。
    pw_loop_add_signal(l, SIGINT, &Impl::onSignal, s.loop);
    pw_loop_add_signal(l, SIGTERM, &Impl::onSignal, s.loop);
  }

  s.context = pw_context_new(l, nullptr, 0);
  if (!s.context) throw std::runtime_error("pw_context_new 失败");
  s.core = pw_context_connect(s.context, nullptr, 0);
  if (!s.core) throw std::runtime_error("连接 PipeWire 失败（daemon 没在跑？）");

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
      // media.role 必须是 "Production"：obs-pwvideo 的 on_registry_global_cb 里
      // 写死了三个 strcmp —— media.type=="Video"、media.class=="Stream/Output/Video"、
      // media.role=="Production"，差一个它的下拉框就看不到这个节点。
      PW_KEY_MEDIA_ROLE, "Production",
      PW_KEY_MEDIA_CLASS, "Stream/Output/Video",  // obs-pwvideo 只认这个
      PW_KEY_NODE_NAME, s.opt.nodeName.c_str(), PW_KEY_NODE_DESCRIPTION,
      s.opt.nodeDescription.c_str(), PW_KEY_NODE_NICK, s.opt.nodeDescription.c_str(),
      PW_KEY_APP_NAME, s.opt.appName.c_str(), nullptr);

  s.stream = pw_stream_new_simple(l, s.opt.nodeName.c_str(), props, &Impl::events(), &s);
  if (!s.stream) throw std::runtime_error("pw_stream_new_simple 失败");

  uint8_t formatBuf[1024];
  spa_pod_builder fb = SPA_POD_BUILDER_INIT(formatBuf, sizeof(formatBuf));
  spa_pod_frame ff{};
  {
    spa_rectangle size{static_cast<uint32_t>(s.opt.width),
                       static_cast<uint32_t>(s.opt.height)};
    // 帧率声明成区间：上限由 fpsCap 决定，消费者可以要得更低但不可能更高。
    const int cap = s.opt.fpsCap > 0 ? s.opt.fpsCap : 30;
    spa_fraction fr{static_cast<uint32_t>(cap), 1};
    spa_fraction frMin{static_cast<uint32_t>(std::max(1, cap / 4)), 1};
    spa_pod_builder_push_object(&fb, &ff, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&fb, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(&fb, SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    // 只要 BGRA：cairo 的 ARGB32 在小端就是 B,G,R,A，可以整块拷
    spa_pod_builder_add(&fb, SPA_FORMAT_VIDEO_format,
                        SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA), 0);
    spa_pod_builder_add(&fb, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
    spa_pod_builder_add(&fb, SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_CHOICE_RANGE_Fraction(&fr, &frMin, &fr), 0);
  }
  const spa_pod* params[1] = {static_cast<spa_pod*>(spa_pod_builder_pop(&fb, &ff))};

  // 只用 TRIGGER，绝对不要加 DRIVER。
  // 实测（OBS 当消费者）：TRIGGER → 帧正常投递；加 DRIVER → 节点属性更好看、
  // 状态也是 running，但 process 回调一次都不进，OBS 里就是一片空白。
  // 节点可见性和这个标志无关，靠 media.role="Production"。见 docs/internals.md。
  const pw_stream_flags flags = static_cast<pw_stream_flags>(
      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_TRIGGER);
  if (pw_stream_connect(s.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0)
    throw std::runtime_error("pw_stream_connect 失败");

  // 声明缓冲区需求（见文件头坑 1）
  {
    const int frameBytes = s.opt.width * s.opt.height * 4;
    const int stride = s.opt.width * 4;
    uint8_t buf[512];
    spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    spa_pod_frame pf{};
    spa_pod_builder_push_object(&pb, &pf, SPA_TYPE_OBJECT_ParamBuffers,
                                SPA_PARAM_Buffers);
    spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_buffers,
                        SPA_POD_CHOICE_RANGE_Int(4, 2, 16), 0);
    spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), 0);
    spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_size,
                        SPA_POD_CHOICE_RANGE_Int(frameBytes, frameBytes, INT32_MAX), 0);
    spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_stride,
                        SPA_POD_CHOICE_RANGE_Int(stride, stride, INT32_MAX), 0);
    spa_pod_builder_add(&pb, SPA_PARAM_BUFFERS_dataType,
                        SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd), 0);
    const spa_pod* bp[1] = {static_cast<spa_pod*>(spa_pod_builder_pop(&pb, &pf))};
    pw_stream_update_params(s.stream, bp, 1);
  }

  s.driveRunning.store(true);
  s.driverThread = std::thread([&s] { s.driverLoop(s.stream); });
}

void VideoNode::run() {
  if (impl_->loop) pw_main_loop_run(impl_->loop);
}

void VideoNode::quit() {
  if (impl_->loop) pw_main_loop_quit(impl_->loop);
}

bool VideoNode::streaming() const {
  return impl_->streamingFlag.load(std::memory_order_relaxed);
}

void VideoNode::stop() {
  Impl& s = *impl_;
  const bool hadLoop = s.loop != nullptr;
  s.driveRunning.store(false);
  if (s.driverThread.joinable()) s.driverThread.join();
  if (s.stream) {
    pw_stream_destroy(s.stream);
    s.stream = nullptr;
  }
  if (s.core) {
    pw_core_disconnect(s.core);
    s.core = nullptr;
  }
  if (s.context) {
    pw_context_destroy(s.context);
    s.context = nullptr;
  }
  if (s.loop) {
    pw_main_loop_destroy(s.loop);
    s.loop = nullptr;
  }
  if (hadLoop) pwInitRelease();
}

}  // namespace pwvideo
