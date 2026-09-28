#pragma once
// Pango text layout and drawing for cairo frames.
//
// One TextRenderer owns one PangoLayout and reuses it across calls, because creating a layout
// per draw costs more than laying out text with an existing one. The layout is bound to the
// target context on every call, so the same renderer can draw into several frames.
#include <pango/pangocairo.h>

#include <string>

#include "cairo_util.hpp"

namespace pwvideo {

/** Description of one text element to lay out and draw. */
struct LabelSpec {
  double sizePx = 16.0;               // absolute size, in pixels
  bool bold = false;                  // PANGO_WEIGHT_SEMIBOLD when set
  double widthPx = 0.0;               // 0 = natural width, no wrapping
  int maxLines = 1;                   // pango's negative-height convention is applied inside
  bool ellipsize = true;              // PANGO_ELLIPSIZE_END
  bool center = true;                 // PANGO_ALIGN_CENTER, otherwise PANGO_ALIGN_LEFT
  std::string family = "sans-serif";
};

/** Pixel size of a laid-out text. */
struct LabelMetrics {
  int width = 0;
  int height = 0;
};

/** Owns one pango layout and the font description behind it. Reuse the instance: creating a
 *  layout per call costs more than laying out text with an existing one. */
class TextRenderer {
 public:
  TextRenderer();
  ~TextRenderer();
  TextRenderer(const TextRenderer&) = delete;
  TextRenderer& operator=(const TextRenderer&) = delete;

  /** Lays out text for drawing into target. Calls pango_cairo_update_layout(target, layout)
   *  first, so the target's font options and scale apply. The returned layout is owned by the
   *  renderer and stays valid until the next layout() call or destruction. */
  PangoLayout* layout(cairo_t* target, const std::string& text, const LabelSpec& spec);

  static LabelMetrics measure(PangoLayout* l);

  /** Fills the laid-out glyphs at (x, y) in colour. */
  static void fill(cairo_t* cr, PangoLayout* l, double x, double y, const Rgba& color);

  /** Strokes the glyph path at (x, y + offsetY), for a shadow or an outline that keeps text
   *  legible over video. Call before fill(), and repeat with decreasing widthPx and increasing
   *  alpha to approximate a soft shadow. Saves and restores the cairo state. */
  static void outline(cairo_t* cr, PangoLayout* l, double x, double y, double widthPx,
                      const Rgba& color, double offsetY = 0.0);

 private:
  PangoLayout* layout_ = nullptr;
  PangoFontDescription* font_ = nullptr;
};

}  // namespace pwvideo
