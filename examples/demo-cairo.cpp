// Example: draws with the cairo extras and publishes the result as a PipeWire video node.
//
//   ./demo-cairo                  publishes the node pwnode-cairo
//   ./demo-cairo --dump out.png   renders one frame to a PNG and exits
//   ./demo-cairo --image PATH     also draws an image fetched through AssetCache
//
// The label carries an outline, which is what keeps text legible over video.
#include "assetcache.hpp"
#include "cairo_util.hpp"
#include "pwvideo.hpp"
#include "text.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  const int kW = 480, kH = 200;
  std::string dumpPath, imageUrl;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
    else if (a == "--image" && i + 1 < argc) imageUrl = argv[++i];
  }

  pwvideo::AssetCache assets;
  pwvideo::SurfacePtr image;
  if (!imageUrl.empty()) {
    image = assets.get(imageUrl, {160, 160, pwvideo::ScaleMode::Contain});
    std::printf("image %s: %s (fetched %llu, failed %llu)\n", imageUrl.c_str(),
                image ? "loaded" : "unavailable",
                static_cast<unsigned long long>(assets.fetched()),
                static_cast<unsigned long long>(assets.failed()));
  }

  pwvideo::CairoFrame frame(kW, kH);
  pwvideo::TextRenderer text;
  std::atomic<uint64_t> ticks{0};

  auto draw = [&](int tick) {
    cairo_t* cr = frame.cr();
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    const double barX = static_cast<double>(tick % kW);
    cairo_rectangle(cr, barX, kH - 12, 40, 12);
    cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.9);
    cairo_fill(cr);

    if (image) {
      cairo_set_source_surface(cr, image.get(), 16, 20);
      cairo_paint(cr);
    }

    const char* line1 = "pwvideo extras";
    const std::string line2 = "frame " + std::to_string(tick);

    pwvideo::LabelSpec spec;
    spec.sizePx = 30;
    spec.widthPx = kW - 220;
    spec.maxLines = 1;
    spec.center = false;
    spec.bold = true;

    PangoLayout* l = text.layout(cr, line1, spec);
    const pwvideo::LabelMetrics m = pwvideo::TextRenderer::measure(l);
    const double x = 208, y = kH / 2.0 - m.height - 4;
    // Two outline passes of decreasing width approximate a soft shadow.
    pwvideo::TextRenderer::outline(cr, l, x, y, 7.0, pwvideo::Rgba{0, 0, 0, 0.35}, 1.5);
    pwvideo::TextRenderer::outline(cr, l, x, y, 3.5, pwvideo::Rgba{0, 0, 0, 0.60}, 1.5);
    pwvideo::TextRenderer::fill(cr, l, x, y, pwvideo::Rgba{1, 1, 1, 1.0});

    spec.sizePx = 18;
    spec.bold = false;
    l = text.layout(cr, line2, spec);
    pwvideo::TextRenderer::outline(cr, l, x, kH / 2.0 + 6, 5.0, pwvideo::Rgba{0, 0, 0, 0.45}, 1.0);
    pwvideo::TextRenderer::fill(cr, l, x, kH / 2.0 + 6, pwvideo::Rgba{1, 1, 1, 0.85});
  };

  if (!dumpPath.empty()) {
    draw(0);
    const bool ok = frame.writePng(dumpPath);
    std::printf("wrote %s: %s (%dx%d)\n", dumpPath.c_str(), ok ? "ok" : "failed", kW, kH);
    return ok ? 0 : 1;
  }

  pwvideo::Options opt;
  opt.width = kW;
  opt.height = kH;
  opt.fpsCap = 30;
  opt.nodeName = "pwnode-cairo";
  opt.nodeDescription = "Cairo Demo";
  opt.appName = "pwvideo-cairo-demo";
  opt.verbose = true;

  pwvideo::VideoNode node(opt, [&](uint8_t* dst, int stride, int w, int h) {
    draw(static_cast<int>(ticks.fetch_add(1, std::memory_order_relaxed)));
    frame.blitTo(dst, stride, w, h);
  });

  node.start();
  std::printf("node %s registered (%dx%d), Ctrl-C to exit\n", opt.nodeName.c_str(), kW, kH);
  std::fflush(stdout);
  node.run();
  return 0;
}
