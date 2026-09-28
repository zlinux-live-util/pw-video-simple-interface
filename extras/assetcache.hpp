#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "cairo_util.hpp"
#include "http.hpp"

namespace pwvideo {

enum class ScaleMode {
  /** Fills the whole box; the source is centre-cropped to the target aspect ratio first. */
  Cover,
  /** Fits inside the box preserving the aspect ratio, centred on a transparent box. */
  Contain,
};

struct AssetSpec {
  /** Target box. Width and height both 0 keeps the decoded size; if exactly one of them is 0 the
   *  other dimension is derived from the source aspect ratio, so the scale stays uniform. */
  int width = 0;
  int height = 0;
  ScaleMode mode = ScaleMode::Cover;
};

/** Fetches images by URL and decodes them into cairo surfaces, with a small LRU cache. */
class AssetCache {
 public:
  struct Options {
    size_t capacity = 3;         // cached entries; the least recently used is evicted first
    std::string userAgent;
    long connectTimeoutMs = 3000;
    long timeoutMs = 8000;
    size_t maxBytes = 8u * 1024u * 1024u;
  };

  /** Equivalent to passing a default-constructed Options. Spelled as two constructors because a
   *  default argument of "{}" cannot use the member initialisers of the nested Options. */
  AssetCache();
  explicit AssetCache(Options opt);
  ~AssetCache();
  AssetCache(const AssetCache&) = delete;
  AssetCache& operator=(const AssetCache&) = delete;

  /** Square convenience overload: Cover-cropped to size x size. */
  SurfacePtr get(const std::string& url, int size);

  /** As spec describes. Returns nullptr when the fetch or the decode fails. Blocking: call it
   *  from a worker thread, never from the frame callback. */
  SurfacePtr get(const std::string& url, const AssetSpec& spec);

  void clear();
  uint64_t fetched() const;
  uint64_t failed() const;

 private:
  struct Entry { std::string url; AssetSpec spec; SurfacePtr surf; };
  mutable std::mutex mu_;        // Mutable: fetched() and failed() read the counters under it.
  std::deque<Entry> cache_;      // most recent first
  Options opt_;
  HttpClient http_;
  uint64_t fetched_ = 0, failed_ = 0;
};

}  // namespace pwvideo
