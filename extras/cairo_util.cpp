// Cairo helpers: frame surface management, copying into a provider buffer, PNG export and the
// rounded-rectangle path.
//
// cairo_image_surface_create() reports failure by returning an error surface rather than null,
// so the status is checked once here and the frame is left empty when the allocation failed.
// An ARGB32 image surface stores exactly 4 bytes per pixel, which is what the copy below relies
// on.
#include "cairo_util.hpp"

#include <algorithm>
#include <cstring>

namespace pwvideo {
namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

CairoFrame::CairoFrame(int width, int height) : width_(width), height_(height) {
  cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    if (surf) cairo_surface_destroy(surf);
    return;
  }
  surf_.reset(surf, CairoSurfaceDeleter{});
  cr_ = ContextPtr(cairo_create(surf_.get()));
}

void CairoFrame::blitTo(uint8_t* dst, int dstStride, int w, int h) const {
  if (!surf_ || !dst) return;
  const int srcStride = cairo_image_surface_get_stride(surf_.get());
  const uint8_t* src = cairo_image_surface_get_data(surf_.get());
  if (!src) return;
  const int copyW = std::min(w, width_);
  const int copyH = std::min(h, height_);
  if (copyW <= 0 || copyH <= 0) return;
  if (copyW == width_ && copyH == height_ && dstStride == srcStride) {
    std::memcpy(dst, src, static_cast<size_t>(srcStride) * height_);
  } else {
    for (int y = 0; y < copyH; ++y)
      std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                  src + static_cast<size_t>(y) * srcStride,
                  static_cast<size_t>(copyW) * 4);
  }
}

bool CairoFrame::writePng(const std::string& path) const {
  if (!surf_) return false;
  return cairo_surface_write_to_png(surf_.get(), path.c_str()) == CAIRO_STATUS_SUCCESS;
}

void roundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -kPi / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, kPi / 2);
  cairo_arc(cr, x + r, y + h - r, r, kPi / 2, kPi);
  cairo_arc(cr, x + r, y + r, r, kPi, 1.5 * kPi);
  cairo_close_path(cr);
}

}  // namespace pwvideo
