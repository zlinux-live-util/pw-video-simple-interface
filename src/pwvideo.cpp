// PipeWire video output.
//
// Registers a node with media.class = Stream/Output/Video, the only class obs-pwvideo accepts.
//
// Four requirements; each of them fails silently when violated. See docs/internals.md for the
// measurements behind them.
//   1) SPA_PARAM_Buffers must be declared through pw_stream_update_params(). Without it the
//      port's Buffers parameter is empty, PipeWire allocates nothing, and every spa_buffer
//      returned by pw_stream_dequeue_buffer() has maxsize=0, so writing a frame at the given
//      stride overruns the allocation.
//   2) Use PW_STREAM_FLAG_TRIGGER and never add PW_STREAM_FLAG_DRIVER. An output stream is not
//      scheduled automatically and the graph cycle must be started with
//      pw_stream_trigger_process(). A timer on the PipeWire loop does not work, hence the
//      driver thread. With DRIVER the node properties and state look correct and the consumer
//      reports streaming, but the process callback is never entered and no frames arrive.
//   3) PW_STREAM_FLAG_MAP_BUFFERS is required for datas[0].data to be writable.
//   4) media.role must be "Production". obs-pwvideo compares media.type == "Video",
//      media.class == "Stream/Output/Video" and media.role == "Production"; a node missing any
//      of the three never appears in its source dropdown, and the plugin logs nothing.
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

// pw_init/pw_deinit operate on process-global state and are therefore reference counted: two
// VideoNode instances must not tear down each other's state on destruction.
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
  std::atomic<int> negFps{0};  // Negotiated frame rate; a consumer may request a lower one.

  // Writes one frame into a PipeWire buffer. Validated against maxsize, never overruns.
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
          std::fprintf(stderr, "[pw] pushed %llu frames (%dx%d)\n",
                       static_cast<unsigned long long>(n), w, h);
      } else if (opt.verbose) {
        std::fprintf(stderr, "[pw] frame skipped: maxsize=%u < need=%zu\n", d.maxsize, need);
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
      std::fprintf(stderr, "[pw] negotiated %ux%u format=%d framerate=%u/%u\n",
                   info.size.width, info.size.height, static_cast<int>(info.format),
                   info.framerate.num, info.framerate.denom);
  }

  // A TRIGGER stream starts its own graph cycles at the target rate. The process callback is
  // only meaningful while the stream is in STREAMING.
  void driverLoop(pw_stream* st) {
    const int cap = opt.fpsCap > 0 ? opt.fpsCap : 30;
    const int floorFps = std::min(5, cap);  // Keep the picture moving if a consumer asks for less.
    auto next = std::chrono::steady_clock::now();
    uint64_t n = 0;
    while (driveRunning.load(std::memory_order_relaxed)) {
      int rate = negFps.load(std::memory_order_relaxed);
      if (rate <= 0) rate = cap;
      rate = std::clamp(rate, floorFps, cap);
      next += std::chrono::nanoseconds(1000000000LL / rate);
      const auto state = pw_stream_get_state(st, nullptr);
      // is_driving() is deliberately not consulted: the consumer (OBS) is part of the graph
      // too, and which end WirePlumber selects as driver must not decide whether this end
      // produces frames. Entering STREAMING is the only condition.
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
      std::fprintf(stderr, "[pw] state -> %d %s\n", static_cast<int>(state), err ? err : "");
    if (state == PW_STREAM_STATE_ERROR && err)
      std::fprintf(stderr, "[pw] stream error: %s\n", err);

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
    throw std::runtime_error("VideoNode: width/height must be positive");
  impl_->opt = std::move(opt);
  impl_->provider = std::move(provider);
  if (!impl_->provider) throw std::runtime_error("VideoNode: provider must be set");
}

VideoNode::~VideoNode() { stop(); }

void VideoNode::start() {
  Impl& s = *impl_;
  pwInitRetain();

  s.loop = pw_main_loop_new(nullptr);
  if (!s.loop) {
    pwInitRelease();
    throw std::runtime_error("pw_main_loop_new failed");
  }
  pw_loop* l = pw_main_loop_get_loop(s.loop);
  if (s.opt.handleSignals) {
    // Handled through pw_loop_add_signal: the exit runs on the main loop, not in signal
    // context.
    pw_loop_add_signal(l, SIGINT, &Impl::onSignal, s.loop);
    pw_loop_add_signal(l, SIGTERM, &Impl::onSignal, s.loop);
  }

  s.context = pw_context_new(l, nullptr, 0);
  if (!s.context) throw std::runtime_error("pw_context_new failed");
  s.core = pw_context_connect(s.context, nullptr, 0);
  if (!s.core) throw std::runtime_error("cannot connect to PipeWire (is the daemon running?)");

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
      // media.role must be "Production": obs-pwvideo's on_registry_global_cb compares
      // media.type == "Video", media.class == "Stream/Output/Video" and
      // media.role == "Production", and a node missing any of the three never appears in its
      // dropdown.
      PW_KEY_MEDIA_ROLE, "Production",
      PW_KEY_MEDIA_CLASS, "Stream/Output/Video",  // The only class obs-pwvideo accepts.
      PW_KEY_NODE_NAME, s.opt.nodeName.c_str(), PW_KEY_NODE_DESCRIPTION,
      s.opt.nodeDescription.c_str(), PW_KEY_NODE_NICK, s.opt.nodeDescription.c_str(),
      PW_KEY_APP_NAME, s.opt.appName.c_str(), nullptr);

  s.stream = pw_stream_new_simple(l, s.opt.nodeName.c_str(), props, &Impl::events(), &s);
  if (!s.stream) throw std::runtime_error("pw_stream_new_simple failed");

  uint8_t formatBuf[1024];
  spa_pod_builder fb = SPA_POD_BUILDER_INIT(formatBuf, sizeof(formatBuf));
  spa_pod_frame ff{};
  {
    spa_rectangle size{static_cast<uint32_t>(s.opt.width),
                       static_cast<uint32_t>(s.opt.height)};
    // The frame rate is advertised as a range: consumers may negotiate lower, never higher.
    const int cap = s.opt.fpsCap > 0 ? s.opt.fpsCap : 30;
    spa_fraction fr{static_cast<uint32_t>(cap), 1};
    spa_fraction frMin{static_cast<uint32_t>(std::max(1, cap / 4)), 1};
    spa_pod_builder_push_object(&fb, &ff, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&fb, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(&fb, SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    // BGRA only: cairo's ARGB32 is B,G,R,A on little-endian, so a frame can be copied as is.
    spa_pod_builder_add(&fb, SPA_FORMAT_VIDEO_format,
                        SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA), 0);
    spa_pod_builder_add(&fb, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
    spa_pod_builder_add(&fb, SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_CHOICE_RANGE_Fraction(&fr, &frMin, &fr), 0);
  }
  const spa_pod* params[1] = {static_cast<spa_pod*>(spa_pod_builder_pop(&fb, &ff))};

  // TRIGGER only; DRIVER must not be added. Measured with OBS as the consumer: TRIGGER delivers
  // frames, while adding DRIVER leaves the properties and state looking correct but never
  // enters the process callback. Node visibility does not depend on this flag; it comes from
  // media.role="Production".
  const pw_stream_flags flags = static_cast<pw_stream_flags>(
      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_TRIGGER);
  if (pw_stream_connect(s.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0)
    throw std::runtime_error("pw_stream_connect failed");

  // Declare the buffer requirements (requirement 1 in the file header).
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
