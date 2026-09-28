#pragma once
// pw-video-simple-interface
//
// Cairo helpers for callers that render a frame before handing it to a provider: a colour
// value, RAII wrappers for the surface and its context, an image surface sized to one output
// frame, and the rounded-rectangle path used by card-style layouts.
//
// The frame surface is CAIRO_FORMAT_ARGB32, which on little-endian hosts is the
// premultiplied-alpha BGRA byte order the video node publishes. No conversion is done when the
// frame is copied into a provider buffer.
#include <cairo/cairo.h>

#include <cstdint>
#include <memory>
#include <string>

namespace pwvideo {

/** Straight RGBA, as cairo_set_source_rgba takes it. */
struct Rgba {
  double r = 0, g = 0, b = 0, a = 1.0;
};

// cairo_surface_t and cairo_t may be referenced from more than one place, so the surface uses a
// shared pointer and the context a unique one. Both deleters tolerate the null handle that the
// smart pointers may hold.
struct CairoSurfaceDeleter {
  void operator()(cairo_surface_t* s) const noexcept {
    if (s) cairo_surface_destroy(s);
  }
};
using SurfacePtr = std::shared_ptr<cairo_surface_t>;

struct CairoContextDeleter {
  void operator()(cairo_t* c) const noexcept {
    if (c) cairo_destroy(c);
  }
};
using ContextPtr = std::unique_ptr<cairo_t, CairoContextDeleter>;

/** An ARGB32 image surface together with its cairo context, sized to one output frame.
 *  Owns both; the surface is premultiplied alpha, which is what the video node publishes. */
class CairoFrame {
 public:
  CairoFrame(int width, int height);
  CairoFrame(const CairoFrame&) = delete;
  CairoFrame& operator=(const CairoFrame&) = delete;

  cairo_t* cr() const { return cr_.get(); }
  cairo_surface_t* surface() const { return surf_.get(); }
  int width() const { return width_; }
  int height() const { return height_; }

  /** Copies the frame into a FrameProvider buffer. The provider's w/h are the negotiated size
   *  and may be smaller than the frame, so the copy is clipped to min(width,w) x min(height,h).
   *  cairo pads rows to a 4-byte boundary and the destination stride is set by PipeWire, so the
   *  rows are copied one by one when the strides differ. */
  void blitTo(uint8_t* dst, int dstStride, int w, int h) const;

  /** Writes the surface to a PNG file. Status is reported by the return value. */
  bool writePng(const std::string& path) const;

 private:
  int width_ = 0, height_ = 0;
  SurfacePtr surf_;
  ContextPtr cr_;
};

/** Appends a rounded-rectangle path (closed) to the current path. The corner radius is clamped
 *  so it cannot exceed half of either side. */
void roundedRect(cairo_t* cr, double x, double y, double w, double h, double r);

}  // namespace pwvideo
