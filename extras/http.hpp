#pragma once
// pw-video-simple-interface, extras
//
// Minimal HTTP(S) GET client over libcurl, used by the optional asset helpers to fetch cover
// images. It is deliberately small: one reusable easy handle, a byte cap on the response, and
// no global state beyond the one-time libcurl initialisation.
//
// Thread contract:
//   * Transfers on one instance are serialised by an internal mutex, so an instance must be
//     used from one thread at a time and a thread that wants its own concurrency creates its
//     own instance.
//   * get() blocks until the transfer completes or times out; never call it from a frame
//     callback.
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pwvideo {

struct HttpRequest {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  long connectTimeoutMs = 3000;
  long timeoutMs = 8000;
  size_t maxBytes = 8u * 1024u * 1024u;
  bool followRedirects = true;
};

struct HttpResponse {
  long status = 0;
  std::string body;
  bool ok() const { return status >= 200 && status < 300; }
};

/** Minimal HTTP(S) GET client over libcurl. One easy handle is kept and reused, so
 *  connections are reused across calls; transfers on one instance are serialised, therefore use
 *  one instance per thread. An empty userAgent leaves curl's default.
 *  A file:// URL is read from disk and reported as status 200. */
class HttpClient {
 public:
  explicit HttpClient(std::string userAgent = {});
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  /** Performs the request. Returns false on a transport failure (res.status stays 0) and, when
   *  error is non-null, stores a short description there. */
  bool get(const HttpRequest& req, HttpResponse& res, std::string* error = nullptr);

 private:
  std::mutex mu_;
  void* handle_ = nullptr;  // CURL*, created on first use, used only while holding mu_
  std::string userAgent_;
};

}  // namespace pwvideo
