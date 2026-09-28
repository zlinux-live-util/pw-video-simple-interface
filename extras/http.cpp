// Minimal HTTP(S) GET client over libcurl.
#include "http.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace pwvideo {
namespace {

std::once_flag g_curlInit;

struct FileCloser {
  void operator()(std::FILE* f) const noexcept {
    if (f) std::fclose(f);
  }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

/** Response sink handed to curl. Returning a short count aborts the transfer, which is how the
 *  byte cap protects a caller from an oversized body. */
struct BodySink {
  std::string* body = nullptr;
  size_t maxBytes = 0;
  bool overflowed = false;
};

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* sink = static_cast<BodySink*>(userdata);
  const size_t n = size * nmemb;
  if (sink->body->size() + n > sink->maxBytes) {
    sink->overflowed = true;
    return 0;  // Aborts the curl transfer
  }
  sink->body->append(ptr, n);
  return n;
}

/** Minimal percent decoding, so paths with spaces and non-ASCII bytes resolve. */
std::string percentDecode(const std::string& in) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

/** Reads a file:// URL directly from disk, applying the byte cap. */
bool readFileUrl(const std::string& url, size_t maxBytes, std::string& body,
                 std::string* error) {
  auto fail = [error](const char* msg) {
    if (error) *error = msg;
    return false;
  };

  const std::string path = percentDecode(url.substr(7));
  FilePtr f(std::fopen(path.c_str(), "rb"));
  if (!f) return fail("cannot open file");

  char chunk[64 * 1024];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f.get())) > 0) {
    if (body.size() + n > maxBytes) return fail("file exceeds maxBytes");
    body.append(chunk, n);
  }
  if (std::ferror(f.get())) return fail("cannot read file");
  return true;
}

}  // namespace

HttpClient::HttpClient(std::string userAgent) : userAgent_(std::move(userAgent)) {
  std::call_once(g_curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpClient::~HttpClient() {
  std::lock_guard<std::mutex> lock(mu_);
  if (handle_) {
    curl_easy_cleanup(static_cast<CURL*>(handle_));
    handle_ = nullptr;
  }
}

bool HttpClient::get(const HttpRequest& req, HttpResponse& res, std::string* error) {
  res.status = 0;
  res.body.clear();

  auto fail = [error](const char* msg) {
    if (error) *error = msg;
    return false;
  };

  if (req.url.rfind("file://", 0) == 0) {
    // No network involved; the transfer succeeds whenever the file is readable and within cap.
    if (!readFileUrl(req.url, req.maxBytes, res.body, error)) return false;
    res.status = 200;
    return true;
  }

  std::lock_guard<std::mutex> lock(mu_);
  CURL* curl = static_cast<CURL*>(handle_);
  if (!curl) {
    curl = curl_easy_init();
    if (!curl) return fail("curl_easy_init failed");
    handle_ = curl;
  }
  curl_easy_reset(curl);

  BodySink sink{&res.body, req.maxBytes, false};

  curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, req.connectTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.timeoutMs);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.followRedirects ? 1L : 0L);
  if (req.followRedirects) curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  if (!userAgent_.empty()) curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent_.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

  curl_slist* headerList = nullptr;
  for (const auto& header : req.headers) {
    const std::string line = header.first + ": " + header.second;
    headerList = curl_slist_append(headerList, line.c_str());
  }
  if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

  const CURLcode rc = curl_easy_perform(curl);

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  if (headerList) curl_slist_free_all(headerList);

  if (rc != CURLE_OK) {
    const bool capped = (rc == CURLE_WRITE_ERROR && sink.overflowed);
    res.body.clear();  // Do not expose a partial body as a result
    return fail(capped ? "response exceeds maxBytes" : curl_easy_strerror(rc));
  }

  res.status = status;
  return true;
}

}  // namespace pwvideo
