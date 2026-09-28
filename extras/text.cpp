// Pango text layout and drawing for cairo frames.
//
// The layout is created against a throwaway 1x1 surface so a renderer exists before any frame
// does; layout() then rebinds it to the real target. Every field of LabelSpec is applied on
// each call, including the ones that clear state left by a previous call, so a reused renderer
// draws identically to a fresh one.
#include "text.hpp"

namespace pwvideo {

TextRenderer::TextRenderer() {
  cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t* cr = cairo_create(tmp);
  layout_ = pango_cairo_create_layout(cr);
  font_ = pango_font_description_new();
  pango_font_description_set_family(font_, "sans-serif");
  cairo_destroy(cr);
  cairo_surface_destroy(tmp);
}

TextRenderer::~TextRenderer() {
  if (font_) pango_font_description_free(font_);
  if (layout_) g_object_unref(layout_);
}

PangoLayout* TextRenderer::layout(cairo_t* target, const std::string& text,
                                  const LabelSpec& spec) {
  pango_cairo_update_layout(target, layout_);

  // The font description is rebuilt from scratch every call, so no attribute of a previous
  // label leaks into this one.
  pango_font_description_set_absolute_size(font_, spec.sizePx * PANGO_SCALE);
  pango_font_description_set_weight(font_, spec.bold ? PANGO_WEIGHT_SEMIBOLD
                                                     : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_family(font_, spec.family.c_str());
  pango_layout_set_font_description(layout_, font_);
  pango_layout_set_text(layout_, text.c_str(), -1);

  if (spec.widthPx == 0.0) {
    // Natural width: clear any width left by a previous call and do not wrap or ellipsize.
    pango_layout_set_width(layout_, -1);
  } else {
    pango_layout_set_width(layout_, static_cast<int>(spec.widthPx * PANGO_SCALE));
    pango_layout_set_alignment(layout_, spec.center ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
    pango_layout_set_wrap(layout_, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout_, spec.ellipsize ? PANGO_ELLIPSIZE_END
                                                       : PANGO_ELLIPSIZE_NONE);
  }
  pango_layout_set_height(layout_, -spec.maxLines);  // negative = at most N lines

  return layout_;
}

LabelMetrics TextRenderer::measure(PangoLayout* l) {
  LabelMetrics m;
  pango_layout_get_pixel_size(l, &m.width, &m.height);
  return m;
}

void TextRenderer::fill(cairo_t* cr, PangoLayout* l, double x, double y, const Rgba& color) {
  cairo_save(cr);
  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, l);
  cairo_restore(cr);
}

void TextRenderer::outline(cairo_t* cr, PangoLayout* l, double x, double y, double widthPx,
                           const Rgba& color, double offsetY) {
  cairo_save(cr);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_translate(cr, 0, offsetY);
  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
  cairo_set_line_width(cr, widthPx);
  cairo_move_to(cr, x, y);
  pango_cairo_layout_path(cr, l);
  cairo_stroke(cr);
  cairo_restore(cr);
}

}  // namespace pwvideo
