// Image fetching: HTTP GET -> gdk-pixbuf decode -> crop and scale -> cairo ARGB32 surface,
// with a small LRU cache in front of the decoded results.
//
// The cache lookup runs under the mutex; the transfer and the decode run outside it, so a slow
// server delays only the call that is waiting for that URL. Two threads missing on the same key
// at the same time download it twice, which is harmless and cheaper than holding the mutex
// across the network.
#include "assetcache.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace pwvideo {
namespace {

struct PixbufUnref {
  void operator()(GdkPixbuf* p) const noexcept {
    if (p) g_object_unref(p);
  }
};
struct LoaderUnref {
  void operator()(GdkPixbufLoader* p) const noexcept {
    if (p) g_object_unref(p);
  }
};
using PixbufPtr = std::unique_ptr<GdkPixbuf, PixbufUnref>;
using LoaderPtr = std::unique_ptr<GdkPixbufLoader, LoaderUnref>;

bool sameSpec(const AssetSpec& a, const AssetSpec& b) {
  return a.width == b.width && a.height == b.height && a.mode == b.mode;
}

/** Returns src as a 4-channel pixbuf. gdk_pixbuf_copy_area documents that both pixbufs must
 *  have the same number of channels, so a source without alpha is widened with an opaque
 *  channel; the RGB bytes are untouched, and an opaque channel is byte-neutral because
 *  premultiplying a colour channel by 255 returns it unchanged. */
PixbufPtr withAlpha(GdkPixbuf* pb) {
  if (gdk_pixbuf_get_has_alpha(pb)) return PixbufPtr(g_object_ref(pb));
  return PixbufPtr(gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0));
}

/** Converts a GdkPixbuf to cairo ARGB32 (BGRA on little-endian, premultiplied alpha) */
cairo_surface_t* pixbufToSurface(GdkPixbuf* pb) {
  const int w = gdk_pixbuf_get_width(pb);
  const int h = gdk_pixbuf_get_height(pb);
  const int nch = gdk_pixbuf_get_n_channels(pb);
  const int sstride = gdk_pixbuf_get_rowstride(pb);
  const guchar* src = gdk_pixbuf_get_pixels(pb);
  const bool alpha = gdk_pixbuf_get_has_alpha(pb) != 0;
  if (w <= 0 || h <= 0 || (nch != 3 && nch != 4)) return nullptr;

  cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return nullptr;
  }
  const int dstride = cairo_image_surface_get_stride(surf);
  uint8_t* dst = cairo_image_surface_get_data(surf);
  std::memset(dst, 0, static_cast<size_t>(dstride) * static_cast<size_t>(h));

  for (int y = 0; y < h; ++y) {
    const uint8_t* srow = src + static_cast<size_t>(y) * static_cast<size_t>(sstride);
    uint32_t* drow = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) *
                                                          static_cast<size_t>(dstride));
    for (int x = 0; x < w; ++x) {
      uint8_t r, g, b, a = 255;
      if (alpha) {
        r = srow[x * 4 + 0];
        g = srow[x * 4 + 1];
        b = srow[x * 4 + 2];
        a = srow[x * 4 + 3];
      } else {
        r = srow[x * 3 + 0];
        g = srow[x * 3 + 1];
        b = srow[x * 3 + 2];
      }
      const uint32_t pr = static_cast<uint32_t>(r) * a / 255u;
      const uint32_t pg = static_cast<uint32_t>(g) * a / 255u;
      const uint32_t pbit = static_cast<uint32_t>(b) * a / 255u;
      drow[x] = (static_cast<uint32_t>(a) << 24) | (pr << 16) | (pg << 8) | pbit;
    }
  }
  cairo_surface_mark_dirty(surf);
  return surf;
}

SurfacePtr toSurface(GdkPixbuf* pb) {
  cairo_surface_t* surf = pixbufToSurface(pb);
  if (!surf) return nullptr;
  return SurfacePtr(surf, CairoSurfaceDeleter{});
}

struct Crop {
  PixbufPtr pixbuf;
  int width = 0;
  int height = 0;
};

/** Centre-crops src to the target aspect ratio. A square target yields the largest centred
 *  square, which is the crop the square-only consumer used. */
bool cropToAspect(GdkPixbuf* src, int tw, int th, Crop& out) {
  const int w = gdk_pixbuf_get_width(src);
  const int h = gdk_pixbuf_get_height(src);
  int cw = w;
  int ch = h;
  // 64-bit products: w * th overflows int for dimensions in the tens of thousands.
  if (static_cast<long long>(w) * th > static_cast<long long>(h) * tw) {
    cw = static_cast<int>(static_cast<long long>(h) * tw / th);
  } else {
    ch = static_cast<int>(static_cast<long long>(w) * th / tw);
  }
  if (cw < 1) cw = 1;
  if (ch < 1) ch = 1;
  const int offX = (w - cw) / 2;
  const int offY = (h - ch) / 2;

  PixbufPtr cropped(gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, cw, ch));
  if (!cropped) return false;
  gdk_pixbuf_fill(cropped.get(), 0x00000000);
  gdk_pixbuf_copy_area(src, offX, offY, cw, ch, cropped.get(), 0, 0);

  out.pixbuf = std::move(cropped);
  out.width = cw;
  out.height = ch;
  return true;
}

