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
		back,
		forward,
		loop,
		forever,
		fade,
		fadeOff,
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
			case GlyphId::back:		return tray_svg::back;
			case GlyphId::forward:	return tray_svg::forward;
			case GlyphId::loop:		return tray_svg::loop;
			case GlyphId::forever:	return tray_svg::forever;
			case GlyphId::fade:		return tray_svg::fade;
			case GlyphId::fadeOff:	return tray_svg::fadeOff;
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
			case TrayMenuIcon::back:		return cachedGlyph (GlyphId::back);
			case TrayMenuIcon::forward:		return cachedGlyph (GlyphId::forward);
			case TrayMenuIcon::loop:		return cachedGlyph (GlyphId::loop);
			case TrayMenuIcon::loop1:
			case TrayMenuIcon::loop2:
			case TrayMenuIcon::loop3:		return cachedGlyph (GlyphId::loop);
			case TrayMenuIcon::loopForever:	return cachedGlyph (GlyphId::forever);
			case TrayMenuIcon::fade:		return cachedGlyph (GlyphId::fade);
			case TrayMenuIcon::fadeOff:		return cachedGlyph (GlyphId::fadeOff);
			case TrayMenuIcon::quit:		return cachedGlyph (GlyphId::quit);
			case TrayMenuIcon::none:
			default:						return nullptr;
		}
	}

	void drawSvg (juce::Graphics& graphics, const juce::Drawable& drawable, juce::Colour colour,
				  float opacity)
	{
		// The asset is monochrome black; recolouring a copy keeps the cached drawable reusable
		// on light and dark menus alike.
		auto copy = drawable.createCopy();
		if (copy == nullptr)
		{
			return;
		}

		copy->replaceColour (juce::Colours::black, colour);
		// Draw in the SVG's own 16x16 coordinates: drawWithin() would stretch the ink's
		// bounding box to the target, which makes each glyph a different size.
		copy->draw (graphics, opacity);
	}

	void drawTickedBadge (juce::Graphics& graphics, juce::Colour core, float opacity)
	{
		constexpr float centreX = 12.9f;
		constexpr float centreY = 12.9f;
		constexpr float radius = 2.9f;

		juce::Path badge;
		badge.setUsingNonZeroWinding (false);
		badge.addEllipse (centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f);

		juce::Path check;
		check.startNewSubPath (11.4f, 13.05f);
		check.lineTo (12.45f, 14.1f);
		check.lineTo (14.5f, 11.35f);

		juce::Path checkStroke;
		juce::PathStrokeType (2.4f, juce::PathStrokeType::curved,
							  juce::PathStrokeType::rounded)
			.createStrokedPath (checkStroke, check);

		// Even-odd fill cuts the check out of the disc: the menu background (or the accent
		// highlight) shows through, so the tick needs no second colour.
		badge.addPath (checkStroke);

		graphics.setColour (core.withMultipliedAlpha (opacity));
		graphics.fillPath (badge);
	}

	juce::Image renderIcon (TrayMenuIcon icon, int pixelSize, bool ticked, juce::Colour core,
							float opacity)
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

		if (ticked)
		{
			drawTickedBadge (graphics, core, clampedOpacity);
		}

		return image;
	}
}

juce::Image makeTrayMenuIcon (TrayMenuIcon icon, int pixelSize, bool ticked, juce::Colour core,
							  float opacity)
{
	return renderIcon (icon, pixelSize, ticked, core, opacity);
}

juce::Image makeTrayMenuIconTemplate (TrayMenuIcon icon, int pixelSize)
{
	return renderIcon (icon, pixelSize, false, juce::Colours::white, 1.0f);
}

} // namespace toybox::standalone
