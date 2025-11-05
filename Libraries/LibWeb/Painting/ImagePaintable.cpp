/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(ImagePaintable);

GC::Ref<ImagePaintable> ImagePaintable::create(Layout::SVGImageBox const& layout_box)
{
    return layout_box.heap().allocate<ImagePaintable>(layout_box, layout_box.dom_node(), false, String {}, true);
}

GC::Ref<ImagePaintable> ImagePaintable::create(Layout::ImageBox const& layout_box)
{
    String alt;
    if (auto element = layout_box.dom_node())
        alt = element->get_attribute_value(HTML::AttributeNames::alt);
    return layout_box.heap().allocate<ImagePaintable>(layout_box, layout_box.image_provider(), layout_box.renders_as_alt_text(), move(alt), false);
}

ImagePaintable::ImagePaintable(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider, bool renders_as_alt_text, String alt_text, bool is_svg_image)
    : PaintableBox(layout_box)
    , m_renders_as_alt_text(renders_as_alt_text)
    , m_alt_text(move(alt_text))
    , m_image_provider(image_provider)
    , m_is_svg_image(is_svg_image)
{
    const_cast<DOM::Document&>(layout_box.document()).register_viewport_client(*this);
}

void ImagePaintable::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_image_provider.image_provider_visit_edges(visitor);
}

void ImagePaintable::finalize()
{
    Base::finalize();

    // NOTE: We unregister from the document in finalize() to avoid trouble
    //       in the scenario where our Document has already been swept by GC.
    document().unregister_viewport_client(*this);
}

void ImagePaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto image_rect = absolute_rect();
        auto image_rect_device_pixels = context.rounded_device_rect(image_rect);
        if (m_renders_as_alt_text) {
            if (!m_alt_text.is_empty()) {
                auto enclosing_rect = context.enclosing_device_rect(image_rect).to_type<int>();
                context.display_list_recorder().draw_rect(enclosing_rect, Gfx::Color::Black);
                context.display_list_recorder().draw_text(enclosing_rect, Utf16String::from_utf8(m_alt_text), *Platform::FontPlugin::the().default_font(12), Gfx::TextAlignment::Center, computed_values().color());
            }
        } else if (auto bitmap = m_image_provider.current_image_bitmap_sized(image_rect_device_pixels.size().to_type<int>())) {
            ScopedCornerRadiusClip corner_clip { context, image_rect_device_pixels, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };
            auto image_int_rect_device_pixels = image_rect_device_pixels.to_type<int>();
            auto bitmap_rect = bitmap->rect();
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), bitmap_rect, image_int_rect_device_pixels);

            // https://drafts.csswg.org/css-images/#the-object-fit
            auto object_fit = m_is_svg_image ? CSS::ObjectFit::Contain : computed_values().object_fit();
            Gfx::IntRect draw_rect = get_replaced_box_painting_area(*this, context, object_fit, bitmap->size());
            context.display_list_recorder().draw_scaled_immutable_bitmap(draw_rect, image_int_rect_device_pixels, *bitmap, scaling_mode);
        }
    }
}

void ImagePaintable::did_set_viewport_rect(CSSPixelRect const& viewport_rect)
{
    const_cast<Layout::ImageProvider&>(m_image_provider).set_visible_in_viewport(viewport_rect.intersects(absolute_rect()));
}

}
