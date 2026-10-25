#include "TrayMenuIcons.h"
#include "TrayMenuIconsSvg.h"

#include <array>
#include <cstddef>
#include <memory>

namespace toybox::standalone
{
namespace
{
	constexpr float kDesignSize = 16.0f;

	enum class GlyphId
	{
		show,
		play,
		pause,
		stop,
		loop,
		fade,
		quit,
		count
	};

	const char* svgSource (GlyphId glyph)
	{
		switch (glyph)
		{
			case GlyphId::show:		return tray_svg::show;
			case GlyphId::play:		return tray_svg::play;
			case GlyphId::pause:	return tray_svg::pause;
			case GlyphId::stop:		return tray_svg::stop;
			case GlyphId::loop:		return tray_svg::loop;
			case GlyphId::fade:		return tray_svg::fade;
			case GlyphId::quit:		return tray_svg::quit;
			case GlyphId::count:
			default:				return nullptr;
		}
	}

	std::unique_ptr<juce::Drawable> parseSvg (const char* source)
	{
		if (source == nullptr)
		{
			return {};
		}

		auto xml = juce::XmlDocument::parse (juce::String::fromUTF8 (source));
		if (xml == nullptr)
		{
			return {};
		}

		return juce::Drawable::createFromSVG (*xml);
	}

	const juce::Drawable* cachedGlyph (GlyphId glyph)
	{
		// Parse all glyphs once. Drawing only ever reads the cached, immutable drawables.
		static const std::array<std::unique_ptr<juce::Drawable>,
								static_cast<std::size_t> (GlyphId::count)> cache = []
		{
			std::array<std::unique_ptr<juce::Drawable>,
					   static_cast<std::size_t> (GlyphId::count)> parsed;

			for (std::size_t i = 0; i < parsed.size(); ++i)
			{
				parsed[i] = parseSvg (svgSource (static_cast<GlyphId> (i)));
			}

			return parsed;
		}();

		const auto index = static_cast<std::size_t> (glyph);
		if (index >= cache.size())
		{
			return nullptr;
		}

		return cache[index].get();
	}

	const juce::Drawable* drawableFor (TrayMenuIcon icon)
	{
		switch (icon)
		{
			case TrayMenuIcon::showWindow:	return cachedGlyph (GlyphId::show);
			case TrayMenuIcon::play:		return cachedGlyph (GlyphId::play);
			case TrayMenuIcon::pause:		return cachedGlyph (GlyphId::pause);
			case TrayMenuIcon::stop:		return cachedGlyph (GlyphId::stop);
			case TrayMenuIcon::loop:		return cachedGlyph (GlyphId::loop);
			case TrayMenuIcon::fade:		return cachedGlyph (GlyphId::fade);
			case TrayMenuIcon::quit:		return cachedGlyph (GlyphId::quit);
			case TrayMenuIcon::none:
			default:						return nullptr;
		}
	}

	void drawSvg (juce::Graphics& graphics, const juce::Drawable& drawable, juce::Colour colour,
				  float opacity)
	{
		// The asset is monochrome, drawn in black: that is the ink it ships in, not a colour the
		// application chose, so it is a constant here rather than a palette role. Recolouring a
		// copy keeps the cached drawable reusable on light and dark menus alike.
		static constexpr juce::uint32 kSourceInk = 0xff000000;

		auto copy = drawable.createCopy();
		if (copy == nullptr)
		{
			return;
		}

		copy->replaceColour (juce::Colour (kSourceInk), colour);
		// Draw in the SVG's own 16x16 coordinates: drawWithin() would stretch the ink's
		// bounding box to the target, which makes each glyph a different size.
		copy->draw (graphics, opacity);
	}

	juce::Image renderIcon (TrayMenuIcon icon, int pixelSize, juce::Colour core, float opacity)
	{
		if (icon == TrayMenuIcon::none)
		{
			return {};
		}

		const auto size = juce::jlimit (8, 128, pixelSize);

		juce::Image image (juce::Image::ARGB, size, size, true);
		juce::Graphics graphics (image);
		graphics.addTransform (juce::AffineTransform::scale ((float) size / kDesignSize));

		const auto clampedOpacity = juce::jlimit (0.0f, 1.0f, opacity);

		if (const auto* drawable = drawableFor (icon))
		{
			drawSvg (graphics, *drawable, core, clampedOpacity);
		}

		return image;
	}
}

juce::Image makeTrayMenuIcon (TrayMenuIcon icon, int pixelSize, juce::Colour core, float opacity)
{
	return renderIcon (icon, pixelSize, core, opacity);
}

juce::Image makeTrayMenuIconTemplate (TrayMenuIcon icon, int pixelSize)
{
	// White is the operating system's contract for a template image - it inverts the silhouette
	// for a dark or a light taskbar - so this is not the application's colour to choose.
	static constexpr juce::uint32 kTemplateInk = 0xffffffff;

	return renderIcon (icon, pixelSize, juce::Colour (kTemplateInk), 1.0f);
}

} // namespace toybox::standalone
