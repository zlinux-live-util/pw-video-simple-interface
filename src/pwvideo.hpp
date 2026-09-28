#pragma once
// pw-video-simple-interface
//
// Publishes a caller-rendered frame as a PipeWire video node. The library registers a node
// with media.class = Stream/Output/Video, asks the caller to fill one frame whenever a
// consumer pulls, and does nothing else: no rendering, no layout, no network.
//
// Thread contract:
//   * The provider runs on the PipeWire main-loop thread (the one blocked in run()), not on
//     the caller's thread. Shared state must be locked or handed over as a read-only snapshot.
//   * It must fill exactly stride * h bytes of premultiplied-alpha BGRA. There is no format
//     conversion between the callback and the consumer.
//   * It must not perform network I/O, block, or allocate: it is the per-frame path.
//   * It is not called while no consumer is attached, so it must not be assumed to run on a
//     timer. Zero frames means zero work.
//   * The w/h it receives are the negotiated size and may differ from the configured size.
//
// The stream setup constraints this library encapsulates are documented in docs/internals.md.
// Read that before changing the PipeWire configuration.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pwvideo {

/** Provides one frame: fills dst with width x height BGRA (premultiplied alpha) at the given
 *  stride. */
using FrameProvider = std::function<void(uint8_t* dst, int dstStride, int width,
                                         int height)>;

struct Options {
  /** Output size advertised to consumers. The size a consumer negotiates is passed to the
   *  provider unchanged. */
  int width = 360;
  int height = 360;

  /** Frame-rate ceiling. The advertised range is [max(1, fpsCap/4), fpsCap]: a consumer may
   *  negotiate lower, never higher. Frames are pushed at the negotiated rate, clamped to
   *  [min(5, fpsCap), fpsCap]. */
  int fpsCap = 30;

  /** PipeWire node name. This is the value obs-pwvideo connects to. */
  std::string nodeName = "pwvideo";

  /** Node description. This is the value obs-pwvideo displays in its source dropdown
   *  (node.description, falling back to node.nick); nodeName is not displayed. */
  std::string nodeDescription = "Video Node";

  /** Set as application.name, identifying the process in pw-dump, pw-top and session tools. */
  std::string appName = "pwvideo";

  /** Logs negotiation, frame pushes and state transitions to stderr. */
  bool verbose = false;

  /** Install SIGINT/SIGTERM handlers. They are registered through pw_loop_add_signal, so the
   *  exit is performed on the main loop rather than in signal context. Set false when
   *  embedding and drive the exit with quit() instead. */
  bool handleSignals = true;

  /** Called when a consumer attaches (true) or detaches (false), on the PipeWire main-loop
   *  thread. Typical use: reset an animation time base on reattach, so time accumulated while
   *  disconnected does not produce a jump in the first frame after reconnecting. */
  std::function<void(bool streaming)> onStreaming;
};

class VideoNode {
 public:
  VideoNode(Options opt, FrameProvider provider);
  ~VideoNode();
  VideoNode(const VideoNode&) = delete;
  VideoNode& operator=(const VideoNode&) = delete;

  /** Connects to PipeWire and registers the node. Throws std::runtime_error on failure. */
  void start();

  /** Runs the main loop until quit() is called or SIGINT/SIGTERM is received. */
  void run();

  /** Makes run() return. Callable from any thread; also the exit path when
   *  handleSignals is false. run() must have returned (join its thread) before stop(). */
  void quit();

  /** Stops the driver thread and releases all resources; the destructor calls it. Idempotent.
   *
   *  When run() is executing on another thread, call quit() and join that thread first:
   *  PipeWire requires streams to be destroyed from the loop thread, otherwise it reports
   *  "pw_stream_destroy called from wrong context, check thread and locking" and resources
   *  may not be released cleanly. */
  void stop();

  /** Whether a consumer is currently consuming, i.e. the stream is in STREAMING. */
  bool streaming() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pwvideo