/** Brings the decoded pixbuf to the requested geometry and converts it to a cairo surface. */
SurfacePtr makeSurface(GdkPixbuf* raw, const AssetSpec& spec) {
  const int w = gdk_pixbuf_get_width(raw);
  const int h = gdk_pixbuf_get_height(raw);
  if (w <= 0 || h <= 0) return nullptr;

  int tw = spec.width > 0 ? spec.width : 0;
  int th = spec.height > 0 ? spec.height : 0;
  if (tw == 0 && th == 0) {
    // Native size: the decoded pixels are published without a crop or a scale.
    return toSurface(raw);
  }
  // A single missing dimension is derived from the source aspect ratio, so the box is never
  // degenerate and the scale stays uniform.
  if (tw == 0) {
    tw = static_cast<int>(std::lround(static_cast<double>(w) * th / h));
  } else if (th == 0) {
    th = static_cast<int>(std::lround(static_cast<double>(h) * tw / w));
  }
  if (tw < 1 || th < 1) return nullptr;

  PixbufPtr src = withAlpha(raw);
  if (!src) return nullptr;

  if (spec.mode == ScaleMode::Cover) {
    Crop crop;
    if (!cropToAspect(src.get(), tw, th, crop)) return nullptr;
    // Fast path: a crop that already has the target size needs no scaling.
    PixbufPtr scaled = (crop.width == tw && crop.height == th)
                           ? std::move(crop.pixbuf)
                           : PixbufPtr(gdk_pixbuf_scale_simple(crop.pixbuf.get(), tw, th,
                                                              GDK_INTERP_BILINEAR));
    if (!scaled) return nullptr;
    return toSurface(scaled.get());
  }

  // Contain: fit inside the box, then centre the result on a transparent box.
  const double factor = std::min(static_cast<double>(tw) / w, static_cast<double>(th) / h);
  int sw = std::max(1, static_cast<int>(std::lround(w * factor)));
  int sh = std::max(1, static_cast<int>(std::lround(h * factor)));
  sw = std::min(sw, tw);
  sh = std::min(sh, th);

  PixbufPtr fitted = (sw == w && sh == h)
                         ? std::move(src)
                         : PixbufPtr(gdk_pixbuf_scale_simple(src.get(), sw, sh,
                                                             GDK_INTERP_BILINEAR));
  if (!fitted) return nullptr;

  PixbufPtr box(gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, tw, th));
  if (!box) return nullptr;
  gdk_pixbuf_fill(box.get(), 0x00000000);
  gdk_pixbuf_copy_area(fitted.get(), 0, 0, sw, sh, box.get(), (tw - sw) / 2, (th - sh) / 2);
  return toSurface(box.get());
}

/** Transfers the URL, decodes the payload and brings it to the requested geometry. Called
 *  without the cache mutex held. */
SurfacePtr load(HttpClient& http, const AssetCache::Options& opt, const std::string& url,
                const AssetSpec& spec) {
  HttpRequest req;
  req.url = url;
  req.connectTimeoutMs = opt.connectTimeoutMs;
  req.timeoutMs = opt.timeoutMs;
  req.maxBytes = opt.maxBytes;

  HttpResponse res;
  if (!http.get(req, res) || !res.ok() || res.body.empty()) return nullptr;

  LoaderPtr loader(gdk_pixbuf_loader_new());
  if (!loader) return nullptr;
  GError* err = nullptr;
  if (!gdk_pixbuf_loader_write(loader.get(),
                              reinterpret_cast<const guchar*>(res.body.data()),
                              static_cast<gsize>(res.body.size()), &err)) {
    if (err) g_error_free(err);
    return nullptr;
  }
  if (!gdk_pixbuf_loader_close(loader.get(), &err)) {
    if (err) g_error_free(err);
    return nullptr;
  }
  GdkPixbuf* raw = gdk_pixbuf_loader_get_pixbuf(loader.get());  // Owned by the loader
  if (!raw) return nullptr;

  return makeSurface(raw, spec);
}

}  // namespace

AssetCache::AssetCache() : AssetCache(Options{}) {}

AssetCache::AssetCache(Options opt) : opt_(std::move(opt)), http_(opt_.userAgent) {}

AssetCache::~AssetCache() = default;

void AssetCache::clear() {
  std::lock_guard lock(mu_);
  cache_.clear();
}

SurfacePtr AssetCache::get(const std::string& url, int size) {
  if (size <= 0) return nullptr;
  AssetSpec spec;
  spec.width = size;
  spec.height = size;
  spec.mode = ScaleMode::Cover;
  return get(url, spec);
}

SurfacePtr AssetCache::get(const std::string& url, const AssetSpec& spec) {
  if (url.empty() || spec.width < 0 || spec.height < 0) return nullptr;

  {
    std::lock_guard lock(mu_);
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->url == url && sameSpec(it->spec, spec)) {
        Entry hit = *it;
        cache_.erase(it);
        cache_.push_front(std::move(hit));
        return cache_.front().surf;
      }
    }
  }

  // The network and the decode run outside the lock so other threads are not blocked; a race
  // at worst fetches the same image twice, which is harmless.
  SurfacePtr surf = load(http_, opt_, url, spec);
  if (!surf) {
    std::lock_guard lock(mu_);
    ++failed_;
    return nullptr;
  }

  std::lock_guard lock(mu_);
  ++fetched_;
  cache_.push_front(Entry{url, spec, surf});
  while (cache_.size() > opt_.capacity) cache_.pop_back();
  return surf;
}

uint64_t AssetCache::fetched() const {
  std::lock_guard lock(mu_);
  return fetched_;
}

uint64_t AssetCache::failed() const {
  std::lock_guard lock(mu_);
  return failed_;
}

}  // namespace pwvideo
